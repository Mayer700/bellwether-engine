#pragma once

#include "PhysicsWorld.h"
#include "Scene.h"
#include "SnowField.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <map>
#include <vector>

namespace Bellwether
{

	class SceneSimulation
	{
	  public:
		explicit SceneSimulation(Scene& scene, const PhysicsWorldSettings& physicsSettings = {});

		void RebuildFromScene();
		void SetTerrainHeightfield(const std::vector<float>& heights, uint32_t sampleCount, float worldExtent,
								   float slopeCutoff = 0.68f, float timberlineHeight = -1000.0f);

		void StepFixed();
		uint32_t Step(double elapsedSeconds);

		bool ApplyEntityImpulse(EntityID id, const glm::vec3& impulse);
		void SetEntityVelocity(EntityID id, const glm::vec3& velocity);

		bool HasPhysicsBody(EntityID id) const;
		PhysicsBodyHandle GetPhysicsBody(EntityID id) const;

		SnowField& GetSnowField()
		{
			return m_SnowField;
		}
		const SnowField& GetSnowField() const
		{
			return m_SnowField;
		}
		PhysicsWorld& GetPhysicsWorld()
		{
			return m_Physics;
		}
		const PhysicsWorld& GetPhysicsWorld() const
		{
			return m_Physics;
		}

		static glm::quat EulerToQuaternion(const glm::vec3& eulerXYZ);
		static glm::vec3 QuaternionToEuler(const glm::quat& q);

	  private:
		struct SyncedEntityState
		{
			PhysicsBodyHandle Handle = 0;
			TransformComponent Transform;
			PhysicsComponent Physics;
			glm::vec2 PreviousXZ{0.0f};
		};

		void SyncSceneEntities();
		PhysicsBodyHandle CreateEntityBody(const Entity& entity);

		Scene& m_Scene;
		PhysicsWorld m_Physics;
		SnowField m_SnowField;
		SceneSnowSettings m_SyncedSnowSettings;
		std::map<EntityID, SyncedEntityState> m_SyncedEntities;
		PhysicsBodyHandle m_TerrainHandle = 0;
		double m_AccumulatedSeconds = 0.0;
	};

} // namespace Bellwether
