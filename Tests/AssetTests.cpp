#include "Bellwether/GltfImporter.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
	int s_Passed = 0, s_Failed = 0;
	void Check(bool condition, const std::string& name)
	{
		std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
		(condition ? s_Passed : s_Failed)++;
	}
} // namespace

int main(int argumentCount, char** arguments)
{
	using namespace Bellwether;
	try
	{
		if (argumentCount != 2)
			throw std::invalid_argument("Asset tests require the generated fixture directory");
		auto root = std::filesystem::path(arguments[1]);
		auto model = ImportGltf(root / "Materials.glb");
		Check(model.Primitives.size() == 6 && model.Placements.size() == 6 && model.Materials.size() == 7,
			  "GLB meshes, material assignment and selected scene import");
		Check(model.Images.size() == 5 && model.Images[0].Width == 2 && model.Images[0].Height == 2 &&
				  model.Images[0].Pixels[0] == 255 && model.Images[0].Pixels[8] == 0 &&
				  model.Images[0].Pixels[10] == 255,
			  "PNG decoding preserves exact top-left image orientation");
		Check(model.Primitives[0].Vertices.size() == 4 &&
				  model.Primitives[0].Indices == std::vector<uint32_t>({0, 2, 1, 0, 3, 2}) &&
				  model.Primitives[0].Vertices[2].TexCoord == glm::vec2(1, 1),
			  "Interleaved accessor offsets and unsigned-short indices");
		Check(model.Placements[0].Transform[3] == glm::vec4(-4, 0, 10, 1), "Node translation and primitive instancing");
		Check(model.Materials[1].Textures[0] == 0 && model.Materials[2].Textures[4] == 0 &&
				  model.Materials[3].Textures[3] == 3 && model.Materials[4].Textures[2] == 1 &&
				  model.Materials[6].Textures[1] == 3,
			  "All five metallic-roughness material texture roles import");
		Check(model.Materials[1].Metallic == 0 && model.Materials[2].Emission == glm::vec3(1) &&
				  !model.Materials[1].DoubleSided,
			  "Material factors retain linear values and single-sided default");
		const auto& tangent = model.Primitives[3].Vertices[0].Tangent;
		Check(model.Primitives[3].Vertices.size() == 6 && tangent.x > .999f && std::abs(tangent.y) < .001f &&
				  tangent.w == -1,
			  "MikkTSpace creates per-corner tangent frames with handedness");
		for (const auto& filename :
			 {"Materials.gltf", "External.gltf", "Embedded.glb", "Sparse.gltf", "InterleavedSparse.gltf"})
		{
			auto alternate = ImportGltf(root / filename);
			Check(alternate.Placements.size() == 6 && alternate.Images[0].Pixels == model.Images[0].Pixels &&
					  alternate.Primitives[0].Vertices[2].Position == model.Primitives[0].Vertices[2].Position,
				  std::string("Equivalent import from ") + filename);
		}
		auto generatedNormals = ImportGltf(root / "NoNormals.gltf");
		Check(generatedNormals.Primitives[0].Vertices.size() == 6 &&
				  generatedNormals.Primitives[0].Vertices[0].Normal.y > .999f,
			  "Missing normals generate independent flat faces");
		auto hierarchy = ImportGltf(root / "Hierarchy.gltf");
		Check(hierarchy.Placements[0].Transform[3] == glm::vec4(-2, 3, 14, 1), "Parent and child transforms compose");
		auto mirrored = ImportGltf(root / "Mirrored.gltf");
		Check(glm::determinant(mirrored.Placements[0].Transform) < 0 && mirrored.Placements[0].Transform[1][1] == 2,
			  "Mirrored and nonuniform transforms are preserved");
		auto colored = ImportGltf(root / "VertexColors.gltf");
		Check(std::abs(colored.Primitives[0].Vertices[0].Color.r - 128.f / 255) < .00001f &&
				  colored.Primitives[0].Vertices[0].Color.g == 1,
			  "Normalized integer vertex colors import");
		auto repeated = ImportGltf(root / "Repeat.gltf");
		Check(repeated.Textures[0].WrapS == 10497 && repeated.Textures[0].WrapT == 33648 &&
				  repeated.Textures[0].MinFilter == 9987,
			  "Independent sampler wrap and mip filtering import");
		auto masked = ImportGltf(root / "Masked.gltf");
		Check(masked.Materials[1].AlphaMask && masked.Materials[1].DoubleSided &&
				  masked.Materials[1].AlphaCutoff == .5f,
			  "Alpha mask, cutoff and double sided material import");
		for (const auto& filename :
			 {"InvalidIndex",	  "InvalidView", "HugeAccessor",   "RequiredExtension", "Blend",
			  "MissingUV",		  "BadImage",	 "Traversal",	   "EncodedTraversal",	"Absolute",
			  "Network",		  "NulURI",		 "MalformedURI",   "ShortBase64",		"InvalidBase64",
			  "Singular",		  "Cycle",		 "NegativeFactor", "UnsupportedUV",		"Morph",
			  "Animated",		  "Skinned",	 "NaNVertex",	   "OutOfRangeIndex",	"UnsortedSparse",
			  "MisalignedSparse", "Deep"})
		{
			bool rejected = false;
			try
			{
				ImportGltf(root / (std::string(filename) + ".gltf"));
			}
			catch (const std::exception& error)
			{
				rejected = std::string(error.what()).size() > 0;
			}
			Check(rejected, std::string("Reject invalid/unsupported asset: ") + filename);
		}
		bool truncated = false;
		try
		{
			ImportGltf(root / "Truncated.glb");
		}
		catch (const std::exception&)
		{
			truncated = true;
		}
		Check(truncated, "Truncated binary GLB rejects");
	}
	catch (const std::exception& error)
	{
		std::cerr << "Asset test exception: " << error.what() << '\n';
		s_Failed++;
	}
	std::cout << "BELLWETHER_ASSETS " << s_Passed << " passed " << s_Failed << " failed\n";
	return s_Failed ? 1 : 0;
}
