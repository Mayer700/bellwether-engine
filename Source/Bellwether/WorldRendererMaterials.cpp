#include "WorldRenderer.h"
#include <algorithm>
#include <cmath>
#include <tuple>

namespace Bellwether
{

	WorldRenderer::MaterialUpload WorldRenderer::UploadMaterials(nvrhi::ICommandList* commands, const ModelAsset& asset,
																 bool legacyColors)
	{
		std::map<std::tuple<size_t, bool, bool>, nvrhi::TextureHandle> textures;
		std::map<std::tuple<int, int, int, int>, nvrhi::SamplerHandle> samplerCache;
		ModelImage white{1, 1, {255, 255, 255, 255}};
		auto upload = [&](size_t imageIndex, bool srgb, bool mipmaps)
		{
			auto key = std::make_tuple(imageIndex, srgb, mipmaps);
			if (textures.contains(key))
				return textures.at(key);
			const auto& image = imageIndex == asset.Images.size() ? white : asset.Images.at(imageIndex);
			uint32_t mipLevels = mipmaps ? 1 + uint32_t(std::floor(std::log2(std::max(image.Width, image.Height)))) : 1;
			auto texture =
				m_Device->createTexture(nvrhi::TextureDesc()
											.setWidth(image.Width)
											.setHeight(image.Height)
											.setMipLevels(mipLevels)
											.setFormat(srgb ? nvrhi::Format::SRGBA8_UNORM : nvrhi::Format::RGBA8_UNORM)
											.setInitialState(nvrhi::ResourceStates::ShaderResource)
											.setKeepInitialState(true)
											.setDebugName("Model texture"));
			if (!texture)
				throw std::runtime_error("Cannot allocate model texture");
			if (mipLevels == 1)
			{
				commands->writeTexture(texture, 0, 0, image.Pixels.data(), image.Width * 4);
			}
			else
			{
				auto pixels = image.Pixels;
				uint32_t width = image.Width, height = image.Height;
				for (uint32_t mip = 0; mip < mipLevels; mip++)
				{
					commands->writeTexture(texture, 0, mip, pixels.data(), width * 4);
					if (mip + 1 == mipLevels)
						break;
					uint32_t nextWidth = std::max(1u, width / 2), nextHeight = std::max(1u, height / 2);
					std::vector<uint8_t> next(size_t(nextWidth) * nextHeight * 4);
					for (uint32_t y = 0; y < nextHeight; y++)
						for (uint32_t x = 0; x < nextWidth; x++)
							for (uint32_t channel = 0; channel < 4; channel++)
							{
								float sum = 0;
								uint32_t count = 0;
								for (uint32_t sourceY = y * height / nextHeight;
									 sourceY < (y + 1) * height / nextHeight; sourceY++)
									for (uint32_t sourceX = x * width / nextWidth;
										 sourceX < (x + 1) * width / nextWidth; sourceX++)
									{
										float value = pixels[(size_t(sourceY) * width + sourceX) * 4 + channel] / 255.f;
										if (srgb && channel < 3)
											value = value <= .04045f ? value / 12.92f
																	 : std::pow((value + .055f) / 1.055f, 2.4f);
										sum += value;
										count++;
									}
								float value = sum / count;
								if (srgb && channel < 3)
									value = value <= .0031308f ? value * 12.92f
															   : 1.055f * std::pow(value, 1 / 2.4f) - .055f;
								next[(size_t(y) * nextWidth + x) * 4 + channel] =
									uint8_t(std::clamp(std::lround(value * 255), 0l, 255l));
							}
					pixels = std::move(next);
					width = nextWidth;
					height = nextHeight;
				}
			}
			textures.emplace(key, texture);
			return texture;
		};
		auto address = [](int mode)
		{
			return mode == 10497   ? nvrhi::SamplerAddressMode::Wrap
				   : mode == 33648 ? nvrhi::SamplerAddressMode::Mirror
								   : nvrhi::SamplerAddressMode::Clamp;
		};
		MaterialUpload result;
		for (const auto& source : asset.Materials)
		{
			Material material;
			struct Constants
			{
				glm::vec4 Parameters;
				glm::vec4 Coverage;
			} constants{
				{source.NormalScale, source.OcclusionStrength, source.AlphaCutoff, source.Textures[2] >= 0 ? 1.f : 0.f},
				{source.BaseColor.a, source.AlphaMask ? 1.f : 0.f, source.DoubleSided ? 1.f : 0.f,
				 legacyColors ? 1.f : 0.f}};
			material.Constants = m_Device->createBuffer(nvrhi::BufferDesc()
															.setByteSize(sizeof(constants))
															.setIsConstantBuffer(true)
															.setInitialState(nvrhi::ResourceStates::ConstantBuffer)
															.setKeepInitialState(true));
			if (!material.Constants)
				throw std::runtime_error("Cannot allocate model material constants");
			commands->writeBuffer(material.Constants, &constants, sizeof(constants));
			auto description =
				nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::ConstantBuffer(0, material.Constants));
			for (uint32_t slot = 0; slot < 5; slot++)
			{
				int index = source.Textures[slot];
				ModelTexture texture;
				texture.Image = asset.Images.size();
				if (index >= 0)
					texture = asset.Textures.at(size_t(index));
				auto image = upload(texture.Image, slot == 0 || slot == 4, texture.MinFilter >= 9984);
				auto key = std::make_tuple(texture.WrapS, texture.WrapT, texture.MinFilter, texture.MagFilter);
				if (!samplerCache.contains(key))
				{
					auto sampler = m_Device->createSampler(
						nvrhi::SamplerDesc()
							.setAddressU(address(texture.WrapS))
							.setAddressV(address(texture.WrapT))
							.setMinFilter(texture.MinFilter == 9729 || texture.MinFilter == 9985 ||
										  texture.MinFilter == 9987)
							.setMagFilter(texture.MagFilter == 9729)
							.setMipFilter(texture.MinFilter == 9986 || texture.MinFilter == 9987));
					if (!sampler)
						throw std::runtime_error("Cannot allocate model texture sampler");
					samplerCache.emplace(key, sampler);
				}
				auto sampler = samplerCache.at(key);
				description.addItem(nvrhi::BindingSetItem::Texture_SRV(slot, image))
					.addItem(nvrhi::BindingSetItem::Sampler(slot, sampler));
			}
			material.Bindings = m_Device->createBindingSet(description, m_MaterialLayout);
			if (!material.Bindings)
				throw std::runtime_error("Cannot bind model material");
			result.Materials.push_back(std::move(material));
		}
		result.SamplerCount = samplerCache.size();
		return result;
	}

	void WorldRenderer::LoadModel(const std::filesystem::path& path)
	{
		auto asset = ImportGltf(path);
		UploadModel(asset);
		ClearSnowField();
	}

	void WorldRenderer::LoadScene(const Scene& scene, const std::filesystem::path& assetRoot)
	{
		scene.GetRenderSettings().Validate();
		const auto& snowSettings = scene.GetSnowSettings();
		snowSettings.Validate();
		auto root = std::filesystem::weakly_canonical(assetRoot);
		SnowField validatedSnow;
		if (snowSettings.Enabled)
		{
			validatedSnow.Reset(snowSettings.ToFieldConfig());
			if (!snowSettings.TracksAsset.empty())
			{
				ValidateSnowTracksAssetPath(snowSettings.TracksAsset);
				auto relative = std::filesystem::path(
					std::u8string(reinterpret_cast<const char8_t*>(snowSettings.TracksAsset.data()),
								  snowSettings.TracksAsset.size()));
				auto tracksPath = std::filesystem::weakly_canonical(root / relative);
				auto prefix = std::mismatch(root.begin(), root.end(), tracksPath.begin(), tracksPath.end());
				if (prefix.first != root.end())
					throw std::invalid_argument("Scene snow tracks link escapes its asset root");
				validatedSnow.LoadTracks(tracksPath);
			}
		}

		ModelAsset merged;
		struct CachedModel
		{
			size_t PrimitiveOffset;
			std::vector<ModelPlacement> Placements;
		};
		std::map<std::filesystem::path, CachedModel> cache;
		size_t bytes = 0;
		for (auto id : scene.GetEntities())
		{
			const auto& entity = scene.GetEntity(id);
			if (entity.ModelAsset.empty())
				continue;
			ValidateModelAssetPath(entity.ModelAsset);
			auto relative = std::filesystem::path(
				std::u8string(reinterpret_cast<const char8_t*>(entity.ModelAsset.data()), entity.ModelAsset.size()));
			auto path = std::filesystem::weakly_canonical(root / relative);
			auto prefix = std::mismatch(root.begin(), root.end(), path.begin(), path.end());
			if (prefix.first != root.end())
				throw std::invalid_argument("Scene model link escapes its asset root");
			if (!cache.contains(path))
			{
				if (cache.size() >= 256)
					throw std::invalid_argument("Scene model budget exceeded");
				auto model = ImportGltf(path);
				if (merged.Materials.size() + model.Materials.size() > 16384 ||
					merged.Textures.size() + model.Textures.size() > 4096 ||
					merged.Images.size() + model.Images.size() > 1024 ||
					merged.Primitives.size() + model.Primitives.size() > 50000)
					throw std::invalid_argument("Combined scene model resource-count budget exceeded");
				for (const auto& primitive : model.Primitives)
					bytes +=
						primitive.Vertices.size() * sizeof(ModelVertex) + primitive.Indices.size() * sizeof(uint32_t);
				for (const auto& image : model.Images)
					bytes += image.Pixels.size();
				if (bytes > 512 * 1024 * 1024)
					throw std::invalid_argument("Combined scene model budget exceeded");
				cache.emplace(path, CachedModel{merged.Primitives.size(), model.Placements});
				size_t imageOffset = merged.Images.size(), textureOffset = merged.Textures.size(),
					   materialOffset = merged.Materials.size();
				for (auto& image : model.Images)
					merged.Images.push_back(std::move(image));
				for (auto texture : model.Textures)
				{
					texture.Image += imageOffset;
					merged.Textures.push_back(texture);
				}
				for (auto material : model.Materials)
				{
					for (auto& texture : material.Textures)
						if (texture >= 0)
							texture += int(textureOffset);
					merged.Materials.push_back(std::move(material));
				}
				for (auto& primitive : model.Primitives)
				{
					primitive.Material += materialOffset;
					merged.Primitives.push_back(std::move(primitive));
				}
			}
			const auto& cached = cache.at(path);
			for (const auto& source : cached.Placements)
			{
				if (merged.Placements.size() >= 100000)
					throw std::invalid_argument("Scene render placement budget exceeded");
				auto placement = source;
				placement.Primitive += cached.PrimitiveOffset;
				placement.Transform = entity.Transform.GetMatrix() * source.Transform;
				float determinant = glm::determinant(placement.Transform);
				if (!std::isfinite(determinant) || std::abs(determinant) < 1e-8f)
					throw std::invalid_argument("Invalid scene model transform");
				for (size_t column = 0; column < 4; column++)
					for (size_t row = 0; row < 4; row++)
						if (!std::isfinite(placement.Transform[column][row]) ||
							std::abs(placement.Transform[column][row]) > 1000000)
							throw std::invalid_argument("Scene model transform exceeds supported bounds");
				merged.Placements.push_back(std::move(placement));
			}
		}
		if (snowSettings.Enabled && snowSettings.RenderGround)
		{
			if (merged.Materials.size() + 1 > 16384 || merged.Primitives.size() + 1 > 50000 ||
				merged.Placements.size() + 1 > 100000)
				throw std::invalid_argument("Scene snow ground exceeds render resource budget");
			constexpr uint32_t groundGrid = 129;
			ModelPrimitive groundPrimitive;
			groundPrimitive.Material = merged.Materials.size();
			groundPrimitive.Vertices.reserve(static_cast<size_t>(groundGrid) * groundGrid);
			groundPrimitive.Indices.reserve(static_cast<size_t>(groundGrid - 1) * (groundGrid - 1) * 6);
			const float extent = snowSettings.WorldExtent;
			const float halfExtent = extent * 0.5f;
			for (uint32_t z = 0; z < groundGrid; z++)
			{
				float v = static_cast<float>(z) / static_cast<float>(groundGrid - 1);
				float wz = v * extent - halfExtent;
				for (uint32_t x = 0; x < groundGrid; x++)
				{
					float u = static_cast<float>(x) / static_cast<float>(groundGrid - 1);
					float wx = u * extent - halfExtent;
					ModelVertex vertex;
					vertex.Position = {wx, 0.0f, wz};
					vertex.Normal = {0.0f, 1.0f, 0.0f};
					vertex.TexCoord = {u, v};
					vertex.Tangent = {1.0f, 0.0f, 0.0f, 1.0f};
					vertex.Color = {1.0f, 1.0f, 1.0f, 1.0f};
					groundPrimitive.Vertices.push_back(vertex);
				}
			}
			for (uint32_t z = 0; z < groundGrid - 1; z++)
			{
				for (uint32_t x = 0; x < groundGrid - 1; x++)
				{
					uint32_t idx = z * groundGrid + x;
					groundPrimitive.Indices.insert(
						groundPrimitive.Indices.end(),
						{idx, idx + groundGrid, idx + 1, idx + 1, idx + groundGrid, idx + groundGrid + 1});
				}
			}
			ModelMaterial groundMaterial;
			groundMaterial.Name = "SnowGround";
			groundMaterial.BaseColor = {0.28f, 0.30f, 0.34f, 1.0f};
			groundMaterial.Roughness = 0.88f;
			groundMaterial.Metallic = 0.0f;
			merged.Materials.push_back(std::move(groundMaterial));
			const size_t primitiveIndex = merged.Primitives.size();
			merged.Primitives.push_back(std::move(groundPrimitive));
			ModelPlacement groundPlacement;
			groundPlacement.Name = "SnowGround";
			groundPlacement.Primitive = primitiveIndex;
			groundPlacement.SurfaceFlags = {1.0f, 0.0f, 0.0f, 0.0f};
			merged.Placements.push_back(std::move(groundPlacement));
		}
		UploadModel(merged);
		m_Settings = scene.GetRenderSettings();
		if (snowSettings.Enabled)
			UpdateSnowField(validatedSnow);
		else
			ClearSnowField();
	}

	void WorldRenderer::UploadModel(const ModelAsset& asset)
	{
		std::vector<Mesh> meshes;
		std::vector<Instance> instances;
		auto commands = m_Device->createCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false));
		if (!commands)
			throw std::runtime_error("Cannot allocate model upload transaction");
		commands->open();
		for (const auto& primitive : asset.Primitives)
			meshes.push_back(UploadMesh(commands, primitive.Vertices, primitive.Indices));
		auto materials = UploadMaterials(commands, asset);
		for (const auto& placement : asset.Placements)
		{
			Instance instance;
			instance.MeshIndex = placement.Primitive;
			instance.MaterialIndex = asset.Primitives[placement.Primitive].Material;
			const auto& material = asset.Materials[instance.MaterialIndex];
			instance.Transform = placement.Transform;
			instance.AlbedoRoughness = {glm::vec3(material.BaseColor), material.Roughness};
			instance.EmissionMetallic = {material.Emission, material.Metallic};
			instance.SurfaceFlags = placement.SurfaceFlags;
			instances.push_back(instance);
		}
		commands->close();
		m_Device->executeCommandList(commands);
		if (!m_Device->waitForIdle())
			throw std::runtime_error("Graphics device lost while completing model upload");
		m_Meshes = std::move(meshes);
		m_Instances = std::move(instances);
		m_Materials = std::move(materials.Materials);
		m_MaterialSamplerCount = materials.SamplerCount;
	}

} // namespace Bellwether
