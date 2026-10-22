#include "Bellwether/Scene.h"
#include "Bellwether/ScriptRuntime.h"

#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace
{
	int s_Passed = 0;
	int s_Failed = 0;
	void Check(bool condition, const std::string& name)
	{
		std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
		(condition ? s_Passed : s_Failed)++;
	}

	template <typename Function> void Rejects(Function function, const std::string& name)
	{
		bool rejected = false;
		try
		{
			function();
		}
		catch (const std::exception&)
		{
			rejected = true;
		}
		Check(rejected, name);
	}
} // namespace

int main(int argumentCount, char** arguments)
{
	using namespace Bellwether;
	try
	{
		if (argumentCount != 2)
			throw std::invalid_argument("Tests require the project directory");
		auto root = std::filesystem::path(arguments[1]);
		Scene scene;
		auto first = scene.CreateEntity("First");
		Check(scene.Exists(first), "create and exists");
		scene.DestroyEntity(first);
		auto second = scene.CreateEntity("Second");
		Check(!scene.Exists(first) && second != first, "destroyed IDs never alias new entities");
		Rejects([&] { scene.GetEntity(first); }, "stale entity rejected");
		Rejects([&] { scene.DestroyEntity(first); }, "double destruction rejected");
		Rejects([&] { scene.CreateEntity(""); }, "empty names rejected");
		scene.GetEntity(second).Transform.Translation = {1, 2, 3};
		auto transformed = scene.GetEntity(second).Transform.GetMatrix() * glm::vec4(0, 0, 0, 1);
		Check(glm::vec3(transformed) == glm::vec3(1, 2, 3), "transform matrix translation");
		auto snapshot = scene.Serialize();
		Scene restored;
		restored.Deserialize(snapshot);
		Check(restored.Serialize() == snapshot, "scene serialization roundtrip");
		auto invalid = snapshot;
		invalid["Entities"].push_back(invalid["Entities"][0]);
		Rejects([&] { restored.Deserialize(invalid); }, "duplicate IDs rejected");
		Check(restored.Serialize() == snapshot, "failed deserialization rolls back");
		invalid = snapshot;
		invalid["Entities"][0]["Transform"]["Scale"] = {0, 1, 1};
		Rejects([&] { restored.Deserialize(invalid); }, "singular transforms rejected");
		invalid = snapshot;
		invalid["Entities"][0]["ID"] = -1;
		Rejects([&] { restored.Deserialize(invalid); }, "negative IDs rejected");
		invalid = snapshot;
		invalid["Entities"][0]["Transform"]["Translation"] = {1, nullptr, 3};
		Rejects([&] { restored.Deserialize(invalid); }, "invalid vector rejected");
		Json commands = Json::array(
			{{{"Operation", "Rename"}, {"ID", second}, {"Name", "Changed"}}, {{"Operation", "Destroy"}, {"ID", 9999}}});
		Rejects([&] { scene.ApplyCommands(commands); }, "invalid command batch rejected");
		Check(scene.Serialize() == snapshot, "command batch is transactional");
		scene.ApplyCommands(
			Json::array({{{"Operation", "Create"}, {"Name", "Agent entity"}, {"Translation", {4, 5, 6}}}}));
		Check(scene.GetEntities().size() == 2, "agent creates real entity");
		auto outputDirectory = std::filesystem::current_path() / "Evidence";
		auto path = outputDirectory / "TestScene.bwscene";
		scene.Save(path);
		restored.Load(path);
		Check(restored.Serialize() == scene.Serialize(), "scene disk roundtrip");
		scene.GetEntity(second).Name = "Saved Again";
		scene.Save(path);
		restored.Load(path);
		Check(restored.GetEntity(second).Name == "Saved Again", "atomic destination replacement");
		std::string firstContents(10000, 'A'), secondContents(10000, 'B');
		auto concurrentPath = outputDirectory / "ConcurrentWrite.txt";
		std::thread writerOne([&] { AtomicWrite(concurrentPath, firstContents); });
		std::thread writerTwo([&] { AtomicWrite(concurrentPath, secondContents); });
		writerOne.join();
		writerTwo.join();
		std::ifstream writtenFile(concurrentPath, std::ios::binary);
		std::string writtenContents((std::istreambuf_iterator<char>(writtenFile)), {});
		Check(writtenContents == firstContents || writtenContents == secondContents,
			  "concurrent writes never interleave documents");
		invalid = snapshot;
		invalid["Format"] = 999;
		Rejects([&] { restored.Deserialize(invalid); }, "unknown scene format rejected");
		Rejects([&] { scene.ApplyCommands(Json::array({{{"Operation", "NotImplemented"}}})); },
				"unsupported agent operations reject explicitly");
		Rejects([&] { scene.ApplyCommands(Json::array({{{"Operation", "Destroy"}, {"ID", 2.1}}})); },
				"fractional command IDs cannot truncate");
		Rejects(
			[&]
			{
				scene.ApplyCommands(
					Json::array({{{"Operation", "Create"}, {"Name", "Typo"}, {"Translatoin", {1, 2, 3}}}}));
			},
			"unknown command fields reject");
		invalid = snapshot;
		invalid["Entities"][0]["UnimplementedComponent"] = Json::object();
		Rejects([&] { restored.Deserialize(invalid); }, "unsupported components cannot silently disappear");
		auto transform = Json{{"Translation", {7, 8, 9}}, {"Rotation", {0, 0.5, 0}}, {"Scale", {2, 3, 4}}};
		scene.ApplyCommands(Json::array({{{"Operation", "SetTransform"}, {"ID", second}, {"Transform", transform}}}));
		Check(scene.GetEntity(second).Transform.Translation == glm::vec3(7, 8, 9) &&
				  scene.GetEntity(second).Transform.Scale == glm::vec3(2, 3, 4),
			  "agent transform changes complete component");
		Scene featureScene;
		featureScene.Load(root / "Scenes/FeatureTest.bwscene");
		std::ifstream scriptFile(root / "Scenes/FeatureTest.lua");
		std::string script((std::istreambuf_iterator<char>(scriptFile)), {});
		std::string error;
		ScriptRuntime runtime(featureScene);
		Check(runtime.Execute(script, error), "entire Lua API feature scene: " + error);
		Check(runtime.Update(0.25, error), "Lua update callback");
		auto ids = featureScene.GetEntities();
		Check(featureScene.GetEntity(ids.back()).Transform.Translation.x == 0.25f, "Lua update changes scene");
		Check(!runtime.Update(-1, error), "invalid timestep rejected");
		Check(!runtime.Update(std::numeric_limits<double>::quiet_NaN(), error), "NaN timestep rejected");
		for (const auto& source :
			 {"Engine.SetPosition(9999, 1, 2, 3)", "Engine.CreateEntity('rolled back'); error('failure')",
			  "Engine.SetPosition(1, 0/0, 0, 0)", "Engine.CreateEntity('loop'); while true do end",
			  "Engine.CreateEntity('')", "local excessive = string.rep('a', 32 * 1024 * 1024)"})
		{
			Scene test = featureScene;
			auto before = test.Serialize();
			ScriptRuntime failing(test);
			Check(!failing.Execute(source, error), "Lua invalid input or instruction budget rejected");
			Check(test.Serialize() == before, "Lua error rolls back scene mutations");
			Check(!failing.Update(0.1, error), "faulted Lua runtime cannot continue");
		}
		Scene sandboxScene;
		ScriptRuntime sandbox(sandboxScene);
		Check(sandbox.Execute("assert(io == nil and os == nil and package == nil and debug == nil and dofile == nil "
							  "and loadfile == nil and pcall == nil and xpcall == nil)",
							  error),
			  "sandbox restricts host access and budget interception");
		Check(ScriptRuntime::GetAPI().size() == 8, "script API manifest");
		std::cout << "BELLWETHER_CORE " << s_Passed << " passed " << s_Failed << " failed\n";
		return s_Failed ? 1 : 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "TEST EXCEPTION: " << exception.what() << '\n';
		return 1;
	}
}
