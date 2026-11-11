#include "Bellwether/Scene.h"
#include "Bellwether/GltfImporter.h"
#include "Bellwether/SnowField.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
	Bellwether::Json SerializeSnowSummary(const Bellwether::SnowField& snow)
	{
		const auto& config = snow.GetConfig();
		return {{"Success", true},
				{"Resolution", snow.GetResolution()},
				{"WorldExtent", config.WorldExtent},
				{"MaxDepth", config.MaxDepth},
				{"BaseDepth", config.BaseDepth},
				{"ActiveTrackCount", snow.GetActiveTrackCount()},
				{"AverageDepth", snow.GetAverageDepth()},
				{"MutationCounter", snow.GetMutationCounter()}};
	}

	double ParseFiniteArg(const char* text, const char* name)
	{
		char* end = nullptr;
		double value = std::strtod(text, &end);
		if (!text || !text[0] || !end || *end != '\0' || !std::isfinite(value))
			throw std::invalid_argument(std::string("Invalid numeric argument for ") + name);
		return value;
	}
} // namespace

int main(int argumentCount, char** arguments)
{
	try
	{
		if (argumentCount < 3)
			throw std::invalid_argument(
				"Usage: BellwetherAgent inspect|validate|create|apply|inspect-model|inspect-snow|stamp-snow|step-snow "
				"...");
		Bellwether::Scene scene;
		std::string command = arguments[1];
		if (command == "inspect-model")
		{
			if (argumentCount != 3)
				throw std::invalid_argument("inspect-model requires exactly one asset path");
			auto model = Bellwether::ImportGltf(arguments[2]);
			std::cout << Bellwether::Json({{"Success", true},
										   {"Primitives", model.Primitives.size()},
										   {"Placements", model.Placements.size()},
										   {"Materials", model.Materials.size()},
										   {"Images", model.Images.size()},
										   {"Textures", model.Textures.size()}})
							 .dump(2)
					  << '\n';
			return 0;
		}
		if (command == "inspect-snow")
		{
			if (argumentCount != 3)
				throw std::invalid_argument("inspect-snow requires exactly one .bwsn track file path");
			Bellwether::SnowField snow;
			snow.LoadTracks(arguments[2]);
			std::cout << SerializeSnowSummary(snow).dump(2) << '\n';
			return 0;
		}
		if (command == "stamp-snow")
		{
			if (argumentCount != 7 && argumentCount != 10)
				throw std::invalid_argument(
					"stamp-snow requires: <tracks.bwsn> <wx> <wz> <radius> <depth> [resolution worldExtent baseDepth]");
			std::filesystem::path tracksPath = arguments[2];
			double wx = ParseFiniteArg(arguments[3], "wx");
			double wz = ParseFiniteArg(arguments[4], "wz");
			double radius = ParseFiniteArg(arguments[5], "radius");
			double depth = ParseFiniteArg(arguments[6], "depth");
			if (std::abs(wx) > 1e6 || std::abs(wz) > 1e6 || radius <= 0.0 || radius > 100.0 || depth < 0.0 ||
				depth > 50.0)
				throw std::invalid_argument("Snow stamp parameters out of bounds");
			Bellwether::SnowField snow;
			if (std::filesystem::exists(tracksPath))
			{
				snow.LoadTracks(tracksPath);
				if (argumentCount == 10)
					throw std::invalid_argument("Cannot override grid dimensions when stamping an existing .bwsn file");
			}
			else if (argumentCount == 10)
			{
				Bellwether::SnowFieldConfig config;
				double res = ParseFiniteArg(arguments[7], "resolution");
				if (res < 4.0 || res > 1024.0 || std::floor(res) != res)
					throw std::invalid_argument("Snow resolution must be an integer in [4, 1024]");
				config.Resolution = static_cast<uint32_t>(res);
				config.WorldExtent = static_cast<float>(ParseFiniteArg(arguments[8], "worldExtent"));
				config.BaseDepth = static_cast<float>(ParseFiniteArg(arguments[9], "baseDepth"));
				snow.Reset(config);
			}
			else
			{
				throw std::invalid_argument(
					"Snow tracks file does not exist; pass [resolution worldExtent baseDepth] to create it");
			}
			bool stamped = snow.StampCircle(glm::vec2(static_cast<float>(wx), static_cast<float>(wz)),
											static_cast<float>(radius), static_cast<float>(depth));
			snow.SaveTracks(tracksPath);
			auto summary = SerializeSnowSummary(snow);
			summary["Stamped"] = stamped;
			std::cout << summary.dump(2) << '\n';
			return 0;
		}
		if (command == "step-snow")
		{
			if (argumentCount != 7 && argumentCount != 8)
				throw std::invalid_argument(
					"step-snow requires: <tracks.bwsn> <dt> <snowfallRate> <windX> <windZ> [ambientCelsius]");
			std::filesystem::path tracksPath = arguments[2];
			double dt = ParseFiniteArg(arguments[3], "dt");
			double snowfall = ParseFiniteArg(arguments[4], "snowfallRate");
			double windX = ParseFiniteArg(arguments[5], "windX");
			double windZ = ParseFiniteArg(arguments[6], "windZ");
			double ambient = argumentCount == 8 ? ParseFiniteArg(arguments[7], "ambientCelsius") : -6.0;
			if (dt <= 0.0 || dt > 60.0 || snowfall < 0.0 || snowfall > 5.0 || std::abs(windX) > 100.0 ||
				std::abs(windZ) > 100.0 || ambient < -60.0 || ambient > 40.0)
				throw std::invalid_argument("Snow weather step parameters out of bounds");
			Bellwether::SnowField snow;
			snow.LoadTracks(tracksPath);
			snow.StepWeather(static_cast<float>(dt), static_cast<float>(snowfall),
							 glm::vec2(static_cast<float>(windX), static_cast<float>(windZ)),
							 static_cast<float>(ambient));
			snow.SaveTracks(tracksPath);
			std::cout << SerializeSnowSummary(snow).dump(2) << '\n';
			return 0;
		}
		if (command == "create")
		{
			if (std::filesystem::exists(arguments[2]))
				throw std::invalid_argument("create refuses to overwrite an existing scene");
			scene.Save(arguments[2]);
		}
		else
		{
			scene.Load(arguments[2]);
			if (command == "apply")
			{
				if (argumentCount != 4)
					throw std::invalid_argument("apply requires a command batch file");
				scene.ApplyCommands(Bellwether::ReadJson(arguments[3]));
				scene.Save(arguments[2]);
			}
			else if (command != "inspect" && command != "validate")
				throw std::invalid_argument("Unknown agent command: " + command);
		}
		std::cout << Bellwether::Json({{"Success", true}, {"Scene", scene.Serialize()}}).dump(2) << '\n';
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << Bellwether::Json({{"Success", false}, {"Error", exception.what()}}).dump() << '\n';
		return 1;
	}
}
