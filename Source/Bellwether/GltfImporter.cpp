#include "GltfImporter.h"
#include <cgltf.h>
#include <mikktspace.h>
#include <stb_image.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>

namespace Bellwether
{

	namespace
	{
		constexpr size_t s_FileLimit = 64 * 1024 * 1024;
		constexpr size_t s_MemoryLimit = 256 * 1024 * 1024;
		constexpr size_t s_VertexLimit = 1000000;
		struct AllocationBudget
		{
			size_t Used = 0;
		};
		struct alignas(std::max_align_t) AllocationHeader
		{
			size_t Size;
		};
		void* Allocate(void* user, size_t size)
		{
			auto& budget = *static_cast<AllocationBudget*>(user);
			if (size > s_MemoryLimit - budget.Used)
				return nullptr;
			auto* header = static_cast<AllocationHeader*>(std::malloc(sizeof(AllocationHeader) + size));
			if (!header)
				return nullptr;
			header->Size = size;
			budget.Used += size;
			return header + 1;
		}
		void Release(void* user, void* pointer)
		{
			if (!pointer)
				return;
			auto* header = static_cast<AllocationHeader*>(pointer) - 1;
			static_cast<AllocationBudget*>(user)->Used -= header->Size;
			std::free(header);
		}
		std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
		{
			std::ifstream input(path, std::ios::binary | std::ios::ate);
			if (!input || input.tellg() <= 0 || input.tellg() > std::streamoff(s_FileLimit))
				throw std::runtime_error("Missing, empty or oversized glTF dependency: " + path.string());
			std::vector<uint8_t> bytes(static_cast<size_t>(input.tellg()));
			input.seekg(0);
			input.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
			if (!input)
				throw std::runtime_error("Truncated glTF dependency: " + path.string());
			return bytes;
		}
		std::vector<uint8_t> ReadUri(std::string uri, const std::filesystem::path& root)
		{
			if (uri.starts_with("data:"))
			{
				auto comma = uri.find(',');
				if (comma == std::string::npos || comma < 7 || uri.substr(comma - 7, 7) != ";base64")
					throw std::invalid_argument("glTF data URI must use base64");
				auto encoded = uri.substr(comma + 1);
				if (encoded.empty() || encoded.size() % 4 || encoded.size() / 4 * 3 > s_FileLimit)
					throw std::invalid_argument("Invalid or oversized glTF base64 payload");
				std::vector<uint8_t> bytes;
				bytes.reserve(encoded.size() / 4 * 3);
				const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
				for (size_t offset = 0; offset < encoded.size(); offset += 4)
				{
					uint32_t value = 0;
					int padding = 0;
					for (size_t index = 0; index < 4; index++)
					{
						char symbol = encoded[offset + index];
						if (symbol == '=')
						{
							if (offset + 4 != encoded.size() || index < 2)
								throw std::invalid_argument("Invalid base64 padding");
							padding++;
							value <<= 6;
						}
						else
						{
							auto digit = alphabet.find(symbol);
							if (digit == std::string::npos || padding)
								throw std::invalid_argument("Invalid base64 character");
							value = (value << 6) | uint32_t(digit);
						}
					}
					if ((padding == 1 && (value & 255)) || (padding == 2 && (value & 65535)))
						throw std::invalid_argument("Noncanonical base64 padding bits");
					bytes.push_back(uint8_t(value >> 16));
					if (padding < 2)
						bytes.push_back(uint8_t(value >> 8));
					if (padding < 1)
						bytes.push_back(uint8_t(value));
				}
				return bytes;
			}
			for (size_t index = 0; index < uri.size(); index++)
				if (uri[index] == '%')
				{
					if (index + 2 >= uri.size() || !std::isxdigit(static_cast<unsigned char>(uri[index + 1])) ||
						!std::isxdigit(static_cast<unsigned char>(uri[index + 2])) || uri.substr(index + 1, 2) == "00")
						throw std::invalid_argument("Invalid glTF URI escape");
					index += 2;
				}
			uri.resize(cgltf_decode_uri(uri.data()));
			if (uri.empty() || uri.find_first_of(":\\?#") != std::string::npos)
				throw std::invalid_argument("glTF dependencies require relative local file URIs");
			auto relative =
				std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(uri.data()), uri.size()));
			if (relative.has_root_path())
				throw std::invalid_argument("Absolute glTF dependency paths are forbidden");
			for (const auto& part : relative)
				if (part == "..")
					throw std::invalid_argument("glTF dependency escapes its asset directory");
			auto candidate = std::filesystem::weakly_canonical(root / relative);
			auto prefix = std::mismatch(root.begin(), root.end(), candidate.begin(), candidate.end());
			if (prefix.first != root.end())
				throw std::invalid_argument("glTF dependency link escapes its asset directory");
			return ReadFile(candidate);
		}
		void Require(bool condition, const char* message)
		{
			if (!condition)
				throw std::invalid_argument(message);
		}
		bool Finite(glm::vec4 value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
		}
		bool UnitRange(glm::vec4 value)
		{
			return Finite(value) && glm::all(glm::greaterThanEqual(value, glm::vec4(0))) &&
				   glm::all(glm::lessThanEqual(value, glm::vec4(1)));
		}
		void GenerateTangents(ModelPrimitive& primitive)
		{
			SMikkTSpaceInterface interface{};
			interface.m_getNumFaces = [](const SMikkTSpaceContext* context)
			{ return int(static_cast<ModelPrimitive*>(context->m_pUserData)->Vertices.size() / 3); };
			interface.m_getNumVerticesOfFace = [](const SMikkTSpaceContext*, int) { return 3; };
			interface.m_getPosition = [](const SMikkTSpaceContext* context, float* output, int face, int corner)
			{
				const auto& vertex =
					static_cast<ModelPrimitive*>(context->m_pUserData)->Vertices[size_t(face) * 3 + corner];
				std::memcpy(output, &vertex.Position, 3 * sizeof(float));
			};
			interface.m_getNormal = [](const SMikkTSpaceContext* context, float* output, int face, int corner)
			{
				const auto& vertex =
					static_cast<ModelPrimitive*>(context->m_pUserData)->Vertices[size_t(face) * 3 + corner];
				std::memcpy(output, &vertex.Normal, 3 * sizeof(float));
			};
			interface.m_getTexCoord = [](const SMikkTSpaceContext* context, float* output, int face, int corner)
			{
				const auto& vertex =
					static_cast<ModelPrimitive*>(context->m_pUserData)->Vertices[size_t(face) * 3 + corner];
				std::memcpy(output, &vertex.TexCoord, 2 * sizeof(float));
			};
			interface.m_setTSpaceBasic =
				[](const SMikkTSpaceContext* context, const float* tangent, float sign, int face, int corner)
			{
				auto& vertex = static_cast<ModelPrimitive*>(context->m_pUserData)->Vertices[size_t(face) * 3 + corner];
				vertex.Tangent = {tangent[0], tangent[1], tangent[2], sign};
			};
			SMikkTSpaceContext context{&interface, &primitive};
			Require(genTangSpaceDefault(&context) != 0, "Cannot generate glTF MikkTSpace tangents");
		}
	} // namespace

	ModelAsset ImportGltf(const std::filesystem::path& path)
	{
		static_assert(sizeof(ModelVertex) == 64);
		auto source = ReadFile(path);
		auto root = std::filesystem::weakly_canonical(path).parent_path();
		AllocationBudget allocation;
		cgltf_options options{};
		options.memory = {Allocate, Release, &allocation};
		cgltf_data* parsed = nullptr;
		auto parseResult = cgltf_parse(&options, source.data(), source.size(), &parsed);
		std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(parsed, cgltf_free);
		Require(parseResult == cgltf_result_success, "Invalid glTF/GLB document or parser memory budget exceeded");
		Require(data->nodes_count <= 100000 && data->meshes_count <= 10000 && data->materials_count <= 4096 &&
					data->images_count <= 256 && data->textures_count <= 1024 && data->accessors_count <= 100000,
				"glTF scene exceeds import budgets");
		Require(data->animations_count == 0 && data->skins_count == 0,
				"Animated or skinned glTF requires the pending animation importer");
		for (size_t index = 0; index < data->extensions_required_count; index++)
			Require(std::string(data->extensions_required[index]) == "KHR_materials_emissive_strength",
					"Required glTF extension is unsupported");
		for (size_t index = 0; index < data->buffers_count; index++)
		{
			auto& buffer = data->buffers[index];
			Require(buffer.size > 0 && buffer.size <= s_FileLimit, "glTF buffer exceeds size budget");
			if (buffer.uri)
			{
				auto bytes = ReadUri(buffer.uri, root);
				Require(bytes.size() >= buffer.size, "Truncated glTF buffer");
				buffer.data = Allocate(&allocation, buffer.size);
				Require(buffer.data != nullptr, "glTF buffer memory budget exceeded");
				std::memcpy(buffer.data, bytes.data(), buffer.size);
				buffer.data_free_method = cgltf_data_free_method_memory_free;
			}
			else
			{
				Require(index == 0 && data->bin && data->bin_size >= buffer.size, "Missing GLB binary chunk");
				buffer.data = const_cast<void*>(data->bin);
				buffer.data_free_method = cgltf_data_free_method_none;
			}
		}
		for (size_t index = 0; index < data->buffer_views_count; index++)
		{
			const auto& view = data->buffer_views[index];
			Require(view.buffer && view.size <= view.buffer->size && view.offset <= view.buffer->size - view.size &&
						!view.has_meshopt_compression,
					"Invalid or compressed glTF buffer view");
		}
		for (size_t index = 0; index < data->accessors_count; index++)
		{
			const auto& accessor = data->accessors[index];
			Require(accessor.count > 0 && accessor.count <= s_VertexLimit * 3 && accessor.offset <= s_FileLimit &&
						accessor.stride > 0 && accessor.stride <= 256,
					"Invalid or oversized glTF accessor");
			size_t componentSize = cgltf_component_size(accessor.component_type);
			Require(componentSize && accessor.offset % componentSize == 0 && accessor.stride % componentSize == 0 &&
						(!accessor.buffer_view || accessor.buffer_view->offset % componentSize == 0),
					"Misaligned glTF accessor");
			if (accessor.is_sparse)
			{
				Require(accessor.sparse.count <= accessor.count && accessor.sparse.indices_byte_offset <= s_FileLimit &&
							accessor.sparse.values_byte_offset <= s_FileLimit,
						"Invalid sparse glTF accessor");
				const auto& sparse = accessor.sparse;
				size_t indexSize = cgltf_component_size(sparse.indices_component_type);
				Require(indexSize && sparse.indices_buffer_view && sparse.values_buffer_view &&
							sparse.indices_buffer_view->stride == 0 && sparse.values_buffer_view->stride == 0 &&
							(sparse.indices_buffer_view->offset + sparse.indices_byte_offset) % indexSize == 0 &&
							(sparse.values_buffer_view->offset + sparse.values_byte_offset) % componentSize == 0,
						"Invalid or misaligned packed sparse glTF data");
			}
		}
		// Reject deep/cyclic hierarchies in linear time before the dependency's
		// ancestor-chain validation can perform quadratic work on hostile input.
		std::vector<uint8_t> hierarchyState(data->nodes_count, 0);
		std::vector<size_t> hierarchyDepth(data->nodes_count, 0);
		std::function<size_t(size_t, size_t)> parentDepth;
		parentDepth = [&](size_t index, size_t callDepth) -> size_t
		{
			Require(callDepth <= 128 && hierarchyState[index] != 1, "glTF hierarchy is cyclic or exceeds depth 128");
			if (hierarchyState[index] == 2)
				return hierarchyDepth[index];
			hierarchyState[index] = 1;
			auto* parent = data->nodes[index].parent;
			size_t depth = parent ? parentDepth(size_t(parent - data->nodes), callDepth + 1) + 1 : 0;
			Require(depth <= 128, "glTF hierarchy exceeds depth 128");
			hierarchyState[index] = 2;
			return hierarchyDepth[index] = depth;
		};
		for (size_t index = 0; index < data->nodes_count; index++)
			parentDepth(index, 0);
		Require(cgltf_validate(data.get()) == cgltf_result_success, "glTF validation failed");
		ModelAsset asset;
		size_t decodedBytes = 0;
		for (size_t index = 0; index < data->images_count; index++)
		{
			const auto& image = data->images[index];
			std::vector<uint8_t> bytes;
			if (image.uri)
				bytes = ReadUri(image.uri, root);
			else
			{
				Require(image.buffer_view != nullptr, "glTF image has no data");
				auto* begin = static_cast<const uint8_t*>(image.buffer_view->buffer->data) + image.buffer_view->offset;
				bytes.assign(begin, begin + image.buffer_view->size);
			}
			Require(!bytes.empty() && bytes.size() <= s_FileLimit, "glTF image data exceeds budget");
			int width = 0, height = 0, channels = 0;
			Require(stbi_info_from_memory(bytes.data(), int(bytes.size()), &width, &height, &channels) && width > 0 &&
						height > 0 && width <= 4096 && height <= 4096,
					"glTF image must be a valid PNG/JPEG at most 4096 pixels per side");
			size_t size = size_t(width) * height * 4;
			Require(size <= s_MemoryLimit - decodedBytes, "Decoded glTF image budget exceeded");
			auto* pixels = stbi_load_from_memory(bytes.data(), int(bytes.size()), &width, &height, &channels, 4);
			std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> ownedPixels(pixels, stbi_image_free);
			Require(pixels != nullptr, "glTF image decoding failed");
			asset.Images.push_back({uint32_t(width), uint32_t(height), {pixels, pixels + size}});
			decodedBytes += size;
		}
		for (size_t index = 0; index < data->textures_count; index++)
		{
			const auto& texture = data->textures[index];
			Require(texture.image && !texture.has_basisu && !texture.has_webp, "Unsupported glTF texture encoding");
			ModelTexture result;
			result.Image = size_t(texture.image - data->images);
			if (texture.sampler)
			{
				result.WrapS = texture.sampler->wrap_s;
				result.WrapT = texture.sampler->wrap_t;
				result.MinFilter = texture.sampler->min_filter ? texture.sampler->min_filter : 9987;
				result.MagFilter = texture.sampler->mag_filter ? texture.sampler->mag_filter : 9729;
			}
			Require((result.WrapS == 10497 || result.WrapS == 33648 || result.WrapS == 33071) &&
						(result.WrapT == 10497 || result.WrapT == 33648 || result.WrapT == 33071) &&
						(result.MagFilter == 9728 || result.MagFilter == 9729) &&
						(result.MinFilter == 9728 || result.MinFilter == 9729 ||
						 (result.MinFilter >= 9984 && result.MinFilter <= 9987)),
					"Invalid glTF sampler");
			asset.Textures.push_back(result);
		}
		asset.Materials.emplace_back();
		for (size_t index = 0; index < data->materials_count; index++)
		{
			const auto& material = data->materials[index];
			Require(!material.has_pbr_specular_glossiness && !material.has_clearcoat && !material.has_transmission &&
						!material.has_volume && !material.has_ior && !material.has_specular && !material.has_sheen &&
						!material.has_iridescence && !material.has_diffuse_transmission && !material.has_anisotropy &&
						!material.has_dispersion && !material.unlit && material.alpha_mode != cgltf_alpha_mode_blend,
					"Unsupported glTF material extension or alpha blending");
			ModelMaterial result;
			result.Name = material.name ? material.name : "Material";
			const auto& pbr = material.pbr_metallic_roughness;
			result.BaseColor = {pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2],
								pbr.base_color_factor[3]};
			result.Roughness = pbr.roughness_factor;
			result.Metallic = pbr.metallic_factor;
			result.Emission = {material.emissive_factor[0], material.emissive_factor[1], material.emissive_factor[2]};
			Require(UnitRange(result.BaseColor) && UnitRange(glm::vec4(result.Emission, 1)) &&
						UnitRange(glm::vec4(result.Roughness, result.Metallic, material.occlusion_texture.scale,
											material.alpha_cutoff)),
					"Invalid glTF material factors");
			if (material.has_emissive_strength)
			{
				float strength = material.emissive_strength.emissive_strength;
				Require(std::isfinite(strength) && strength >= 0 && strength <= 10000,
						"Invalid glTF emissive strength");
				result.Emission *= strength;
			}
			result.NormalScale = material.normal_texture.scale;
			Require(std::isfinite(result.NormalScale) && result.NormalScale >= 0 && result.NormalScale <= 100,
					"Invalid glTF normal scale");
			result.OcclusionStrength = material.occlusion_texture.scale;
			result.AlphaCutoff = material.alpha_cutoff;
			result.AlphaMask = material.alpha_mode == cgltf_alpha_mode_mask;
			result.DoubleSided = material.double_sided;
			std::array<const cgltf_texture_view*, 5> views{&pbr.base_color_texture, &pbr.metallic_roughness_texture,
														   &material.normal_texture, &material.occlusion_texture,
														   &material.emissive_texture};
			for (size_t slot = 0; slot < views.size(); slot++)
				if (views[slot]->texture)
				{
					Require(views[slot]->texcoord == 0 && !views[slot]->has_transform,
							"glTF texture requires unsupported UV set or texture transform");
					result.Textures[slot] = int(views[slot]->texture - data->textures);
				}
			asset.Materials.push_back(result);
		}
		std::vector<std::vector<size_t>> primitiveIndices(data->meshes_count);
		size_t vertexBytes = 0;
		for (size_t meshIndex = 0; meshIndex < data->meshes_count; meshIndex++)
			for (size_t primitiveIndex = 0; primitiveIndex < data->meshes[meshIndex].primitives_count; primitiveIndex++)
			{
				const auto& primitive = data->meshes[meshIndex].primitives[primitiveIndex];
				Require(primitive.type == cgltf_primitive_type_triangles && !primitive.targets_count &&
							!primitive.has_draco_mesh_compression && !primitive.mappings_count,
						"glTF requires unsupported topology, morphing, compression or material variants");
				ModelPrimitive result;
				result.Material = primitive.material ? size_t(primitive.material - data->materials) + 1 : 0;
				const auto& material = asset.Materials[result.Material];
				auto position = cgltf_find_accessor(&primitive, cgltf_attribute_type_position, 0);
				Require(position && position->type == cgltf_type_vec3 &&
							position->component_type == cgltf_component_type_r_32f && position->count <= s_VertexLimit,
						"glTF primitive requires bounded float POSITION VEC3");
				Require(position->count * sizeof(ModelVertex) <= s_MemoryLimit - vertexBytes,
						"glTF geometry budget exceeded");
				result.Vertices.resize(position->count);
				bool hasNormals = false, hasTangents = false, hasUV = false;
				for (size_t attributeIndex = 0; attributeIndex < primitive.attributes_count; attributeIndex++)
				{
					const auto& attribute = primitive.attributes[attributeIndex];
					Require(attribute.index == 0 && attribute.type >= cgltf_attribute_type_position &&
								attribute.type <= cgltf_attribute_type_color,
							"Unsupported glTF vertex attribute");
					const auto& accessor = *attribute.data;
					size_t components = cgltf_num_components(accessor.type);
					Require(accessor.count == position->count && components >= 2 && components <= 4,
							"Mismatched glTF vertex attributes");
					size_t expected = attribute.type == cgltf_attribute_type_texcoord  ? 2
									  : attribute.type == cgltf_attribute_type_tangent ? 4
																					   : 3;
					Require(components == expected || (attribute.type == cgltf_attribute_type_color && components == 4),
							"Invalid glTF attribute dimensions");
					std::vector<float> values(accessor.count * components);
					auto baseAccessor = accessor;
					baseAccessor.is_sparse = false;
					Require(cgltf_accessor_unpack_floats(&baseAccessor, values.data(), values.size()) == values.size(),
							"Cannot unpack glTF accessor");
					if (accessor.is_sparse)
					{
						// Sparse replacements are tightly packed even when the base accessor
						// uses an interleaved stride. Do not reuse its stride for replacements.
						const auto& sparse = accessor.sparse;
						auto packed = baseAccessor;
						packed.buffer_view = sparse.values_buffer_view;
						packed.offset = sparse.values_byte_offset;
						packed.stride = cgltf_calc_size(accessor.type, accessor.component_type);
						packed.count = sparse.count;
						std::vector<float> replacements(sparse.count * components);
						Require(cgltf_accessor_unpack_floats(&packed, replacements.data(), replacements.size()) ==
									replacements.size(),
								"Cannot unpack sparse glTF replacements");
						cgltf_accessor indices{};
						indices.type = cgltf_type_scalar;
						indices.component_type = sparse.indices_component_type;
						indices.buffer_view = sparse.indices_buffer_view;
						indices.offset = sparse.indices_byte_offset;
						indices.stride = cgltf_component_size(indices.component_type);
						indices.count = sparse.count;
						size_t previous = 0;
						for (size_t replacement = 0; replacement < sparse.count; replacement++)
						{
							size_t destination = cgltf_accessor_read_index(&indices, replacement);
							Require(destination < accessor.count && (replacement == 0 || destination > previous),
									"Sparse glTF indices must be increasing and in range");
							std::copy_n(replacements.data() + replacement * components, components,
										values.data() + destination * components);
							previous = destination;
						}
					}
					for (size_t vertexIndex = 0; vertexIndex < accessor.count; vertexIndex++)
					{
						glm::vec4 value(0, 0, 0, 1);
						std::memcpy(&value, values.data() + vertexIndex * components, components * sizeof(float));
						Require(Finite(value) && glm::all(glm::lessThanEqual(glm::abs(value), glm::vec4(1000000))),
								"Non-finite or out-of-bounds glTF vertex data");
						auto& vertex = result.Vertices[vertexIndex];
						switch (attribute.type)
						{
						case cgltf_attribute_type_position:
							vertex.Position = value;
							break;
						case cgltf_attribute_type_normal:
							Require(glm::length(glm::vec3(value)) > .0001f, "Zero glTF normal");
							vertex.Normal = glm::normalize(glm::vec3(value));
							hasNormals = true;
							break;
						case cgltf_attribute_type_texcoord:
							vertex.TexCoord = value;
							hasUV = true;
							break;
						case cgltf_attribute_type_tangent:
							Require(glm::length(glm::vec3(value)) > .0001f && std::abs(value.w) == 1,
									"Invalid glTF tangent");
							vertex.Tangent = glm::vec4(glm::normalize(glm::vec3(value)), value.w);
							hasTangents = true;
							break;
						case cgltf_attribute_type_color:
							Require(UnitRange(value), "Invalid glTF vertex color");
							vertex.Color = value;
							break;
						default:
							break;
						}
					}
				}
				Require(hasUV || std::all_of(material.Textures.begin(), material.Textures.end(),
											 [](int texture) { return texture < 0; }),
						"Textured glTF primitive requires TEXCOORD_0");
				size_t indexCount = primitive.indices ? primitive.indices->count : position->count;
				Require(indexCount && indexCount % 3 == 0 && indexCount <= s_VertexLimit * 3,
						"Invalid glTF triangle indices");
				if (primitive.indices)
					Require(!primitive.indices->is_sparse && primitive.indices->type == cgltf_type_scalar &&
								(primitive.indices->component_type == cgltf_component_type_r_8u ||
								 primitive.indices->component_type == cgltf_component_type_r_16u ||
								 primitive.indices->component_type == cgltf_component_type_r_32u),
							"Unsupported glTF index accessor");
				result.Indices.resize(indexCount);
				for (size_t index = 0; index < indexCount; index++)
				{
					auto vertex = primitive.indices ? cgltf_accessor_read_index(primitive.indices, index) : index;
					Require(vertex < result.Vertices.size(), "glTF mesh index out of range");
					result.Indices[index] = uint32_t(vertex);
				}
				bool generateTangents = material.Textures[2] >= 0 && !hasTangents;
				if (!hasNormals || generateTangents)
				{
					Require(indexCount * sizeof(ModelVertex) <= s_MemoryLimit - vertexBytes,
							"Expanded glTF geometry exceeds budget");
					std::vector<ModelVertex> expanded;
					expanded.reserve(indexCount);
					for (auto index : result.Indices)
						expanded.push_back(result.Vertices[index]);
					result.Vertices = std::move(expanded);
					std::iota(result.Indices.begin(), result.Indices.end(), 0u);
					if (!hasNormals)
						for (size_t index = 0; index < indexCount; index += 3)
						{
							auto edge1 = result.Vertices[index + 1].Position - result.Vertices[index].Position;
							auto edge2 = result.Vertices[index + 2].Position - result.Vertices[index].Position;
							auto normal = glm::cross(edge1, edge2);
							Require(glm::length(normal) > .000001f,
									"Cannot generate normals for a degenerate glTF triangle");
							for (size_t corner = 0; corner < 3; corner++)
								result.Vertices[index + corner].Normal = glm::normalize(normal);
						}
					if (generateTangents)
						GenerateTangents(result);
				}
				for (const auto& vertex : result.Vertices)
					if (material.Textures[2] >= 0)
						Require(Finite(vertex.Tangent) && glm::length(glm::vec3(vertex.Tangent)) > .1f &&
									std::abs(glm::dot(vertex.Normal, glm::vec3(vertex.Tangent))) < .01f,
								"Invalid glTF tangent frame");
				vertexBytes += result.Vertices.size() * sizeof(ModelVertex) + result.Indices.size() * sizeof(uint32_t);
				Require(vertexBytes <= s_MemoryLimit, "glTF geometry budget exceeded");
				primitiveIndices[meshIndex].push_back(asset.Primitives.size());
				asset.Primitives.push_back(std::move(result));
			}
		std::vector<uint8_t> active(data->nodes_count, 0);
		std::function<void(const cgltf_node*, const glm::mat4&, size_t)> visit;
		visit = [&](const cgltf_node* node, const glm::mat4& parent, size_t depth)
		{
			size_t index = size_t(node - data->nodes);
			Require(depth <= 128 && !active[index], "glTF node hierarchy is cyclic, repeated or too deep");
			active[index] = 1;
			Require(!node->skin && !node->has_mesh_gpu_instancing && !node->camera && !node->light,
					"glTF node contains unsupported skin, instancing, camera or light");
			glm::mat4 local;
			cgltf_node_transform_local(node, &local[0][0]);
			auto transform = parent * local;
			float determinant = glm::determinant(transform);
			Require(std::isfinite(determinant) && std::abs(determinant) > 1e-8f && transform[0][3] == 0 &&
						transform[1][3] == 0 && transform[2][3] == 0 && transform[3][3] == 1,
					"Invalid glTF node transform");
			for (size_t column = 0; column < 4; column++)
				Require(Finite(transform[column]) &&
							glm::all(glm::lessThanEqual(glm::abs(transform[column]), glm::vec4(1000000))),
						"Non-finite or out-of-bounds glTF node transform");
			if (node->mesh)
				for (auto primitive : primitiveIndices[size_t(node->mesh - data->meshes)])
				{
					Require(asset.Placements.size() < 100000, "glTF placement budget exceeded");
					asset.Placements.push_back({node->name ? node->name : "Mesh", primitive, transform});
				}
			for (size_t child = 0; child < node->children_count; child++)
				visit(node->children[child], transform, depth + 1);
		};
		const cgltf_scene* scene = data->scene ? data->scene : data->scenes_count ? &data->scenes[0] : nullptr;
		if (scene)
			for (size_t index = 0; index < scene->nodes_count; index++)
				visit(scene->nodes[index], glm::mat4(1), 0);
		else
			for (size_t index = 0; index < data->nodes_count; index++)
				if (!data->nodes[index].parent)
					visit(&data->nodes[index], glm::mat4(1), 0);
		Require(!asset.Placements.empty(), "glTF selected scene contains no renderable mesh placements");
		return asset;
	}

} // namespace Bellwether
