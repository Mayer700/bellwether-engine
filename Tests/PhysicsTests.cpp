#include "Bellwether/PhysicsWorld.h"

#include <cfenv>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
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

	bool Near(float first, float second, float tolerance = 0.025f)
	{
		return std::abs(first - second) < tolerance;
	}

	Bellwether::PhysicsBodySettings Ground()
	{
		Bellwether::PhysicsBodySettings settings;
		settings.Position = {0.0f, -0.5f, 0.0f};
		settings.HalfExtents = {20.0f, 0.5f, 20.0f};
		return settings;
	}

	Bellwether::PhysicsBodySettings Ball(const glm::vec3& position)
	{
		Bellwether::PhysicsBodySettings settings;
		settings.Shape = Bellwether::PhysicsShape::Sphere;
		settings.Motion = Bellwether::PhysicsMotion::Dynamic;
		settings.Position = position;
		settings.Collision.Category = 1;
		return settings;
	}

	void TestCollisionAndQueries()
	{
		using namespace Bellwether;
		PhysicsWorld world;
		const auto ground = world.CreateBody(Ground());
		const auto ball = world.CreateBody(Ball({-2.0f, 4.0f, 0.0f}));
		auto boxSettings = Ball({2.0f, 4.0f, 0.0f});
		boxSettings.Shape = PhysicsShape::Box;
		const auto box = world.CreateBody(boxSettings);
		Check(world.GetBodyCount() == 3 && world.Exists(ball) && !world.Exists(0), "body registration and null handle");
		world.StepFixed();
		Check(world.GetBodyState(ball).Position.y < 4.0f && world.GetBodyState(ball).LinearVelocity.y < 0.0f,
			  "dynamic sphere falls under real gravity");
		for (int tick = 1; tick < 300; ++tick)
			world.StepFixed();
		Check(Near(world.GetBodyState(ball).Position.y, 0.5f), "sphere settles against static box floor");
		Check(Near(world.GetBodyState(box).Position.y, 0.5f), "dynamic box settles against static box floor");
		Check(!world.GetBodyState(ball).Active && !world.GetBodyState(box).Active, "resting dynamic bodies sleep");
		Check(world.GetBodyState(ground).Position == Ground().Position &&
				  world.GetBodyState(ground).LinearVelocity == glm::vec3(0.0f),
			  "static body retains pose and zero velocity");
		const auto sphereHit = world.Raycast({-2.0f, 6.0f, 0.0f}, {0.0f, -10.0f, 0.0f});
		Check(sphereHit && sphereHit->Body == ball && Near(sphereHit->Position.y, 1.0f) &&
				  Near(sphereHit->Fraction, 0.5f) && sphereHit->Normal.y > 0.99f,
			  "closest ray hit carries stable body, world point, fraction and surface normal");
		const auto staticHit = world.Raycast({-2.0f, 6.0f, 0.0f}, {0.0f, -10.0f, 0.0f}, 1u << 0);
		Check(staticHit && staticHit->Body == ground && Near(staticHit->Position.y, 0.0f),
			  "ray category mask skips dynamic category");
		const auto ignoredHit = world.Raycast({-2.0f, 6.0f, 0.0f}, {0.0f, -10.0f, 0.0f}, 0xff, ball);
		Check(ignoredHit && ignoredHit->Body == ground, "ray ignores requested body without slicing the filter");
		const auto insideHit = world.Raycast(world.GetBodyState(ball).Position, {1.0f, 0.0f, 0.0f});
		Check(insideHit && insideHit->Body == ball && insideHit->Fraction == 0.0f &&
				  std::isfinite(insideHit->Normal.x) && std::isfinite(insideHit->Normal.y),
			  "ray beginning inside a sphere returns the solid-body zero-fraction hit");
		Check(!world.Raycast({100.0f, 6.0f, 0.0f}, {0.0f, -10.0f, 0.0f}), "ray miss returns no hit");
		Check(!world.Raycast({0.0f, 6.0f, 0.0f}, {0.0f, -10.0f, 0.0f}, 0), "empty query mask returns no hit");
		Check(!world.Raycast({-2.0f, 6.0f, 0.0f}, {0.0f, -2.0f, 0.0f}), "ray respects finite segment length");
		world.AddImpulse(ball, {0.0f, 3.0f, 0.0f});
		Check(world.GetBodyState(ball).Active && world.GetBodyState(ball).LinearVelocity.y > 2.9f,
			  "impulse wakes sleeping body and changes velocity");
		world.SetLinearVelocity(ball, {1.0f, 0.0f, 0.0f});
		Check(world.GetBodyState(ball).LinearVelocity == glm::vec3(1.0f, 0.0f, 0.0f),
			  "linear velocity setter changes the actual rigid body");
		const auto rotation = glm::angleAxis(0.7f, glm::vec3(0.0f, 1.0f, 0.0f));
		world.SetBodyTransform(box, {8.0f, 5.0f, 0.0f}, rotation);
		const auto transformed = world.GetBodyState(box);
		Check(transformed.Position == glm::vec3(8.0f, 5.0f, 0.0f) &&
				  std::abs(glm::dot(transformed.Rotation, rotation)) > 0.9999f,
			  "body teleport and unit rotation reach the physics state");
		world.DestroyBody(ball);
		Check(!world.Exists(ball) && world.GetBodyCount() == 2, "body destruction removes physics ownership");
		Rejects([&] { world.GetBodyState(ball); }, "stale body query rejected");
		Rejects([&] { world.DestroyBody(ball); }, "double body destruction rejected");
		Rejects([&] { world.SetLinearVelocity(ball, {0.0f, 0.0f, 0.0f}); }, "stale mutation rejected");
		Rejects([&] { world.AddImpulse(ball, {0.0f, 0.0f, 0.0f}); }, "stale impulse rejected");
		Rejects([&] { world.SetBodyTransform(ball, {}, {}); }, "stale transform rejected");
		Rejects([&] { world.Raycast({}, {1.0f, 0.0f, 0.0f}, 0xff, ball); }, "stale ignored ray body rejected");
		const auto replacement = world.CreateBody(Ball({-2.0f, 4.0f, 0.0f}));
		Check(replacement != ball && !world.Exists(ball), "recycled Jolt slots never alias stable handles");
		const auto replacementHit = world.Raycast({-2.0f, 6.0f, 0.0f}, {0.0f, -10.0f, 0.0f});
		Check(replacementHit && replacementHit->Body == replacement, "ray returns replacement handle after slot reuse");
		PhysicsWorld other;
		const auto otherBody = other.CreateBody(Ground());
		Check(otherBody != ground && !world.Exists(otherBody) && !other.Exists(ground),
			  "body handles cannot alias worlds");
		Rejects([&] { other.DestroyBody(ground); }, "cross-world mutation rejected");
		other.StepFixed();
		world.StepFixed();
		Check(other.GetTickCount() == 1 && world.GetTickCount() == 301, "simultaneous worlds simulate independently");
		PhysicsWorld equalHits;
		const auto older = equalHits.CreateBody(Ground());
		const auto newer = equalHits.CreateBody(Ground());
		const auto equalHit = equalHits.Raycast({0.0f, 4.0f, 0.0f}, {0.0f, -8.0f, 0.0f});
		Check(equalHit && equalHit->Body == older, "equal-fraction ray ties choose the oldest stable body handle");
		equalHits.DestroyBody(older);
		equalHits.CreateBody(Ground());
		const auto recycledHit = equalHits.Raycast({0.0f, 4.0f, 0.0f}, {0.0f, -8.0f, 0.0f});
		Check(recycledHit && recycledHit->Body == newer, "ray tie ordering survives backend body-slot recycling");
	}

	void TestCollisionFiltering()
	{
		using namespace Bellwether;
		PhysicsWorld world;
		auto floorSettings = Ground();
		floorSettings.Collision.Mask = 1u << 1;
		world.CreateBody(floorSettings);
		const auto colliding = world.CreateBody(Ball({-2.0f, 2.0f, 0.0f}));
		auto ignoredSettings = Ball({2.0f, 2.0f, 0.0f});
		ignoredSettings.Collision.Category = 2;
		const auto ignoredByFloor = world.CreateBody(ignoredSettings);
		ignoredSettings.Position = {6.0f, 2.0f, 0.0f};
		ignoredSettings.Collision.Category = 1;
		ignoredSettings.Collision.Mask = 0;
		const auto ignoresFloor = world.CreateBody(ignoredSettings);
		for (int tick = 0; tick < 120; ++tick)
			world.StepFixed();
		Check(Near(world.GetBodyState(colliding).Position.y, 0.5f), "mutual collision masks admit matching body");
		Check(world.GetBodyState(ignoredByFloor).Position.y < -5.0f, "static collision mask rejects dynamic category");
		Check(world.GetBodyState(ignoresFloor).Position.y < -5.0f, "dynamic collision mask rejects static category");

		PhysicsWorld stacked;
		stacked.CreateBody(Ground());
		auto lower = Ball({0.0f, 0.5f, 0.0f});
		lower.Shape = PhysicsShape::Box;
		const auto lowerID = stacked.CreateBody(lower);
		const auto upperID = stacked.CreateBody(Ball({0.0f, 3.0f, 0.0f}));
		for (int tick = 0; tick < 300; ++tick)
			stacked.StepFixed();
		Check(Near(stacked.GetBodyState(lowerID).Position.y, 0.5f) &&
				  Near(stacked.GetBodyState(upperID).Position.y, 1.5f),
			  "dynamic sphere collides with another dynamic box");

		PhysicsWorld highCategory;
		auto highFloor = Ground();
		highFloor.Collision.Category = 7;
		highFloor.Collision.Mask = 1u << 6;
		const auto highGround = highCategory.CreateBody(highFloor);
		auto highBall = Ball({0.0f, 2.0f, 0.0f});
		highBall.Collision.Category = 6;
		highBall.Collision.Mask = 1u << 7;
		const auto highBody = highCategory.CreateBody(highBall);
		for (int tick = 0; tick < 180; ++tick)
			highCategory.StepFixed();
		Check(Near(highCategory.GetBodyState(highBody).Position.y, 0.5f),
			  "highest collision category and mask bits participate in simulation");
		const auto highHit = highCategory.Raycast({0.0f, 4.0f, 0.0f}, {0.0f, -8.0f, 0.0f}, 1u << 7);
		Check(highHit && highHit->Body == highGround, "highest ray category bit queries the selected body");
		highCategory.SetBodyTransform(highGround, {10.0f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f});
		const auto movedHit = highCategory.Raycast({25.0f, 4.0f, 0.0f}, {0.0f, -8.0f, 0.0f}, 1u << 7);
		Check(movedHit && movedHit->Body == highGround, "moving a static body updates its broadphase bounds");

		PhysicsWorldSettings zeroGravity;
		zeroGravity.Gravity = glm::vec3(0.0f);
		PhysicsWorld massWorld(zeroGravity);
		auto heavy = Ball({0.0f, 0.0f, 0.0f});
		heavy.Mass = 2.0f;
		const auto heavyBody = massWorld.CreateBody(heavy);
		massWorld.AddImpulse(heavyBody, {2.0f, 0.0f, 0.0f});
		Check(Near(massWorld.GetBodyState(heavyBody).LinearVelocity.x, 1.0f, 0.0001f),
			  "configured mass sets real impulse response");
		Rejects([&] { massWorld.AddImpulse(heavyBody, {1000.0f, 0.0f, 0.0f}); },
				"impulse exceeding body speed limit rejected before Jolt can clamp it");
		Check(Near(massWorld.GetBodyState(heavyBody).LinearVelocity.x, 1.0f, 0.0001f),
			  "rejected impulse preserves velocity");
		massWorld.StepFixed();
		Check(massWorld.GetBodyState(heavyBody).Position.x > 0.0f &&
				  massWorld.GetBodyState(heavyBody).Position.y == 0.0f,
			  "custom zero gravity is used during actual simulation");
	}

	void TestFixedTimeAndReplay()
	{
		using namespace Bellwether;
		PhysicsWorld first;
		PhysicsWorld second;
		first.CreateBody(Ground());
		second.CreateBody(Ground());
		const auto firstBall = first.CreateBody(Ball({0.0f, 8.0f, 0.0f}));
		const auto secondBall = second.CreateBody(Ball({0.0f, 8.0f, 0.0f}));
		const double step = first.GetSettings().FixedStepSeconds;
		Check(first.Advance(step * 0.5) == 0 && first.GetTickCount() == 0 &&
				  std::abs(first.GetInterpolationAlpha() - 0.5) < 1.0e-9,
			  "sub-tick frame retains time without simulating");
		Check(first.Advance(step * 0.5) == 1 && first.GetTickCount() == 1 && first.GetInterpolationAlpha() < 1.0e-9,
			  "two half frames advance exactly one fixed tick");
		second.StepFixed();
		Check(first.Advance(0.0) == 0 && first.GetTickCount() == 1, "zero elapsed time does not simulate");
		bool identical = true;
		for (int tick = 1; tick < 300; ++tick)
		{
			first.Advance(step);
			second.StepFixed();
			const auto stateA = first.GetBodyState(firstBall);
			const auto stateB = second.GetBodyState(secondBall);
			identical = identical && stateA.Position == stateB.Position && stateA.Rotation == stateB.Rotation &&
						stateA.LinearVelocity == stateB.LinearVelocity &&
						stateA.AngularVelocity == stateB.AngularVelocity && stateA.Active == stateB.Active;
		}
		Check(identical, "same-binary ordered replay has identical body state on every fixed tick");
		Check(first.GetTickCount() == 300 && second.GetTickCount() == 300, "fixed time tick counter matches replay");
		const auto oldTick = first.GetTickCount();
		const auto oldPosition = first.GetBodyState(firstBall).Position;
		Rejects([&] { first.Advance(step * 9.0); }, "oversized frame rejected instead of silently discarding time");
		Rejects([&] { first.Advance(-0.01); }, "negative elapsed time rejected");
		Rejects([&] { first.Advance(std::numeric_limits<double>::quiet_NaN()); }, "NaN elapsed time rejected");
		Rejects([&] { first.Advance(std::numeric_limits<double>::infinity()); }, "infinite elapsed time rejected");
		Check(first.GetTickCount() == oldTick && first.GetBodyState(firstBall).Position == oldPosition &&
				  first.GetInterpolationAlpha() < 1.0e-9,
			  "invalid advance does not mutate simulation or accumulator");
		Check(first.Advance(step * first.GetSettings().MaximumSubsteps) == first.GetSettings().MaximumSubsteps,
			  "maximum supported catch-up runs all admitted substeps");
		const auto initialRounding = std::fegetround();
		if (std::fesetround(FE_DOWNWARD) != 0)
			throw std::runtime_error("Test host does not support explicit floating point rounding");
		bool fpuRejected = false;
		try
		{
			first.StepFixed();
		}
		catch (const std::logic_error&)
		{
			fpuRejected = true;
		}
		std::fesetround(initialRounding);
		Check(fpuRejected, "nonstandard host floating point rounding rejected");
		first.StepFixed();
		Check(first.GetTickCount() == 309, "world remains usable after invalid host rounding is corrected");
	}

	void TestSleepingNeighborChanges()
	{
		using namespace Bellwether;
		auto floorSettings = Ground();
		floorSettings.HalfExtents = {2.0f, 0.5f, 2.0f};
		PhysicsWorld removed;
		const auto removedFloor = removed.CreateBody(floorSettings);
		const auto falling = removed.CreateBody(Ball({0.0f, 2.0f, 0.0f}));
		for (int tick = 0; tick < 300; ++tick)
			removed.StepFixed();
		Check(!removed.GetBodyState(falling).Active, "removal fixture body is sleeping on its support");
		removed.DestroyBody(removedFloor);
		Check(removed.GetBodyState(falling).Active, "removing static support wakes sleeping body in old bounds");
		for (int tick = 0; tick < 30; ++tick)
			removed.StepFixed();
		Check(removed.GetBodyState(falling).Position.y < 0.0f, "unsupported sleeping body falls after support removal");

		PhysicsWorld teleported;
		const auto movedFloor = teleported.CreateBody(floorSettings);
		const auto oldNeighbor = teleported.CreateBody(Ball({0.0f, 2.0f, 0.0f}));
		auto destinationFloor = floorSettings;
		destinationFloor.Position.x = 10.0f;
		teleported.CreateBody(destinationFloor);
		const auto newNeighbor = teleported.CreateBody(Ball({10.0f, 2.0f, 0.0f}));
		for (int tick = 0; tick < 300; ++tick)
			teleported.StepFixed();
		Check(!teleported.GetBodyState(oldNeighbor).Active && !teleported.GetBodyState(newNeighbor).Active,
			  "teleport fixture bodies are sleeping at old and new bounds");
		teleported.SetBodyTransform(movedFloor, {10.0f, 0.5f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f});
		Check(teleported.GetBodyState(oldNeighbor).Active && teleported.GetBodyState(newNeighbor).Active,
			  "static teleport wakes neighbors in both old and new bounds");
		for (int tick = 0; tick < 30; ++tick)
			teleported.StepFixed();
		Check(teleported.GetBodyState(oldNeighbor).Position.y < 0.0f,
			  "body no longer hangs after supporting static shape teleports away");

		PhysicsWorld inserted;
		inserted.CreateBody(floorSettings);
		const auto insertionNeighbor = inserted.CreateBody(Ball({0.0f, 2.0f, 0.0f}));
		for (int tick = 0; tick < 300; ++tick)
			inserted.StepFixed();
		Check(!inserted.GetBodyState(insertionNeighbor).Active, "insertion fixture neighbor is asleep");
		auto obstacle = Ground();
		obstacle.Position = {0.0f, 0.75f, 0.0f};
		obstacle.HalfExtents = {0.5f, 0.5f, 0.5f};
		inserted.CreateBody(obstacle);
		Check(inserted.GetBodyState(insertionNeighbor).Active,
			  "inserting overlapping static geometry wakes sleeping neighbors");
	}

	void TestOrderedCollisionReplay()
	{
		using namespace Bellwether;
		PhysicsWorld first;
		PhysicsWorld second;
		const auto firstFloor = first.CreateBody(Ground());
		const auto secondFloor = second.CreateBody(Ground());
		std::vector<PhysicsBodyHandle> firstBodies;
		std::vector<PhysicsBodyHandle> secondBodies;
		for (int index = 0; index < 6; ++index)
		{
			auto body = Ball({0.15f * (index % 2), 0.5f + 1.1f * index, 0.0f});
			body.Shape = index % 2 == 0 ? PhysicsShape::Box : PhysicsShape::Sphere;
			body.Mass = 1.0f + index;
			body.Restitution = 0.2f;
			firstBodies.push_back(first.CreateBody(body));
			secondBodies.push_back(second.CreateBody(body));
		}
		bool identical = true;
		for (int frame = 0; frame < 200; ++frame)
		{
			if (frame == 30)
			{
				first.AddImpulse(firstBodies.front(), {2.0f, 4.0f, 0.0f});
				second.AddImpulse(secondBodies.front(), {2.0f, 4.0f, 0.0f});
			}
			if (frame == 60)
			{
				first.DestroyBody(firstBodies.back());
				second.DestroyBody(secondBodies.back());
				firstBodies.back() = first.CreateBody(Ball({1.0f, 4.0f, 0.0f}));
				secondBodies.back() = second.CreateBody(Ball({1.0f, 4.0f, 0.0f}));
			}
			if (frame == 90)
			{
				first.SetBodyTransform(firstFloor, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f});
				second.SetBodyTransform(secondFloor, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f});
			}
			first.Advance(first.GetSettings().FixedStepSeconds * 2.0);
			second.StepFixed();
			second.StepFixed();
			for (size_t index = 0; index < firstBodies.size(); ++index)
			{
				const auto stateA = first.GetBodyState(firstBodies[index]);
				const auto stateB = second.GetBodyState(secondBodies[index]);
				identical = identical && stateA.Position == stateB.Position && stateA.Rotation == stateB.Rotation &&
							stateA.LinearVelocity == stateB.LinearVelocity &&
							stateA.AngularVelocity == stateB.AngularVelocity && stateA.Active == stateB.Active;
			}
		}
		Check(
			identical && first.GetTickCount() == 400 && second.GetTickCount() == 400,
			"collision-stack replay stays identical across frame batching, impulses, replacement and support movement");
	}

	void TestValidationAndTeardown()
	{
		using namespace Bellwether;
		PhysicsWorld world;
		const auto ground = world.CreateBody(Ground());
		const auto state = world.GetBodyState(ground);
		auto invalid = Ball({0.0f, 2.0f, 0.0f});
		invalid.Position.x = std::numeric_limits<float>::quiet_NaN();
		Rejects([&] { world.CreateBody(invalid); }, "NaN body position rejected");
		invalid = Ball({0.0f, 2.0f, 0.0f});
		invalid.Rotation = {0.0f, 0.0f, 0.0f, 0.0f};
		Rejects([&] { world.CreateBody(invalid); }, "zero quaternion rejected");
		invalid.Rotation = {2.0f, 0.0f, 0.0f, 0.0f};
		Rejects([&] { world.CreateBody(invalid); }, "non-unit quaternion rejected");
		invalid.Rotation = {1.00002f, 0.0f, 0.0f, 0.0f};
		Rejects([&] { world.CreateBody(invalid); }, "rotation exceeding Jolt's normalization tolerance rejected");
		invalid = Ball({0.0f, 2.0f, 0.0f});
		invalid.HalfExtents.x = 0.0f;
		Rejects([&] { world.CreateBody(invalid); }, "zero box dimension rejected");
		invalid = Ball({0.0f, 2.0f, 0.0f});
		invalid.Radius = -1.0f;
		Rejects([&] { world.CreateBody(invalid); }, "negative sphere radius rejected");
		invalid.Radius = std::numeric_limits<float>::infinity();
		Rejects([&] { world.CreateBody(invalid); }, "infinite sphere radius rejected");
		invalid = Ball({0.0f, 2.0f, 0.0f});
		invalid.Mass = 0.0f;
		Rejects([&] { world.CreateBody(invalid); }, "zero dynamic mass rejected");
		invalid = Ball({0.0f, 2.0f, 0.0f});
		invalid.Friction = -0.2f;
		Rejects([&] { world.CreateBody(invalid); }, "negative friction rejected");
		invalid.Friction = 1.1f;
		Rejects([&] { world.CreateBody(invalid); }, "excess friction rejected");
		invalid = Ball({0.0f, 2.0f, 0.0f});
		invalid.Restitution = 1.1f;
		Rejects([&] { world.CreateBody(invalid); }, "excess restitution rejected");
		invalid = Ball({0.0f, 2.0f, 0.0f});
		invalid.Collision.Category = 8;
		Rejects([&] { world.CreateBody(invalid); }, "invalid collision category rejected");
		invalid = Ball({0.0f, 2.0f, 0.0f});
		invalid.Shape = static_cast<PhysicsShape>(999);
		Rejects([&] { world.CreateBody(invalid); }, "unknown shape rejected");
		invalid = Ball({0.0f, 2.0f, 0.0f});
		invalid.Motion = static_cast<PhysicsMotion>(999);
		Rejects([&] { world.CreateBody(invalid); }, "unknown motion rejected");
		invalid = Ground();
		invalid.LinearVelocity = {1.0f, 0.0f, 0.0f};
		Rejects([&] { world.CreateBody(invalid); }, "static initial velocity rejected");
		invalid = Ball({0.0f, 2.0f, 0.0f});
		invalid.LinearVelocity = {400.0f, 400.0f, 0.0f};
		Rejects([&] { world.CreateBody(invalid); }, "excess initial speed rejected without silent clamping");
		Rejects([&] { world.SetLinearVelocity(ground, {1.0f, 0.0f, 0.0f}); }, "static velocity mutation rejected");
		Rejects([&] { world.AddImpulse(ground, {1.0f, 0.0f, 0.0f}); }, "static impulse rejected");
		Rejects([&] { world.SetBodyTransform(ground, {1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 0.0f, 0.0f}); },
				"invalid transform rejected before moving body");
		Rejects([&] { world.Raycast({}, {}); }, "zero ray rejected");
		Rejects([&] { world.Raycast({std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}); },
				"NaN ray origin rejected");
		Rejects([&] { world.Raycast({}, {std::numeric_limits<float>::infinity(), 0.0f, 0.0f}); },
				"infinite ray displacement rejected");
		Check(world.GetBodyCount() == 1 && world.GetBodyState(ground).Position == state.Position,
			  "invalid creations and mutations preserve existing world");
		const auto dynamicBody = world.CreateBody(Ball({0.0f, 3.0f, 0.0f}));
		Rejects([&] { world.SetLinearVelocity(dynamicBody, {400.0f, 400.0f, 0.0f}); },
				"velocity setter rejects unsupported total speed");
		Rejects([&] { world.AddImpulse(dynamicBody, {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}); },
				"NaN impulse rejected");

		bool threadRejected = false;
		std::thread wrongThread(
			[&]
			{
				try
				{
					world.StepFixed();
				}
				catch (const std::logic_error&)
				{
					threadRejected = true;
				}
			});
		wrongThread.join();
		Check(threadRejected && world.GetTickCount() == 0, "foreign-thread access rejected before simulation");

		PhysicsWorldSettings badSettings;
		badSettings.MaximumBodies = 0;
		Rejects([&] { PhysicsWorld invalidWorld(badSettings); }, "zero world capacity rejected");
		badSettings = {};
		badSettings.MaximumBodyPairs = 1;
		Rejects([&] { PhysicsWorld invalidWorld(badSettings); },
				"too few body-pair buckets rejected before Jolt asserts");
		badSettings = {};
		badSettings.MaximumContactConstraints = 1;
		Rejects([&] { PhysicsWorld invalidWorld(badSettings); },
				"too few contact buckets rejected before Jolt asserts");
		badSettings = {};
		badSettings.MaximumSubsteps = 0;
		Rejects([&] { PhysicsWorld invalidWorld(badSettings); }, "zero substep limit rejected");
		badSettings = {};
		badSettings.FixedStepSeconds = std::numeric_limits<double>::quiet_NaN();
		Rejects([&] { PhysicsWorld invalidWorld(badSettings); }, "invalid fixed step rejected");
		badSettings = {};
		badSettings.Gravity.y = std::numeric_limits<float>::infinity();
		Rejects([&] { PhysicsWorld invalidWorld(badSettings); }, "invalid gravity rejected");
		PhysicsWorldSettings limited;
		limited.MaximumBodies = 1;
		{
			PhysicsWorld small(limited);
			const auto onlyBody = small.CreateBody(Ground());
			Rejects([&] { small.CreateBody(Ball({0.0f, 1.0f, 0.0f})); },
					"body capacity exhaustion is an explicit error");
			Check(small.GetBodyCount() == 1 && small.Exists(onlyBody), "capacity error preserves existing body");
			small.DestroyBody(onlyBody);
			const auto newBody = small.CreateBody(Ground());
			Check(newBody != onlyBody && small.GetBodyCount() == 1, "body capacity recovers after destruction");
		}
		bool repeatedTeardown = true;
		for (int iteration = 0; iteration < 20; ++iteration)
		{
			PhysicsWorld temporary(limited);
			const auto body = temporary.CreateBody(Ball({0.0f, 2.0f, 0.0f}));
			temporary.StepFixed();
			repeatedTeardown = repeatedTeardown && temporary.Exists(body);
		}
		world.StepFixed();
		Check(repeatedTeardown && world.GetTickCount() == 1,
			  "repeated active-body world teardown leaves other registered worlds usable");

		PhysicsWorldSettings insufficient;
		insufficient.MaximumContactConstraints = 4;
		insufficient.MaximumBodyPairs = 4;
		PhysicsWorld crowded(insufficient);
		crowded.CreateBody(Ground());
		for (int index = 0; index < 12; ++index)
			crowded.CreateBody(Ball({0.0f, 0.25f, 0.0f}));
		bool capacityFault = false;
		try
		{
			crowded.StepFixed();
		}
		catch (const std::runtime_error& exception)
		{
			capacityFault = std::string(exception.what()).find("solver capacity exceeded") != std::string::npos;
		}
		Check(capacityFault, "real solver contact-capacity overflow reports a fault");
		Rejects([&] { crowded.StepFixed(); }, "faulted simulation refuses another step");
		Rejects([&] { crowded.CreateBody(Ground()); }, "faulted simulation refuses mutations");
		Rejects([&] { crowded.Raycast({}, {0.0f, 1.0f, 0.0f}); }, "faulted simulation refuses misleading queries");

		PhysicsWorld escaped;
		auto escaping = Ball({99999.0f, 5.0f, 0.0f});
		escaping.LinearVelocity = {500.0f, 0.0f, 0.0f};
		const auto escapedBody = escaped.CreateBody(escaping);
		Rejects([&] { escaped.StepFixed(); }, "simulation leaving supported position envelope faults explicitly");
		Rejects([&] { escaped.GetBodyState(escapedBody); },
				"world-envelope fault prevents presenting a valid simulation");
	}

} // namespace

int main()
{
	std::cout << std::unitbuf;
	try
	{
		TestCollisionAndQueries();
		TestCollisionFiltering();
		TestFixedTimeAndReplay();
		TestSleepingNeighborChanges();
		TestOrderedCollisionReplay();
		TestValidationAndTeardown();
	}
	catch (const std::exception& exception)
	{
		std::cerr << "FAIL unhandled physics test error: " << exception.what() << '\n';
		++s_Failed;
	}
	std::cout << "PHYSICS_TEST_COMPLETE " << s_Passed << " passed, " << s_Failed << " failed\n";
	return s_Failed == 0 ? 0 : 1;
}
