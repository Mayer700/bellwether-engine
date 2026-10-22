#include "Bellwether/VulkanContext.h"
#include <chrono>
#include <iostream>

int main(int argumentCount, char** arguments)
{
	try
	{
		bool test = false;
		std::filesystem::path worldDirectory, readbackPath = "Readback.ppm", capturePath = "World.ppm";
		for (int index = 1; index < argumentCount; index++)
		{
			std::string option = arguments[index];
			if (option == "--gpu-test")
				test = true;
			else if (option == "--world" || option == "--readback" || option == "--capture")
			{
				if (++index >= argumentCount)
					throw std::invalid_argument("Missing value for " + option);
				if (option == "--world")
					worldDirectory = arguments[index];
				else if (option == "--readback")
					readbackPath = arguments[index];
				else
					capturePath = arguments[index];
			}
			else
				throw std::invalid_argument("Unknown runtime option: " + option);
		}
		Bellwether::VulkanContext context(960, 600, "Bellwether Engine | Runtime", test);
		bool worldTest = !worldDirectory.empty();
		if (worldTest)
			context.LoadWorld(worldDirectory);
		std::cout << "VULKAN_DEVICE " << context.GetDeviceName() << '\n';
		unsigned frames = 0;
		auto started = std::chrono::steady_clock::now();
		while (!context.ShouldClose() && (!test || frames < 12))
		{
			if (test && std::chrono::steady_clock::now() - started > std::chrono::seconds(30))
				throw std::runtime_error("GPU regression exceeded 30 second deadline");
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
