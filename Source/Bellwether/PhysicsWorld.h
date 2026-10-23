#pragma once

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <optional>

namespace Bellwether
{

	using PhysicsBodyHandle = uint64_t;

	enum class PhysicsShape
	{
		Box,
		Sphere
	};

	enum class PhysicsMotion
	{
		Static,
		Dynamic
	};

	struct PhysicsCollisionFilter
	{
		// Category is an index in [0, 7]; both bodies' masks must admit the other category.
		uint8_t Category = 0;
		uint8_t Mask = 0xff;
	};

	struct PhysicsBodySettings
	{
		PhysicsShape Shape = PhysicsShape::Box;
		PhysicsMotion Motion = PhysicsMotion::Static;
		glm::vec3 Position{0.0f};
		glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
		glm::vec3 HalfExtents{0.5f};
		float Radius = 0.5f;
		float Mass = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		glm::vec3 LinearVelocity{0.0f};
		PhysicsCollisionFilter Collision;
		bool AllowSleeping = true;
	};

	struct PhysicsBodyState
	{
		glm::vec3 Position{0.0f};
		glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
		glm::vec3 LinearVelocity{0.0f};
		glm::vec3 AngularVelocity{0.0f};
		bool Active = false;
	};

	struct PhysicsRayHit
	{
		PhysicsBodyHandle Body = 0;
		glm::vec3 Position{0.0f};
		glm::vec3 Normal{0.0f};
		float Fraction = 0.0f;
	};

	struct PhysicsWorldSettings
	{
		glm::vec3 Gravity{0.0f, -9.81f, 0.0f};
		double FixedStepSeconds = 1.0 / 60.0;
		uint32_t MaximumBodies = 4096;
		uint32_t MaximumBodyPairs = 32768;
		uint32_t MaximumContactConstraints = 16384;
		uint32_t MaximumSubsteps = 8;
	};

	// Single owner thread; all methods except destruction enforce the creating thread.
	// No entity, Lua, rendering or save integration is implied by this independent world.
	class PhysicsWorld
	{
	  public:
		explicit PhysicsWorld(const PhysicsWorldSettings& settings = {});
		~PhysicsWorld();
		PhysicsWorld(const PhysicsWorld&) = delete;
		PhysicsWorld& operator=(const PhysicsWorld&) = delete;
		PhysicsWorld(PhysicsWorld&&) = delete;
		PhysicsWorld& operator=(PhysicsWorld&&) = delete;

		PhysicsBodyHandle CreateBody(const PhysicsBodySettings& settings);
		void DestroyBody(PhysicsBodyHandle handle);
		bool Exists(PhysicsBodyHandle handle) const;
		size_t GetBodyCount() const;
		PhysicsBodyState GetBodyState(PhysicsBodyHandle handle) const;
		void SetBodyTransform(PhysicsBodyHandle handle, const glm::vec3& position, const glm::quat& rotation);
		void SetLinearVelocity(PhysicsBodyHandle handle, const glm::vec3& velocity);
		void AddImpulse(PhysicsBodyHandle handle, const glm::vec3& impulse);

		// Always advances exactly one configured tick. Variable time is never passed to Jolt.
		void StepFixed();
		// Retains fractional time; rejects catch-up exceeding MaximumSubsteps before changing state.
		uint32_t Advance(double elapsedSeconds);
		uint64_t GetTickCount() const;
		double GetInterpolationAlpha() const;
		const PhysicsWorldSettings& GetSettings() const;

		// displacement is the finite ray segment, not a normalized direction or an infinite ray.
		std::optional<PhysicsRayHit> Raycast(const glm::vec3& origin, const glm::vec3& displacement,
											 uint8_t categoryMask = 0xff, PhysicsBodyHandle ignoredBody = 0) const;

	  private:
		class Implementation;
		std::unique_ptr<Implementation> m_Implementation;
	};

} // namespace Bellwether
