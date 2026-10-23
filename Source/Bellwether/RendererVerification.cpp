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
		return report;
	}
} // namespace Bellwether
