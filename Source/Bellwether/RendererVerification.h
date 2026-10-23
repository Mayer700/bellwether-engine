#pragma once
#include "WorldRenderer.h"

namespace Bellwether
{
	Json VerifyRenderer(nvrhi::IDevice* device, WorldRenderer& world, const std::filesystem::path& outputDirectory,
						const std::filesystem::path& fixtureDirectory);
}
