#pragma once

#include "GltfImporter.h"
#include "Scene.h"
#include "SceneSimulation.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Bellwether
{

	enum class SnowTestScenario
	{
		TracksAndPhysics = 0,
		ThermalMeltAndIce = 1,
		BlizzardDrift = 2,
		SunnySparkle = 3
	};

	SnowTestScenario ParseSnowScenario(const std::string& name);
	const char* GetSnowScenarioName(SnowTestScenario scenario);

	struct DynamicInstanceUpdate
	{
		size_t PlacementIndex = 0;
		glm::mat4 Transform{1.0f};
		bool Visible = true;
	};

	class SnowSandbox
	{
	  public:
		explicit SnowSandbox(SnowTestScenario scenario = SnowTestScenario::TracksAndPhysics);

		void ResetScenario(SnowTestScenario scenario);
		void Step(float deltaSeconds);

		bool StampAt(glm::vec2 worldXZ, float radius = 1.6f, float pressureDepth = 0.34f);
		bool StampStroke(glm::vec2 startXZ, glm::vec2 endXZ, float radius = 1.5f, float pressureDepth = 0.34f);
		bool MeltAt(glm::vec2 worldXZ, float radius = 2.6f, float heatPower = 22.0f, float dt = 0.25f);
		void TriggerColdSnapFreeze(float durationSeconds = 2.5f);
		EntityID LaunchSled(glm::vec2 originXZ, glm::vec2 velocityXZ, bool boulder = false);

		void SetRenderSettings(const RenderSettings& settings);
		const RenderSettings& GetRenderSettings() const;
		void SetSnowfallIntensity(float intensity);
		void CycleWindPreset(int direction);
		void CycleSparklePreset(int direction);
		void SetProbePosition(glm::vec2 worldXZ);
		void SetProbeVisible(bool visible);

		std::optional<glm::vec3> RaycastGround(const glm::vec3& rayOrigin, const glm::vec3& rayDisplacement) const;
		float EvaluateTerrainHeight(float x, float z) const;
		SnowSample SampleAt(glm::vec2 worldXZ) const;

		SnowTestScenario GetScenario() const
		{
			return m_Scenario;
		}
		const char* GetScenarioName() const
		{
			return GetSnowScenarioName(m_Scenario);
		}
		float GetAmbientCelsius() const;
		uint64_t GetSnowMutationCounter() const;
		float GetElapsedSeconds() const
		{
			return m_ElapsedSeconds;
		}
		const Scene& GetScene() const
		{
			return m_Scene;
		}
		const SnowField& GetSnowField() const;
		const SceneSimulation& GetSimulation() const
		{
			return *m_Simulation;
		}
		const ModelAsset& GetRenderAsset() const
		{
			return m_RenderAsset;
		}
		std::vector<DynamicInstanceUpdate> GetDynamicInstances() const;
		EntityID GetPrimaryPowderSled() const
		{
			return m_PrimaryPowderSled;
		}
		EntityID GetPrimaryIceSled() const
		{
			return m_PrimaryIceSled;
		}
		static constexpr float GetWorldExtent()
		{
			return s_WorldExtent;
		}

	  private:
		void BuildStaticGeometry();
		void ConfigureScenarioState(SnowTestScenario scenario);

		static constexpr float s_WorldExtent = 64.0f;
		static constexpr uint32_t s_HeightGrid = 65;
		static constexpr uint32_t s_RenderGroundGrid = 193;
		static constexpr uint32_t s_SnowResolution = 192;
		static constexpr size_t s_MaxUserSleds = 8;

		struct DynamicSlot
		{
			EntityID Entity = 0;
			size_t PlacementIndex = 0;
			bool IsBoulder = false;
		};

		SnowTestScenario m_Scenario = SnowTestScenario::TracksAndPhysics;
		Scene m_Scene;
		std::unique_ptr<SceneSimulation> m_Simulation;
		std::vector<float> m_TerrainHeights;
		ModelAsset m_RenderAsset;
		std::vector<DynamicSlot> m_DynamicSlots;
		size_t m_NextUserSlot = 0;
		size_t m_ProbePlacementIndex = 0;
		glm::vec2 m_ProbeXZ{0.0f, 0.0f};
		bool m_ProbeVisible = true;
		EntityID m_PrimaryPowderSled = 0;
		EntityID m_PrimaryIceSled = 0;
		float m_ElapsedSeconds = 0.0f;
		float m_ColdSnapTimer = 0.0f;
		float m_BaseAmbientCelsius = -6.0f;
		size_t m_WindPresetIndex = 1;
		size_t m_SparklePresetIndex = 1;
	};

} // namespace Bellwether
