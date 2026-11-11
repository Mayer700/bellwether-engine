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
		auto executableDirectory = GetExecutableDirectory();
		Check(
			std::filesystem::is_directory(executableDirectory) &&
				std::filesystem::is_regular_file(executableDirectory / std::filesystem::path(arguments[0]).filename()),
			"executable resource root is independent of working directory");
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
		invalid = snapshot;
		invalid["NextID"] = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1;
		Rejects([&] { restored.Deserialize(invalid); }, "overflowing NextID rejected");
		invalid = snapshot;
		invalid["Entities"][0]["Script"] = 42;
		Rejects([&] { restored.Deserialize(invalid); }, "non-string Script rejected");
		Rejects([&] { ReadJson(outputDirectory / "DoesNotExist.json"); }, "missing JSON rejected before file_size");
		Rejects([&] { ReadJson(outputDirectory); }, "directory JSON read rejected");
		Rejects([&] { AtomicWrite(outputDirectory, "oops"); }, "AtomicWrite refuses directory overwrite");
		Rejects([&] { scene.ApplyCommands(Json::array({42})); }, "non-object scene command rejected");
		Rejects([&] { scene.ApplyCommands(Json::array({{{"Operation", "Create"}, {"Name", 99}}})); },
				"non-string Create Name rejected");
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
		Scene modelScene;
		auto modelID = modelScene.CreateEntity("Editable glTF model");
		Check(!modelScene.Serialize()["Entities"][0].contains("Model"), "legacy entity omits absent model component");
		modelScene.ApplyCommands(
			Json::array({{{"Operation", "SetModel"}, {"ID", modelID}, {"Asset", "ModelFixtures/Materials.glb"}}}));
		auto modelCheckpoint = modelScene.Serialize();
		Check(modelScene.GetEntity(modelID).ModelAsset == "ModelFixtures/Materials.glb" &&
				  modelCheckpoint["Entities"][0]["Model"]["Asset"] == "ModelFixtures/Materials.glb",
			  "agent sets serialized editable model reference");
		Scene modelRestored;
		modelRestored.Deserialize(modelCheckpoint);
		Check(modelRestored.Serialize() == modelCheckpoint,
			  "model component scene roundtrip does not require asset IO");
		auto modelPath = outputDirectory / "ModelScene.bwscene";
		modelScene.Save(modelPath);
		modelRestored.Load(modelPath);
		Check(modelRestored.GetEntity(modelID).ModelAsset == "ModelFixtures/Materials.glb",
			  "model component disk roundtrip");
		modelScene.ApplyCommands(
			Json::array({{{"Operation", "SetTransform"}, {"ID", modelID}, {"Transform", transform}}}));
		Check(modelScene.GetEntity(modelID).ModelAsset == "ModelFixtures/Materials.glb",
			  "transform edit preserves model component");
		modelCheckpoint = modelScene.Serialize();
		Rejects(
			[&]
			{
				modelScene.ApplyCommands(
					Json::array({{{"Operation", "SetModel"}, {"ID", 9999}, {"Asset", "Missing.glb"}}}));
			},
			"model command rejects missing entity");
		Rejects(
			[&]
			{ modelScene.ApplyCommands(Json::array({{{"Operation", "SetModel"}, {"ID", modelID}, {"Asset", 42}}})); },
			"model command rejects non-string reference");
		Rejects(
			[&]
			{
				modelScene.ApplyCommands(Json::array(
					{{{"Operation", "SetModel"}, {"ID", modelID}, {"Asset", "Valid.glb"}, {"Unimplemented", true}}}));
			},
			"model command rejects unknown fields");
		for (const auto& asset : std::vector<std::string>{"/Root.glb",
														  "//server/share.glb",
														  "C:/Root.glb",
														  "Folder\\Root.glb",
														  "../Outside.glb",
														  "Folder/../Outside.glb",
														  "./Root.glb",
														  "Folder/./Root.glb",
														  "Folder//Root.glb",
														  "Folder/",
														  "https://host/Root.glb",
														  "file:///Root.glb",
														  "Root.obj",
														  "Root.glb?query",
														  "Root.glb#fragment",
														  "NUL.glb",
														  "Folder./Root.glb",
														  "Folder /Root.glb",
														  "CON/Root.glb",
														  "Root*.glb",
														  std::string("Bad\0Root.glb", 12),
														  std::string("Bad\nRoot.glb"),
														  std::string(1025, 'a') + ".glb",
														  std::string("Bad\xc0\xaf.glb"),
														  std::string("Bad\xed\xa0\x80.glb"),
														  std::string("Bad\xf4\x90\x80\x80.glb"),
														  std::string("Bad\xe2\x82")})
			Rejects(
				[&]
				{
					modelScene.ApplyCommands(
						Json::array({{{"Operation", "SetModel"}, {"ID", modelID}, {"Asset", asset}}}));
				},
				"model command rejects malformed or non-portable asset path");
		Check(modelScene.Serialize() == modelCheckpoint, "invalid model edits preserve previous scene");
		for (const auto& model : {Json{{"Asset", "Valid.glb"}, {"Unknown", true}}, Json::array({"Valid.glb"}),
								  Json{{"Asset", nullptr}}, Json{{"Asset", "../Outside.glb"}}, Json::object()})
		{
			auto malformedModel = modelCheckpoint;
			malformedModel["Entities"][0]["Model"] = model;
			Rejects([&] { modelRestored.Deserialize(malformedModel); }, "scene rejects malformed model component");
		}
		Scene expectedModelRestored;
		expectedModelRestored.Load(modelPath);
		Check(modelRestored.Serialize() == expectedModelRestored.Serialize(),
			  "invalid model deserialization rolls back");
		Rejects(
			[&]
			{
				modelScene.ApplyCommands(
					Json::array({{{"Operation", "SetModel"}, {"ID", modelID}, {"Asset", "Replacement.gltf"}},
								 {{"Operation", "SetModel"}, {"ID", modelID}, {"Asset", "../Outside.glb"}}}));
			},
			"model batch rejects invalid final edit");
		Check(modelScene.Serialize() == modelCheckpoint, "model command batch rolls back all edits");
		auto modelPrefab = modelCheckpoint["Entities"][0];
		modelPrefab.erase("ID");
		auto prefabID = modelScene.SpawnPrefab(modelPrefab);
		Check(prefabID != modelID && modelScene.GetEntity(prefabID).ModelAsset == "ModelFixtures/Materials.glb",
			  "prefab preserves model with fresh entity identity");
		auto prefabCheckpoint = modelScene.Serialize();
		auto prefabWithID = prefabCheckpoint["Entities"][0];
		Rejects([&] { modelScene.SpawnPrefab(prefabWithID); }, "prefab rejects conflicting ID field");
		Check(modelScene.Serialize() == prefabCheckpoint, "rejected ID prefab leaves scene unchanged");
		modelPrefab["Model"]["Asset"] = "../Outside.glb";
		Rejects([&] { modelScene.SpawnPrefab(modelPrefab); }, "prefab rejects malformed model asset");
		Check(modelScene.Serialize() == prefabCheckpoint, "invalid model prefab rolls back entity and ID allocation");
		modelScene.ApplyCommands(Json::array({{{"Operation", "SetModel"}, {"ID", modelID}, {"Asset", ""}}}));
		Check(modelScene.GetEntity(modelID).ModelAsset.empty() &&
				  !modelScene.Serialize()["Entities"][0].contains("Model"),
			  "empty asset removes model component");
		for (const auto& asset : {"Models/House.GLB", "Models/House.gltf", "Models/\xe9\x9b\xaa.glb"})
		{
			modelScene.ApplyCommands(Json::array({{{"Operation", "SetModel"}, {"ID", modelID}, {"Asset", asset}}}));
			Check(modelScene.GetEntity(modelID).ModelAsset == asset,
				  "portable UTF-8 and supported glTF extensions accepted");
		}
		std::string longestModelPath;
		for (int segment = 0; segment < 4; segment++)
			longestModelPath += std::string(200, 'a') + '/';
		longestModelPath += std::string(216, 'b') + ".glb";
		modelScene.ApplyCommands(
			Json::array({{{"Operation", "SetModel"}, {"ID", modelID}, {"Asset", longestModelPath}}}));
		Check(modelScene.GetEntity(modelID).ModelAsset.size() == 1024, "model path exact byte limit accepted");
		Rejects([&] { ValidateModelAssetPath("a" + longestModelPath); }, "model path beyond byte limit rejected");
		Rejects([&] { ValidateModelAssetPath(std::string(252, 'a') + ".glb"); },
				"model path segment beyond portable byte limit rejected");
		Rejects([&] { ValidateModelAssetPath("COM\xc2\xb9.glb"); }, "Unicode Windows device names rejected");
		modelScene.GetEntity(modelID).ModelAsset = "../Unsafe.glb";
		Rejects([&] { modelScene.Serialize(); }, "serialization rejects directly mutated unsafe model reference");
		Scene featureScene;
		featureScene.Load(root / "Scenes/FeatureTest.bwscene");
		Check(featureScene.GetEntity(2).ModelAsset == "ModelFixtures/Materials.glb",
			  "feature scene contains editable model component");
		Check(featureScene.GetEntity(3).Physics.Enabled &&
				  featureScene.GetEntity(3).Physics.Shape == PhysicsShape::Capsule &&
				  featureScene.GetEntity(3).Physics.Motion == PhysicsMotion::Dynamic,
			  "feature scene contains editable physics component");
		auto physCheckpoint = featureScene.Serialize();
		featureScene.ApplyCommands(Json::array(
			{Json{{"Operation", "SetPhysics"},
				  {"ID", 2},
				  {"Physics", {{"Enabled", true}, {"Shape", "Sphere"}, {"Radius", 0.75}, {"Mass", 12.0}}}},
			 Json{{"Operation", "SetTransform"},
				  {"ID", 2},
				  {"Transform", {{"Translation", {4, 5, 6}}, {"Rotation", {0, 0, 0}}, {"Scale", {1, 1, 1}}}}}}));
		Check(featureScene.GetEntity(2).Physics.Enabled &&
				  featureScene.GetEntity(2).Physics.Shape == PhysicsShape::Sphere &&
				  featureScene.GetEntity(2).Transform.Translation.x == 4.0f,
			  "SetPhysics and SetTransform preserve each other");
		Rejects(
			[&]
			{
				featureScene.ApplyCommands(
					Json::array({{{"Operation", "SetPhysics"}, {"ID", 2}, {"Physics", {{"Shape", "TriangleSoup"}}}},
								 {{"Operation", "SetPhysics"}, {"ID", 2}, {"Physics", {{"Mass", 50.0}}}}}));
			},
			"unsupported physics shape rejects explicitly");
		Rejects(
			[&]
			{
				featureScene.ApplyCommands(
					Json::array({{{"Operation", "SetPhysics"}, {"ID", 2}, {"Physics", {{"Mass", -10.0}}}}}));
			},
			"negative physics mass rejected");
		featureScene.Deserialize(physCheckpoint);
		Check(featureScene.Serialize() == physCheckpoint, "physics scene roundtrip restores cleanly");
		SnowFieldConfig luaSnowConfig;
		luaSnowConfig.Resolution = 64;
		luaSnowConfig.WorldExtent = 64.0f;
		luaSnowConfig.BaseDepth = 0.5f;
		luaSnowConfig.MaxDepth = 1.5f;
		SnowField featureSnow(luaSnowConfig);
		std::ifstream scriptFile(root / "Scenes/FeatureTest.lua");
		std::string script((std::istreambuf_iterator<char>(scriptFile)), {});
		std::string error;
		ScriptRuntime runtime(featureScene, &featureSnow);
		Check(runtime.Execute(script, error), "entire Lua API feature scene: " + error);
		Check(featureSnow.GetActiveTrackCount() > 0 && featureSnow.Sample({0.0f, 0.0f}).EffectiveDepth < 0.5f &&
				  featureSnow.Sample({3.0f, 3.0f}).EffectiveDepth < 0.5f,
			  "Lua snow stamping and melting mutate bound snowfield");
		Check(featureScene.GetRenderSettings().WindVelocity.x == 4.5f &&
				  featureScene.GetRenderSettings().SnowfallIntensity == 0.85f,
			  "Lua wind and snowfall updates mutate render settings");
		Check(runtime.Update(0.25, error), "Lua update callback");
		auto ids = featureScene.GetEntities();
		Check(featureScene.GetEntity(ids.back()).Transform.Translation.x == 0.25f, "Lua update changes scene");
		Check(!runtime.Update(-1, error), "invalid timestep rejected");
		Check(!runtime.Update(std::numeric_limits<double>::quiet_NaN(), error), "NaN timestep rejected");
		for (const auto& source :
			 {"Engine.SetPosition(9999, 1, 2, 3)", "Engine.CreateEntity('rolled back'); error('failure')",
			  "Engine.CreateEntity(123)", "local id = Engine.CreateEntity('ok'); Engine.Rename(id, 456)",
			  "Engine.SetPosition(1, 0/0, 0, 0)", "Engine.CreateEntity('loop'); while true do end",
			  "Engine.CreateEntity('')", "local excessive = string.rep('a', 32 * 1024 * 1024)",
			  "Engine.SetScale(1, 0, 1, 1)", "Engine.SetRotation(1, 0/0, 0, 0)",
			  "Engine.StampSnow(0, 0, 1.5, 0.3); Engine.SetWind(4, 0, 0); error('snow rollback')",
			  "Engine.StampSnow(0, 0, 1.5, 0.3); Engine.StampSnow(0, 0, -1, 0.2)", "Engine.MeltSnow(0, 0, 1, -5, 0.5)",
			  "Engine.SetWind(500, 0, 0)", "Engine.SetSnowfall(-0.2)",
			  "Engine.StampSnow(0, 0, 1.5, 0.3); while true do end"})
		{
			Scene test = featureScene;
			SnowField testSnow = featureSnow.Snapshot();
			auto before = test.Serialize();
			auto beforeSnow = featureSnow.Snapshot();
			ScriptRuntime failing(test, &testSnow);
			Check(!failing.Execute(source, error), "Lua invalid input or instruction budget rejected");
			Check(test.Serialize() == before, "Lua error rolls back scene mutations");
			Check(testSnow == beforeSnow, "Lua error rolls back snowfield mutations");
			Check(!failing.Update(0.1, error), "faulted Lua runtime cannot continue");
		}
		for (const auto& missingSnowOp : {"Engine.SetWind(2, 0, 0); return Engine.GetSnowDepth(0, 0)",
										  "Engine.StampSnow(0, 0, 1, 0.2)", "Engine.MeltSnow(0, 0, 1, 10, 0.5)",
										  "Engine.GetSnowCompression(0, 0)", "Engine.GetSnowSpeedFactor(0, 0)"})
		{
			Scene test = featureScene;
			auto before = test.Serialize();
			ScriptRuntime noSnow(test);
			Check(!noSnow.Execute(missingSnowOp, error), "snow Lua call rejects cleanly without bound snowfield");
			Check(test.Serialize() == before, "missing snowfield error rolls back prior weather mutations");
		}
		Scene sandboxScene;
		ScriptRuntime sandbox(sandboxScene);
		Check(sandbox.Execute("assert(io == nil and os == nil and package == nil and debug == nil and dofile == nil "
							  "and loadfile == nil and pcall == nil and xpcall == nil)",
							  error),
			  "sandbox restricts host access and budget interception");
		Check(ScriptRuntime::GetAPI().size() == 21, "script API manifest");
		RenderSettings renderSettings;
		Check(RenderSettings::Deserialize(renderSettings.Serialize()).Serialize() == renderSettings.Serialize(),
			  "renderer settings roundtrip");
		Scene rendererScene;
		rendererScene.ApplyCommands(Json::array(
			{{{"Operation", "SetRendererSettings"}, {"Settings", {{"Exposure", 0.6}, {"Shadows", false}}}}}));
		Check(rendererScene.GetRenderSettings().Exposure == 0.6f && !rendererScene.GetRenderSettings().Shadows,
			  "agent renderer commands update actual settings");
		rendererScene.ApplyCommands(
			Json::array({{{"Operation", "SetRendererSettings"}, {"Settings", {{"Exposure", 0.8}}}}}));
		Check(rendererScene.GetRenderSettings().Exposure == 0.8f && !rendererScene.GetRenderSettings().Shadows,
			  "partial renderer commands preserve other scene preferences");
		auto rendererCheckpoint = rendererScene.Serialize();
		Rejects(
			[&]
			{
				rendererScene.ApplyCommands(
					Json::array({{{"Operation", "SetRendererSettings"}, {"Settings", {{"Exposure", 2}}}},
								 {{"Operation", "SetRendererSettings"}, {"Settings", {{"Exposure", -1}}}}}));
			},
			"renderer command batch rejects invalid exposure");
		Check(rendererScene.Serialize() == rendererCheckpoint, "renderer settings batch rolls back");
		Scene rendererRestored;
		rendererRestored.Deserialize(rendererCheckpoint);
		Check(rendererRestored.GetRenderSettings().Serialize() == rendererScene.GetRenderSettings().Serialize(),
			  "scene persists renderer settings");
		for (const auto& settings :
			 {Json{{"Exposure", 0}}, Json{{"Exposure", "one"}}, Json{{"Shadows", 1}}, Json{{"FogDensity", -1}},
			  Json{{"CameraSize", 0}}, Json{{"SunDirection", {0, 0, 0}}}, Json{{"SunDirection", {0, -1, 0}}},
			  Json{{"SunDirection", {1e38, 1e38, 0}}}, Json{{"SunDirection", {10000, .02, 0}}},
			  Json{{"SunRadiance", {-1, 1, 1}}}, Json{{"SnowCoverage", 1.5}}, Json{{"SnowfallIntensity", -0.1}},
			  Json{{"WindVelocity", {200, 0, 0}}}, Json{{"SnowSparkle", 3}}, Json{{"SnowSubsurface", -1}},
			  Json{{"Unknown", 1}}})
			Rejects([&] { RenderSettings::Deserialize(settings); }, "invalid renderer property rejected");
		Scene winterScene;
		winterScene.ApplyCommands(Json::array({{{"Operation", "SetRendererSettings"},
												{"Settings",
												 {{"SnowCoverage", 0.85},
												  {"SnowfallIntensity", 0.6},
												  {"WindVelocity", {5.5, -1.0, 2.0}},
												  {"SnowSparkle", 1.25},
												  {"SnowSubsurface", 0.9}}}},
											   {{"Operation", "SetSnowSettings"},
												{"Settings",
												 {{"Enabled", true},
												  {"Resolution", 256},
												  {"WorldExtent", 256.0},
												  {"BaseDepth", 0.55},
												  {"TracksAsset", "Tracks/Run.bwsn"}}}}}));
		Check(winterScene.GetRenderSettings().SnowCoverage == 0.85f &&
				  winterScene.GetRenderSettings().SnowfallIntensity == 0.6f &&
				  winterScene.GetRenderSettings().WindVelocity.x == 5.5f && winterScene.GetSnowSettings().Enabled &&
				  winterScene.GetSnowSettings().Resolution == 256 &&
				  winterScene.GetSnowSettings().TracksAsset == "Tracks/Run.bwsn",
			  "winter render and snowfield settings update via agent commands");
		auto winterSnapshot = winterScene.Serialize();
		Scene winterRestored;
		winterRestored.Deserialize(winterSnapshot);
		Check(winterRestored.Serialize() == winterSnapshot, "winter scene settings roundtrip");
		Rejects(
			[&]
			{
				winterScene.ApplyCommands(Json::array(
					{{{"Operation", "SetSnowSettings"}, {"Settings", {{"Resolution", 512}}}},
					 {{"Operation", "SetSnowSettings"}, {"Settings", {{"TracksAsset", "../Escape.bwsn"}}}}}));
			},
			"invalid snow tracks path rejects batch");
		Check(winterScene.Serialize() == winterSnapshot, "invalid snow settings batch rolls back");
		Rejects([&] { SceneSnowSettings::Deserialize(Json{{"Resolution", 128.5}}); },
				"fractional snow resolution rejected");
		Rejects([&] { SceneSnowSettings::Deserialize(Json{{"TracksAsset", "Tracks/Bad.json"}}); },
				"non-.bwsn snow tracks extension rejected");
		Rejects([&] { renderSettings.GetViewProjection(0); }, "camera rejects zero aspect");
		Rejects([&] { renderSettings.GetShadowMatrices(0); }, "shadow matrices reject invalid resolution");
		auto clip = renderSettings.GetViewProjection(1.6f) * glm::vec4(renderSettings.CameraTarget, 1);
		Check(std::abs(clip.x) < 0.00001f && std::abs(clip.y) < 0.00001f && clip.z > 0 && clip.z < 1,
			  "camera target projects to visible Vulkan depth");
		renderSettings.SunDirection = {0, 1, 0};
		auto cascades = renderSettings.GetShadowMatrices(2048);
		for (const auto& cascade : cascades)
		{
			auto center = cascade * glm::vec4(renderSettings.CameraTarget, 1);
			Check(std::isfinite(center.x) && std::isfinite(center.y) && center.z > 0 && center.z < 1,
				  "vertical sun produces finite visible cascade");
		}
		std::cout << "BELLWETHER_CORE " << s_Passed << " passed " << s_Failed << " failed\n";
		return s_Failed ? 1 : 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "TEST EXCEPTION: " << exception.what() << '\n';
		return 1;
	}
}
