#include "Bellwether/VulkanContext.h"
#include "Bellwether/Scene.h"
#include <chrono>
#include <iostream>

namespace
{
	void PrintRuntimeUsage()
	{
		std::cout << "Bellwether Engine Runtime\n"
				  << "Usage:\n"
				  << "  BellwetherRuntime.exe                         Launch interactive real-time Snow Sandbox\n"
				  << "  BellwetherRuntime.exe --snow-sandbox [--scenario <tracks|melt|blizzard|sparkle>]\n"
				  << "  BellwetherRuntime.exe --scene <file.bwscene>  Preview authored scene\n"
				  << "  BellwetherRuntime.exe --gltf <file.glb> [--settings <file.json>]\n"
				  << "  BellwetherRuntime.exe --world <dir>           Preview world directory fixture\n"
				  << "  BellwetherRuntime.exe --gpu-test ...          Automated Vulkan/NVRHI GPU verification\n\n"
				  << "Real-Time Snow Sandbox Controls:\n"
				  << "  WASD / Arrows     Pan camera\n"
				  << "  Q / E             Zoom camera out / in\n"
				  << "  Left Mouse Drag   Carve snow tracks & footprints\n"
				  << "  Right Mouse / M   Apply localized snow melt heat\n"
				  << "  Space             Stamp / drag trench under camera\n"
				  << "  F                 Trigger sub-zero cold snap refreeze\n"
				  << "  G / Enter         Launch snow sled / boulder physics test\n"
				  << "  1 / 2             Decrease / increase snowfall intensity\n"
				  << "  3 / 4             Cycle wind vector presets\n"
				  << "  5 / 6             Cycle snow sparkle & subsurface presets\n"
				  << "  F1..F4 / T / R    Select scenario (Tracks, Melt, Blizzard, Sparkle) / cycle / reset\n"
				  << "  H                 Print live probe snow stats in console\n";
	}
} // namespace

int main(int argumentCount, char** arguments)
{
	std::cout << std::unitbuf;
	std::cerr << std::unitbuf;
	try
	{
		bool test = false, rendererTest = false, modelTest = false, snowTest = false, snowSandbox = false;
		bool hasScenario = false;
		Bellwether::SnowTestScenario scenario = Bellwether::SnowTestScenario::TracksAndPhysics;
		std::filesystem::path worldDirectory, readbackPath = "Readback.ppm", capturePath = "World.ppm";
		std::filesystem::path settingsPath, scenePath, modelPath;
		for (int index = 1; index < argumentCount; index++)
		{
			std::string option = arguments[index];
			if (option == "--help" || option == "-h")
			{
				PrintRuntimeUsage();
				return 0;
			}
			else if (option == "--gpu-test")
				test = true;
			else if (option == "--renderer-test")
				rendererTest = true;
			else if (option == "--model-test")
				modelTest = true;
			else if (option == "--snow-test")
				snowTest = true;
			else if (option == "--snow-sandbox")
				snowSandbox = true;
			else if (option == "--scenario")
			{
				if (++index >= argumentCount)
					throw std::invalid_argument("Missing value for --scenario");
				scenario = Bellwether::ParseSnowScenario(arguments[index]);
				hasScenario = true;
				snowSandbox = true;
			}
			else if (option == "--world" || option == "--readback" || option == "--capture" || option == "--settings" ||
					 option == "--scene" || option == "--gltf")
			{
				if (++index >= argumentCount)
					throw std::invalid_argument("Missing value for " + option);
				if (option == "--world")
					worldDirectory = arguments[index];
				else if (option == "--gltf")
					modelPath = arguments[index];
				else if (option == "--readback")
					readbackPath = arguments[index];
				else if (option == "--settings")
					settingsPath = arguments[index];
				else if (option == "--scene")
					scenePath = arguments[index];
				else
					capturePath = arguments[index];
			}
			else
				throw std::invalid_argument("Unknown runtime option: " + option);
		}
		if (rendererTest && (!test || worldDirectory.empty()))
			throw std::invalid_argument("--renderer-test requires --gpu-test and the feature fixture --world");
		if (modelTest && (!test || modelPath.empty()))
			throw std::invalid_argument("--model-test requires --gpu-test and the model fixture --gltf");
		if (snowTest && !test)
			throw std::invalid_argument("--snow-test requires --gpu-test");
		if (!settingsPath.empty() && !scenePath.empty())
			throw std::invalid_argument("Choose --settings or --scene for renderer configuration");
		if (!modelPath.empty() && !worldDirectory.empty())
			throw std::invalid_argument("Choose --world or --gltf for content");

		const bool hasFileContent = !worldDirectory.empty() || !modelPath.empty() || !scenePath.empty();
		if ((snowSandbox || snowTest) && hasFileContent)
		{
			throw std::invalid_argument(hasScenario
											? "Choose --snow-sandbox (or omit file inputs) when using --scenario"
											: "Choose either Snow Sandbox mode or --world/--gltf/--scene");
		}
		if (snowTest)
			snowSandbox = true;
		if (!test && !hasFileContent)
			snowSandbox = true;

		Bellwether::VulkanContext context(960, 600, "Bellwether Engine | Runtime", test);
		bool worldTest = hasFileContent || snowSandbox;
		if (!worldDirectory.empty())
			context.LoadWorld(worldDirectory);
		if (!modelPath.empty())
			context.LoadModel(modelPath);
		if (!scenePath.empty() && modelPath.empty() && worldDirectory.empty())
			context.LoadScene(scenePath);
		if (snowSandbox)
		{
			context.LoadSnowSandbox(scenario);
			if (!test)
				PrintRuntimeUsage();
		}
		if (!settingsPath.empty())
			context.SetRenderSettings(Bellwether::RenderSettings::Deserialize(Bellwether::ReadJson(settingsPath)));
		if (!scenePath.empty() && (!modelPath.empty() || !worldDirectory.empty()))
		{
			Bellwether::Scene scene;
			scene.Load(scenePath);
			context.SetRenderSettings(scene.GetRenderSettings());
		}
		std::cout << "VULKAN_DEVICE " << context.GetDeviceName() << '\n';
		unsigned frames = 0;
		auto started = std::chrono::steady_clock::now();
		auto previousFrame = started;
		while (!context.ShouldClose() && (!test || frames < 12))
		{
			if (test && std::chrono::steady_clock::now() - started > std::chrono::seconds(30))
				throw std::runtime_error("GPU regression exceeded 30 second deadline");
			const auto now = std::chrono::steady_clock::now();
			const float deltaSeconds = std::chrono::duration<float>(now - previousFrame).count();
			previousFrame = now;
			if (!test && worldTest)
				context.StepInteractivePreview(deltaSeconds);
			if (context.RenderFrame())
				frames++;
			if (test && frames == 4)
				context.Resize(800, 600);
		}
		if (test)
		{
			context.VerifyReadback(readbackPath);
			if (worldTest)
				context.CaptureWorld(capturePath);
			if (rendererTest)
				context.VerifyRenderer(capturePath.parent_path() / "Renderer", worldDirectory);
			if (modelTest)
				context.VerifyModelRenderer(capturePath.parent_path() / "Models", modelPath.parent_path());
			if (snowTest)
				context.VerifySnowSandbox(capturePath.parent_path() / "SnowSandbox");
			context.Shutdown();
			if (!context.ShouldClose())
				throw std::runtime_error("Shutdown did not close the context");
			bool rejectedClosedFrame = false;
			try
			{
				context.RenderFrame();
			}
			catch (const std::logic_error&)
			{
				rejectedClosedFrame = true;
			}
			if (!rejectedClosedFrame)
				throw std::runtime_error("Closed context accepted a frame");
			context.Shutdown();
			if (context.GetValidationErrors())
				throw std::runtime_error("NVRHI validation reported errors");
			std::cout << "BELLWETHER_VULKAN 12 frames, resize, 2961 readback pixels passed\n";
		}
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "Runtime error: " << exception.what() << '\n';
		return 1;
	}
}
