#include "Bellwether/SnowField.h"
#include "Bellwether/SnowSandbox.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

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

int main()
{
	using namespace Bellwether;
	try
	{
		SnowFieldConfig config;
		config.Resolution = 64;
		config.WorldExtent = 64.0f;
		config.BaseDepth = 0.50f;
		config.MaxDepth = 1.50f;
		SnowField snow(config);

		auto virgin = snow.Sample({0.0f, 0.0f});
		Check(std::abs(virgin.BaseDepth - 0.50f) < 1e-4f && std::abs(virgin.EffectiveDepth - 0.50f) < 1e-4f &&
				  virgin.SpeedFactor < 0.75f,
			  "fresh deep snow reduces initial mobility");

		Check(!snow.StampCircle({200.0f, 200.0f}, 2.0f, 0.30f), "out-of-bounds stamp returns false without mutation");
		Check(snow.GetMutationCounter() == 0, "out-of-bounds stamp does not bump mutation counter");

		Check(snow.StampSegment({-8.0f, 0.0f}, {8.0f, 0.0f}, 1.5f, 0.35f, 0.40f),
			  "segment stamp creates continuous track");
		auto centerTrack = snow.Sample({0.0f, 0.0f});
		auto midTrack = snow.Sample({4.0f, 0.0f});
		auto rimSample = snow.Sample({0.0f, 1.85f});
		Check(centerTrack.Compression > 0.25f && centerTrack.EffectiveDepth < 0.25f,
			  "track center compresses and lowers effective snow depth");
		Check(std::abs(centerTrack.Compression - midTrack.Compression) < 0.03f,
			  "swept segment stamp is uniform along path");
		Check(centerTrack.SpeedFactor > virgin.SpeedFactor,
			  "packed trail improves walkable speed compared to deep powder");
		Check(rimSample.Displacement > 0.005f && rimSample.EffectiveDepth > virgin.EffectiveDepth,
			  "displaced snow forms a raised berm around the track");

		uint32_t tracksBefore = snow.GetActiveTrackCount();
		SnowField beforeRestamp = snow.Snapshot();
		snow.StampCircle({0.0f, 0.0f}, 1.5f, 0.20f, 0.40f);
		Check(snow.Sample({0.0f, 0.0f}).Compression >= centerTrack.Compression - 1e-5f &&
				  snow.Sample({0.0f, 1.85f}).Displacement <= rimSample.Displacement + 1e-4f,
			  "lighter restamp inside packed trail does not inflate berms");
		snow.Restore(beforeRestamp);
		Check(snow == beforeRestamp && snow.GetActiveTrackCount() == tracksBefore, "snapshot and restore are exact");

		// Heat source melts snow and builds temporary moisture
		Check(snow.ApplyHeatSource({-12.0f, 10.0f}, 3.5f, 0.28f, 1.5f), "heat source melts local snow");
		auto melted = snow.Sample({-12.0f, 10.0f});
		Check(melted.BaseDepth < virgin.BaseDepth - 0.1f && melted.Moisture > 0.05f,
			  "thermal melting reduces depth and adds moisture");

		// Cold snap refreezes wet melt into lower-friction surface ice
		snow.StepWeather(2.0f, 0.0f, {0.0f, 0.0f}, -18.0f);
		auto refrozen = snow.Sample({-12.0f, 10.0f});
		Check(refrozen.Ice > 0.01f && refrozen.FrictionFactor < virgin.FrictionFactor,
			  "sub-zero step freezes melt moisture into slick ice");

		// Blizzard drift & snowfall heals tracks deterministically
		uint32_t tracksAfterFreeze = snow.GetActiveTrackCount();
		snow.StepWeather(8.0f, 0.04f, {10.0f, 2.0f}, -8.0f);
		Check(snow.GetActiveTrackCount() < tracksAfterFreeze &&
				  snow.Sample({0.0f, 0.0f}).Compression < centerTrack.Compression,
			  "snowfall and wind drift gradually heal old tracks");

		// Heightfield slope & cliff shedding (including boundary edge spans)
		std::vector<float> cliffHeights(64 * 64, 0.0f);
		for (uint32_t z = 0; z < 64; z++)
			for (uint32_t x = 0; x < 64; x++)
				cliffHeights[size_t(z) * 64 + x] = x > 32 ? static_cast<float>(x - 32) * 3.0f : 0.0f;
		SnowField terrainSnow(config);
		terrainSnow.InitializeFromHeightfield(cliffHeights, 64, 0.75f, -50.0f);
		Check(terrainSnow.Sample({-16.0f, 0.0f}).BaseDepth > 0.45f &&
				  terrainSnow.Sample({16.0f, 0.0f}).BaseDepth < 0.15f &&
				  terrainSnow.Sample({31.9f, 0.0f}).BaseDepth < 0.15f,
			  "steep slopes including grid boundaries shed snow compared to flat terrain");

		// No-op weather step preserves MutationCounter
		const uint64_t mutBeforeNoop = snow.GetMutationCounter();
		snow.StepWeather(0.0f, 0.1f, {2.0f, 0.0f}, -6.0f);
		snow.StepWeather(1.0f, 0.0f, {0.0f, 0.0f}, -6.0f);
		Check(snow.GetMutationCounter() == mutBeforeNoop,
			  "zero-dt and calm zero-snowfall dry weather steps do not bump mutation counter");

		// Binary track persistence + corruption rollback
		auto trackPath = std::filesystem::current_path() / "Evidence/TestTracks.bwsn";
		snow.SaveTracks(trackPath);
		SnowField loaded;
		loaded.LoadTracks(trackPath);
		Check(loaded == snow, "binary track save and load preserves full state");

		auto bytes = snow.SerializeBinary();
		auto corrupted = bytes;
		corrupted[bytes.size() - 6] ^= 0xff;
		SnowField safeCopy = loaded;
		Rejects([&] { safeCopy.DeserializeBinary(corrupted); }, "corrupted track checksum rejects");
		Check(safeCopy == loaded, "failed track load rolls back cleanly");

		std::vector<uint8_t> rgba;
		snow.PackRGBA8(rgba);
		Check(rgba.size() == size_t(config.Resolution) * config.Resolution * 4, "RGBA8 GPU pack matches grid size");

		// Exported byte bridge compatibility (depth.u8 + ice.u8)
		std::vector<uint8_t> depthBytes(32 * 32, 128);
		std::vector<uint8_t> iceBytes(32 * 32, 200);
		SnowField bridgeSnow(config);
		bridgeSnow.LoadExportedBytes(depthBytes, iceBytes, 32);
		auto bridgeSample = bridgeSnow.Sample({0.0f, 0.0f});
		Check(std::abs(bridgeSample.Ice - 200.0f / 255.0f) < 0.01f && bridgeSample.BaseDepth > 0.7f,
			  "exported byte maps resample onto SnowField");

		// Explicit invalid input rejections
		Rejects([] { SnowField bad(SnowFieldConfig{.Resolution = 2}); }, "invalid low resolution rejected");
		Rejects([] { SnowField bad(SnowFieldConfig{.BaseDepth = 5.0f, .MaxDepth = 1.0f}); },
				"BaseDepth exceeding MaxDepth rejected");
		Rejects([&] { snow.StampCircle({std::numeric_limits<float>::quiet_NaN(), 0.0f}, 1.0f, 0.2f); },
				"NaN stamp position rejected");
		Rejects([&] { snow.StampCircle({0.0f, 0.0f}, 0.0f, 0.2f); }, "zero stamp radius rejected");
		Rejects([&] { snow.ApplyHeatSource({0.0f, 0.0f}, 1.0f, -5.0f, 1.0f); }, "negative heat rejected");
		Rejects([&] { snow.StepWeather(-1.0f, 0.1f, {0.0f, 0.0f}); }, "negative weather dt rejected");

		// Real-time SnowSandbox tests (all scenarios, snow-coupled sleds, melt/freeze, launch pooling, raycasting)
		Check(ParseSnowScenario("tracks") == SnowTestScenario::TracksAndPhysics, "ParseSnowScenario tracks");
		Check(ParseSnowScenario("melt") == SnowTestScenario::ThermalMeltAndIce, "ParseSnowScenario melt");
		Check(ParseSnowScenario("blizzard") == SnowTestScenario::BlizzardDrift, "ParseSnowScenario blizzard");
		Check(ParseSnowScenario("sparkle") == SnowTestScenario::SunnySparkle, "ParseSnowScenario sparkle");
		Rejects([] { ParseSnowScenario("invalid_mode"); }, "ParseSnowScenario rejects unknown scenario");

		SnowSandbox sandbox(SnowTestScenario::TracksAndPhysics);
		Check(sandbox.GetRenderAsset().Primitives.size() >= 3 && sandbox.GetRenderAsset().Placements.size() >= 10,
			  "SnowSandbox builds complete procedural alpine geometry");
		for (int i = 0; i < 90; ++i)
			sandbox.Step(1.0f / 60.0f);
		const EntityID powderId = sandbox.GetPrimaryPowderSled();
		const EntityID iceId = sandbox.GetPrimaryIceSled();
		Check(sandbox.GetScene().Exists(powderId) && sandbox.GetScene().Exists(iceId) &&
				  sandbox.GetScene().GetEntity(iceId).Transform.Translation.x >
					  sandbox.GetScene().GetEntity(powderId).Transform.Translation.x + 0.15f,
			  "SnowSandbox twin sled simulation demonstrates ice vs deep-powder drag");

		const uint64_t mutBeforeStamp = sandbox.GetSnowMutationCounter();
		Check(sandbox.StampStroke({-6.0f, 9.0f}, {6.0f, 9.0f}, 1.5f, 0.34f) &&
				  sandbox.GetSnowMutationCounter() > mutBeforeStamp,
			  "SnowSandbox StampStroke modifies SnowField and advances mutation counter");

		// Pool recycling across 10 launches (exceeding 8 dynamic user slots cleanly)
		EntityID firstLaunch = 0;
		EntityID tenthLaunch = 0;
		for (int launch = 0; launch < 10; ++launch)
		{
			EntityID spawned =
				sandbox.LaunchSled({-8.0f, -2.0f + float(launch) * 0.6f}, {10.0f, 0.0f}, (launch % 2) == 1);
			if (launch == 0)
				firstLaunch = spawned;
			tenthLaunch = spawned;
		}
		Check(tenthLaunch != 0 && tenthLaunch != firstLaunch && sandbox.GetScene().Exists(tenthLaunch),
			  "SnowSandbox LaunchSled recycles dynamic entity pool safely beyond 8 launches");

		const auto dynInstances = sandbox.GetDynamicInstances();
		bool allDynValid = !dynInstances.empty();
		for (const auto& inst : dynInstances)
		{
			if (inst.PlacementIndex >= sandbox.GetRenderAsset().Placements.size() ||
				std::abs(glm::determinant(glm::mat3(inst.Transform))) < 1e-6f)
				allDynValid = false;
		}
		Check(allDynValid, "SnowSandbox dynamic instance transforms stay valid and non-singular");

		// Melt & sub-zero freeze scenario
		sandbox.ResetScenario(SnowTestScenario::ThermalMeltAndIce);
		const glm::vec2 meltSpot(2.5f, 10.5f);
		const float preMeltDepth = sandbox.SampleAt(meltSpot).EffectiveDepth;
		Check(sandbox.MeltAt(meltSpot, 3.6f, 1.4f, 0.25f) &&
				  sandbox.SampleAt(meltSpot).EffectiveDepth < preMeltDepth,
			  "SnowSandbox MeltAt melts snow around target point");
		sandbox.TriggerColdSnapFreeze(2.5f);
		for (int i = 0; i < 120; ++i)
			sandbox.Step(1.0f / 60.0f);
		Check(sandbox.SampleAt(meltSpot).Ice > 0.20f && sandbox.GetAmbientCelsius() < -15.0f,
			  "SnowSandbox cold snap freezes wet snow into slick surface ice");

		// Blizzard drift & raycast ground queries
		sandbox.ResetScenario(SnowTestScenario::BlizzardDrift);
		sandbox.StampAt({0.0f, 4.0f}, 1.8f, 0.42f);
		const float carvedInBlizzard = sandbox.SampleAt({0.0f, 4.0f}).EffectiveDepth;
		for (int i = 0; i < 90; ++i)
			sandbox.Step(1.0f / 60.0f);
		Check(sandbox.SampleAt({0.0f, 4.0f}).EffectiveDepth > carvedInBlizzard + 0.01f,
			  "SnowSandbox BlizzardDrift snowfall heals carved tracks");

		sandbox.ResetScenario(SnowTestScenario::SunnySparkle);
		sandbox.CycleWindPreset(1);
		sandbox.CycleSparklePreset(1);
		Check(sandbox.RaycastGround({0.0f, 100.0f, 4.0f}, {0.0f, -200.0f, 0.0f}).has_value() &&
				  !sandbox.RaycastGround({200.0f, 100.0f, 200.0f}, {0.0f, -200.0f, 0.0f}).has_value(),
			  "SnowSandbox RaycastGround discriminates hits inside and outside alpine terrain");

		Rejects([&] { sandbox.Step(-0.05f); }, "SnowSandbox Step rejects negative dt");
		Rejects([&] { sandbox.Step(std::numeric_limits<float>::infinity()); }, "SnowSandbox Step rejects Inf dt");
		Rejects([&] { sandbox.TriggerColdSnapFreeze(-1.0f); }, "SnowSandbox TriggerColdSnapFreeze rejects <= 0s");
		Rejects([&] { sandbox.LaunchSled({std::numeric_limits<float>::quiet_NaN(), 0.0f}, {1.0f, 0.0f}); },
				"SnowSandbox LaunchSled rejects NaN origin");

		std::cout << "BELLWETHER_SNOW " << s_Passed << " passed " << s_Failed << " failed\n";
		return s_Failed ? 1 : 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "SNOW TEST EXCEPTION: " << exception.what() << '\n';
		return 1;
	}
}
