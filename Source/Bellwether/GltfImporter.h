#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace Bellwether
{

	struct ModelVertex
	{
		glm::vec3 Position{0};
		glm::vec3 Normal{0, 1, 0};
		glm::vec2 TexCoord{0};
		glm::vec4 Tangent{1, 0, 0, 1};
		glm::vec4 Color{1};
	};

	struct ModelImage
	{
		uint32_t Width = 0, Height = 0;
		std::vector<uint8_t> Pixels;
	};

	struct ModelTexture
	{
		size_t Image = 0;
		int WrapS = 10497, WrapT = 10497, MinFilter = 9987, MagFilter = 9729;
	};

	struct ModelMaterial
	{
		std::string Name;
		glm::vec4 BaseColor{1};
		glm::vec3 Emission{0};
		float Roughness = 1, Metallic = 1, NormalScale = 1, OcclusionStrength = 1, AlphaCutoff = .5f;
		bool AlphaMask = false, DoubleSided = false;
		// Base color, metallic/roughness, normal, occlusion, emissive. -1 means absent.
		std::array<int, 5> Textures{-1, -1, -1, -1, -1};
	};

	struct ModelPrimitive
	{
		std::vector<ModelVertex> Vertices;
		std::vector<uint32_t> Indices;
		size_t Material = 0;
	};

	struct ModelPlacement
	{
		std::string Name;
		size_t Primitive = 0;
		glm::mat4 Transform{1};
	};

	struct ModelAsset
	{
		std::vector<ModelImage> Images;
		std::vector<ModelTexture> Textures;
		std::vector<ModelMaterial> Materials;
		std::vector<ModelPrimitive> Primitives;
		std::vector<ModelPlacement> Placements;
	};

	// Loads a static glTF 2.0 scene transactionally. Dependent files must remain
	// below the model's directory. Unsupported features fail with diagnostics.
	ModelAsset ImportGltf(const std::filesystem::path& path);

} // namespace Bellwether
