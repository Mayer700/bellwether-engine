#include "SnowSandbox.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>

namespace Bellwether
{

	namespace
	{
		float SmoothRamp(float edge0, float edge1, float value)
		{
			if (std::abs(edge1 - edge0) < 1e-6f)
				return value >= edge1 ? 1.0f : 0.0f;
			float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		}

		void AppendQuad(std::vector<ModelVertex>& vertices, std::vector<uint32_t>& indices, const glm::vec3& p0,
						const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, const glm::vec3& normal)
		{
			const uint32_t base = static_cast<uint32_t>(vertices.size());
			const glm::vec3 tangentVec = glm::normalize(p1 - p0);
			const glm::vec4 tangent(tangentVec, 1.0f);
			vertices.push_back({p0, normal, {0.0f, 0.0f}, tangent, glm::vec4(1.0f)});
			vertices.push_back({p1, normal, {1.0f, 0.0f}, tangent, glm::vec4(1.0f)});
			vertices.push_back({p2, normal, {1.0f, 1.0f}, tangent, glm::vec4(1.0f)});
			vertices.push_back({p3, normal, {0.0f, 1.0f}, tangent, glm::vec4(1.0f)});
			indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
		}

		ModelPrimitive CreateBoxPrimitive(const glm::vec3& halfExtents, size_t material)
		{
			ModelPrimitive prim;
			prim.Material = material;
			const float x = halfExtents.x;
			const float y = halfExtents.y;
			const float z = halfExtents.z;
			// +Z
			AppendQuad(prim.Vertices, prim.Indices, {-x, -y, z}, {x, -y, z}, {x, y, z}, {-x, y, z}, {0, 0, 1});
			// -Z
			AppendQuad(prim.Vertices, prim.Indices, {x, -y, -z}, {-x, -y, -z}, {-x, y, -z}, {x, y, -z}, {0, 0, -1});
			// +X
			AppendQuad(prim.Vertices, prim.Indices, {x, -y, z}, {x, -y, -z}, {x, y, -z}, {x, y, z}, {1, 0, 0});
			// -X
			AppendQuad(prim.Vertices, prim.Indices, {-x, -y, -z}, {-x, -y, z}, {-x, y, z}, {-x, y, -z}, {-1, 0, 0});
			// +Y
			AppendQuad(prim.Vertices, prim.Indices, {-x, y, z}, {x, y, z}, {x, y, -z}, {-x, y, -z}, {0, 1, 0});
			// -Y
			AppendQuad(prim.Vertices, prim.Indices, {-x, -y, -z}, {x, -y, -z}, {x, -y, z}, {-x, -y, z}, {0, -1, 0});
			return prim;
		}

		ModelPrimitive CreateGableRoofPrimitive(float halfWidth, float height, float halfDepth, size_t material)
		{
			ModelPrimitive prim;
			prim.Material = material;
			const glm::vec3 fl(-halfWidth, 0.0f, halfDepth);
			const glm::vec3 fr(halfWidth, 0.0f, halfDepth);
			const glm::vec3 ft(0.0f, height, halfDepth);
			const glm::vec3 bl(-halfWidth, 0.0f, -halfDepth);
			const glm::vec3 br(halfWidth, 0.0f, -halfDepth);
			const glm::vec3 bt(0.0f, height, -halfDepth);

			auto pushTri = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
			{
				glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
				glm::vec4 tan(glm::normalize(b - a), 1.0f);
				const uint32_t base = static_cast<uint32_t>(prim.Vertices.size());
				prim.Vertices.push_back({a, n, {0, 0}, tan, glm::vec4(1)});
				prim.Vertices.push_back({b, n, {1, 0}, tan, glm::vec4(1)});
				prim.Vertices.push_back({c, n, {0.5f, 1}, tan, glm::vec4(1)});
				prim.Indices.insert(prim.Indices.end(), {base, base + 1, base + 2});
			};
			pushTri(fl, fr, ft);
			pushTri(br, bl, bt);
			glm::vec3 leftNormal = glm::normalize(glm::cross(ft - fl, bl - fl));
			AppendQuad(prim.Vertices, prim.Indices, bl, fl, ft, bt, leftNormal);
			glm::vec3 rightNormal = glm::normalize(glm::cross(br - fr, bt - fr));
			AppendQuad(prim.Vertices, prim.Indices, fr, br, bt, ft, rightNormal);
			AppendQuad(prim.Vertices, prim.Indices, bl, br, fr, fl, {0, -1, 0});
			return prim;
		}

		ModelPrimitive CreateSpherePrimitive(float radius, uint32_t slices, uint32_t stacks, size_t material)
		{
			ModelPrimitive prim;
			prim.Material = material;
			constexpr float pi = 3.14159265359f;
			for (uint32_t stack = 0; stack <= stacks; ++stack)
			{
				const float v = static_cast<float>(stack) / static_cast<float>(stacks);
				const float phi = v * pi;
				for (uint32_t slice = 0; slice <= slices; ++slice)
				{
					const float u = static_cast<float>(slice) / static_cast<float>(slices);
					const float theta = u * 2.0f * pi;
					glm::vec3 normal(std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta));
					glm::vec3 tangentVec(-std::sin(theta), 0.0f, std::cos(theta));
					if (glm::length(tangentVec) < 1e-4f)
						tangentVec = {1.0f, 0.0f, 0.0f};
					prim.Vertices.push_back({normal * radius, glm::normalize(normal), {u, v},
											 glm::vec4(glm::normalize(tangentVec), 1.0f), glm::vec4(1.0f)});
				}
			}
			const uint32_t row = slices + 1;
			for (uint32_t stack = 0; stack < stacks; ++stack)
			{
				for (uint32_t slice = 0; slice < slices; ++slice)
				{
					const uint32_t a = stack * row + slice;
					const uint32_t b = a + row;
					prim.Indices.insert(prim.Indices.end(), {a, b, a + 1, a + 1, b, b + 1});
				}
			}
			return prim;
		}

		ModelPrimitive CreateRingPrimitive(float innerRadius, float outerRadius, float halfHeight, uint32_t segments,
										   size_t material)
		{
			ModelPrimitive prim;
			prim.Material = material;
			constexpr float pi = 3.14159265359f;
			for (uint32_t i = 0; i < segments; ++i)
			{
				const float a0 = (static_cast<float>(i) / static_cast<float>(segments)) * 2.0f * pi;
				const float a1 = (static_cast<float>(i + 1) / static_cast<float>(segments)) * 2.0f * pi;
				const glm::vec3 i0(std::cos(a0) * innerRadius, halfHeight, std::sin(a0) * innerRadius);
				const glm::vec3 o0(std::cos(a0) * outerRadius, halfHeight, std::sin(a0) * outerRadius);
				const glm::vec3 i1(std::cos(a1) * innerRadius, halfHeight, std::sin(a1) * innerRadius);
				const glm::vec3 o1(std::cos(a1) * outerRadius, halfHeight, std::sin(a1) * outerRadius);
				AppendQuad(prim.Vertices, prim.Indices, o0, o1, i1, i0, {0.0f, 1.0f, 0.0f});
			}
			return prim;
		}
	} // namespace

	SnowTestScenario ParseSnowScenario(const std::string& name)
	{
		if (name == "tracks" || name == "1")
			return SnowTestScenario::TracksAndPhysics;
		if (name == "melt" || name == "ice" || name == "2")
			return SnowTestScenario::ThermalMeltAndIce;
		if (name == "blizzard" || name == "drift" || name == "3")
			return SnowTestScenario::BlizzardDrift;
		if (name == "sparkle" || name == "sunny" || name == "4")
			return SnowTestScenario::SunnySparkle;
		throw std::invalid_argument("Unknown snow scenario '" + name + "' (expected tracks, melt, blizzard, or sparkle)");
	}

	const char* GetSnowScenarioName(SnowTestScenario scenario)
	{
		switch (scenario)
		{
		case SnowTestScenario::TracksAndPhysics:
			return "Tracks & Physics Drag";
		case SnowTestScenario::ThermalMeltAndIce:
			return "Thermal Melt & Ice";
		case SnowTestScenario::BlizzardDrift:
			return "Blizzard Drift & Healing";
		case SnowTestScenario::SunnySparkle:
			return "Sunny Crystal Sparkle";
		}
		return "Snow Sandbox";
	}

	SnowSandbox::SnowSandbox(SnowTestScenario scenario)
	{
		m_TerrainHeights.resize(static_cast<size_t>(s_HeightGrid) * s_HeightGrid);
		const float halfExtent = s_WorldExtent * 0.5f;
		for (uint32_t z = 0; z < s_HeightGrid; ++z)
		{
			const float wz = (static_cast<float>(z) / static_cast<float>(s_HeightGrid - 1)) * s_WorldExtent - halfExtent;
			for (uint32_t x = 0; x < s_HeightGrid; ++x)
			{
				const float wx =
					(static_cast<float>(x) / static_cast<float>(s_HeightGrid - 1)) * s_WorldExtent - halfExtent;
				m_TerrainHeights[static_cast<size_t>(z) * s_HeightGrid + x] = EvaluateTerrainHeight(wx, wz);
			}
		}
		BuildStaticGeometry();
		ResetScenario(scenario);
	}

	float SnowSandbox::EvaluateTerrainHeight(float x, float z) const
	{
		if (!std::isfinite(x) || !std::isfinite(z))
			return 0.0f;
		// Keep the central test arena flat for unbiased snow vs ice physics comparisons,
		// while adding a sledding ramp on the north edge and steep snow-shedding ridges in the back.
		const float northRidge = SmoothRamp(-13.5f, -26.5f, z) * 7.2f;
		const float westRidge = SmoothRamp(-16.5f, -27.5f, x) * 6.4f;
		const float sledSlope =
			SmoothRamp(-6.5f, -15.5f, z) * (1.0f - SmoothRamp(10.0f, 18.0f, std::abs(x))) * 2.1f;
		const float ridgeNoise =
			(northRidge > 0.1f || westRidge > 0.1f) ? (std::sin(x * 0.35f) * std::cos(z * 0.32f) * 0.45f) : 0.0f;
		return std::max(0.0f, std::max({northRidge, westRidge, sledSlope}) + ridgeNoise);
	}

	void SnowSandbox::BuildStaticGeometry()
	{
		m_RenderAsset = ModelAsset{};

		auto addMaterial = [&](const std::string& name, const glm::vec4& color, float roughness, float metallic,
							   const glm::vec3& emission = glm::vec3(0.0f), bool doubleSided = false) -> size_t
		{
			ModelMaterial mat;
			mat.Name = name;
			mat.BaseColor = color;
			mat.Roughness = roughness;
			mat.Metallic = metallic;
			mat.Emission = emission;
			mat.DoubleSided = doubleSided;
			m_RenderAsset.Materials.push_back(mat);
			return m_RenderAsset.Materials.size() - 1;
		};

		const size_t groundMat = addMaterial("AlpineRock", {0.22f, 0.24f, 0.28f, 1.0f}, 0.90f, 0.0f);
		const size_t timberMat = addMaterial("CabinTimber", {0.32f, 0.18f, 0.10f, 1.0f}, 0.78f, 0.0f);
		const size_t roofMat = addMaterial("CabinSlate", {0.19f, 0.21f, 0.26f, 1.0f}, 0.82f, 0.0f);
		const size_t windowMat = addMaterial("CabinGlow", {1.0f, 0.72f, 0.32f, 1.0f}, 0.30f, 0.0f, {3.8f, 2.0f, 0.65f});
		const size_t stoneMat = addMaterial("HearthStone", {0.26f, 0.27f, 0.30f, 1.0f}, 0.86f, 0.0f);
		const size_t emberMat = addMaterial("CampfireEmbers", {0.95f, 0.38f, 0.10f, 1.0f}, 0.45f, 0.0f,
											{8.5f, 2.5f, 0.40f});
		const size_t steelMat = addMaterial("WeatherBeaconSteel", {0.58f, 0.64f, 0.72f, 1.0f}, 0.24f, 0.92f);
		const size_t beaconMat = addMaterial("BeaconTip", {0.35f, 0.75f, 1.0f, 1.0f}, 0.20f, 0.0f, {0.45f, 1.8f, 3.6f});
		const size_t powderSledMat = addMaterial("PowderSled", {0.78f, 0.24f, 0.14f, 1.0f}, 0.42f, 0.25f);
		const size_t iceSledMat = addMaterial("IceSled", {0.16f, 0.54f, 0.84f, 1.0f}, 0.22f, 0.80f);
		const size_t userSledMat = addMaterial("LaunchedSled", {0.86f, 0.62f, 0.18f, 1.0f}, 0.32f, 0.45f);
		const size_t probeMat = addMaterial("SnowProbe", {0.20f, 0.88f, 0.95f, 1.0f}, 0.25f, 0.1f, {0.4f, 1.5f, 1.9f},
											true);

		// Terrain mesh (193x193 bilinearly sampled from m_TerrainHeights)
		{
			ModelPrimitive ground;
			ground.Material = groundMat;
			ground.Vertices.reserve(static_cast<size_t>(s_RenderGroundGrid) * s_RenderGroundGrid);
			ground.Indices.reserve(static_cast<size_t>(s_RenderGroundGrid - 1) * (s_RenderGroundGrid - 1) * 6);
			const float halfExtent = s_WorldExtent * 0.5f;
			const float eps = s_WorldExtent / static_cast<float>(s_RenderGroundGrid - 1);
			for (uint32_t z = 0; z < s_RenderGroundGrid; ++z)
			{
				const float v = static_cast<float>(z) / static_cast<float>(s_RenderGroundGrid - 1);
				const float wz = v * s_WorldExtent - halfExtent;
				for (uint32_t x = 0; x < s_RenderGroundGrid; ++x)
				{
					const float u = static_cast<float>(x) / static_cast<float>(s_RenderGroundGrid - 1);
					const float wx = u * s_WorldExtent - halfExtent;
					const float h = EvaluateTerrainHeight(wx, wz);
					const float hL = EvaluateTerrainHeight(wx - eps, wz);
					const float hR = EvaluateTerrainHeight(wx + eps, wz);
					const float hD = EvaluateTerrainHeight(wx, wz - eps);
					const float hU = EvaluateTerrainHeight(wx, wz + eps);
					const glm::vec3 normal = glm::normalize(glm::vec3(hL - hR, 2.0f * eps, hD - hU));
					ground.Vertices.push_back(
						{{wx, h, wz}, normal, {u, v}, {1.0f, 0.0f, 0.0f, 1.0f}, glm::vec4(1.0f)});
				}
			}
			for (uint32_t z = 0; z < s_RenderGroundGrid - 1; ++z)
			{
				for (uint32_t x = 0; x < s_RenderGroundGrid - 1; ++x)
				{
					const uint32_t idx = z * s_RenderGroundGrid + x;
					ground.Indices.insert(ground.Indices.end(),
										  {idx, idx + s_RenderGroundGrid, idx + 1, idx + 1, idx + s_RenderGroundGrid,
										   idx + s_RenderGroundGrid + 1});
				}
			}
			const size_t primIdx = m_RenderAsset.Primitives.size();
			m_RenderAsset.Primitives.push_back(std::move(ground));
			m_RenderAsset.Placements.push_back({"SnowGround", primIdx, glm::mat4(1.0f), {1.0f, 0.0f, 0.0f, 0.0f}});
		}

		auto addStaticPrimitive = [&](const std::string& name, ModelPrimitive prim, const glm::mat4& transform)
		{
			const size_t primIdx = m_RenderAsset.Primitives.size();
			m_RenderAsset.Primitives.push_back(std::move(prim));
			m_RenderAsset.Placements.push_back({name, primIdx, transform, {0.0f, 0.0f, 0.0f, 0.0f}});
		};

		// Alpine shelter cabin (-9.5, y, -8.5)
		{
			const glm::vec3 cabinPos(-9.5f, EvaluateTerrainHeight(-9.5f, -8.5f), -8.5f);
			addStaticPrimitive("CabinWalls", CreateBoxPrimitive({1.8f, 1.2f, 1.5f}, timberMat),
							   glm::translate(glm::mat4(1.0f), cabinPos + glm::vec3(0.0f, 1.2f, 0.0f)));
			addStaticPrimitive("CabinRoof", CreateGableRoofPrimitive(2.15f, 1.25f, 1.8f, roofMat),
							   glm::translate(glm::mat4(1.0f), cabinPos + glm::vec3(0.0f, 2.38f, 0.0f)));
			addStaticPrimitive("CabinWindow", CreateBoxPrimitive({0.38f, 0.35f, 0.06f}, windowMat),
							   glm::translate(glm::mat4(1.0f), cabinPos + glm::vec3(0.0f, 1.35f, 1.51f)));
		}

		// Campfire hearth (-3.5, y, 7.0)
		{
			const glm::vec3 hearthPos(-3.5f, EvaluateTerrainHeight(-3.5f, 7.0f), 7.0f);
			const size_t stonePrimIdx = m_RenderAsset.Primitives.size();
			m_RenderAsset.Primitives.push_back(CreateBoxPrimitive({0.24f, 0.18f, 0.24f}, stoneMat));
			for (int i = 0; i < 6; ++i)
			{
				const float angle = static_cast<float>(i) * (6.2831853f / 6.0f);
				const glm::vec3 offset(std::cos(angle) * 0.72f, 0.16f, std::sin(angle) * 0.72f);
				glm::mat4 m = glm::translate(glm::mat4(1.0f), hearthPos + offset);
				m = glm::rotate(m, angle, {0.0f, 1.0f, 0.0f});
				m_RenderAsset.Placements.push_back({"HearthStone", stonePrimIdx, m, {0.0f, 0.0f, 0.0f, 0.0f}});
			}
			addStaticPrimitive("CampfireCore", CreateSpherePrimitive(0.38f, 14, 10, emberMat),
							   glm::translate(glm::mat4(1.0f), hearthPos + glm::vec3(0.0f, 0.28f, 0.0f)));
		}

		// Metallic weather beacon (8.5, y, -5.5)
		{
			const glm::vec3 beaconPos(8.5f, EvaluateTerrainHeight(8.5f, -5.5f), -5.5f);
			addStaticPrimitive("WeatherPost", CreateBoxPrimitive({0.25f, 1.6f, 0.25f}, steelMat),
							   glm::translate(glm::mat4(1.0f), beaconPos + glm::vec3(0.0f, 1.6f, 0.0f)));
			addStaticPrimitive("WeatherBeacon", CreateSpherePrimitive(0.32f, 14, 10, beaconMat),
							   glm::translate(glm::mat4(1.0f), beaconPos + glm::vec3(0.0f, 3.35f, 0.0f)));
		}

		// Timber/stone backstop obstacles on the right side of the lanes
		{
			const glm::vec3 cratePos(11.0f, EvaluateTerrainHeight(11.0f, 0.8f), 0.8f);
			addStaticPrimitive("LaneBackstopA", CreateBoxPrimitive({0.85f, 0.75f, 0.85f}, timberMat),
							   glm::translate(glm::mat4(1.0f), cratePos + glm::vec3(0.0f, 0.75f, 0.0f)));
			const glm::vec3 boulderPos(5.5f, EvaluateTerrainHeight(5.5f, 6.2f), 6.2f);
			addStaticPrimitive("BoulderObstacle", CreateBoxPrimitive({0.70f, 0.65f, 0.70f}, stoneMat),
							   glm::translate(glm::mat4(1.0f), boulderPos + glm::vec3(0.0f, 0.65f, 0.0f)));
		}

		// Dynamic sled & snowball prototype meshes + pooled placements
		const size_t powderSledPrim = m_RenderAsset.Primitives.size();
		m_RenderAsset.Primitives.push_back(CreateBoxPrimitive({0.72f, 0.34f, 0.48f}, powderSledMat));
		const size_t iceSledPrim = m_RenderAsset.Primitives.size();
		m_RenderAsset.Primitives.push_back(CreateBoxPrimitive({0.72f, 0.34f, 0.48f}, iceSledMat));
		const size_t userSledPrim = m_RenderAsset.Primitives.size();
		m_RenderAsset.Primitives.push_back(CreateBoxPrimitive({0.68f, 0.32f, 0.46f}, userSledMat));
		const size_t userBoulderPrim = m_RenderAsset.Primitives.size();
		m_RenderAsset.Primitives.push_back(CreateSpherePrimitive(0.50f, 16, 12, userSledMat));

		m_DynamicSlots.clear();
		// Slot 0: Primary powder sled
		m_DynamicSlots.push_back({0, m_RenderAsset.Placements.size(), false});
		m_RenderAsset.Placements.push_back({"PrimaryPowderSled", powderSledPrim, glm::mat4(1.0f), {0.0f, 0.0f, 0.0f, 0.0f}});
		// Slot 1: Primary ice sled
		m_DynamicSlots.push_back({0, m_RenderAsset.Placements.size(), false});
		m_RenderAsset.Placements.push_back({"PrimaryIceSled", iceSledPrim, glm::mat4(1.0f), {0.0f, 0.0f, 0.0f, 0.0f}});
		// Slots 2..9: User-launched sleds & snow boulders (alternating box sled / sphere boulder)
		for (size_t i = 0; i < s_MaxUserSleds; ++i)
		{
			const bool isBoulder = (i % 2) == 1;
			m_DynamicSlots.push_back({0, m_RenderAsset.Placements.size(), isBoulder});
			m_RenderAsset.Placements.push_back({isBoulder ? "UserSnowBoulder" : "UserSnowSled",
												isBoulder ? userBoulderPrim : userSledPrim, glm::mat4(1.0f),
												{0.0f, 0.0f, 0.0f, 0.0f}});
		}

		// Interactive camera-target probe ring
		const size_t probePrim = m_RenderAsset.Primitives.size();
		m_RenderAsset.Primitives.push_back(CreateRingPrimitive(0.95f, 1.25f, 0.06f, 24, probeMat));
		m_ProbePlacementIndex = m_RenderAsset.Placements.size();
		m_RenderAsset.Placements.push_back({"SnowProbe", probePrim, glm::mat4(1.0f), {0.0f, 0.0f, 0.0f, 0.0f}});
	}

	void SnowSandbox::ResetScenario(SnowTestScenario scenario)
	{
		m_Scenario = scenario;
		m_ElapsedSeconds = 0.0f;
		m_ColdSnapTimer = 0.0f;
		m_NextUserSlot = 0;
		for (auto& slot : m_DynamicSlots)
			slot.Entity = 0;
		m_PrimaryPowderSled = 0;
		m_PrimaryIceSled = 0;

		m_Scene = Scene{};

		SceneSnowSettings snowCfg;
		snowCfg.Enabled = true;
		snowCfg.Resolution = s_SnowResolution;
		snowCfg.WorldExtent = s_WorldExtent;
		snowCfg.BaseDepth = 0.50f;
		snowCfg.MaxDepth = 1.60f;
		snowCfg.TrackHealRate = 0.08f;
		snowCfg.WindDriftStrength = 0.25f;
		snowCfg.RenderGround = true;

		RenderSettings renderCfg;
		renderCfg.CameraTarget = {0.0f, 0.5f, 0.5f};
		renderCfg.CameraSize = 26.0f;
		renderCfg.Shadows = true;
		renderCfg.Exposure = 0.96f;
		renderCfg.FogDensity = 0.004f;
		renderCfg.SunDirection = {-0.62f, 0.78f, -0.36f};
		renderCfg.SunRadiance = {4.1f, 3.5f, 2.8f};
		renderCfg.SnowCoverage = 0.92f;
		renderCfg.SnowSparkle = 0.85f;
		renderCfg.SnowSubsurface = 0.90f;
		renderCfg.SnowfallIntensity = 0.50f;
		renderCfg.WindVelocity = {3.8f, 0.0f, -1.4f};
		m_BaseAmbientCelsius = -6.0f;
		m_WindPresetIndex = 1;
		m_SparklePresetIndex = 1;

		switch (scenario)
		{
		case SnowTestScenario::TracksAndPhysics:
			break;
		case SnowTestScenario::ThermalMeltAndIce:
			snowCfg.BaseDepth = 0.54f;
			renderCfg.SnowfallIntensity = 0.25f;
			renderCfg.SnowSparkle = 1.05f;
			renderCfg.WindVelocity = {1.5f, 0.0f, -0.6f};
			renderCfg.CameraTarget = {-2.0f, 0.5f, 4.5f};
			m_BaseAmbientCelsius = -14.0f;
			m_WindPresetIndex = 0;
			break;
		case SnowTestScenario::BlizzardDrift:
			snowCfg.BaseDepth = 0.52f;
			snowCfg.TrackHealRate = 0.35f;
			snowCfg.WindDriftStrength = 0.65f;
			renderCfg.SnowfallIntensity = 0.95f;
			renderCfg.WindVelocity = {9.5f, 0.0f, -3.5f};
			renderCfg.FogDensity = 0.007f;
			renderCfg.SunRadiance = {2.7f, 2.6f, 2.5f};
			renderCfg.SnowSparkle = 0.45f;
			m_BaseAmbientCelsius = -12.0f;
			m_WindPresetIndex = 3;
			m_SparklePresetIndex = 0;
			break;
		case SnowTestScenario::SunnySparkle:
			snowCfg.BaseDepth = 0.48f;
			renderCfg.SnowfallIntensity = 0.12f;
			renderCfg.WindVelocity = {1.2f, 0.0f, -0.4f};
			renderCfg.SunDirection = {-0.78f, 0.48f, -0.40f};
			renderCfg.SunRadiance = {5.4f, 4.7f, 3.8f};
			renderCfg.SnowSparkle = 1.85f;
			renderCfg.SnowSubsurface = 1.25f;
			renderCfg.Exposure = 0.90f;
			m_BaseAmbientCelsius = -9.0f;
			m_SparklePresetIndex = 2;
			break;
		}

		m_Scene.SetSnowSettings(snowCfg);
		m_Scene.SetRenderSettings(renderCfg);
		m_ProbeXZ = {renderCfg.CameraTarget.x, renderCfg.CameraTarget.z};

		auto addStaticBoxCollider = [&](const std::string& name, const glm::vec3& pos, const glm::vec3& halfExtents)
		{
			EntityID id = m_Scene.CreateEntity(name);
			auto& entity = m_Scene.GetEntity(id);
			entity.Transform.Translation = pos;
			entity.Physics.Enabled = true;
			entity.Physics.Shape = PhysicsShape::Box;
			entity.Physics.Motion = PhysicsMotion::Static;
			entity.Physics.HalfExtents = halfExtents;
			entity.Physics.Friction = 0.45f;
		};

		{
			const glm::vec3 cabinPos(-9.5f, EvaluateTerrainHeight(-9.5f, -8.5f), -8.5f);
			addStaticBoxCollider("Cabin", cabinPos + glm::vec3(0.0f, 1.2f, 0.0f), {1.8f, 1.2f, 1.5f});
			const glm::vec3 beaconPos(8.5f, EvaluateTerrainHeight(8.5f, -5.5f), -5.5f);
			addStaticBoxCollider("Beacon", beaconPos + glm::vec3(0.0f, 1.6f, 0.0f), {0.30f, 1.6f, 0.30f});
			const glm::vec3 cratePos(11.0f, EvaluateTerrainHeight(11.0f, 0.8f), 0.8f);
			addStaticBoxCollider("CrateBackstop", cratePos + glm::vec3(0.0f, 0.75f, 0.0f), {0.85f, 0.75f, 0.85f});
			const glm::vec3 boulderPos(5.5f, EvaluateTerrainHeight(5.5f, 6.2f), 6.2f);
			addStaticBoxCollider("Boulder", boulderPos + glm::vec3(0.0f, 0.65f, 0.0f), {0.70f, 0.65f, 0.70f});
		}

		// Configure primary comparison sleds before building SceneSimulation
		auto createSledEntity = [&](const std::string& name, const glm::vec3& pos) -> EntityID
		{
			EntityID id = m_Scene.CreateEntity(name);
			auto& entity = m_Scene.GetEntity(id);
			entity.Transform.Translation = pos;
			entity.Physics.Enabled = true;
			entity.Physics.Shape = PhysicsShape::Box;
			entity.Physics.Motion = PhysicsMotion::Dynamic;
			entity.Physics.HalfExtents = {0.72f, 0.34f, 0.48f};
			entity.Physics.Mass = 55.0f;
			entity.Physics.Friction = 0.24f;
			entity.Physics.Restitution = 0.15f;
			entity.Physics.SnowTracking = true;
			entity.Physics.SnowDrag = 5.5f;
			return id;
		};

		m_PrimaryPowderSled = createSledEntity("PowderSled", {-11.5f, 0.36f, -1.6f});
		m_PrimaryIceSled = createSledEntity("IceSled", {-11.5f, 0.36f, 2.8f});
		m_DynamicSlots[0].Entity = m_PrimaryPowderSled;
		m_DynamicSlots[1].Entity = m_PrimaryIceSled;

		m_Simulation = std::make_unique<SceneSimulation>(m_Scene);
		m_Simulation->SetAmbientCelsius(m_BaseAmbientCelsius);
		m_Simulation->SetTerrainHeightfield(m_TerrainHeights, s_HeightGrid, s_WorldExtent, 0.74f, -100.0f);

		ConfigureScenarioState(scenario);
	}

	void SnowSandbox::ConfigureScenarioState(SnowTestScenario scenario)
	{
		SnowField& snow = m_Simulation->GetSnowField();

		// Pre-carve a curved showcase trail in the foreground/north slope
		snow.StampSegment({-4.0f, -10.5f}, {2.0f, -4.5f}, 1.05f, 0.38f, 0.42f);
		snow.StampSegment({2.0f, -4.5f}, {7.0f, -2.5f}, 1.05f, 0.38f, 0.42f);

		// Create the slick Ice Test Lane at z in [2.1, 3.5] across x in [-13, 9]
		const auto& cfg = snow.GetConfig();
		const float halfExtent = cfg.WorldExtent * 0.5f;
		const float cellStep = cfg.WorldExtent / static_cast<float>(cfg.Resolution - 1);
		for (uint32_t gz = 0; gz < cfg.Resolution; ++gz)
		{
			const float wz = -halfExtent + static_cast<float>(gz) * cellStep;
			if (wz < 2.0f || wz > 3.6f)
				continue;
			for (uint32_t gx = 0; gx < cfg.Resolution; ++gx)
			{
				const float wx = -halfExtent + static_cast<float>(gx) * cellStep;
				if (wx < -13.0f || wx > 9.5f)
					continue;
				SnowCell cell = snow.GetCell(gx, gz);
				cell.BaseDepth = std::min(cell.BaseDepth, 0.14f);
				cell.Compression = 0.02f;
				cell.Ice = 0.88f;
				snow.SetCell(gx, gz, cell);
			}
		}

		// Melt a thermal ring around the hearth (-3.5, 7.0) and refreeze an icy border
		snow.ApplyHeatSource({-3.5f, 7.0f}, 2.9f, 22.0f, 0.85f);
		snow.StepWeather(1.6f, 0.0f, {0.0f, 0.0f}, -18.0f);

		if (scenario == SnowTestScenario::ThermalMeltAndIce)
		{
			snow.ApplyHeatSource({-1.5f, 4.5f}, 3.6f, 20.0f, 0.75f);
			snow.StepWeather(2.2f, 0.0f, {0.0f, 0.0f}, -20.0f);
		}
		else if (scenario == SnowTestScenario::BlizzardDrift)
		{
			snow.StampSegment({-8.0f, -5.5f}, {8.0f, -5.5f}, 1.3f, 0.40f, 0.45f);
		}
		else if (scenario == SnowTestScenario::SunnySparkle)
		{
			snow.StampSegment({-6.5f, -5.0f}, {6.5f, 0.0f}, 1.25f, 0.40f, 0.45f);
			snow.StampCircle({0.0f, -3.0f}, 1.8f, 0.40f, 0.45f);
		}

		// Launch the twin sleds side-by-side (Powder vs Ice) so physics coupling is immediately visible
		m_Simulation->SetEntityVelocity(m_PrimaryPowderSled, {10.5f, 0.0f, 0.0f});
		m_Simulation->SetEntityVelocity(m_PrimaryIceSled, {10.5f, 0.0f, 0.0f});
	}

	void SnowSandbox::Step(float deltaSeconds)
	{
		if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f)
			throw std::invalid_argument("SnowSandbox step deltaSeconds must be finite and non-negative");
		const float dt = std::min(deltaSeconds, 0.1f);
		if (dt == 0.0f)
			return;

		m_ElapsedSeconds += dt;
		if (m_ColdSnapTimer > 0.0f)
		{
			m_ColdSnapTimer = std::max(0.0f, m_ColdSnapTimer - dt);
			m_Simulation->SetAmbientCelsius(m_ColdSnapTimer > 0.0f ? -22.0f : m_BaseAmbientCelsius);
		}

		if (m_Scenario == SnowTestScenario::ThermalMeltAndIce)
		{
			m_Simulation->GetSnowField().ApplyHeatSource({-3.5f, 7.0f}, 2.4f, 6.0f, dt);
		}

		m_Simulation->Step(static_cast<double>(dt));
	}

	bool SnowSandbox::StampAt(glm::vec2 worldXZ, float radius, float pressureDepth)
	{
		return m_Simulation->GetSnowField().StampCircle(worldXZ, radius, pressureDepth);
	}

	bool SnowSandbox::StampStroke(glm::vec2 startXZ, glm::vec2 endXZ, float radius, float pressureDepth)
	{
		return m_Simulation->GetSnowField().StampSegment(startXZ, endXZ, radius, pressureDepth);
	}

	bool SnowSandbox::MeltAt(glm::vec2 worldXZ, float radius, float heatPower, float dt)
	{
		return m_Simulation->GetSnowField().ApplyHeatSource(worldXZ, radius, heatPower, dt);
	}

	void SnowSandbox::TriggerColdSnapFreeze(float durationSeconds)
	{
		if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0f || durationSeconds > 60.0f)
			throw std::invalid_argument("Cold snap duration must be finite and in (0, 60]");
		m_ColdSnapTimer = durationSeconds;
		m_Simulation->SetAmbientCelsius(-22.0f);
		m_Simulation->GetSnowField().StepWeather(
			1.2f, 0.0f, {m_Scene.GetRenderSettings().WindVelocity.x, m_Scene.GetRenderSettings().WindVelocity.z},
			-22.0f);
	}

	EntityID SnowSandbox::LaunchSled(glm::vec2 originXZ, glm::vec2 velocityXZ, bool boulder)
	{
		if (!std::isfinite(originXZ.x) || !std::isfinite(originXZ.y) || !std::isfinite(velocityXZ.x) ||
			!std::isfinite(velocityXZ.y))
			throw std::invalid_argument("Sled launch coordinates and velocity must be finite");

		const float halfBound = s_WorldExtent * 0.5f - 3.0f;
		const glm::vec2 clampedXZ = glm::clamp(originXZ, glm::vec2(-halfBound), glm::vec2(halfBound));
		glm::vec2 clampedVel = velocityXZ;
		const float speed = glm::length(clampedVel);
		if (speed > 35.0f)
			clampedVel *= (35.0f / speed);
		if (speed < 0.5f)
			clampedVel = {8.5f, 0.0f};

		// Pick matching user slot type (slots 2..9 alternate box sleds and sphere boulders)
		size_t chosenSlot = 0;
		for (size_t offset = 0; offset < s_MaxUserSleds; ++offset)
		{
			const size_t candidate = 2 + ((m_NextUserSlot + offset) % s_MaxUserSleds);
			if (m_DynamicSlots[candidate].IsBoulder == boulder)
			{
				chosenSlot = candidate;
				m_NextUserSlot = (m_NextUserSlot + offset + 1) % s_MaxUserSleds;
				break;
			}
		}
		if (chosenSlot == 0)
		{
			chosenSlot = 2 + (m_NextUserSlot % s_MaxUserSleds);
			m_NextUserSlot = (m_NextUserSlot + 1) % s_MaxUserSleds;
		}

		auto& slot = m_DynamicSlots[chosenSlot];
		if (slot.Entity != 0 && m_Scene.Exists(slot.Entity))
		{
			m_Scene.DestroyEntity(slot.Entity);
			slot.Entity = 0;
		}

		const float terrainY = EvaluateTerrainHeight(clampedXZ.x, clampedXZ.y);
		const float halfHeight = slot.IsBoulder ? 0.50f : 0.34f;
		EntityID id = m_Scene.CreateEntity(slot.IsBoulder ? "UserSnowBoulder" : "UserSnowSled");
		auto& entity = m_Scene.GetEntity(id);
		entity.Transform.Translation = {clampedXZ.x, terrainY + halfHeight + 0.08f, clampedXZ.y};
		entity.Physics.Enabled = true;
		entity.Physics.Shape = slot.IsBoulder ? PhysicsShape::Sphere : PhysicsShape::Box;
		entity.Physics.Motion = PhysicsMotion::Dynamic;
		entity.Physics.Radius = 0.50f;
		entity.Physics.HalfExtents = {0.68f, 0.32f, 0.46f};
		entity.Physics.Mass = slot.IsBoulder ? 45.0f : 55.0f;
		entity.Physics.Friction = 0.22f;
		entity.Physics.Restitution = slot.IsBoulder ? 0.25f : 0.12f;
		entity.Physics.SnowTracking = true;
		entity.Physics.SnowDrag = 5.0f;
		slot.Entity = id;

		m_Simulation->SyncFromScene();
		m_Simulation->SetEntityVelocity(id, {clampedVel.x, 0.0f, clampedVel.y});
		return id;
	}

	void SnowSandbox::SetRenderSettings(const RenderSettings& settings)
	{
		m_Scene.SetRenderSettings(settings);
	}

	const RenderSettings& SnowSandbox::GetRenderSettings() const
	{
		return m_Scene.GetRenderSettings();
	}

	void SnowSandbox::SetSnowfallIntensity(float intensity)
	{
		RenderSettings settings = m_Scene.GetRenderSettings();
		settings.SnowfallIntensity = std::clamp(intensity, 0.0f, 1.0f);
		m_Scene.SetRenderSettings(settings);
	}

	void SnowSandbox::CycleWindPreset(int direction)
	{
		static constexpr std::array<glm::vec3, 4> s_WindPresets = {{
			{0.5f, 0.0f, -0.2f},
			{3.8f, 0.0f, -1.4f},
			{-6.5f, 0.0f, 2.5f},
			{10.5f, 0.0f, -4.0f},
		}};
		if (direction >= 0)
			m_WindPresetIndex = (m_WindPresetIndex + 1) % s_WindPresets.size();
		else
			m_WindPresetIndex = (m_WindPresetIndex + s_WindPresets.size() - 1) % s_WindPresets.size();

		RenderSettings settings = m_Scene.GetRenderSettings();
		settings.WindVelocity = s_WindPresets[m_WindPresetIndex];
		m_Scene.SetRenderSettings(settings);
	}

	void SnowSandbox::CycleSparklePreset(int direction)
	{
		static constexpr std::array<std::pair<float, float>, 3> s_SparklePresets = {{
			{0.25f, 0.45f},
			{0.95f, 0.90f},
			{1.85f, 1.35f},
		}};
		if (direction >= 0)
			m_SparklePresetIndex = (m_SparklePresetIndex + 1) % s_SparklePresets.size();
		else
			m_SparklePresetIndex = (m_SparklePresetIndex + s_SparklePresets.size() - 1) % s_SparklePresets.size();

		RenderSettings settings = m_Scene.GetRenderSettings();
		settings.SnowSparkle = s_SparklePresets[m_SparklePresetIndex].first;
		settings.SnowSubsurface = s_SparklePresets[m_SparklePresetIndex].second;
		m_Scene.SetRenderSettings(settings);
	}

	void SnowSandbox::SetProbePosition(glm::vec2 worldXZ)
	{
		if (std::isfinite(worldXZ.x) && std::isfinite(worldXZ.y))
			m_ProbeXZ = worldXZ;
	}

	void SnowSandbox::SetProbeVisible(bool visible)
	{
		m_ProbeVisible = visible;
	}

	std::optional<glm::vec3> SnowSandbox::RaycastGround(const glm::vec3& rayOrigin,
														const glm::vec3& rayDisplacement) const
	{
		const auto hit = m_Simulation->GetPhysicsWorld().Raycast(rayOrigin, rayDisplacement);
		if (!hit.has_value())
			return std::nullopt;
		return hit->Position;
	}

	SnowSample SnowSandbox::SampleAt(glm::vec2 worldXZ) const
	{
		return m_Simulation->GetSnowField().Sample(worldXZ);
	}

	float SnowSandbox::GetAmbientCelsius() const
	{
		return m_Simulation->GetAmbientCelsius();
	}

	uint64_t SnowSandbox::GetSnowMutationCounter() const
	{
		return m_Simulation->GetSnowField().GetMutationCounter();
	}

	const SnowField& SnowSandbox::GetSnowField() const
	{
		return m_Simulation->GetSnowField();
	}

	std::vector<DynamicInstanceUpdate> SnowSandbox::GetDynamicInstances() const
	{
		std::vector<DynamicInstanceUpdate> updates;
		updates.reserve(m_DynamicSlots.size() + 1);
		for (const auto& slot : m_DynamicSlots)
		{
			if (slot.Entity != 0 && m_Scene.Exists(slot.Entity))
				updates.push_back({slot.PlacementIndex, m_Scene.GetEntity(slot.Entity).Transform.GetMatrix(), true});
			else
				updates.push_back({slot.PlacementIndex, glm::mat4(1.0f), false});
		}
		const float terrainY = EvaluateTerrainHeight(m_ProbeXZ.x, m_ProbeXZ.y);
		const float snowLift = m_Simulation->GetSnowField().Sample(m_ProbeXZ).EffectiveDepth * 0.65f;
		updates.push_back({m_ProbePlacementIndex,
						   glm::translate(glm::mat4(1.0f), {m_ProbeXZ.x, terrainY + snowLift + 0.08f, m_ProbeXZ.y}),
						   m_ProbeVisible});
		return updates;
	}

} // namespace Bellwether
