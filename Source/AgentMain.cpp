#include "Bellwether/Scene.h"
#include "Bellwether/GltfImporter.h"

#include <iostream>

int main(int argumentCount, char** arguments)
{
	try
	{
		if (argumentCount < 3)
			throw std::invalid_argument(
				"Usage: BellwetherAgent inspect|validate scene | create scene | apply scene commands.json");
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
