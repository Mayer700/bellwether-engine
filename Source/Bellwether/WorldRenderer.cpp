#include "WorldRenderer.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>

namespace Bellwether
{

	static std::vector<char> ReadBinary(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if (!input || input.tellg() < 0 || input.tellg() > 256 * 1024 * 1024)
			throw std::runtime_error("Invalid or missing binary file: " + path.string());
		std::vector<char> data(static_cast<size_t>(input.tellg()));
		input.seekg(0);
		input.read(data.data(), static_cast<std::streamsize>(data.size()));
		if (!input)
			throw std::runtime_error("Truncated binary file: " + path.string());
		return data;
	}

	static std::filesystem::path ResolveAssetPath(const std::filesystem::path& directory,
												  const std::filesystem::path& relative)
	{
		if (relative.has_root_path() || relative.generic_string().find("..") != std::string::npos)
			throw std::invalid_argument("Mesh paths must stay within the content directory");
		auto root = std::filesystem::weakly_canonical(directory);
		auto candidate = std::filesystem::weakly_canonical(root / relative);
		auto prefix = std::mismatch(root.begin(), root.end(), candidate.begin(), candidate.end());
		if (prefix.first != root.end())
			throw std::invalid_argument("Asset link resolves outside the content directory");
		return candidate;
	}

	WorldRenderer::WorldRenderer(nvrhi::IDevice* device) : m_Device(device)
	{
		if (!device)
			throw std::invalid_argument("World renderer requires a graphics device");
		static_assert(sizeof(FrameData) == 256);
		auto directory = GetExecutableDirectory() / "Shaders";
		auto shader = [&](const char* filename, nvrhi::ShaderType type)
		{
			auto data = ReadBinary(directory / filename);
			auto result = device->createShader(nvrhi::ShaderDesc().setShaderType(type).setDebugName(filename),
											   data.data(), data.size());
			if (!result)
				throw std::runtime_error(std::string("Cannot create shader: ") + filename);
			return result;
		};
		m_VertexShader = shader("World.vert.spv", nvrhi::ShaderType::Vertex);
		m_FragmentShader = shader("World.frag.spv", nvrhi::ShaderType::Pixel);
		m_ShadowShader = shader("Shadow.vert.spv", nvrhi::ShaderType::Vertex);
		m_ShadowFragmentShader = shader("Shadow.frag.spv", nvrhi::ShaderType::Pixel);
		m_FullscreenShader = shader("Fullscreen.vert.spv", nvrhi::ShaderType::Vertex);
		m_TonemapShader = shader("Tonemap.frag.spv", nvrhi::ShaderType::Pixel);
		nvrhi::VertexAttributeDesc attributes[] = {nvrhi::VertexAttributeDesc()
													   .setName("POSITION")
													   .setFormat(nvrhi::Format::RGB32_FLOAT)
													   .setOffset(0)
													   .setElementStride(64),
												   nvrhi::VertexAttributeDesc()
													   .setName("NORMAL")
													   .setFormat(nvrhi::Format::RGB32_FLOAT)
													   .setOffset(12)
													   .setElementStride(64),
												   nvrhi::VertexAttributeDesc()
													   .setName("TEXCOORD")
													   .setFormat(nvrhi::Format::RG32_FLOAT)
													   .setOffset(24)
													   .setElementStride(64),
												   nvrhi::VertexAttributeDesc()
													   .setName("TANGENT")
													   .setFormat(nvrhi::Format::RGBA32_FLOAT)
													   .setOffset(32)
													   .setElementStride(64),
												   nvrhi::VertexAttributeDesc()
													   .setName("COLOR")
													   .setFormat(nvrhi::Format::RGBA32_FLOAT)
													   .setOffset(48)
													   .setElementStride(64)};
		m_InputLayout = device->createInputLayout(attributes, 5, m_VertexShader);
		auto materialLayout =
			nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Pixel).setRegisterSpaceAndDescriptorSet(1);
		materialLayout.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
		for (uint32_t slot = 0; slot < 5; slot++)
			materialLayout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot))
				.addItem(nvrhi::BindingLayoutItem::Sampler(slot));
		m_MaterialLayout = device->createBindingLayout(materialLayout);
		m_Layout = device->createBindingLayout(nvrhi::BindingLayoutDesc()
												   .setRegisterSpaceAndDescriptorSet(0)
												   .setVisibility(nvrhi::ShaderType::All)
												   .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
												   .addItem(nvrhi::BindingLayoutItem::PushConstants(1, 96))
												   .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
												   .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
												   .addItem(nvrhi::BindingLayoutItem::Sampler(0)));
		m_ShadowLayout = device->createBindingLayout(nvrhi::BindingLayoutDesc()
														 .setRegisterSpaceAndDescriptorSet(0)
														 .setVisibility(nvrhi::ShaderType::Vertex)
														 .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
														 .addItem(nvrhi::BindingLayoutItem::PushConstants(1, 96)));
		m_PostLayout = device->createBindingLayout(nvrhi::BindingLayoutDesc()
													   .setRegisterSpaceAndDescriptorSet(0)
													   .setVisibility(nvrhi::ShaderType::All)
													   .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
													   .addItem(nvrhi::BindingLayoutItem::Sampler(0))
													   .addItem(nvrhi::BindingLayoutItem::PushConstants(0, 16)));
		m_Camera = device->createBuffer(nvrhi::BufferDesc()
											.setByteSize(sizeof(FrameData))
											.setIsConstantBuffer(true)
											.setIsVolatile(true)
											.setMaxVersions(64)
											.setDebugName("Camera"));
		m_ShadowSampler = device->createSampler(nvrhi::SamplerDesc()
													.setAllFilters(false)
													.setAllAddressModes(nvrhi::SamplerAddressMode::Border)
													.setBorderColor(nvrhi::Color(1)));
		m_LinearSampler = device->createSampler(
			nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
		if (!m_InputLayout || !m_Layout || !m_MaterialLayout || !m_ShadowLayout || !m_PostLayout || !m_Camera ||
			!m_ShadowSampler || !m_LinearSampler)
			throw std::runtime_error("Cannot create renderer layouts, frame data or samplers");
		for (size_t index = 0; index < 2; index++)
		{
			m_ShadowMaps[index] = device->createTexture(nvrhi::TextureDesc()
															.setWidth(s_ShadowResolution)
															.setHeight(s_ShadowResolution)
															.setFormat(nvrhi::Format::D32)
															.setIsRenderTarget(true)
															.setInitialState(nvrhi::ResourceStates::ShaderResource)
															.setKeepInitialState(true)
															.setDebugName("Sun Cascade " + std::to_string(index)));
			if (!m_ShadowMaps[index])
				throw std::runtime_error("Cannot allocate sun shadow map");
			m_ShadowFramebuffers[index] =
				device->createFramebuffer(nvrhi::FramebufferDesc().setDepthAttachment(m_ShadowMaps[index]));
			if (!m_ShadowFramebuffers[index])
				throw std::runtime_error("Cannot create sun shadow framebuffer");
		}
		m_Bindings = device->createBindingSet(nvrhi::BindingSetDesc()
												  .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_Camera))
												  .addItem(nvrhi::BindingSetItem::PushConstants(1, 96))
												  .addItem(nvrhi::BindingSetItem::Texture_SRV(0, m_ShadowMaps[0]))
												  .addItem(nvrhi::BindingSetItem::Texture_SRV(1, m_ShadowMaps[1]))
												  .addItem(nvrhi::BindingSetItem::Sampler(0, m_ShadowSampler)),
											  m_Layout);
		m_ShadowBindings = device->createBindingSet(nvrhi::BindingSetDesc()
														.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_Camera))
														.addItem(nvrhi::BindingSetItem::PushConstants(1, 96)),
													m_ShadowLayout);
		auto shadowPipeline = nvrhi::GraphicsPipelineDesc()
								  .setVertexShader(m_ShadowShader)
								  .setPixelShader(m_ShadowFragmentShader)
								  .setInputLayout(m_InputLayout)
								  .addBindingLayout(m_ShadowLayout)
								  .addBindingLayout(m_MaterialLayout);
		shadowPipeline.renderState.rasterState.setCullMode(nvrhi::RasterCullMode::None)
			.setFrontCounterClockwise(true)
			.setDepthBias(2)
			.setSlopeScaleDepthBias(1.5f);
		shadowPipeline.renderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(true).setDepthFunc(
			nvrhi::ComparisonFunc::Less);
		m_ShadowPipeline =
			device->createGraphicsPipeline(shadowPipeline, m_ShadowFramebuffers[0]->getFramebufferInfo());
		if (!m_VertexShader || !m_FragmentShader || !m_Layout || !m_Camera || !m_Bindings || !m_InputLayout ||
			!m_ShadowBindings || !m_ShadowPipeline || !m_LinearSampler || !m_ShadowSampler)
			throw std::runtime_error("World renderer resource initialization failed");
	}

	WorldRenderer::Mesh WorldRenderer::UploadMesh(nvrhi::ICommandList* commands, const std::vector<float>& vertices,
												  const std::vector<uint32_t>& indices)
	{
		std::vector<ModelVertex> converted;
		converted.reserve(vertices.size() / 8);
		for (size_t index = 0; index < vertices.size(); index += 8)
		{
			ModelVertex vertex;
			vertex.Position = {vertices[index], vertices[index + 1], vertices[index + 2]};
			vertex.Normal = {vertices[index + 3], vertices[index + 4], vertices[index + 5]};
			vertex.TexCoord = {vertices[index + 6], vertices[index + 7]};
			converted.push_back(vertex);
		}
		return UploadMesh(commands, converted, indices);
	}

	WorldRenderer::Mesh WorldRenderer::UploadMesh(nvrhi::ICommandList* commands,
												  const std::vector<ModelVertex>& vertices,
												  const std::vector<uint32_t>& indices)
	{
		Mesh result;
		glm::vec3 minimum = vertices.at(0).Position, maximum = minimum;
		for (const auto& vertex : vertices)
		{
			auto position = vertex.Position;
			minimum = glm::min(minimum, position);
			maximum = glm::max(maximum, position);
		}
		result.BoundsCenter = (minimum + maximum) * .5f;
		result.BoundsRadius = glm::length(maximum - minimum) * .5f;
		result.IndexCount = static_cast<uint32_t>(indices.size());
		result.Vertices = m_Device->createBuffer(nvrhi::BufferDesc()
													 .setByteSize(vertices.size() * sizeof(ModelVertex))
													 .setIsVertexBuffer(true)
													 .setInitialState(nvrhi::ResourceStates::VertexBuffer)
													 .setKeepInitialState(true)
													 .setDebugName("World Vertices"));
		result.Indices = m_Device->createBuffer(nvrhi::BufferDesc()
													.setByteSize(indices.size() * sizeof(uint32_t))
													.setIsIndexBuffer(true)
													.setInitialState(nvrhi::ResourceStates::IndexBuffer)
													.setKeepInitialState(true)
													.setDebugName("World Indices"));
		if (!result.Vertices || !result.Indices)
			throw std::runtime_error("Cannot allocate mesh buffers");
		commands->writeBuffer(result.Vertices, vertices.data(), vertices.size() * sizeof(ModelVertex));
		commands->writeBuffer(result.Indices, indices.data(), indices.size() * sizeof(uint32_t));
		return result;
	}

	void WorldRenderer::Load(const std::filesystem::path& directory)
	{
		auto manifest = ReadJson(directory / "world.json");
		auto candidateSettings =
			manifest.contains("Renderer") ? RenderSettings::Deserialize(manifest["Renderer"]) : RenderSettings{};
		if (manifest.at("format") != 1)
			throw std::runtime_error("Unsupported world bridge format");
		struct CPUMesh
		{
			std::vector<float> Vertices;
			std::vector<uint32_t> Indices;
		};
		std::vector<CPUMesh> sourceMeshes;
		std::vector<Instance> instances;
		for (const auto& entry : manifest.at("meshes"))
		{
			auto relative = std::filesystem::path(entry.at("path").get<std::string>());
			auto data = ReadBinary(ResolveAssetPath(directory, relative));
			if (data.size() < 8)
				throw std::runtime_error("Truncated mesh header");
			uint32_t vertexCount = 0, indexCount = 0;
			std::memcpy(&vertexCount, data.data(), 4);
			std::memcpy(&indexCount, data.data() + 4, 4);
			if (!vertexCount || !indexCount || indexCount % 3 || vertexCount > 10000000 || indexCount > 30000000 ||
				data.size() != 8 + size_t(vertexCount) * 32 + size_t(indexCount) * 4)
				throw std::runtime_error("Invalid mesh sizes");
			std::vector<float> vertices(size_t(vertexCount) * 8);
			std::vector<uint32_t> indices(indexCount);
			std::memcpy(vertices.data(), data.data() + 8, vertices.size() * 4);
			std::memcpy(indices.data(), data.data() + 8 + vertices.size() * 4, indices.size() * 4);
			for (auto value : vertices)
				if (!std::isfinite(value))
					throw std::runtime_error("Non-finite mesh vertex data");
			for (size_t vertex = 0; vertex < vertices.size(); vertex += 8)
			{
				auto normal = glm::vec3(vertices[vertex + 3], vertices[vertex + 4], vertices[vertex + 5]);
				float lengthSquared = glm::dot(normal, normal);
				if (!std::isfinite(lengthSquared) || lengthSquared < 1e-12f)
					throw std::runtime_error("Invalid mesh normal");
			}
			for (auto index : indices)
				if (index >= vertexCount)
					throw std::runtime_error("Mesh index out of range");
			sourceMeshes.push_back({std::move(vertices), std::move(indices)});
		}
		for (const auto& entry : manifest.at("draws"))
		{
			if (!entry.at("visible").get<bool>())
				continue;
			Instance instance;
			if (!entry.at("mesh").is_number_unsigned() && !entry.at("mesh").is_number_integer())
				throw std::runtime_error("Mesh instance index must be integral");
			instance.MeshIndex = entry.at("mesh");
			if (instance.MeshIndex >= sourceMeshes.size() || entry.at("matrix").size() != 16)
				throw std::runtime_error("Invalid mesh instance");
			for (int column = 0; column < 4; column++)
				for (int row = 0; row < 4; row++)
					instance.Transform[column][row] = entry.at("matrix")[column * 4 + row];
			for (int column = 0; column < 4; column++)
				for (int row = 0; row < 4; row++)
					if (!std::isfinite(instance.Transform[column][row]))
						throw std::runtime_error("Non-finite instance transform");
			float determinant = glm::determinant(instance.Transform);
			if (!std::isfinite(determinant) || std::abs(determinant) < 0.00000001f)
				throw std::runtime_error("Singular instance transform");
			if (instance.Transform[0][3] != 0 || instance.Transform[1][3] != 0 || instance.Transform[2][3] != 0 ||
				instance.Transform[3][3] != 1)
				throw std::runtime_error("Mesh transforms must be affine");
			instance.AlbedoRoughness =
				glm::vec4(ReadVector(Json::array({entry.at("color")[0], entry.at("color")[1], entry.at("color")[2]})),
						  entry.at("roughness").get<float>());
			instance.EmissionMetallic = glm::vec4(ReadVector(entry.at("emission")), entry.at("metallic").get<float>());
			if (glm::any(glm::lessThan(glm::vec3(instance.AlbedoRoughness), glm::vec3(0))) ||
				glm::any(glm::greaterThan(glm::vec3(instance.AlbedoRoughness), glm::vec3(1))) ||
				glm::any(glm::lessThan(glm::vec3(instance.EmissionMetallic), glm::vec3(0))) ||
				glm::any(glm::greaterThan(glm::vec3(instance.EmissionMetallic), glm::vec3(10000))))
				throw std::runtime_error("Albedo must be in [0,1] and HDR emission in [0,10000]");
			if (!std::isfinite(instance.AlbedoRoughness.w) || instance.AlbedoRoughness.w < 0 ||
				instance.AlbedoRoughness.w > 1 || !std::isfinite(instance.EmissionMetallic.w) ||
				instance.EmissionMetallic.w < 0 || instance.EmissionMetallic.w > 1)
				throw std::runtime_error("Invalid material roughness or metallic value");
			instances.push_back(instance);
		}
		int size = manifest.at("height_size");
		float extent = manifest.at("world_size");
		auto heightBytes = ReadBinary(ResolveAssetPath(directory, "height.f32"));
		if (size < 2 || size > 8192 || heightBytes.size() != size_t(size) * size * 4 || !std::isfinite(extent) ||
			extent <= 0)
			throw std::runtime_error("Invalid heightfield");
		std::vector<float> heights(size_t(size) * size);
		std::memcpy(heights.data(), heightBytes.data(), heightBytes.size());
		for (auto value : heights)
			if (!std::isfinite(value))
				throw std::runtime_error("Non-finite terrain height");
		constexpr int gridSize = 513;
		std::vector<float> vertices;
		std::vector<uint32_t> indices;
		vertices.reserve(gridSize * gridSize * 8);
		auto sample = [&](int x, int z)
		{ return heights[size_t(std::clamp(z, 0, size - 1)) * size + std::clamp(x, 0, size - 1)]; };
		for (int z = 0; z < gridSize; z++)
		{
			for (int x = 0; x < gridSize; x++)
			{
				int hx = x * (size - 1) / (gridSize - 1), hz = z * (size - 1) / (gridSize - 1);
				auto normal = glm::normalize(glm::vec3(sample(hx - 1, hz) - sample(hx + 1, hz), 2 * extent / size,
													   sample(hx, hz - 1) - sample(hx, hz + 1)));
				vertices.insert(vertices.end(), {x * extent / (gridSize - 1) - extent / 2, sample(hx, hz),
												 z * extent / (gridSize - 1) - extent / 2, normal.x, normal.y, normal.z,
												 x / float(gridSize - 1), z / float(gridSize - 1)});
			}
		}
		for (uint32_t z = 0; z < gridSize - 1; z++)
			for (uint32_t x = 0; x < gridSize - 1; x++)
			{
				auto index = z * gridSize + x;
				indices.insert(indices.end(),
							   {index, index + gridSize, index + 1, index + 1, index + gridSize, index + gridSize + 1});
			}
		Instance terrain;
		terrain.MeshIndex = sourceMeshes.size();
		terrain.AlbedoRoughness = {.85f, .91f, .98f, .85f};
		sourceMeshes.push_back({std::move(vertices), std::move(indices)});
		instances.push_back(terrain);
		// Malformed files cannot leave an immediate GPU command list open. Only
		// validated CPU data reaches this upload transaction.
		std::vector<Mesh> meshes;
		meshes.reserve(sourceMeshes.size());
		auto commands = m_Device->createCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false));
		commands->open();
		for (const auto& source : sourceMeshes)
			meshes.push_back(UploadMesh(commands, source.Vertices, source.Indices));
		ModelAsset legacy;
		legacy.Materials.emplace_back();
		legacy.Materials[0].DoubleSided = true;
		auto materials = UploadMaterials(commands, legacy, true);
		commands->close();
		m_Device->executeCommandList(commands);
		if (!m_Device->waitForIdle())
			throw std::runtime_error("Graphics device lost while completing world upload");
		m_Meshes = std::move(meshes);
		m_Instances = std::move(instances);
		m_Materials = std::move(materials.Materials);
		m_MaterialSamplerCount = materials.SamplerCount;
		m_Settings = candidateSettings;
	}

	void WorldRenderer::DrawGeometry(nvrhi::ICommandList* commands, nvrhi::IFramebuffer* framebuffer,
									 nvrhi::IGraphicsPipeline* pipeline, nvrhi::IBindingSet* bindings, uint32_t width,
									 uint32_t height, float range)
	{
		auto target = m_Settings.CameraTarget;
		nvrhi::GraphicsState state;
		state.setPipeline(pipeline)
			.setFramebuffer(framebuffer)
			.addBindingSet(bindings)
			.setViewport(
				nvrhi::ViewportState().addViewportAndScissorRect(nvrhi::Viewport(float(width), float(height))));
		for (const auto& instance : m_Instances)
		{
			const auto& mesh = m_Meshes[instance.MeshIndex];
			auto center = glm::vec3(instance.Transform * glm::vec4(mesh.BoundsCenter, 1));
			float scaleBound = std::sqrt(glm::dot(glm::vec3(instance.Transform[0]), glm::vec3(instance.Transform[0])) +
										 glm::dot(glm::vec3(instance.Transform[1]), glm::vec3(instance.Transform[1])) +
										 glm::dot(glm::vec3(instance.Transform[2]), glm::vec3(instance.Transform[2])));
			if (glm::length(glm::vec2(center.x - target.x, center.z - target.z)) >
				range + mesh.BoundsRadius * scaleBound)
				continue;
			state.vertexBuffers.resize(0);
			state.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(mesh.Vertices).setSlot(0).setOffset(0));
			state.setIndexBuffer(
				nvrhi::IndexBufferBinding().setBuffer(mesh.Indices).setFormat(nvrhi::Format::R32_UINT).setOffset(0));
			state.bindings.resize(1);
			state.addBindingSet(m_Materials.at(instance.MaterialIndex).Bindings);
			commands->setGraphicsState(state);
			struct ObjectData
			{
				glm::mat4 Transform;
				glm::vec4 AlbedoRoughness;
				glm::vec4 EmissionMetallic;
			};
			ObjectData object{instance.Transform, instance.AlbedoRoughness, instance.EmissionMetallic};
			static_assert(sizeof(ObjectData) == 96);
			commands->setPushConstants(&object, sizeof(object));
			commands->drawIndexed(nvrhi::DrawArguments().setVertexCount(mesh.IndexCount));
		}
	}

	void WorldRenderer::ResizeTargets(uint32_t width, uint32_t height)
	{
		if (m_Width == width && m_Height == height)
			return;
		auto color = m_Device->createTexture(nvrhi::TextureDesc()
												 .setWidth(width)
												 .setHeight(height)
												 .setFormat(nvrhi::Format::RGBA16_FLOAT)
												 .setIsRenderTarget(true)
												 .setInitialState(nvrhi::ResourceStates::ShaderResource)
												 .setKeepInitialState(true)
												 .setDebugName("Linear HDR"));
		auto depth = m_Device->createTexture(nvrhi::TextureDesc()
												 .setWidth(width)
												 .setHeight(height)
												 .setFormat(nvrhi::Format::D32)
												 .setIsRenderTarget(true)
												 .setInitialState(nvrhi::ResourceStates::DepthWrite)
												 .setKeepInitialState(true)
												 .setDebugName("HDR Depth"));
		if (!color || !depth)
			throw std::runtime_error("Cannot allocate HDR render targets");
		auto framebuffer =
			m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(color).setDepthAttachment(depth));
		auto bindings = m_Device->createBindingSet(nvrhi::BindingSetDesc()
													   .addItem(nvrhi::BindingSetItem::Texture_SRV(0, color))
													   .addItem(nvrhi::BindingSetItem::Sampler(0, m_LinearSampler))
													   .addItem(nvrhi::BindingSetItem::PushConstants(0, 16)),
												   m_PostLayout);
		if (!framebuffer || !bindings)
			throw std::runtime_error("Cannot bind HDR render targets");
		auto lightingPipeline = m_LightingPipeline;
		if (!lightingPipeline)
		{
			auto descriptor = nvrhi::GraphicsPipelineDesc()
								  .setVertexShader(m_VertexShader)
								  .setPixelShader(m_FragmentShader)
								  .setInputLayout(m_InputLayout)
								  .addBindingLayout(m_Layout)
								  .addBindingLayout(m_MaterialLayout);
			descriptor.renderState.rasterState.setCullMode(nvrhi::RasterCullMode::None).setFrontCounterClockwise(true);
			descriptor.renderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(true).setDepthFunc(
				nvrhi::ComparisonFunc::Less);
			lightingPipeline = m_Device->createGraphicsPipeline(descriptor, framebuffer->getFramebufferInfo());
			if (!lightingPipeline)
				throw std::runtime_error("Cannot create HDR lighting pipeline");
		}
		m_HDR = color;
		m_HDRDepth = depth;
		m_HDRFramebuffer = framebuffer;
		m_PostBindings = bindings;
		m_LightingPipeline = lightingPipeline;
		m_Width = width;
		m_Height = height;
	}

	void WorldRenderer::Render(nvrhi::ICommandList* commands, nvrhi::IFramebuffer* framebuffer, uint32_t width,
							   uint32_t height)
	{
		if (!commands || !framebuffer || !width || !height || width > 8192 || height > 8192)
			throw std::invalid_argument("Render requires valid commands, framebuffer and dimensions up to 8192");
		const auto& info = framebuffer->getFramebufferInfo();
		if (info.colorFormats.size() != 1 || info.sampleCount != 1)
			throw std::invalid_argument("Output requires one color target and one sample");
		if (info.width != width || info.height != height)
			throw std::invalid_argument("Output dimensions do not match the supplied framebuffer");
		ResizeTargets(width, height);
		auto format = std::make_pair(info.colorFormats[0], info.depthFormat);
		if (!m_Pipelines.contains(format))
		{
			auto descriptor = nvrhi::GraphicsPipelineDesc()
								  .setVertexShader(m_FullscreenShader)
								  .setPixelShader(m_TonemapShader)
								  .addBindingLayout(m_PostLayout);
			descriptor.renderState.rasterState.setCullMode(nvrhi::RasterCullMode::None);
			descriptor.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
			auto pipeline = m_Device->createGraphicsPipeline(descriptor, info);
			if (!pipeline)
				throw std::runtime_error("Cannot create tone mapping pipeline");
			m_Pipelines[format] = pipeline;
		}
		auto matrices = m_Settings.GetShadowMatrices(s_ShadowResolution);
		FrameData frame{};
		frame.LightViewProjection = matrices;
		frame.CameraPosition = glm::vec4(m_Settings.GetCameraPosition(), 1);
		frame.SunDirection = glm::vec4(glm::normalize(m_Settings.SunDirection), 0);
		frame.SunRadianceFog = glm::vec4(m_Settings.SunRadiance, m_Settings.FogDensity);
		frame.RenderParameters = glm::vec4(m_Settings.Shadows ? 1.f : 0.f, 0, 0, 0);
		for (size_t cascade = 0; cascade < 2; cascade++)
		{
			// Always initialize the sampled maps, including when the lighting toggle is off.
			commands->clearDepthStencilTexture(m_ShadowMaps[cascade], nvrhi::AllSubresources, true, 1, false, 0);
			if (m_Settings.Shadows)
			{
				frame.ViewProjection = matrices[cascade];
				commands->writeBuffer(m_Camera, &frame, sizeof(frame));
				DrawGeometry(commands, m_ShadowFramebuffers[cascade], m_ShadowPipeline, m_ShadowBindings,
							 s_ShadowResolution, s_ShadowResolution, cascade == 0 ? 70.f : 140.f);
			}
		}
		frame.ViewProjection = m_Settings.GetViewProjection(float(width) / height);
		commands->writeBuffer(m_Camera, &frame, sizeof(frame));
		commands->clearTextureFloat(m_HDR, nvrhi::AllSubresources, nvrhi::Color(.055f, .09f, .14f, 1));
		commands->clearDepthStencilTexture(m_HDRDepth, nvrhi::AllSubresources, true, 1, false, 0);
		DrawGeometry(commands, m_HDRFramebuffer, m_LightingPipeline, m_Bindings, width, height,
					 std::max(80.f, m_Settings.CameraSize * 2));
		nvrhi::GraphicsState state;
		state.setPipeline(m_Pipelines.at(format))
			.setFramebuffer(framebuffer)
			.addBindingSet(m_PostBindings)
			.setViewport(
				nvrhi::ViewportState().addViewportAndScissorRect(nvrhi::Viewport(float(width), float(height))));
		commands->setGraphicsState(state);
		glm::vec4 postSettings(m_Settings.Exposure, 0, 0, 0);
		commands->setPushConstants(&postSettings, sizeof(postSettings));
		commands->draw(nvrhi::DrawArguments().setVertexCount(3));
	}

	void WorldRenderer::SetSettings(const RenderSettings& settings)
	{
		settings.Validate();
		m_Settings = settings;
	}

	const RenderSettings& WorldRenderer::GetSettings() const
	{
		return m_Settings;
	}

	nvrhi::ITexture* WorldRenderer::GetHDRTexture() const
	{
		return m_HDR;
	}

	nvrhi::ITexture* WorldRenderer::GetShadowTexture(size_t cascade) const
	{
		return m_ShadowMaps.at(cascade);
	}

} // namespace Bellwether
