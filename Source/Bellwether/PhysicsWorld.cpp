#include "PhysicsWorld.h"
#include "SnowField.h"

// Jolt.h must precede every other Jolt header.
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/FPFlushDenormals.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <atomic>
#include <cfenv>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

namespace Bellwether
{
	namespace
	{
		constexpr JPH::ObjectLayer s_MovingBit = 1u << 11;
		constexpr float s_MaximumPosition = 100000.0f;
		constexpr float s_MaximumVelocity = 500.0f;
		std::atomic<PhysicsBodyHandle> s_NextBodyHandle{1};

		void JoltTrace(const char* format, ...)
		{
			std::va_list arguments;
			va_start(arguments, format);
			std::vfprintf(stderr, format, arguments);
			va_end(arguments);
			std::fputc('\n', stderr);
		}

#ifdef JPH_ENABLE_ASSERTS
		thread_local bool s_HandlingUpdateResult = false;

		class UpdateResultScope
		{
		  public:
			UpdateResultScope() : m_Previous(s_HandlingUpdateResult)
			{
				s_HandlingUpdateResult = true;
			}
			~UpdateResultScope()
			{
				s_HandlingUpdateResult = m_Previous;
			}

		  private:
			bool m_Previous;
		};

		bool JoltAssert(const char* expression, const char* message, const char* file, JPH::uint line)
		{
			std::fprintf(stderr, "Jolt assertion at %s:%u: %s %s\n", file, line, expression,
						 message == nullptr ? "" : message);
			// Jolt 5.5 asserts its documented, returned capacity error before returning it.
			// Defer only that assertion while this thread's Step checks and faults on the result.
			if (s_HandlingUpdateResult && std::strcmp(expression, "errors == EPhysicsUpdateError::None") == 0)
				return false;
			return true;
		}
#endif

		class JoltRegistration
		{
		  public:
			JoltRegistration()
			{
				if (JPH::Factory::sInstance != nullptr)
					throw std::runtime_error("Bellwether physics requires ownership of Jolt's process registration");
				JPH::RegisterDefaultAllocator();
				m_Factory = std::make_unique<JPH::Factory>();
				JPH::Factory::sInstance = m_Factory.get();
				JPH::Trace = JoltTrace;
#ifdef JPH_ENABLE_ASSERTS
				JPH::AssertFailed = JoltAssert;
#endif
				try
				{
					JPH::RegisterTypes();
				}
				catch (...)
				{
					JPH::UnregisterTypes();
					JPH::Factory::sInstance = nullptr;
					RestoreCallbacks();
					throw;
				}
			}

			~JoltRegistration()
			{
				JPH::UnregisterTypes();
				JPH::Factory::sInstance = nullptr;
				RestoreCallbacks();
			}

		  private:
			void RestoreCallbacks()
			{
				JPH::Trace = m_PreviousTrace;
#ifdef JPH_ENABLE_ASSERTS
				JPH::AssertFailed = m_PreviousAssert;
#endif
			}

			JPH::TraceFunction m_PreviousTrace = JPH::Trace;
#ifdef JPH_ENABLE_ASSERTS
			JPH::AssertFailedFunction m_PreviousAssert = JPH::AssertFailed;
#endif
			std::unique_ptr<JPH::Factory> m_Factory;
		};

		void EnsureJoltRegistration()
		{
			// Function-local static initialization is synchronized; worlds do not replace global state.
			static JoltRegistration registration;
		}

		void ValidateVector(const glm::vec3& value, float maximum, const char* name)
		{
			if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z) ||
				std::abs(value.x) > maximum || std::abs(value.y) > maximum || std::abs(value.z) > maximum)
				throw std::invalid_argument(std::string(name) + " must have finite, bounded components");
		}

		void ValidateRotation(const glm::quat& rotation)
		{
			if (!std::isfinite(rotation.w) || !std::isfinite(rotation.x) || !std::isfinite(rotation.y) ||
				!std::isfinite(rotation.z) || std::abs(glm::dot(rotation, rotation) - 1.0f) > 0.00001f)
				throw std::invalid_argument("Physics rotation must be a finite unit quaternion");
		}

		void ValidateRange(float value, float minimum, float maximum, const char* name)
		{
			if (!std::isfinite(value) || value < minimum || value > maximum)
				throw std::invalid_argument(std::string(name) + " is outside the supported range");
		}

		void ValidateSettings(const PhysicsWorldSettings& settings)
		{
			ValidateVector(settings.Gravity, 1000.0f, "Physics gravity");
			if (!std::isfinite(settings.FixedStepSeconds) || settings.FixedStepSeconds < 1.0 / 240.0 ||
				settings.FixedStepSeconds > 1.0 / 15.0)
				throw std::invalid_argument("Physics fixed step must be between 1/240 and 1/15 seconds");
			if (settings.MaximumBodies < 1 || settings.MaximumBodies > 65536 || settings.MaximumBodyPairs < 4 ||
				settings.MaximumBodyPairs > 1048576 || settings.MaximumContactConstraints < 4 ||
				settings.MaximumContactConstraints > 1048576 || settings.MaximumSubsteps < 1 ||
				settings.MaximumSubsteps > 64)
				throw std::invalid_argument("Physics capacity or substep limit is outside the supported range");
		}

		void ValidateBody(const PhysicsBodySettings& settings)
		{
			if (settings.Motion != PhysicsMotion::Static && settings.Motion != PhysicsMotion::Dynamic)
				throw std::invalid_argument("Unsupported physics motion type");
			if (settings.Shape != PhysicsShape::Box && settings.Shape != PhysicsShape::Sphere &&
				settings.Shape != PhysicsShape::Capsule)
				throw std::invalid_argument("Unsupported physics shape");
			ValidateVector(settings.Position, s_MaximumPosition, "Physics position");
			ValidateRotation(settings.Rotation);
			ValidateVector(settings.LinearVelocity, s_MaximumVelocity, "Physics linear velocity");
			if (glm::dot(settings.LinearVelocity, settings.LinearVelocity) > s_MaximumVelocity * s_MaximumVelocity)
				throw std::invalid_argument("Physics linear velocity exceeds 500 metres per second");
			if (settings.Motion == PhysicsMotion::Static && settings.LinearVelocity != glm::vec3(0.0f))
				throw std::invalid_argument("Static bodies cannot have linear velocity");
			ValidateVector(settings.HalfExtents, 10000.0f, "Physics box dimensions");
			if (settings.HalfExtents.x < 0.01f || settings.HalfExtents.y < 0.01f || settings.HalfExtents.z < 0.01f)
				throw std::invalid_argument("Physics box half extents must be at least 0.01 metres");
			ValidateRange(settings.Radius, 0.01f, 10000.0f, "Physics shape radius");
			ValidateRange(settings.HalfHeight, 0.01f, 10000.0f, "Physics capsule half-height");
			ValidateRange(settings.Mass, 0.001f, 1000000.0f, "Physics mass");
			ValidateRange(settings.Friction, 0.0f, 1.0f, "Physics friction");
			ValidateRange(settings.Restitution, 0.0f, 1.0f, "Physics restitution");
			if (settings.Collision.Category > 7)
				throw std::invalid_argument("Physics collision category must be an index from 0 to 7");
		}

		void ValidateHeightField(const PhysicsHeightFieldSettings& settings)
		{
			if (settings.SampleCount < 4 || settings.SampleCount > 2048)
				throw std::invalid_argument("Heightfield SampleCount must be between 4 and 2048");
			if (settings.Heights.size() != static_cast<size_t>(settings.SampleCount) * settings.SampleCount)
				throw std::invalid_argument("Heightfield Heights size does not match SampleCount * SampleCount");
			ValidateRange(settings.WorldExtent, 1.0f, 100000.0f, "Heightfield WorldExtent");
			ValidateVector(settings.Position, s_MaximumPosition, "Heightfield position");
			for (float height : settings.Heights)
				ValidateRange(height, -10000.0f, 10000.0f, "Heightfield elevation");
			ValidateRange(settings.Friction, 0.0f, 1.0f, "Heightfield friction");
			ValidateRange(settings.Restitution, 0.0f, 1.0f, "Heightfield restitution");
			if (settings.Collision.Category > 7)
				throw std::invalid_argument("Heightfield collision category must be an index from 0 to 7");
		}

		PhysicsBodyHandle AllocateHandle()
		{
			auto handle = s_NextBodyHandle.load(std::memory_order_relaxed);
			for (;;)
			{
				if (handle == std::numeric_limits<PhysicsBodyHandle>::max())
					throw std::overflow_error("Physics body handle space exhausted");
				if (s_NextBodyHandle.compare_exchange_weak(handle, handle + 1, std::memory_order_relaxed))
					return handle;
			}
		}

		JPH::ObjectLayer EncodeLayer(const PhysicsBodySettings& settings)
		{
			return static_cast<JPH::ObjectLayer>(settings.Collision.Category | (settings.Collision.Mask << 3) |
												 (settings.Motion == PhysicsMotion::Dynamic ? s_MovingBit : 0));
		}

		class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface
		{
		  public:
			JPH::uint GetNumBroadPhaseLayers() const override
			{
				return 2;
			}
			JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
			{
				return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>((layer & s_MovingBit) != 0));
			}
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
			const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
			{
				return layer == JPH::BroadPhaseLayer(0) ? "Static" : "Dynamic";
			}
#endif
		};

		class BroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
		{
		  public:
			bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhaseLayer) const override
			{
				return (layer & s_MovingBit) != 0 || broadPhaseLayer == JPH::BroadPhaseLayer(1);
			}
		};

		class ObjectFilter final : public JPH::ObjectLayerPairFilter
		{
		  public:
			bool ShouldCollide(JPH::ObjectLayer first, JPH::ObjectLayer second) const override
			{
				if (((first | second) & s_MovingBit) == 0)
					return false;
				const auto firstCategory = first & 7u;
				const auto secondCategory = second & 7u;
				return ((first >> 3) & (1u << secondCategory)) != 0 && ((second >> 3) & (1u << firstCategory)) != 0;
			}
		};

		class RayLayerFilter final : public JPH::ObjectLayerFilter
		{
		  public:
			explicit RayLayerFilter(uint8_t mask) : m_Mask(mask)
			{
			}
			bool ShouldCollide(JPH::ObjectLayer layer) const override
			{
				return (m_Mask & (1u << (layer & 7u))) != 0;
			}

		  private:
			uint8_t m_Mask;
		};

		JPH::Vec3 ToJolt(const glm::vec3& value)
		{
			return {value.x, value.y, value.z};
		}

		JPH::Quat ToJolt(const glm::quat& value)
		{
			return {value.x, value.y, value.z, value.w};
		}

		glm::vec3 FromJolt(JPH::Vec3Arg value)
		{
			return {value.GetX(), value.GetY(), value.GetZ()};
		}

	} // namespace

	class PhysicsWorld::Implementation
	{
	  public:
		struct BodyRecord
		{
			JPH::BodyID ID;
			PhysicsMotion Motion;
		};

		explicit Implementation(const PhysicsWorldSettings& settings)
			: Settings(settings), OwnerThread(std::this_thread::get_id()), Jobs(JPH::cMaxPhysicsJobs)
		{
			System.Init(settings.MaximumBodies, 0, settings.MaximumBodyPairs, settings.MaximumContactConstraints,
						Layers, BroadFilter, PairFilter);
			System.SetGravity(ToJolt(settings.Gravity));
		}

		~Implementation()
		{
			for (const auto& [handle, record] : Bodies)
			{
				(void)handle;
				System.GetBodyInterface().RemoveBody(record.ID);
				System.GetBodyInterface().DestroyBody(record.ID);
			}
		}

		void CheckAccess() const
		{
			if (std::this_thread::get_id() != OwnerThread)
				throw std::logic_error("PhysicsWorld must be used on its creating thread");
			if (Faulted)
				throw std::runtime_error("PhysicsWorld is faulted after a simulation error; recreate the world");
		}

		const BodyRecord& GetBody(PhysicsBodyHandle handle) const
		{
			CheckAccess();
			const auto record = Bodies.find(handle);
			if (record == Bodies.end())
				throw std::invalid_argument("Unknown or destroyed physics body handle");
			return record->second;
		}

		JPH::AABox GetBounds(const JPH::BodyID& bodyID) const
		{
			JPH::BodyLockRead lock(System.GetBodyLockInterface(), bodyID);
			if (!lock.Succeeded())
				throw std::runtime_error("Physics body bounds could not be locked");
			return lock.GetBody().GetWorldSpaceBounds();
		}

		void WakeNeighbors(JPH::AABox bounds)
		{
			// Include resting contact gaps and speculative contacts around the changed shape.
			bounds.ExpandBy(JPH::Vec3::sReplicate(0.1f));
			JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> collector;
			System.GetBroadPhaseQuery().CollideAABox(bounds, collector,
													 JPH::SpecifiedBroadPhaseLayerFilter(JPH::BroadPhaseLayer(1)), {});
			// Broadphase traversal order is not a simulation contract. Normalize activation order
			// and recheck actual bounds rather than relying on widened broadphase bounds.
			std::sort(collector.mHits.begin(), collector.mHits.end(),
					  [](const JPH::BodyID& first, const JPH::BodyID& second)
					  { return first.GetIndexAndSequenceNumber() < second.GetIndexAndSequenceNumber(); });
			for (const auto& bodyID : collector.mHits)
				if (GetBounds(bodyID).Overlaps(bounds))
					System.GetBodyInterface().ActivateBody(bodyID);
		}

		void Step()
		{
			CheckAccess();
			if (std::fegetround() != FE_TONEAREST)
				throw std::logic_error("Physics simulation requires round-to-nearest floating point mode");
			if (TickCount == std::numeric_limits<uint64_t>::max())
				throw std::overflow_error("Physics tick counter exhausted");
			JPH::FPFlushDenormals flushDenormals;
#ifdef JPH_ENABLE_ASSERTS
			UpdateResultScope updateResultScope;
#endif
			const auto error = System.Update(static_cast<float>(Settings.FixedStepSeconds), 1, &Allocator, &Jobs);
			if (error != JPH::EPhysicsUpdateError::None)
			{
				Faulted = true;
				throw std::runtime_error("Jolt solver capacity exceeded (error bits " +
										 std::to_string(static_cast<uint32_t>(error)) +
										 "); simulation was not rolled back; recreate with higher capacities");
			}
			try
			{
				for (const auto& [handle, record] : Bodies)
				{
					(void)handle;
					if (record.Motion != PhysicsMotion::Dynamic)
						continue;
					const auto& bodies = System.GetBodyInterface();
					ValidateVector(FromJolt(JPH::Vec3(bodies.GetPosition(record.ID))), s_MaximumPosition,
								   "Simulated physics position");
					ValidateVector(FromJolt(bodies.GetLinearVelocity(record.ID)), s_MaximumVelocity,
								   "Simulated physics linear velocity");
					ValidateVector(FromJolt(bodies.GetAngularVelocity(record.ID)), 1000.0f,
								   "Simulated physics angular velocity");
					const auto rotation = bodies.GetRotation(record.ID);
					ValidateRotation({rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ()});
				}
			}
			catch (const std::invalid_argument& exception)
			{
				Faulted = true;
				throw std::runtime_error(std::string("Physics state left the supported finite world envelope: ") +
										 exception.what() + "; simulation was not rolled back; recreate the world");
			}
			++TickCount;
		}

		PhysicsWorldSettings Settings;
		std::thread::id OwnerThread;
		BroadPhaseLayers Layers;
		BroadPhaseFilter BroadFilter;
		ObjectFilter PairFilter;
		JPH::TempAllocatorMalloc Allocator;
		JPH::JobSystemSingleThreaded Jobs;
		JPH::PhysicsSystem System;
		std::map<PhysicsBodyHandle, BodyRecord> Bodies;
		uint64_t TickCount = 0;
		double AccumulatedSeconds = 0.0;
		bool Faulted = false;
	};

	PhysicsWorld::PhysicsWorld(const PhysicsWorldSettings& settings)
	{
		ValidateSettings(settings);
		EnsureJoltRegistration();
		m_Implementation = std::make_unique<Implementation>(settings);
	}

	PhysicsWorld::~PhysicsWorld() = default;

	PhysicsBodyHandle PhysicsWorld::CreateBody(const PhysicsBodySettings& settings)
	{
		auto& world = *m_Implementation;
		world.CheckAccess();
		ValidateBody(settings);
		if (world.Bodies.size() >= world.Settings.MaximumBodies)
			throw std::runtime_error("Physics body capacity reached; increase MaximumBodies");
		JPH::ShapeRefC shape;
		if (settings.Shape == PhysicsShape::Box)
		{
			const float convexRadius = std::min(
				0.05f, std::min({settings.HalfExtents.x, settings.HalfExtents.y, settings.HalfExtents.z}) * 0.1f);
			shape = new JPH::BoxShape(ToJolt(settings.HalfExtents), convexRadius);
		}
		else if (settings.Shape == PhysicsShape::Capsule)
			shape = new JPH::CapsuleShape(settings.HalfHeight, settings.Radius);
		else
			shape = new JPH::SphereShape(settings.Radius);
		const auto handle = AllocateHandle();
		const auto record = world.Bodies.emplace(handle, Implementation::BodyRecord{{}, settings.Motion}).first;
		auto& bodies = world.System.GetBodyInterface();
		JPH::Body* body = nullptr;
		bool added = false;
		try
		{
			JPH::BodyCreationSettings creation(shape, JPH::RVec3(ToJolt(settings.Position)), ToJolt(settings.Rotation),
											   settings.Motion == PhysicsMotion::Static ? JPH::EMotionType::Static
																						: JPH::EMotionType::Dynamic,
											   EncodeLayer(settings));
			creation.mUserData = handle;
			creation.mFriction = settings.Friction;
			creation.mRestitution = settings.Restitution;
			creation.mLinearVelocity = ToJolt(settings.LinearVelocity);
			creation.mAllowSleeping = settings.AllowSleeping;
			creation.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
			creation.mMassPropertiesOverride.mMass = settings.Mass;
			body = bodies.CreateBody(creation);
			if (body == nullptr)
				throw std::runtime_error("Jolt could not allocate a rigid body");
			record->second.ID = body->GetID();
			bodies.AddBody(body->GetID(), settings.Motion == PhysicsMotion::Dynamic ? JPH::EActivation::Activate
																					: JPH::EActivation::DontActivate);
			added = true;
			world.WakeNeighbors(world.GetBounds(body->GetID()));
		}
		catch (...)
		{
			if (body != nullptr)
			{
				if (added)
					bodies.RemoveBody(body->GetID());
				bodies.DestroyBody(body->GetID());
			}
			world.Bodies.erase(record);
			throw;
		}
		return handle;
	}

	PhysicsBodyHandle PhysicsWorld::CreateHeightField(const PhysicsHeightFieldSettings& settings)
	{
		auto& world = *m_Implementation;
		world.CheckAccess();
		ValidateHeightField(settings);
		if (world.Bodies.size() >= world.Settings.MaximumBodies)
			throw std::runtime_error("Physics body capacity reached; increase MaximumBodies");

		const float cellStep = settings.WorldExtent / static_cast<float>(settings.SampleCount - 1);
		const JPH::Vec3 offset(-settings.WorldExtent * 0.5f, 0.0f, -settings.WorldExtent * 0.5f);
		const JPH::Vec3 scale(cellStep, 1.0f, cellStep);
		JPH::HeightFieldShapeSettings hfSettings(settings.Heights.data(), offset, scale, settings.SampleCount);
		const auto shapeResult = hfSettings.Create();
		if (shapeResult.HasError())
			throw std::runtime_error(std::string("Jolt HeightField creation failed: ") +
									 shapeResult.GetError().c_str());

		const auto handle = AllocateHandle();
		const auto record = world.Bodies.emplace(handle, Implementation::BodyRecord{{}, PhysicsMotion::Static}).first;
		auto& bodies = world.System.GetBodyInterface();
		JPH::Body* body = nullptr;
		bool added = false;
		try
		{
			const auto layer =
				static_cast<JPH::ObjectLayer>(settings.Collision.Category | (settings.Collision.Mask << 3));
			JPH::BodyCreationSettings creation(shapeResult.Get(), JPH::RVec3(ToJolt(settings.Position)),
											   JPH::Quat::sIdentity(), JPH::EMotionType::Static, layer);
			creation.mUserData = handle;
			creation.mFriction = settings.Friction;
			creation.mRestitution = settings.Restitution;
			body = bodies.CreateBody(creation);
			if (body == nullptr)
				throw std::runtime_error("Jolt could not allocate a heightfield body");
			record->second.ID = body->GetID();
			bodies.AddBody(body->GetID(), JPH::EActivation::DontActivate);
			added = true;
			world.WakeNeighbors(world.GetBounds(body->GetID()));
		}
		catch (...)
		{
			if (body != nullptr)
			{
				if (added)
					bodies.RemoveBody(body->GetID());
				bodies.DestroyBody(body->GetID());
			}
			world.Bodies.erase(record);
			throw;
		}
		return handle;
	}

	void PhysicsWorld::DestroyBody(PhysicsBodyHandle handle)
	{
		auto& world = *m_Implementation;
		const auto bodyID = world.GetBody(handle).ID;
		const auto oldBounds = world.GetBounds(bodyID);
		auto& bodies = world.System.GetBodyInterface();
		bodies.RemoveBody(bodyID);
		bodies.DestroyBody(bodyID);
		world.Bodies.erase(handle);
		world.WakeNeighbors(oldBounds);
	}

	bool PhysicsWorld::Exists(PhysicsBodyHandle handle) const
	{
		m_Implementation->CheckAccess();
		return m_Implementation->Bodies.contains(handle);
	}

	size_t PhysicsWorld::GetBodyCount() const
	{
		m_Implementation->CheckAccess();
		return m_Implementation->Bodies.size();
	}

	PhysicsBodyState PhysicsWorld::GetBodyState(PhysicsBodyHandle handle) const
	{
		const auto& world = *m_Implementation;
		const auto& record = world.GetBody(handle);
		const auto& bodies = world.System.GetBodyInterface();
		JPH::RVec3 position;
		JPH::Quat rotation;
		bodies.GetPositionAndRotation(record.ID, position, rotation);
		return {
			FromJolt(JPH::Vec3(position)),
			{rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ()},
			record.Motion == PhysicsMotion::Dynamic ? FromJolt(bodies.GetLinearVelocity(record.ID)) : glm::vec3(0.0f),
			record.Motion == PhysicsMotion::Dynamic ? FromJolt(bodies.GetAngularVelocity(record.ID)) : glm::vec3(0.0f),
			bodies.IsActive(record.ID)};
	}

	void PhysicsWorld::SetBodyTransform(PhysicsBodyHandle handle, const glm::vec3& position, const glm::quat& rotation)
	{
		auto& world = *m_Implementation;
		const auto& record = world.GetBody(handle);
		ValidateVector(position, s_MaximumPosition, "Physics position");
		ValidateRotation(rotation);
		const auto oldBounds = world.GetBounds(record.ID);
		world.System.GetBodyInterface().SetPositionAndRotation(
			record.ID, JPH::RVec3(ToJolt(position)), ToJolt(rotation),
			record.Motion == PhysicsMotion::Dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
		world.WakeNeighbors(oldBounds);
		world.WakeNeighbors(world.GetBounds(record.ID));
	}

	void PhysicsWorld::SetLinearVelocity(PhysicsBodyHandle handle, const glm::vec3& velocity)
	{
		auto& world = *m_Implementation;
		const auto& record = world.GetBody(handle);
		if (record.Motion != PhysicsMotion::Dynamic)
			throw std::invalid_argument("Only dynamic bodies accept linear velocity");
		ValidateVector(velocity, s_MaximumVelocity, "Physics linear velocity");
		if (glm::dot(velocity, velocity) > s_MaximumVelocity * s_MaximumVelocity)
			throw std::invalid_argument("Physics linear velocity exceeds 500 metres per second");
		world.System.GetBodyInterface().SetLinearVelocity(record.ID, ToJolt(velocity));
	}

	void PhysicsWorld::AddImpulse(PhysicsBodyHandle handle, const glm::vec3& impulse)
	{
		auto& world = *m_Implementation;
		const auto& record = world.GetBody(handle);
		if (record.Motion != PhysicsMotion::Dynamic)
			throw std::invalid_argument("Only dynamic bodies accept impulses");
		ValidateVector(impulse, 1000000.0f, "Physics impulse");
		{
			JPH::BodyLockRead lock(world.System.GetBodyLockInterface(), record.ID);
			if (!lock.Succeeded())
				throw std::runtime_error("Physics impulse target could not be locked");
			const auto& body = lock.GetBody();
			const auto resultingVelocity =
				glm::dvec3(FromJolt(body.GetLinearVelocity())) +
				glm::dvec3(impulse) * static_cast<double>(body.GetMotionProperties()->GetInverseMass());
			if (glm::dot(resultingVelocity, resultingVelocity) > s_MaximumVelocity * s_MaximumVelocity)
				throw std::invalid_argument(
					"Physics impulse would exceed the body's 500 metres per second speed limit");
		}
		world.System.GetBodyInterface().AddImpulse(record.ID, ToJolt(impulse));
	}

	bool PhysicsWorld::ApplySnowInteraction(PhysicsBodyHandle handle, SnowField& snow, const glm::vec2& previousXZ,
											float contactRadius, float dragStrength, float pressureDepth)
	{
		auto& world = *m_Implementation;
		const auto& record = world.GetBody(handle);
		if (record.Motion != PhysicsMotion::Dynamic)
			throw std::invalid_argument("Snow physics interaction requires a dynamic rigid body");
		if (!std::isfinite(previousXZ.x) || !std::isfinite(previousXZ.y))
			throw std::invalid_argument("Snow previousXZ position must be finite");
		ValidateRange(contactRadius, 0.02f, 50.0f, "Snow contact radius");
		ValidateRange(dragStrength, 0.0f, 100.0f, "Snow drag strength");
		ValidateRange(pressureDepth, 0.0f, 10.0f, "Snow pressure depth");

		const auto state = GetBodyState(handle);
		const auto groundCheck = Raycast(state.Position, {0.0f, -(contactRadius + 1.25f), 0.0f}, 0xff, handle);
		if (!groundCheck.has_value())
			return false;

		const glm::vec2 currentXZ(state.Position.x, state.Position.z);
		const auto sample = snow.Sample(currentXZ);
		bool stamped = false;
		if (pressureDepth > 0.0f && sample.BaseDepth > 0.01f)
		{
			const float appliedPressure = std::min(sample.BaseDepth, pressureDepth);
			stamped = snow.StampSegment(previousXZ, currentXZ, contactRadius, appliedPressure);
		}

		const float snowResistance = (1.0f - sample.SpeedFactor) * (1.0f - sample.Ice * 0.75f);
		const float dt = static_cast<float>(world.Settings.FixedStepSeconds);
		const float factor = 1.0f / (1.0f + std::max(0.0f, dragStrength * snowResistance * dt));
		if (factor < 0.9999f)
		{
			glm::vec3 updatedVelocity(state.LinearVelocity.x * factor, state.LinearVelocity.y,
									  state.LinearVelocity.z * factor);
			if (glm::dot(updatedVelocity, updatedVelocity) > 1e-8f)
				SetLinearVelocity(handle, updatedVelocity);
		}
		return stamped;
	}

	void PhysicsWorld::StepFixed()
	{
		m_Implementation->Step();
	}

	uint32_t PhysicsWorld::Advance(double elapsedSeconds)
	{
		auto& world = *m_Implementation;
		world.CheckAccess();
		const double step = world.Settings.FixedStepSeconds;
		if (!std::isfinite(elapsedSeconds) || elapsedSeconds < 0.0 ||
			elapsedSeconds > step * world.Settings.MaximumSubsteps)
			throw std::invalid_argument(
				"Physics elapsed time must be finite, nonnegative and within the substep budget");
		const double accumulated = world.AccumulatedSeconds + elapsedSeconds;
		const auto substeps = static_cast<uint32_t>(std::floor((accumulated + step * 1.0e-10) / step));
		if (substeps > world.Settings.MaximumSubsteps)
			throw std::invalid_argument("Physics catch-up exceeds the substep budget; no time was consumed");
		// Check before committing the accumulator so invalid host FPU state is transactional too.
		if (std::fegetround() != FE_TONEAREST)
			throw std::logic_error("Physics simulation requires round-to-nearest floating point mode");
		world.AccumulatedSeconds = accumulated;
		for (uint32_t tick = 0; tick < substeps; ++tick)
		{
			world.Step();
			world.AccumulatedSeconds = std::max(0.0, world.AccumulatedSeconds - step);
		}
		return substeps;
	}

	uint64_t PhysicsWorld::GetTickCount() const
	{
		m_Implementation->CheckAccess();
		return m_Implementation->TickCount;
	}

	double PhysicsWorld::GetInterpolationAlpha() const
	{
		m_Implementation->CheckAccess();
		return m_Implementation->AccumulatedSeconds / m_Implementation->Settings.FixedStepSeconds;
	}

	const PhysicsWorldSettings& PhysicsWorld::GetSettings() const
	{
		m_Implementation->CheckAccess();
		return m_Implementation->Settings;
	}

	std::optional<PhysicsRayHit> PhysicsWorld::Raycast(const glm::vec3& origin, const glm::vec3& displacement,
													   uint8_t categoryMask, PhysicsBodyHandle ignoredBody) const
	{
		const auto& world = *m_Implementation;
		world.CheckAccess();
		ValidateVector(origin, s_MaximumPosition, "Physics ray origin");
		ValidateVector(displacement, s_MaximumPosition, "Physics ray displacement");
		if (glm::dot(displacement, displacement) < 1.0e-12f)
			throw std::invalid_argument("Physics ray displacement must not be zero");
		JPH::IgnoreSingleBodyFilter ignoredFilter(ignoredBody == 0 ? JPH::BodyID() : world.GetBody(ignoredBody).ID);
		const JPH::RRayCast ray{JPH::RVec3(ToJolt(origin)), ToJolt(displacement)};
		JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
		world.System.GetNarrowPhaseQuery().CastRay(ray, {}, collector, {}, RayLayerFilter(categoryMask), ignoredFilter);
		if (collector.mHits.empty())
			return std::nullopt;
		JPH::RayCastResult hit;
		PhysicsBodyHandle handle = 0;
		for (const auto& candidate : collector.mHits)
		{
			JPH::BodyLockRead candidateLock(world.System.GetBodyLockInterface(), candidate.mBodyID);
			if (!candidateLock.Succeeded())
				throw std::runtime_error("Physics ray candidate could not be locked");
			const auto candidateHandle = candidateLock.GetBody().GetUserData();
			if (!world.Bodies.contains(candidateHandle))
				throw std::runtime_error("Physics ray returned an unowned body");
			if (handle == 0 || candidate.mFraction < hit.mFraction ||
				(candidate.mFraction == hit.mFraction && candidateHandle < handle))
			{
				hit = candidate;
				handle = candidateHandle;
			}
		}
		JPH::BodyLockRead lock(world.System.GetBodyLockInterface(), hit.mBodyID);
		if (!lock.Succeeded())
			throw std::runtime_error("Physics ray hit could not be locked");
		const auto point = ray.GetPointOnRay(hit.mFraction);
		return PhysicsRayHit{handle, FromJolt(JPH::Vec3(point)),
							 FromJolt(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point)),
							 hit.mFraction};
	}

} // namespace Bellwether
