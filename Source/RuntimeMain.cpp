#include "Bellwether/VulkanContext.h"
#include "Bellwether/Scene.h"
#include <chrono>
#include <iostream>

int main(int argumentCount, char** arguments)
{
	try
	{
		bool test = false, rendererTest = false, modelTest = false;
		std::filesystem::path worldDirectory, readbackPath = "Readback.ppm", capturePath = "World.ppm";
		std::filesystem::path settingsPath, scenePath, modelPath;
		for (int index = 1; index < argumentCount; index++)
		{
			std::string option = arguments[index];
			if (option == "--gpu-test")
				test = true;
			else if (option == "--renderer-test")
				rendererTest = true;
			else if (option == "--model-test")
				modelTest = true;
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
		if (!settingsPath.empty() && !scenePath.empty())
			throw std::invalid_argument("Choose --settings or --scene for renderer configuration");
		if (!modelPath.empty() && !worldDirectory.empty())
			throw std::invalid_argument("Choose --world or --gltf for content");
		Bellwether::VulkanContext context(960, 600, "Bellwether Engine | Runtime", test);
		bool worldTest = !worldDirectory.empty() || !modelPath.empty() || !scenePath.empty();
		if (!worldDirectory.empty())
			context.LoadWorld(worldDirectory);
		if (!modelPath.empty())
			context.LoadModel(modelPath);
		if (!scenePath.empty() && modelPath.empty() && worldDirectory.empty())
			context.LoadScene(scenePath);
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
