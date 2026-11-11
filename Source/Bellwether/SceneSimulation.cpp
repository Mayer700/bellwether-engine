#include "SceneSimulation.h"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>

namespace Bellwether
{

	glm::quat SceneSimulation::EulerToQuaternion(const glm::vec3& eulerXYZ)
	{
		glm::quat qz = glm::angleAxis(eulerXYZ.z, glm::vec3(0.0f, 0.0f, 1.0f));
		glm::quat qy = glm::angleAxis(eulerXYZ.y, glm::vec3(0.0f, 1.0f, 0.0f));
		glm::quat qx = glm::angleAxis(eulerXYZ.x, glm::vec3(1.0f, 0.0f, 0.0f));
		return glm::normalize(qz * qy * qx);
	}

	glm::vec3 SceneSimulation::QuaternionToEuler(const glm::quat& q)
	{
		glm::quat nq = glm::normalize(q);
		// Recover intrinsic Z * Y * X Euler angles matching TransformComponent::GetMatrix() (Rz * Ry * Rx).
		glm::mat3 m = glm::mat3_cast(nq);
		// In column-major m[col][row]: m[0][2] = -sin(y)
		float sy = std::clamp(-m[0][2], -1.0f, 1.0f);
		float ry = std::asin(sy);
		float cy = std::cos(ry);
		float rx = 0.0f;
		float rz = 0.0f;
		if (std::abs(cy) > 1e-4f)
		{
			rx = std::atan2(m[1][2], m[2][2]);
			rz = std::atan2(m[0][1], m[0][0]);
		}
		else
		{
			rx = std::atan2(-m[2][1], m[1][1]);
			rz = 0.0f;
		}
		return {rx, ry, rz};
	}

	SceneSimulation::SceneSimulation(Scene& scene, const PhysicsWorldSettings& physicsSettings)
		: m_Scene(scene), m_Physics(physicsSettings)
	{
		RebuildFromScene();
	}

	PhysicsBodyHandle SceneSimulation::CreateEntityBody(const Entity& entity)
	{
		entity.Physics.Validate();
		PhysicsBodySettings settings;
		settings.Shape = entity.Physics.Shape;
		settings.Motion = entity.Physics.Motion;
		settings.Position = entity.Transform.Translation;
		settings.Rotation = EulerToQuaternion(entity.Transform.Rotation);

		glm::vec3 absScale = glm::max(glm::abs(entity.Transform.Scale), glm::vec3(0.01f));
		settings.HalfExtents = glm::clamp(entity.Physics.HalfExtents * absScale, glm::vec3(0.01f), glm::vec3(10000.0f));
		float radialScale = std::max(absScale.x, absScale.z);
		settings.Radius = std::clamp(entity.Physics.Radius * radialScale, 0.01f, 10000.0f);
		settings.HalfHeight = std::clamp(entity.Physics.HalfHeight * absScale.y, 0.01f, 10000.0f);
		settings.Mass = entity.Physics.Mass;
		settings.Friction = entity.Physics.Friction;
		settings.Restitution = entity.Physics.Restitution;
		return m_Physics.CreateBody(settings);
	}

	void SceneSimulation::RebuildFromScene()
	{
		m_Scene.GetSnowSettings().Validate();
		for (const auto& [entityID, bodyHandle] : m_Bodies)
		{
			(void)entityID;
			if (m_Physics.Exists(bodyHandle))
				m_Physics.DestroyBody(bodyHandle);
		}
		m_Bodies.clear();
		m_PreviousXZ.clear();

		m_SnowField.Reset(m_Scene.GetSnowSettings().ToFieldConfig());

		for (EntityID id : m_Scene.GetEntities())
		{
			const Entity& entity = m_Scene.GetEntity(id);
			if (!entity.Physics.Enabled)
				continue;
			PhysicsBodyHandle handle = CreateEntityBody(entity);
			m_Bodies.emplace(id, handle);
			m_PreviousXZ.emplace(id, glm::vec2(entity.Transform.Translation.x, entity.Transform.Translation.z));
		}
	}

	void SceneSimulation::SetTerrainHeightfield(const std::vector<float>& heights, uint32_t sampleCount,
												float worldExtent, float slopeCutoff, float timberlineHeight)
	{
		if (m_TerrainHandle != 0 && m_Physics.Exists(m_TerrainHandle))
		{
			m_Physics.DestroyBody(m_TerrainHandle);
			m_TerrainHandle = 0;
		}
		PhysicsHeightFieldSettings terrain;
		terrain.SampleCount = sampleCount;
		terrain.WorldExtent = worldExtent;
		terrain.Heights = heights;
		terrain.Friction = 0.65f;
		m_TerrainHandle = m_Physics.CreateHeightField(terrain);

		if (m_Scene.GetSnowSettings().Enabled)
			m_SnowField.InitializeFromHeightfield(heights, sampleCount, slopeCutoff, timberlineHeight);
	}

	void SceneSimulation::SyncSceneEntities()
	{
		for (auto it = m_Bodies.begin(); it != m_Bodies.end();)
		{
			EntityID id = it->first;
			if (!m_Scene.Exists(id) || !m_Scene.GetEntity(id).Physics.Enabled)
			{
				if (m_Physics.Exists(it->second))
					m_Physics.DestroyBody(it->second);
				m_PreviousXZ.erase(id);
				it = m_Bodies.erase(it);
			}
			else
			{
				++it;
			}
		}

		for (EntityID id : m_Scene.GetEntities())
		{
			const Entity& entity = m_Scene.GetEntity(id);
			if (entity.Physics.Enabled && !m_Bodies.contains(id))
			{
				PhysicsBodyHandle handle = CreateEntityBody(entity);
				m_Bodies.emplace(id, handle);
				m_PreviousXZ.emplace(id, glm::vec2(entity.Transform.Translation.x, entity.Transform.Translation.z));
			}
		}
	}

	void SceneSimulation::StepFixed()
	{
		SyncSceneEntities();

		const auto& snowSettings = m_Scene.GetSnowSettings();
		const auto& renderSettings = m_Scene.GetRenderSettings();
		const float dt = static_cast<float>(m_Physics.GetSettings().FixedStepSeconds);

		if (snowSettings.Enabled)
		{
			const float snowfallRate = renderSettings.SnowfallIntensity * 0.025f;
			const glm::vec2 wind(renderSettings.WindVelocity.x, renderSettings.WindVelocity.z);
			m_SnowField.StepWeather(dt, snowfallRate, wind, -6.0f);

			for (const auto& [id, bodyHandle] : m_Bodies)
			{
				const Entity& entity = m_Scene.GetEntity(id);
				if (entity.Physics.Motion != PhysicsMotion::Dynamic || !entity.Physics.SnowTracking)
					continue;
				float radius = entity.Physics.Shape == PhysicsShape::Box
								   ? std::max(entity.Physics.HalfExtents.x, entity.Physics.HalfExtents.z)
								   : entity.Physics.Radius;
				radius = std::max(
					0.05f, radius * std::max(std::abs(entity.Transform.Scale.x), std::abs(entity.Transform.Scale.z)));
				float pressure = std::clamp(entity.Physics.Mass * 0.012f, 0.08f, snowSettings.MaxDepth * 0.6f);
				glm::vec2 prevXZ = m_PreviousXZ[id];
				m_Physics.ApplySnowInteraction(bodyHandle, m_SnowField, prevXZ, radius, entity.Physics.SnowDrag,
											   pressure);
			}
		}

		m_Physics.StepFixed();

		for (const auto& [id, bodyHandle] : m_Bodies)
		{
			Entity& entity = m_Scene.GetEntity(id);
			const auto state = m_Physics.GetBodyState(bodyHandle);
			m_PreviousXZ[id] = {state.Position.x, state.Position.z};
			if (entity.Physics.Motion == PhysicsMotion::Dynamic)
			{
				entity.Transform.Translation = state.Position;
				entity.Transform.Rotation = QuaternionToEuler(state.Rotation);
			}
		}
	}

	uint32_t SceneSimulation::Step(double elapsedSeconds)
	{
		const auto& settings = m_Physics.GetSettings();
		const double step = settings.FixedStepSeconds;
		if (!std::isfinite(elapsedSeconds) || elapsedSeconds < 0.0 || elapsedSeconds > step * settings.MaximumSubsteps)
			throw std::invalid_argument(
				"SceneSimulation elapsed time must be finite, nonnegative and within the substep budget");
		const double accumulated = m_AccumulatedSeconds + elapsedSeconds;
		const auto substeps = static_cast<uint32_t>(std::floor((accumulated + step * 1.0e-10) / step));
		if (substeps > settings.MaximumSubsteps)
			throw std::invalid_argument("SceneSimulation catch-up exceeds the substep budget");
		if (std::fegetround() != FE_TONEAREST)
			throw std::logic_error("SceneSimulation requires round-to-nearest floating point mode");

		m_AccumulatedSeconds = accumulated;
		for (uint32_t tick = 0; tick < substeps; ++tick)
		{
			StepFixed();
			m_AccumulatedSeconds = std::max(0.0, m_AccumulatedSeconds - step);
		}
		return substeps;
	}

	bool SceneSimulation::ApplyEntityImpulse(EntityID id, const glm::vec3& impulse)
	{
		auto it = m_Bodies.find(id);
		if (it == m_Bodies.end())
			return false;
		m_Physics.AddImpulse(it->second, impulse);
		return true;
	}

	void SceneSimulation::SetEntityVelocity(EntityID id, const glm::vec3& velocity)
	{
		auto it = m_Bodies.find(id);
		if (it == m_Bodies.end())
			throw std::invalid_argument("Entity does not have an active physics body");
		m_Physics.SetLinearVelocity(it->second, velocity);
	}

	bool SceneSimulation::HasPhysicsBody(EntityID id) const
	{
		return m_Bodies.contains(id) && m_Physics.Exists(m_Bodies.at(id));
	}

	PhysicsBodyHandle SceneSimulation::GetPhysicsBody(EntityID id) const
	{
		return m_Bodies.at(id);
	}

} // namespace Bellwether
