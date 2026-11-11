#pragma once
#include "WorldRenderer.h"

namespace Bellwether
{
	Json VerifyModelRenderer(nvrhi::IDevice* device, WorldRenderer& world, const std::filesystem::path& outputDirectory,
							 const std::filesystem::path& fixtureDirectory);
	Json VerifyRenderer(nvrhi::IDevice* device, WorldRenderer& world, const std::filesystem::path& outputDirectory,
						const std::filesystem::path& fixtureDirectory);
	Json VerifySnowRenderer(nvrhi::IDevice* device, WorldRenderer& world, const std::filesystem::path& outputDirectory,
							const std::filesystem::path& fixtureDirectory);
} // namespace Bellwether
