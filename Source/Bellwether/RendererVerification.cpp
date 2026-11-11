#include "RendererVerification.h"
#include <glm/gtc/packing.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>

namespace Bellwether
{
	struct Capture
	{
		std::vector<unsigned char> Pixels;
		std::vector<float> Linear;
		float Peak = 0, GPUSeconds = 0;
	};

	static void WriteCapture(const std::filesystem::path& path, const Capture& image, uint32_t width, uint32_t height)
	{
		std::ofstream output(path, std::ios::binary);
		output << "P6\n" << width << " " << height << "\n255\n";
		for (size_t index = 0; index < image.Pixels.size(); index += 4)
			output.write(reinterpret_cast<const char*>(image.Pixels.data() + index), 3);
		if (!output)
			throw std::runtime_error("Cannot write renderer verification capture");
	}

	static Capture RenderCapture(nvrhi::IDevice* device, WorldRenderer& world, uint32_t width, uint32_t height)
	{
		auto description = nvrhi::TextureDesc()
							   .setWidth(width)
							   .setHeight(height)
							   .setFormat(nvrhi::Format::RGBA8_UNORM)
							   .setIsRenderTarget(true)
							   .setInitialState(nvrhi::ResourceStates::CopySource)
							   .setKeepInitialState(true);
		auto color = device->createTexture(description);
		auto framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(color));
		auto staging = device->createStagingTexture(description, nvrhi::CpuAccessMode::Read);
		auto timer = device->createTimerQuery();
		auto commands = device->createCommandList();
		commands->open();
		commands->beginTimerQuery(timer);
		world.Render(commands, framebuffer, width, height);
		commands->endTimerQuery(timer);
		commands->copyTexture(staging, nvrhi::TextureSlice(), color, nvrhi::TextureSlice());
		auto hdr = world.GetHDRTexture();
		if (!hdr || hdr->getDesc().format != nvrhi::Format::RGBA16_FLOAT)
			throw std::runtime_error("Renderer is not using a half-float HDR target");
		auto hdrStaging = device->createStagingTexture(hdr->getDesc(), nvrhi::CpuAccessMode::Read);
		commands->copyTexture(hdrStaging, nvrhi::TextureSlice(), hdr, nvrhi::TextureSlice());
		commands->close();
		device->executeCommandList(commands);
		device->waitForIdle();
		Capture capture;
		capture.Pixels.resize(size_t(width) * height * 4);
		capture.Linear.resize(size_t(width) * height * 4);
		size_t pitch = 0;
		auto* bytes = static_cast<const unsigned char*>(
			device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
		if (!bytes)
			throw std::runtime_error("Cannot map post-process readback");
		for (size_t row = 0; row < height; row++)
			std::memcpy(capture.Pixels.data() + row * width * 4, bytes + row * pitch, width * 4);
		device->unmapStagingTexture(staging);
		bytes = static_cast<const unsigned char*>(
			device->mapStagingTexture(hdrStaging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
		if (!bytes)
			throw std::runtime_error("Cannot map half-float readback");
		bool finite = true;
		for (size_t row = 0; row < height; row++)
			for (size_t column = 0; column < width * 4; column++)
			{
				uint16_t bits = 0;
				std::memcpy(&bits, bytes + row * pitch + column * 2, 2);
				float value = glm::unpackHalf1x16(bits);
				capture.Linear[row * width * 4 + column] = value;
				finite &= std::isfinite(value) && value >= 0;
				if (column % 4 != 3)
					capture.Peak = std::max(capture.Peak, value);
			}
		device->unmapStagingTexture(hdrStaging);
		if (!finite)
			throw std::runtime_error("HDR target contains invalid values");
		if (!device->pollTimerQuery(timer))
			throw std::runtime_error("GPU timer did not complete with readback");
		capture.GPUSeconds = device->getTimerQueryTime(timer);
		device->runGarbageCollection();
		return capture;
	}

	static float Luminance(const Capture& image, size_t pixel)
	{
		auto offset = pixel * 4;
		return (image.Pixels[offset] + image.Pixels[offset + 1] + image.Pixels[offset + 2]) / 3.f;
	}

	static size_t ProjectPixel(const RenderSettings& settings, glm::vec3 position, uint32_t width, uint32_t height)
	{
		auto clip = settings.GetViewProjection(float(width) / height) * glm::vec4(position, 1);
		int x = int((clip.x / clip.w * .5f + .5f) * width), y = int((.5f - clip.y / clip.w * .5f) * height);
		if (x < 0 || y < 0 || x >= int(width) || y >= int(height))
			throw std::runtime_error("Verification point is outside the image");
		return size_t(y) * width + x;
	}

	Json VerifyModelRenderer(nvrhi::IDevice* device, WorldRenderer& world, const std::filesystem::path& outputDirectory,
							 const std::filesystem::path& fixtureDirectory)
	{
		if (ReadJson(fixtureDirectory / "Fixture.json").value("Fixture", "") != "BellwetherModels_v1")
			throw std::invalid_argument("Model renderer verification requires BellwetherModels_v1 fixtures");
		std::filesystem::create_directories(outputDirectory);
		constexpr uint32_t width = 960, height = 600;
		RenderSettings settings;
		settings.CameraTarget = {0, 0, 12};
		settings.CameraSize = 18;
		settings.Shadows = false;
		settings.FogDensity = 0;
		settings.SunRadiance = {0, 0, 0};
		world.LoadModel(fixtureDirectory / "Materials.glb");
		world.SetSettings(settings);
		auto baseline = RenderCapture(device, world, width, height);
		WriteCapture(outputDirectory / "Materials.ppm", baseline, width, height);
		Json report = {{"Checks", Json::array()}};
		int failed = 0;
		auto check = [&](bool passed, const std::string& name)
		{
			report["Checks"].push_back({{"Name", name}, {"Passed", passed}});
			std::cout << (passed ? "PASS " : "FAIL ") << name << '\n';
			failed += !passed;
		};
		auto sample = [&](const Capture& image, glm::vec3 point)
		{
			auto offset = ProjectPixel(settings, point, width, height) * 4;
			return glm::vec3(image.Linear[offset], image.Linear[offset + 1], image.Linear[offset + 2]);
		};
		auto close = [](glm::vec3 actual, glm::vec3 expected, float tolerance = .008f)
		{ return glm::all(glm::lessThanEqual(glm::abs(actual - expected), glm::vec3(tolerance))); };
		float gray = std::pow((128.f / 255 + .055f) / 1.055f, 2.4f);
		std::array<glm::vec3, 4> colors{glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1), glm::vec3(gray)};
		bool baseCorrect = true, emissiveCorrect = true;
		for (size_t quadrant = 0; quadrant < 4; quadrant++)
		{
			float x = quadrant % 2 ? .5f : -.5f, z = quadrant / 2 ? .5f : -.5f;
			baseCorrect &= close(sample(baseline, {-4 + x, 0, 10 + z}), colors[quadrant] * glm::vec3(.32f, .43f, .59f));
			emissiveCorrect &= close(sample(baseline, {x, 0, 10 + z}), colors[quadrant]);
		}
		check(baseCorrect, "base-color sRGB decoding, UV orientation and glTF front-face winding");
		check(emissiveCorrect, "emissive sRGB texture samples preserve linear radiance");
		check(close(sample(baseline, {4, 0, 10}), glm::vec3(.32f, .43f, .59f) * (128.f / 255)),
			  "occlusion texture uses linear red channel rather than sRGB");
		check(close(sample(baseline, {-4, 0, 14}), glm::vec3(.32f, .43f, .59f)),
			  "flat normal map retains the surface normal");
		glm::vec3 encodedNormal(218.f / 255 * 2 - 1, 128.f / 255 * 2 - 1, 218.f / 255 * 2 - 1);
		auto tangentNormal = glm::normalize(encodedNormal);
		auto expectedNormal = glm::vec3(tangentNormal.x, tangentNormal.z, tangentNormal.y);
		auto ambient = glm::mix(glm::vec3(.10f, .15f, .23f), glm::vec3(.32f, .43f, .59f), expectedNormal.y * .5f + .5f);
		check(close(sample(baseline, {0, 0, 14}), ambient),
			  "MikkTSpace tangent normal map changes the measured surface lighting");
		for (const auto& filename : {"Materials.gltf", "External.gltf", "Embedded.glb"})
		{
			world.LoadModel(fixtureDirectory / filename);
			auto alternate = RenderCapture(device, world, width, height);
			check(alternate.Pixels == baseline.Pixels, std::string("identical GPU output from ") + filename);
		}
		world.LoadModel(fixtureDirectory / "Mirrored.gltf");
		auto mirrored = RenderCapture(device, world, width, height);
		check(close(sample(mirrored, {-4.5f, 0, 9.5f}), glm::vec3(0, .43f, 0)),
			  "negative node scale preserves visibility and mirrors the texture");
		world.LoadModel(fixtureDirectory / "VertexColors.gltf");
		auto colored = RenderCapture(device, world, width, height);
		check(close(sample(colored, {-4.5f, 0, 9.5f}), glm::vec3(.32f * 128 / 255, 0, 0)),
			  "linear normalized vertex colors multiply the sampled base color");
		world.LoadModel(fixtureDirectory / "Minified.gltf");
		auto minified = RenderCapture(device, world, width, height);
		WriteCapture(outputDirectory / "Minified.ppm", minified, width, height);
		check(close(sample(minified, {0, 0, 12}), glm::vec3(.5f), .012f),
			  "minification uses a complete mip chain averaged in linear color space");
		world.LoadModel(fixtureDirectory / "ManyMaterials.gltf");
		auto manyMaterials = RenderCapture(device, world, width, height);
		check(world.GetMaterialCount() > 1200 && world.GetMaterialSamplerCount() <= 2 &&
				  manyMaterials.Pixels == baseline.Pixels,
			  "over 1200 materials reuse two sampler states without changing rendering");
		world.LoadModel(fixtureDirectory / "Materials.glb");
		settings.SunRadiance = glm::vec3(2);
		world.SetSettings(settings);
		auto lit = RenderCapture(device, world, width, height);
		// Independent CPU evaluation of the direct GGX equations at the metallic panel.
		auto normal = glm::vec3(0, 1, 0);
		auto light = glm::normalize(settings.SunDirection);
		auto view = glm::normalize(settings.GetCameraPosition() - glm::vec3(4, 0, 14));
		auto halfway = glm::normalize(light + view);
		float nL = glm::dot(normal, light), nV = glm::dot(normal, view), nH = glm::dot(normal, halfway);
		float roughness = 64.f / 255, alphaSquared = std::pow(roughness, 4.f);
		float distribution =
			alphaSquared / (3.14159265359f * std::pow(nH * nH * (alphaSquared - 1) + 1, 2.f) + .00001f);
		float geometryK = std::pow(roughness + 1, 2.f) / 8;
		float geometry = nL / (nL * (1 - geometryK) + geometryK) * nV / (nV * (1 - geometryK) + geometryK);
		float specular = distribution * geometry / (4 * nL * nV + .001f);
		auto expectedMetal = glm::vec3(.32f, .43f, .59f) + specular * settings.SunRadiance * nL;
		check(close(sample(lit, {4, 0, 14}), expectedMetal, .015f),
			  "metallic blue and roughness green channels drive GGX lighting");
		world.LoadModel(fixtureDirectory / "Backside.gltf");
		settings.SunDirection = {1, .1f, 0};
		world.SetSettings(settings);
		auto backside = RenderCapture(device, world, width, height);
		check(close(sample(backside, {0, 0, 14}), ambient),
			  "back-facing double-sided normal map reverses the complete tangent frame");
		settings.SunDirection = RenderSettings{}.SunDirection;
		settings.Shadows = true;
		world.SetSettings(settings);
		auto readShadow = [&]
		{
			auto* texture = world.GetShadowTexture(0);
			uint32_t size = texture->getDesc().width;
			auto staging = device->createStagingTexture(texture->getDesc(), nvrhi::CpuAccessMode::Read);
			auto commands = device->createCommandList();
			commands->open();
			commands->copyTexture(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice());
			commands->close();
			device->executeCommandList(commands);
			device->waitForIdle();
			size_t pitch = 0;
			auto* source = static_cast<const uint8_t*>(
				device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
			if (!source)
				throw std::runtime_error("Cannot read cutout shadow depth");
			std::vector<float> depths(size_t(size) * size);
			for (uint32_t row = 0; row < size; row++)
				std::memcpy(depths.data() + size_t(row) * size, source + pitch * row, size * sizeof(float));
			device->unmapStagingTexture(staging);
			return depths;
		};
		world.LoadModel(fixtureDirectory / "Opaque.gltf");
		auto opaque = RenderCapture(device, world, width, height);
		auto opaqueShadow = readShadow();
		world.LoadModel(fixtureDirectory / "Masked.gltf");
		auto masked = RenderCapture(device, world, width, height);
		auto maskedShadow = readShadow();
		WriteCapture(outputDirectory / "Masked.ppm", masked, width, height);
		size_t cutoutPixels = 0, cutoutDepths = 0;
		for (size_t pixel = 0; pixel < width * height; pixel++)
			cutoutPixels += std::abs(Luminance(opaque, pixel) - Luminance(masked, pixel)) > 10;
		for (size_t pixel = 0; pixel < opaqueShadow.size(); pixel++)
			cutoutDepths += opaqueShadow[pixel] < .999f && maskedShadow[pixel] > .999f;
		check(cutoutPixels > 1000, "alpha-cutout texels leave holes in color and depth coverage");
		check(cutoutDepths > 1000, "alpha-cutout holes also reach the sun shadow depth map");
		world.LoadModel(fixtureDirectory / "Materials.glb");
		settings.Shadows = false;
		settings.SunRadiance = {0, 0, 0};
		world.SetSettings(settings);
		for (const auto& filename : {"InvalidView.gltf", "BadImage.gltf", "OutOfRangeIndex.gltf", "Skinned.gltf"})
		{
			bool rejected = false;
			try
			{
				world.LoadModel(fixtureDirectory / filename);
			}
			catch (const std::exception&)
			{
				rejected = true;
			}
			auto recovered = RenderCapture(device, world, width, height);
			check(rejected && recovered.Pixels == baseline.Pixels && world.GetDrawCount() == 6,
				  std::string("rejected model preserves previous GPU scene: ") + filename);
		}
		Scene scene;
		scene.Load(fixtureDirectory / "Scene.bwscene");
		world.LoadScene(scene, fixtureDirectory);
		auto authored = RenderCapture(device, world, width, height);
		check(authored.Pixels == baseline.Pixels,
			  "serialized model component renders identically to direct glTF import");
		scene.Load(fixtureDirectory / "MultiScene.bwscene");
		world.LoadScene(scene, fixtureDirectory);
		check(world.GetDrawCount() == 12 && world.GetMeshCount() == 6,
			  "multiple model entities share uploaded geometry");
		scene.Load(fixtureDirectory / "Scene.bwscene");
		auto transformed = scene.Serialize();
		transformed["Entities"][0]["Transform"]["Translation"] = {2, 0, 0};
		scene.Deserialize(transformed);
		world.LoadScene(scene, fixtureDirectory);
		auto moved = RenderCapture(device, world, width, height);
		check(close(sample(moved, {-2.5f, 0, 9.5f}), glm::vec3(.32f, 0, 0)),
			  "entity transform composes with glTF node transforms");
		auto missing = transformed;
		missing["Entities"][0]["Model"]["Asset"] = "Missing.glb";
		scene.Deserialize(missing);
		bool missingRejected = false;
		try
		{
			world.LoadScene(scene, fixtureDirectory);
		}
		catch (const std::exception&)
		{
			missingRejected = true;
		}
		check(missingRejected && RenderCapture(device, world, width, height).Pixels == moved.Pixels,
			  "missing model in authored scene preserves the previous GPU scene");
		scene.Deserialize(transformed);
		scene.ApplyCommands(Json::array({{{"Operation", "SetModel"}, {"ID", 1}, {"Asset", ""}}}));
		world.LoadScene(scene, fixtureDirectory);
		auto empty = RenderCapture(device, world, width, height);
		check(world.GetDrawCount() == 0 && empty.Pixels != moved.Pixels,
			  "removing a model component clears its rendered instances");
		report["Failed"] = failed;
		report["CutoutColorPixels"] = cutoutPixels;
		report["CutoutShadowTexels"] = cutoutDepths;
		report["GraySRGBLinear"] = gray;
		AtomicWrite(outputDirectory / "Models.json", report.dump(2));
		if (failed)
			throw std::runtime_error("Model renderer verification failed " + std::to_string(failed) + " checks");
		std::cout << "BELLWETHER_MODEL_RENDERER " << report["Checks"].size() << " passed 0 failed\n";
		return report;
	}

	Json VerifyRenderer(nvrhi::IDevice* device, WorldRenderer& world, const std::filesystem::path& outputDirectory,
						const std::filesystem::path& fixtureDirectory)
	{
		auto fixture = ReadJson(fixtureDirectory / "world.json");
		if (fixture.value("Fixture", "") != "BellwetherRenderer_v1" || fixture.at("meshes").size() != 1)
			throw std::invalid_argument("Renderer checks require the built BellwetherRenderer_v1 fixture");
		std::filesystem::create_directories(outputDirectory);
		struct RestoreSettings
		{
			WorldRenderer& World;
			RenderSettings Original;
			~RestoreSettings()
			{
				World.SetSettings(Original);
			}
		} restore{world, world.GetSettings()};
		Json report = {{"Checks", Json::array()}};
		int failed = 0;
		auto check = [&](bool passed, const std::string& name)
		{
			report["Checks"].push_back({{"Name", name}, {"Passed", passed}});
			std::cout << (passed ? "PASS " : "FAIL ") << name << '\n';
			if (!passed)
				failed++;
		};
		constexpr uint32_t width = 960, height = 600;
		RenderSettings settings;
		settings.Shadows = false;
		world.SetSettings(settings);
		auto unshadowed = RenderCapture(device, world, width, height);
		WriteCapture(outputDirectory / "ShadowsOff.ppm", unshadowed, width, height);
		check(unshadowed.Peak > 4, "HDR readback retains radiance above one");
		settings.Shadows = true;
		world.SetSettings(settings);
		auto shadowed = RenderCapture(device, world, width, height);
		WriteCapture(outputDirectory / "ShadowsOn.ppm", shadowed, width, height);
		size_t darker = 0, brighter = 0;
		for (size_t pixel = 0; pixel < width * height; pixel++)
		{
			float difference = Luminance(unshadowed, pixel) - Luminance(shadowed, pixel);
			darker += difference > 4;
			brighter += difference < -4;
		}
		check(darker > 3000 && brighter < 50, "shadow toggle darkens geometry without adding light");
		auto shadowPoint = ProjectPixel(settings, {5, 0, 12}, width, height);
		auto litPoint = ProjectPixel(settings, {-5, 0, 13}, width, height);
		float fullShadow = Luminance(shadowed, shadowPoint);
		check(Luminance(unshadowed, shadowPoint) - fullShadow > 15,
			  "cube shadow reaches the analytically projected ground point");
		check(std::abs(Luminance(unshadowed, litPoint) - Luminance(shadowed, litPoint)) < 3,
			  "unoccluded ground remains lit");
		size_t filteredSamples = 0;
		for (float z = 12; z < 15; z += .025f)
		{
			auto pixel = ProjectPixel(settings, {5, 0, z}, width, height);
			float value = Luminance(shadowed, pixel);
			filteredSamples += value > fullShadow + 4 && value < Luminance(unshadowed, pixel) - 4;
		}
		check(filteredSamples > 2, "PCF produces a filtered shadow boundary");
		settings.Shadows = false;
		settings.Exposure = .25f;
		world.SetSettings(settings);
		auto low = RenderCapture(device, world, width, height);
		settings.Exposure = 2.f;
		world.SetSettings(settings);
		auto high = RenderCapture(device, world, width, height);
		double lowMean = 0, highMean = 0;
		size_t tonemapMismatches = 0;
		for (size_t pixel = 0; pixel < width * height; pixel++)
		{
			lowMean += Luminance(low, pixel);
			highMean += Luminance(high, pixel);
			if (pixel % 113 != 0)
				continue;
			for (size_t channel = 0; channel < 3; channel++)
			{
				float value = high.Linear[pixel * 4 + channel] * settings.Exposure;
				float aces =
					std::clamp((value * (2.51f * value + .03f)) / (value * (2.43f * value + .59f) + .14f), 0.f, 1.f);
				int expected = int(std::lround(std::pow(aces, 1 / 2.2f) * 255));
				tonemapMismatches += std::abs(expected - int(high.Pixels[pixel * 4 + channel])) > 2;
			}
		}
		check(highMean / (width * height) > lowMean / (width * height) + 30,
			  "exposure changes the post-process output");
		check(std::abs(high.Peak - low.Peak) < .001f, "exposure preserves linear HDR scene radiance");
		check(tonemapMismatches == 0, "GPU tone mapping matches HDR readback including image orientation");
		world.SetSettings(restore.Original);
		auto resized = RenderCapture(device, world, 640, 480);
		check(resized.Pixels.size() == 640 * 480 * 4 && resized.Peak > 4,
			  "HDR targets resize for a different aspect ratio");
		settings.CameraTarget = {150, 1, 51};
		settings.Exposure = 1;
		world.SetSettings(settings);
		auto frontier = RenderCapture(device, world, width, height);
		size_t terrainPixels = 0;
		for (size_t pixel = 0; pixel < width * height; pixel++)
			terrainPixels += Luminance(frontier, pixel) > 180;
		check(terrainPixels > width * height * .9,
			  "large terrain bounds remain visible when the camera reaches the frontier");
		settings = RenderSettings{};
		settings.Shadows = false;
		world.SetSettings(settings);
		auto invalidDirectory = outputDirectory / "InvalidAsset";
		std::filesystem::create_directories(invalidDirectory / "meshes");
		auto originalMesh = fixtureDirectory / fixture.at("meshes")[0].at("path").get<std::string>();
		std::filesystem::copy_file(originalMesh, invalidDirectory / "meshes/fixture.bin",
								   std::filesystem::copy_options::overwrite_existing);
		std::filesystem::copy_file(originalMesh, outputDirectory / "escape.bin",
								   std::filesystem::copy_options::overwrite_existing);
		std::filesystem::copy_file(fixtureDirectory / "height.f32", invalidDirectory / "height.f32",
								   std::filesystem::copy_options::overwrite_existing);
		auto rejectAsset = [&](const Json& invalid, const std::string& description)
		{
			auto drawCount = world.GetDrawCount();
			auto previousSettings = world.GetSettings().Serialize();
			AtomicWrite(invalidDirectory / "world.json", invalid.dump());
			bool rejected = false;
			try
			{
				world.Load(invalidDirectory);
			}
			catch (const std::exception&)
			{
				rejected = true;
			}
			check(rejected && drawCount == world.GetDrawCount() && previousSettings == world.GetSettings().Serialize(),
				  description);
		};
		auto invalid = fixture;
		invalid["draws"][0]["mesh"] = -1;
		rejectAsset(invalid, "negative mesh index rejects without replacing the loaded world");
		invalid = fixture;
		invalid["draws"][0]["color"] = Json::array({1, 0});
		rejectAsset(invalid, "truncated color array rejects without replacing the loaded world");
		invalid = fixture;
		invalid["draws"][0]["color"][0] = 2;
		rejectAsset(invalid, "out-of-range albedo rejects without replacing the loaded world");
		invalid = fixture;
		invalid["draws"][0]["emission"][0] = 1e38;
		rejectAsset(invalid, "unrepresentable HDR emission rejects without replacing the loaded world");
		invalid = fixture;
		for (int index : {0, 5, 10})
			invalid["draws"][0]["matrix"][index] = 1e20;
		rejectAsset(invalid, "overflowing transform determinant rejects without replacing the loaded world");
		invalid = fixture;
		invalid["draws"][0]["matrix"][3] = .1;
		rejectAsset(invalid, "non-affine mesh transforms reject without replacing the loaded world");
		invalid = fixture;
		invalid["meshes"][0]["path"] = "../escape.bin";
		rejectAsset(invalid, "mesh path traversal rejects before reading another location");
		invalid = fixture;
		invalid["Renderer"]["Exposure"] = -1;
		rejectAsset(invalid, "invalid asset renderer settings reject without replacing the loaded world");
		std::ifstream meshInput(originalMesh, std::ios::binary);
		std::string meshBytes((std::istreambuf_iterator<char>(meshInput)), {});
		auto corruptedMesh = meshBytes;
		float nan = std::numeric_limits<float>::quiet_NaN();
		std::memcpy(corruptedMesh.data() + 8, &nan, 4);
		AtomicWrite(invalidDirectory / "meshes/fixture.bin", corruptedMesh);
		rejectAsset(fixture, "non-finite mesh data rejects without replacing the loaded world");
		corruptedMesh = meshBytes;
		float zeroNormal[3] = {0, 0, 0};
		std::memcpy(corruptedMesh.data() + 8 + 3 * sizeof(float), zeroNormal, sizeof(zeroNormal));
		AtomicWrite(invalidDirectory / "meshes/fixture.bin", corruptedMesh);
		rejectAsset(fixture, "zero mesh normal rejects before it can create invalid GPU lighting");
		corruptedMesh = meshBytes;
		uint32_t invalidIndex = std::numeric_limits<uint32_t>::max();
		std::memcpy(corruptedMesh.data() + corruptedMesh.size() - 4, &invalidIndex, 4);
		AtomicWrite(invalidDirectory / "meshes/fixture.bin", corruptedMesh);
		rejectAsset(fixture, "invalid mesh indices reject without replacing the loaded world");
		AtomicWrite(invalidDirectory / "meshes/fixture.bin", meshBytes);
		AtomicWrite(invalidDirectory / "height.f32", "truncated");
		rejectAsset(fixture, "truncated terrain rejects without replacing the loaded world");
		auto recovered = RenderCapture(device, world, width, height);
		check(recovered.Pixels == unshadowed.Pixels, "rendered world survives all rejected asset loads unchanged");
		report["Failed"] = failed;
		report["HDRPeak"] = unshadowed.Peak;
		report["ShadowedPixels"] = darker;
		report["FilteredSamples"] = filteredSamples;
		report["TonemapMismatches"] = tonemapMismatches;
		report["GPUTimeMilliseconds"] = {{"NoShadows", unshadowed.GPUSeconds * 1000},
										 {"Shadows", shadowed.GPUSeconds * 1000}};
		AtomicWrite(outputDirectory / "Renderer.json", report.dump(2));
		if (failed)
			throw std::runtime_error("Renderer verification failed " + std::to_string(failed) + " checks");
		std::cout << "BELLWETHER_RENDERER " << report["Checks"].size() << " passed 0 failed\n";
		report["Snow"] = VerifySnowRenderer(device, world, outputDirectory, fixtureDirectory);
		return report;
	}

	Json VerifySnowRenderer(nvrhi::IDevice* device, WorldRenderer& world, const std::filesystem::path& outputDirectory,
							const std::filesystem::path& fixtureDirectory)
	{
		std::filesystem::create_directories(outputDirectory);
		world.Load(fixtureDirectory);
		world.ClearSnowField();

		Json report;
		report["Checks"] = Json::array();
		size_t failed = 0;
		auto check = [&](bool condition, const std::string& name)
		{
			report["Checks"].push_back({{"Name", name}, {"Passed", condition}});
			if (!condition)
				failed++;
		};

		auto writePpm = [](const std::filesystem::path& path, const Capture& capture, uint32_t width, uint32_t height)
		{
			std::ofstream stream(path, std::ios::binary);
			if (!stream)
				throw std::runtime_error("Cannot open capture output: " + path.string());
			stream << "P6\n" << width << ' ' << height << "\n255\n";
			for (size_t i = 0; i < size_t(width) * height; i++)
			{
				const char rgb[3] = {static_cast<char>(capture.Pixels[i * 4 + 0]),
									 static_cast<char>(capture.Pixels[i * 4 + 1]),
									 static_cast<char>(capture.Pixels[i * 4 + 2])};
				stream.write(rgb, 3);
			}
		};

		auto linearFinite = [](const Capture& capture) -> bool
		{
			for (float value : capture.Linear)
				if (!std::isfinite(value) || value < 0.0f)
					return false;
			return !capture.Linear.empty();
		};

		auto averageLuma = [](const Capture& capture) -> double
		{
			double total = 0.0;
			const size_t count = capture.Linear.size() / 4;
			for (size_t i = 0; i < count; i++)
			{
				const double r = capture.Linear[i * 4 + 0];
				const double g = capture.Linear[i * 4 + 1];
				const double b = capture.Linear[i * 4 + 2];
				total += 0.2126 * r + 0.7152 * g + 0.0722 * b;
			}
			return count ? total / double(count) : 0.0;
		};

		auto averageBlue = [](const Capture& capture) -> double
		{
			double total = 0.0;
			const size_t count = capture.Linear.size() / 4;
			for (size_t i = 0; i < count; i++)
				total += capture.Linear[i * 4 + 2];
			return count ? total / double(count) : 0.0;
		};

		auto peakHDR = [](const Capture& capture) -> float
		{
			float peak = 0.0f;
			const size_t count = capture.Linear.size() / 4;
			for (size_t i = 0; i < count; i++)
			{
				peak = std::max(peak, capture.Linear[i * 4 + 0]);
				peak = std::max(peak, capture.Linear[i * 4 + 1]);
				peak = std::max(peak, capture.Linear[i * 4 + 2]);
			}
			return peak;
		};

		auto maxByteDelta = [](const Capture& a, const Capture& b) -> uint32_t
		{
			uint32_t best = 0;
			const size_t count = std::min(a.Pixels.size(), b.Pixels.size());
			for (size_t i = 0; i < count; i += 4)
			{
				for (size_t c = 0; c < 3; c++)
				{
					const int diff = std::abs(int(a.Pixels[i + c]) - int(b.Pixels[i + c]));
					best = std::max(best, static_cast<uint32_t>(diff));
				}
			}
			return best;
		};

		const uint32_t width = 320;
		const uint32_t height = 180;

		RenderSettings bareSettings;
		bareSettings.Shadows = true;
		bareSettings.Exposure = 1.0f;
		world.SetSettings(bareSettings);
		world.ClearSnowField();
		auto bare = RenderCapture(device, world, width, height);
		check(linearFinite(bare), "bare baseline HDR capture finite");
		writePpm(outputDirectory / "SnowBare.ppm", bare, width, height);

		SnowFieldConfig snowConfig;
		snowConfig.Resolution = 256;
		snowConfig.WorldExtent = 20.0f;
		snowConfig.MaxDepth = 0.60f;
		snowConfig.BaseDepth = 0.36f;
		SnowField snow(snowConfig);

		RenderSettings snowSettings = bareSettings;
		snowSettings.SnowCoverage = 1.0f;
		snowSettings.SnowSubsurface = 0.75f;
		snowSettings.SnowSparkle = 0.0f;
		snowSettings.SnowfallIntensity = 0.0f;
		world.SetSettings(snowSettings);
		world.UpdateSnowField(snow);
		auto virgin = RenderCapture(device, world, width, height);
		check(linearFinite(virgin), "virgin snow HDR capture finite");
		check(virgin.Pixels != bare.Pixels, "virgin snow visibly alters terrain pixels");
		check(averageLuma(virgin) > averageLuma(bare) * 1.15, "virgin snow increases scene luminance");
		check(averageBlue(virgin) > averageBlue(bare) * 1.15, "virgin snow shifts winter icy blue luminance upward");
		writePpm(outputDirectory / "SnowVirgin.ppm", virgin, width, height);

		// Stamp deep sled trail & crater directly under camera target
		snow.StampSegment(glm::vec2(-8.0f, 0.0f), glm::vec2(8.0f, 0.0f), 1.4f, 0.34f);
		snow.StampSegment(glm::vec2(0.0f, -8.0f), glm::vec2(0.0f, 8.0f), 1.2f, 0.34f);
		snow.StampCircle(glm::vec2(0.0f, 0.0f), 2.0f, 0.34f);
		world.UpdateSnowField(snow);
		auto tracked = RenderCapture(device, world, width, height);
		check(linearFinite(tracked), "tracked snow HDR capture finite");
		check(tracked.Pixels != virgin.Pixels, "stamped snow tracks modify rendered frame");
		check(maxByteDelta(tracked, virgin) >= 12, "stamped snow track normal/albedo delta is pronounced");
		check(averageLuma(tracked) < averageLuma(virgin), "carved dark track lowers mean luminance");
		writePpm(outputDirectory / "SnowTracked.ppm", tracked, width, height);

		// Enable HDR crystal sparkle
		snowSettings.SnowSparkle = 1.6f;
		world.SetSettings(snowSettings);
		auto sparkle = RenderCapture(device, world, width, height);
		check(linearFinite(sparkle), "sparkle snow HDR capture finite");
		check(sparkle.Pixels != tracked.Pixels, "snow sparkle modifies rendered pixels");
		check(peakHDR(sparkle) > peakHDR(tracked), "snow sparkle boosts HDR specular crystal peak");
		writePpm(outputDirectory / "SnowSparkle.ppm", sparkle, width, height);

		// Verify external depth.u8 / ice.u8 track bridge load
		const std::filesystem::path bridgeDir = outputDirectory / "BridgeWorld";
		std::filesystem::remove_all(bridgeDir);
		std::filesystem::copy(fixtureDirectory, bridgeDir,
							  std::filesystem::copy_options::recursive |
								  std::filesystem::copy_options::overwrite_existing);
		std::vector<uint8_t> depthBytes(256u * 256u, 255u);
		std::vector<uint8_t> iceBytes(256u * 256u, 0u);
		for (uint32_t y = 96; y < 160; y++)
		{
			for (uint32_t x = 104; x < 152; x++)
			{
				const size_t idx = size_t(y) * 256u + x;
				depthBytes[idx] = 24u;
				iceBytes[idx] = 220u;
			}
		}
		{
			std::ofstream depthStream(bridgeDir / "depth.u8", std::ios::binary);
			depthStream.write(reinterpret_cast<const char*>(depthBytes.data()),
							  static_cast<std::streamsize>(depthBytes.size()));
			std::ofstream iceStream(bridgeDir / "ice.u8", std::ios::binary);
			iceStream.write(reinterpret_cast<const char*>(iceBytes.data()),
							static_cast<std::streamsize>(iceBytes.size()));
		}
		bool rejectedMissingSnowSize = false;
		try
		{
			world.Load(bridgeDir);
		}
		catch (const std::runtime_error&)
		{
			rejectedMissingSnowSize = true;
		}
		check(rejectedMissingSnowSize, "depth.u8 without snow_size in world.json rejects cleanly");
		auto bridgeManifest = ReadJson(bridgeDir / "world.json");
		bridgeManifest["snow_size"] = 256;
		AtomicWrite(bridgeDir / "world.json", bridgeManifest.dump(2));
		world.Load(bridgeDir);
		snowSettings.SnowSparkle = 0.0f;
		world.SetSettings(snowSettings);
		auto bridged = RenderCapture(device, world, width, height);
		check(linearFinite(bridged), "bridged depth.u8/ice.u8 capture finite");
		check(bridged.Pixels != virgin.Pixels, "bridged depth.u8/ice.u8 track renders distinct ice trail");
		writePpm(outputDirectory / "SnowBridge.ppm", bridged, width, height);
		std::filesystem::remove_all(bridgeDir);

		// Verify clean recovery when clearing snowfield back to bare baseline
		world.Load(fixtureDirectory);
		world.ClearSnowField();
		world.SetSettings(bareSettings);
		auto recovered = RenderCapture(device, world, width, height);
		check(recovered.Pixels == bare.Pixels, "ClearSnowField restores exact bare terrain rendering");

		report["BareLuma"] = averageLuma(bare);
		report["VirginLuma"] = averageLuma(virgin);
		report["TrackedLuma"] = averageLuma(tracked);
		report["TrackedPeakHDR"] = peakHDR(tracked);
		report["SparklePeakHDR"] = peakHDR(sparkle);
		report["Failed"] = failed;
		AtomicWrite(outputDirectory / "Snow.json", report.dump(2));
		if (failed)
			throw std::runtime_error("Snow renderer verification failed " + std::to_string(failed) + " checks");
		std::cout << "BELLWETHER_SNOW " << report["Checks"].size() << " passed 0 failed\n";
		return report;
	}
} // namespace Bellwether
