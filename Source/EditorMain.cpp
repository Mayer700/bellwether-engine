#include "Bellwether/VulkanContext.h"
#include <iostream>

// The editor shell deliberately does not pretend to provide authoring panels yet.
// Scene authoring is currently exposed through the tested BellwetherAgent command service.
int main()
{
	try
	{
		Bellwether::VulkanContext context(1280, 800, "Bellwether Engine | Editor foundation");
		while (!context.ShouldClose())
			context.RenderFrame();
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "Editor error: " << exception.what() << '\n';
		return 1;
	}
}
