#include "SnowField.h"

#include "Scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace Bellwether
{

	namespace
	{
		constexpr std::array<uint8_t, 4> s_TrackMagic = {'B', 'W', 'S', 'N'};
		constexpr uint32_t s_TrackFormatVersion = 1;
		constexpr uint32_t s_MinResolution = 4;
		constexpr uint32_t s_MaxResolution = 1024;

		bool IsFiniteVec2(glm::vec2 value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y);
		}

		void ValidateConfig(const SnowFieldConfig& config)
		{
			if (config.Resolution < s_MinResolution || config.Resolution > s_MaxResolution)
				throw std::invalid_argument("SnowField resolution must be between 4 and 1024");
			if (!std::isfinite(config.WorldExtent) || config.WorldExtent <= 0.0f || config.WorldExtent > 100000.0f)
				throw std::invalid_argument("SnowField WorldExtent must be finite and in (0, 100000]");
			if (!std::isfinite(config.MaxDepth) || config.MaxDepth < 0.01f || config.MaxDepth > 100.0f)
				throw std::invalid_argument("SnowField MaxDepth must be finite and in [0.01, 100]");
			if (!std::isfinite(config.BaseDepth) || config.BaseDepth < 0.0f || config.BaseDepth > config.MaxDepth)
				throw std::invalid_argument("SnowField BaseDepth must be in [0, MaxDepth]");
			if (!std::isfinite(config.MinWalkableSpeedFactor) || config.MinWalkableSpeedFactor < 0.05f ||
				config.MinWalkableSpeedFactor > 1.0f)
				throw std::invalid_argument("SnowField MinWalkableSpeedFactor must be in [0.05, 1.0]");
			if (!std::isfinite(config.TrackHealRate) || config.TrackHealRate < 0.0f || config.TrackHealRate > 100.0f)
				throw std::invalid_argument("SnowField TrackHealRate must be in [0, 100]");
			if (!std::isfinite(config.WindDriftStrength) || config.WindDriftStrength < 0.0f ||
				config.WindDriftStrength > 100.0f)
				throw std::invalid_argument("SnowField WindDriftStrength must be in [0, 100]");
		}

		float DistanceToSegmentSq(glm::vec2 point, glm::vec2 start, glm::vec2 end)
		{
			glm::vec2 delta = end - start;
			float lenSq = glm::dot(delta, delta);
			if (lenSq <= 1e-10f)
			{
				glm::vec2 diff = point - start;
				return glm::dot(diff, diff);
			}
			float t = std::clamp(glm::dot(point - start, delta) / lenSq, 0.0f, 1.0f);
			glm::vec2 closest = start + delta * t;
			glm::vec2 diff = point - closest;
			return glm::dot(diff, diff);
		}

		uint32_t ComputeCRC32(const uint8_t* data, size_t size)
		{
			uint32_t crc = 0xffffffffu;
			for (size_t i = 0; i < size; i++)
			{
				crc ^= static_cast<uint32_t>(data[i]);
				for (int bit = 0; bit < 8; bit++)
					crc = (crc & 1u) ? ((crc >> 1) ^ 0xedb88320u) : (crc >> 1);
			}
			return crc ^ 0xffffffffu;
		}

		void AppendU32(std::vector<uint8_t>& out, uint32_t value)
		{
			for (int shift = 0; shift < 32; shift += 8)
				out.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
		}

		void AppendU64(std::vector<uint8_t>& out, uint64_t value)
		{
			for (int shift = 0; shift < 64; shift += 8)
				out.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
		}

		void AppendF32(std::vector<uint8_t>& out, float value)
		{
			uint32_t bits = 0;
			std::memcpy(&bits, &value, sizeof(bits));
			AppendU32(out, bits);
		}

		uint32_t ReadU32(const uint8_t* ptr)
		{
			uint32_t value = 0;
			for (int i = 0; i < 4; i++)
				value |= static_cast<uint32_t>(ptr[i]) << (i * 8);
			return value;
		}

		uint64_t ReadU64(const uint8_t* ptr)
		{
			uint64_t value = 0;
			for (int i = 0; i < 8; i++)
				value |= static_cast<uint64_t>(ptr[i]) << (i * 8);
			return value;
		}

		float ReadF32(const uint8_t* ptr)
		{
			uint32_t bits = ReadU32(ptr);
			float value = 0.0f;
			std::memcpy(&value, &bits, sizeof(value));
			return value;
		}

		float SmoothStep(float edge0, float edge1, float x)
		{
			if (edge1 <= edge0)
				return x >= edge1 ? 1.0f : 0.0f;
			float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		}
	} // namespace

	SnowField::SnowField()
	{
		Reset(SnowFieldConfig{});
	}

	SnowField::SnowField(const SnowFieldConfig& config)
	{
		Reset(config);
	}

	void SnowField::Reset(const SnowFieldConfig& config)
	{
		ValidateConfig(config);
		m_Config = config;
		SnowCell initial{};
		initial.BaseDepth = m_Config.BaseDepth;
		m_Cells.assign(static_cast<size_t>(m_Config.Resolution) * m_Config.Resolution, initial);
		m_MutationCounter = 0;
	}

	float SnowField::GetCellWorldSize() const
	{
		return m_Config.WorldExtent / static_cast<float>(m_Config.Resolution - 1);
	}

	void SnowField::InitializeFromHeightfield(const std::vector<float>& heights, uint32_t heightGridSize,
											  float slopeCutoff, float timberlineHeight)
	{
		if (heightGridSize < 2 || heightGridSize > 8192 ||
			heights.size() != static_cast<size_t>(heightGridSize) * heightGridSize)
			throw std::invalid_argument("Invalid terrain heightfield dimensions for SnowField");
		if (!std::isfinite(slopeCutoff) || slopeCutoff < 0.0f || slopeCutoff > 1.0f || !std::isfinite(timberlineHeight))
			throw std::invalid_argument("Invalid snow heightfield slope or timberline parameter");
		for (float h : heights)
			if (!std::isfinite(h))
				throw std::invalid_argument("Terrain heightfield contains non-finite elevation");

		auto sampleHeight = [&](float u, float v)
		{
			float fx = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(heightGridSize - 1);
			float fz = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(heightGridSize - 1);
			uint32_t x0 = std::min(static_cast<uint32_t>(std::floor(fx)), heightGridSize - 1);
			uint32_t z0 = std::min(static_cast<uint32_t>(std::floor(fz)), heightGridSize - 1);
			uint32_t x1 = std::min(x0 + 1, heightGridSize - 1);
			uint32_t z1 = std::min(z0 + 1, heightGridSize - 1);
			float tx = fx - static_cast<float>(x0);
			float tz = fz - static_cast<float>(z0);
			float h00 = heights[static_cast<size_t>(z0) * heightGridSize + x0];
			float h10 = heights[static_cast<size_t>(z0) * heightGridSize + x1];
			float h01 = heights[static_cast<size_t>(z1) * heightGridSize + x0];
			float h11 = heights[static_cast<size_t>(z1) * heightGridSize + x1];
			return glm::mix(glm::mix(h00, h10, tx), glm::mix(h01, h11, tx), tz);
		};

		float uvStep = 1.0f / static_cast<float>(heightGridSize - 1);
		float normalScale = 2.0f * m_Config.WorldExtent / static_cast<float>(heightGridSize - 1);
		std::vector<SnowCell> next(m_Cells.size());
		for (uint32_t z = 0; z < m_Config.Resolution; z++)
		{
			float v = static_cast<float>(z) / static_cast<float>(m_Config.Resolution - 1);
			for (uint32_t x = 0; x < m_Config.Resolution; x++)
			{
				float u = static_cast<float>(x) / static_cast<float>(m_Config.Resolution - 1);
				float h = sampleHeight(u, v);
				glm::vec3 normal =
					glm::normalize(glm::vec3(sampleHeight(u - uvStep, v) - sampleHeight(u + uvStep, v), normalScale,
											 sampleHeight(u, v - uvStep) - sampleHeight(u, v + uvStep)));
				float slopeWeight = SmoothStep(slopeCutoff * 0.65f, std::min(1.0f, slopeCutoff + 0.16f), normal.y);
				float altWeight = SmoothStep(timberlineHeight - 4.0f, timberlineHeight + 6.0f, h);
				SnowCell cell{};
				cell.BaseDepth = std::clamp(m_Config.BaseDepth * slopeWeight * altWeight, 0.0f, m_Config.MaxDepth);
				if (normal.y < 0.88f && altWeight > 0.25f)
					cell.Ice = std::clamp((0.88f - normal.y) * 1.4f, 0.0f, 0.45f);
				next[static_cast<size_t>(z) * m_Config.Resolution + x] = cell;
			}
		}
		m_Cells = std::move(next);
		m_MutationCounter++;
	}

	void SnowField::LoadExportedBytes(const std::vector<uint8_t>& depthBytes, const std::vector<uint8_t>& iceBytes,
									  uint32_t gridSize)
	{
		if (gridSize < 2 || gridSize > 4096 || depthBytes.size() != static_cast<size_t>(gridSize) * gridSize)
			throw std::invalid_argument("Invalid exported snow depth byte buffer");
		if (!iceBytes.empty() && iceBytes.size() != depthBytes.size())
			throw std::invalid_argument("Exported ice buffer size does not match depth buffer");

		auto sampleU8 = [&](const std::vector<uint8_t>& bytes, float u, float v)
		{
			float fx = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(gridSize - 1);
			float fz = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(gridSize - 1);
			uint32_t x0 = std::min(static_cast<uint32_t>(std::floor(fx)), gridSize - 1);
			uint32_t z0 = std::min(static_cast<uint32_t>(std::floor(fz)), gridSize - 1);
			uint32_t x1 = std::min(x0 + 1, gridSize - 1);
			uint32_t z1 = std::min(z0 + 1, gridSize - 1);
			float tx = fx - static_cast<float>(x0);
			float tz = fz - static_cast<float>(z0);
			float a = bytes[static_cast<size_t>(z0) * gridSize + x0] / 255.0f;
			float b = bytes[static_cast<size_t>(z0) * gridSize + x1] / 255.0f;
			float c = bytes[static_cast<size_t>(z1) * gridSize + x0] / 255.0f;
			float d = bytes[static_cast<size_t>(z1) * gridSize + x1] / 255.0f;
			return glm::mix(glm::mix(a, b, tx), glm::mix(c, d, tx), tz);
		};

		std::vector<SnowCell> next(m_Cells.size());
		for (uint32_t z = 0; z < m_Config.Resolution; z++)
		{
			float v = static_cast<float>(z) / static_cast<float>(m_Config.Resolution - 1);
			for (uint32_t x = 0; x < m_Config.Resolution; x++)
			{
				float u = static_cast<float>(x) / static_cast<float>(m_Config.Resolution - 1);
				SnowCell cell{};
				cell.BaseDepth = sampleU8(depthBytes, u, v) * m_Config.MaxDepth;
				if (!iceBytes.empty())
					cell.Ice = sampleU8(iceBytes, u, v);
				next[static_cast<size_t>(z) * m_Config.Resolution + x] = cell;
			}
		}
		m_Cells = std::move(next);
		m_MutationCounter++;
	}

	bool SnowField::StampCircle(glm::vec2 worldXZ, float radius, float pressureDepth, float rimFactor)
	{
		return StampSegment(worldXZ, worldXZ, radius, pressureDepth, rimFactor);
	}

	bool SnowField::StampSegment(glm::vec2 startXZ, glm::vec2 endXZ, float radius, float pressureDepth, float rimFactor)
	{
		if (!IsFiniteVec2(startXZ) || !IsFiniteVec2(endXZ))
			throw std::invalid_argument("Snow stamp coordinates must be finite");
		if (!std::isfinite(radius) || radius <= 0.0f || radius > m_Config.WorldExtent)
			throw std::invalid_argument("Snow stamp radius must be positive and within WorldExtent");
		if (!std::isfinite(pressureDepth) || pressureDepth < 0.0f || pressureDepth > m_Config.MaxDepth * 4.0f)
			throw std::invalid_argument("Snow stamp pressureDepth must be finite and non-negative");
		if (!std::isfinite(rimFactor) || rimFactor < 0.0f || rimFactor > 1.0f)
			throw std::invalid_argument("Snow stamp rimFactor must be in [0, 1]");
		if (pressureDepth == 0.0f)
			return false;

		float halfExtent = m_Config.WorldExtent * 0.5f;
		float outerRadius = radius * 1.48f;
		glm::vec2 minWorld = glm::min(startXZ, endXZ) - glm::vec2(outerRadius);
		glm::vec2 maxWorld = glm::max(startXZ, endXZ) + glm::vec2(outerRadius);
		if (maxWorld.x < -halfExtent || minWorld.x > halfExtent || maxWorld.y < -halfExtent || minWorld.y > halfExtent)
			return false;

		float cellSize = GetCellWorldSize();
		int maxIndex = static_cast<int>(m_Config.Resolution - 1);
		int minX = std::clamp(static_cast<int>(std::floor((minWorld.x + halfExtent) / cellSize)), 0, maxIndex);
		int maxX = std::clamp(static_cast<int>(std::ceil((maxWorld.x + halfExtent) / cellSize)), 0, maxIndex);
		int minZ = std::clamp(static_cast<int>(std::floor((minWorld.y + halfExtent) / cellSize)), 0, maxIndex);
		int maxZ = std::clamp(static_cast<int>(std::ceil((maxWorld.y + halfExtent) / cellSize)), 0, maxIndex);

		float radiusSq = radius * radius;
		float outerRadiusSq = outerRadius * outerRadius;
		float displacedVolume = 0.0f;
		bool changed = false;

		struct RimCell
		{
			size_t Index;
			float Weight;
		};
		std::vector<RimCell> rimCells;
		float rimWeightSum = 0.0f;

		for (int z = minZ; z <= maxZ; z++)
		{
			float wz = -halfExtent + static_cast<float>(z) * cellSize;
			for (int x = minX; x <= maxX; x++)
			{
				float wx = -halfExtent + static_cast<float>(x) * cellSize;
				float distSq = DistanceToSegmentSq({wx, wz}, startXZ, endXZ);
				if (distSq >= outerRadiusSq)
					continue;
				size_t index = static_cast<size_t>(z) * m_Config.Resolution + static_cast<size_t>(x);
				float dist = std::sqrt(distSq);
				if (dist < radius)
				{
					SnowCell& cell = m_Cells[index];
					if (cell.Ice > 0.92f)
						continue;
					float t = 1.0f - (dist / radius);
					float kernel = t * t * (3.0f - 2.0f * t);
					float iceResist = 1.0f - cell.Ice * 0.85f;
					float targetComp = std::min(cell.BaseDepth, pressureDepth * kernel * iceResist);
					float addedComp = std::max(0.0f, targetComp - cell.Compression);
					float crushedDisp = cell.Displacement * kernel;
					if (addedComp > 1e-6f || crushedDisp > 1e-6f)
					{
						cell.Compression += addedComp;
						cell.Displacement = std::max(0.0f, cell.Displacement - crushedDisp);
						displacedVolume += addedComp + crushedDisp;
						changed = true;
					}
				}
				else if (rimFactor > 0.0f)
				{
					float ringT = (dist - radius) / std::max(1e-5f, outerRadius - radius);
					float sine = std::sin(ringT * 3.14159265f);
					float weight = sine * sine;
					if (weight > 1e-4f)
					{
						rimCells.push_back({index, weight});
						rimWeightSum += weight;
					}
				}
				(void)radiusSq;
			}
		}

		if (rimFactor > 0.0f && displacedVolume > 1e-6f && rimWeightSum > 1e-6f)
		{
			float maxDisp = m_Config.MaxDepth * 0.45f;
			float perWeight = (displacedVolume * rimFactor) / rimWeightSum;
			for (const auto& rim : rimCells)
			{
				SnowCell& cell = m_Cells[rim.Index];
				if (cell.BaseDepth <= 0.01f)
					continue;
				float updated = std::min(maxDisp, cell.Displacement + perWeight * rim.Weight);
				if (updated > cell.Displacement)
				{
					cell.Displacement = updated;
					changed = true;
				}
			}
		}

		if (changed)
			m_MutationCounter++;
		return changed;
	}

	bool SnowField::ApplyHeatSource(glm::vec2 worldXZ, float radius, float heatPower, float dt)
	{
		if (!IsFiniteVec2(worldXZ))
			throw std::invalid_argument("Heat source position must be finite");
		if (!std::isfinite(radius) || radius <= 0.0f || radius > m_Config.WorldExtent)
			throw std::invalid_argument("Heat source radius must be positive and within WorldExtent");
		if (!std::isfinite(heatPower) || heatPower < 0.0f || !std::isfinite(dt) || dt < 0.0f)
			throw std::invalid_argument("Heat power and timestep must be finite and non-negative");
		if (heatPower == 0.0f || dt == 0.0f)
			return false;

		float halfExtent = m_Config.WorldExtent * 0.5f;
		if (worldXZ.x + radius < -halfExtent || worldXZ.x - radius > halfExtent || worldXZ.y + radius < -halfExtent ||
			worldXZ.y - radius > halfExtent)
			return false;

		float cellSize = GetCellWorldSize();
		int maxIndex = static_cast<int>(m_Config.Resolution - 1);
		int minX = std::clamp(static_cast<int>(std::floor((worldXZ.x - radius + halfExtent) / cellSize)), 0, maxIndex);
		int maxX = std::clamp(static_cast<int>(std::ceil((worldXZ.x + radius + halfExtent) / cellSize)), 0, maxIndex);
		int minZ = std::clamp(static_cast<int>(std::floor((worldXZ.y - radius + halfExtent) / cellSize)), 0, maxIndex);
		int maxZ = std::clamp(static_cast<int>(std::ceil((worldXZ.y + radius + halfExtent) / cellSize)), 0, maxIndex);

		float radiusSq = radius * radius;
		bool changed = false;
		for (int z = minZ; z <= maxZ; z++)
		{
			float wz = -halfExtent + static_cast<float>(z) * cellSize;
			for (int x = minX; x <= maxX; x++)
			{
				float wx = -halfExtent + static_cast<float>(x) * cellSize;
				glm::vec2 d = glm::vec2(wx, wz) - worldXZ;
				float distSq = glm::dot(d, d);
				if (distSq >= radiusSq)
					continue;
				float t = 1.0f - distSq / radiusSq;
				float kernel = t * t;
				float melt = heatPower * kernel * dt;
				SnowCell& cell = m_Cells[static_cast<size_t>(z) * m_Config.Resolution + static_cast<size_t>(x)];
				float prevBase = cell.BaseDepth;
				float prevIce = cell.Ice;
				float prevDisp = cell.Displacement;

				float iceMelt = std::min(cell.Ice, melt * 0.8f);
				cell.Ice = std::max(0.0f, cell.Ice - iceMelt);

				float dispMelt = std::min(cell.Displacement, melt * 0.6f);
				cell.Displacement = std::max(0.0f, cell.Displacement - dispMelt);

				float remainingMelt = std::max(0.0f, melt - dispMelt * 0.5f);
				float snowMelt = std::min(cell.BaseDepth, remainingMelt);
				cell.BaseDepth = std::max(0.0f, cell.BaseDepth - snowMelt);
				cell.Compression = std::min(cell.Compression, cell.BaseDepth);

				if (snowMelt > 1e-6f || iceMelt > 1e-6f)
					cell.Moisture = std::clamp(cell.Moisture + (snowMelt + iceMelt) * 0.85f, 0.0f, 1.0f);
				if (cell.BaseDepth <= 0.005f)
					cell.Moisture = std::max(0.0f, cell.Moisture - melt * 0.35f);

				if (cell.BaseDepth != prevBase || cell.Ice != prevIce || cell.Displacement != prevDisp)
					changed = true;
			}
		}

		if (changed)
			m_MutationCounter++;
		return changed;
	}

	void SnowField::StepWeather(float dt, float snowfallRate, glm::vec2 windVelocity, float ambientCelsius)
	{
		if (!std::isfinite(dt) || dt < 0.0f || dt > 3600.0f)
			throw std::invalid_argument("SnowField weather step dt must be in [0, 3600]");
		if (!std::isfinite(snowfallRate) || snowfallRate < 0.0f || snowfallRate > 10.0f)
			throw std::invalid_argument("SnowField snowfallRate must be in [0, 10]");
		if (!IsFiniteVec2(windVelocity) || glm::length(windVelocity) > 200.0f)
			throw std::invalid_argument("SnowField windVelocity must be finite and <= 200 m/s");
		if (!std::isfinite(ambientCelsius) || ambientCelsius < -100.0f || ambientCelsius > 100.0f)
			throw std::invalid_argument("SnowField ambientCelsius must be in [-100, 100]");
		if (dt == 0.0f)
			return;

		float windSpeed = glm::length(windVelocity);
		float cellSize = GetCellWorldSize();
		float heal = (snowfallRate * 0.65f + m_Config.TrackHealRate * (0.25f + windSpeed * 0.12f)) * dt;
		float accumulate = snowfallRate * dt;
		float warmMelt = ambientCelsius > 0.0f ? ambientCelsius * 0.004f * dt : 0.0f;
		float freezeRate = ambientCelsius < -1.5f ? (-ambientCelsius - 1.5f) * 0.015f * dt : 0.0f;

		std::vector<SnowCell> next = m_Cells;
		uint32_t res = m_Config.Resolution;
		glm::vec2 windDir = windSpeed > 1e-4f ? windVelocity / windSpeed : glm::vec2(0.0f);
		float advectAlpha =
			windSpeed > 0.01f
				? std::clamp(windSpeed * m_Config.WindDriftStrength * dt / std::max(0.25f, cellSize), 0.0f, 0.38f)
				: 0.0f;

		auto sampleOld = [&](float fx, float fz) -> SnowCell
		{
			fx = std::clamp(fx, 0.0f, static_cast<float>(res - 1));
			fz = std::clamp(fz, 0.0f, static_cast<float>(res - 1));
			uint32_t x0 = std::min(static_cast<uint32_t>(std::floor(fx)), res - 1);
			uint32_t z0 = std::min(static_cast<uint32_t>(std::floor(fz)), res - 1);
			uint32_t x1 = std::min(x0 + 1, res - 1);
			uint32_t z1 = std::min(z0 + 1, res - 1);
			float tx = fx - static_cast<float>(x0);
			float tz = fz - static_cast<float>(z0);
			const SnowCell& c00 = m_Cells[static_cast<size_t>(z0) * res + x0];
			const SnowCell& c10 = m_Cells[static_cast<size_t>(z0) * res + x1];
			const SnowCell& c01 = m_Cells[static_cast<size_t>(z1) * res + x0];
			const SnowCell& c11 = m_Cells[static_cast<size_t>(z1) * res + x1];
			auto lerpField = [&](auto member)
			{
				float a = c00.*member + (c10.*member - c00.*member) * tx;
				float b = c01.*member + (c11.*member - c01.*member) * tx;
				return a + (b - a) * tz;
			};
			return SnowCell{lerpField(&SnowCell::BaseDepth), lerpField(&SnowCell::Compression),
							lerpField(&SnowCell::Displacement), lerpField(&SnowCell::Ice),
							lerpField(&SnowCell::Moisture)};
		};

		for (uint32_t z = 0; z < res; z++)
		{
			for (uint32_t x = 0; x < res; x++)
			{
				size_t idx = static_cast<size_t>(z) * res + x;
				SnowCell cell = m_Cells[idx];

				if (advectAlpha > 0.0f)
				{
					SnowCell upwind = sampleOld(static_cast<float>(x) - windDir.x, static_cast<float>(z) - windDir.y);
					// Wind carries loose upper powder & softens track compression/berms along drift direction.
					cell.BaseDepth = glm::mix(cell.BaseDepth, upwind.BaseDepth, advectAlpha * 0.35f);
					cell.Compression = glm::mix(cell.Compression, upwind.Compression, advectAlpha * 0.45f);
					cell.Displacement = glm::mix(cell.Displacement, upwind.Displacement, advectAlpha * 0.55f);
				}

				if (accumulate > 0.0f)
				{
					cell.BaseDepth = std::min(m_Config.MaxDepth, cell.BaseDepth + accumulate);
					cell.Ice = std::max(0.0f, cell.Ice - accumulate * 0.35f);
					cell.Moisture = std::max(0.0f, cell.Moisture - accumulate * 0.25f);
				}

				if (heal > 0.0f && (accumulate > 0.0f || windSpeed > 0.25f))
				{
					cell.Compression = std::max(0.0f, cell.Compression - heal);
					cell.Displacement = std::max(0.0f, cell.Displacement - heal * 1.15f);
				}

				if (warmMelt > 0.0f)
				{
					float melted = std::min(cell.BaseDepth, warmMelt);
					cell.BaseDepth -= melted;
					cell.Displacement = std::max(0.0f, cell.Displacement - warmMelt);
					cell.Ice = std::max(0.0f, cell.Ice - warmMelt * 1.5f);
					cell.Moisture = std::clamp(cell.Moisture + melted * 0.8f, 0.0f, 1.0f);
				}
				else if (freezeRate > 0.0f && cell.Moisture > 0.0f)
				{
					float refreeze = std::min(cell.Moisture, freezeRate);
					cell.Moisture -= refreeze;
					cell.Ice = std::min(1.0f, cell.Ice + refreeze * 0.7f);
				}

				cell.BaseDepth = std::clamp(cell.BaseDepth, 0.0f, m_Config.MaxDepth);
				cell.Compression = std::clamp(cell.Compression, 0.0f, cell.BaseDepth);
				cell.Displacement = std::clamp(cell.Displacement, 0.0f, m_Config.MaxDepth * 0.45f);
				cell.Ice = std::clamp(cell.Ice, 0.0f, 1.0f);
				cell.Moisture = std::clamp(cell.Moisture, 0.0f, 1.0f);
				next[idx] = cell;
			}
		}

		m_Cells = std::move(next);
		m_MutationCounter++;
	}

	SnowSample SnowField::Sample(glm::vec2 worldXZ) const
	{
		if (!IsFiniteVec2(worldXZ))
			throw std::invalid_argument("Snow sample coordinates must be finite");

		float halfExtent = m_Config.WorldExtent * 0.5f;
		float u = std::clamp((worldXZ.x + halfExtent) / m_Config.WorldExtent, 0.0f, 1.0f);
		float v = std::clamp((worldXZ.y + halfExtent) / m_Config.WorldExtent, 0.0f, 1.0f);
		float fx = u * static_cast<float>(m_Config.Resolution - 1);
		float fz = v * static_cast<float>(m_Config.Resolution - 1);
		uint32_t x0 = std::min(static_cast<uint32_t>(std::floor(fx)), m_Config.Resolution - 1);
		uint32_t z0 = std::min(static_cast<uint32_t>(std::floor(fz)), m_Config.Resolution - 1);
		uint32_t x1 = std::min(x0 + 1, m_Config.Resolution - 1);
		uint32_t z1 = std::min(z0 + 1, m_Config.Resolution - 1);
		float tx = fx - static_cast<float>(x0);
		float tz = fz - static_cast<float>(z0);

		const SnowCell& c00 = m_Cells[static_cast<size_t>(z0) * m_Config.Resolution + x0];
		const SnowCell& c10 = m_Cells[static_cast<size_t>(z0) * m_Config.Resolution + x1];
		const SnowCell& c01 = m_Cells[static_cast<size_t>(z1) * m_Config.Resolution + x0];
		const SnowCell& c11 = m_Cells[static_cast<size_t>(z1) * m_Config.Resolution + x1];

		auto interp = [&](auto member)
		{
			float a = c00.*member + (c10.*member - c00.*member) * tx;
			float b = c01.*member + (c11.*member - c01.*member) * tx;
			return a + (b - a) * tz;
		};

		SnowSample sample{};
		sample.BaseDepth = interp(&SnowCell::BaseDepth);
		sample.Compression = interp(&SnowCell::Compression);
		sample.Displacement = interp(&SnowCell::Displacement);
		sample.Ice = interp(&SnowCell::Ice);
		sample.Moisture = interp(&SnowCell::Moisture);
		sample.EffectiveDepth =
			std::clamp(sample.BaseDepth - sample.Compression + sample.Displacement, 0.0f, m_Config.MaxDepth);
		sample.Coverage = std::clamp(sample.EffectiveDepth / 0.10f, 0.0f, 1.0f);

		// Loose depth creates sinkage drag; previously compressed tracks pack the base and recover mobility.
		float looseSink = std::max(0.0f, sample.BaseDepth - sample.Compression * 1.1f);
		float sinkFactor = std::clamp(looseSink / std::max(0.25f, m_Config.MaxDepth * 0.45f), 0.0f, 1.0f);
		float trailBonus = std::clamp(sample.Compression / std::max(0.08f, m_Config.BaseDepth), 0.0f, 1.0f) * 0.08f;
		sample.SpeedFactor = std::clamp(glm::mix(1.0f, m_Config.MinWalkableSpeedFactor, sinkFactor) + trailBonus,
										m_Config.MinWalkableSpeedFactor, 1.04f);
		sample.FrictionFactor = std::clamp(glm::mix(0.95f, 0.16f, sample.Ice) - sample.Moisture * 0.12f, 0.12f, 1.0f);
		return sample;
	}

	const SnowCell& SnowField::GetCell(uint32_t x, uint32_t z) const
	{
		if (x >= m_Config.Resolution || z >= m_Config.Resolution)
			throw std::out_of_range("SnowField cell coordinates out of range");
		return m_Cells[static_cast<size_t>(z) * m_Config.Resolution + x];
	}

	void SnowField::SetCell(uint32_t x, uint32_t z, const SnowCell& cell)
	{
		if (x >= m_Config.Resolution || z >= m_Config.Resolution)
			throw std::out_of_range("SnowField cell coordinates out of range");
		if (!std::isfinite(cell.BaseDepth) || cell.BaseDepth < 0.0f || cell.BaseDepth > m_Config.MaxDepth ||
			!std::isfinite(cell.Compression) || cell.Compression < 0.0f || cell.Compression > cell.BaseDepth ||
			!std::isfinite(cell.Displacement) || cell.Displacement < 0.0f ||
			cell.Displacement > m_Config.MaxDepth * 0.5f || !std::isfinite(cell.Ice) || cell.Ice < 0.0f ||
			cell.Ice > 1.0f || !std::isfinite(cell.Moisture) || cell.Moisture < 0.0f || cell.Moisture > 1.0f)
			throw std::invalid_argument("Invalid SnowCell state");
		m_Cells[static_cast<size_t>(z) * m_Config.Resolution + x] = cell;
		m_MutationCounter++;
	}

	void SnowField::PackRGBA8(std::vector<uint8_t>& outPixels) const
	{
		outPixels.resize(m_Cells.size() * 4);
		float invMax = 1.0f / m_Config.MaxDepth;
		for (size_t i = 0; i < m_Cells.size(); i++)
		{
			const SnowCell& cell = m_Cells[i];
			float effective =
				std::clamp(cell.BaseDepth - cell.Compression + cell.Displacement, 0.0f, m_Config.MaxDepth);
			float coverage = std::clamp(effective / 0.10f, 0.0f, 1.0f);
			outPixels[i * 4 + 0] = static_cast<uint8_t>(std::clamp(std::lround(effective * invMax * 255.0f), 0l, 255l));
			outPixels[i * 4 + 1] =
				static_cast<uint8_t>(std::clamp(std::lround(cell.Compression * invMax * 255.0f), 0l, 255l));
			outPixels[i * 4 + 2] = static_cast<uint8_t>(std::clamp(std::lround(cell.Ice * 255.0f), 0l, 255l));
			outPixels[i * 4 + 3] = static_cast<uint8_t>(std::clamp(std::lround(coverage * 255.0f), 0l, 255l));
		}
	}

	std::vector<uint8_t> SnowField::SerializeBinary() const
	{
		constexpr size_t headerBytes = 4 + 4 + 4 + 6 * 4 + 8;
		size_t cellBytes = m_Cells.size() * 5 * sizeof(float);
		std::vector<uint8_t> buffer;
		buffer.reserve(headerBytes + cellBytes + 4);
		buffer.insert(buffer.end(), s_TrackMagic.begin(), s_TrackMagic.end());
		AppendU32(buffer, s_TrackFormatVersion);
		AppendU32(buffer, m_Config.Resolution);
		AppendF32(buffer, m_Config.WorldExtent);
		AppendF32(buffer, m_Config.BaseDepth);
		AppendF32(buffer, m_Config.MaxDepth);
		AppendF32(buffer, m_Config.MinWalkableSpeedFactor);
		AppendF32(buffer, m_Config.TrackHealRate);
		AppendF32(buffer, m_Config.WindDriftStrength);
		AppendU64(buffer, m_MutationCounter);
		for (const auto& cell : m_Cells)
		{
			AppendF32(buffer, cell.BaseDepth);
			AppendF32(buffer, cell.Compression);
			AppendF32(buffer, cell.Displacement);
			AppendF32(buffer, cell.Ice);
			AppendF32(buffer, cell.Moisture);
		}
		uint32_t crc = ComputeCRC32(buffer.data(), buffer.size());
		AppendU32(buffer, crc);
		return buffer;
	}

	void SnowField::DeserializeBinary(const std::vector<uint8_t>& bytes)
	{
		constexpr size_t headerBytes = 4 + 4 + 4 + 6 * 4 + 8;
		if (bytes.size() < headerBytes + 4)
			throw std::runtime_error("Snow track binary data is truncated");
		if (!std::equal(s_TrackMagic.begin(), s_TrackMagic.end(), bytes.begin()))
			throw std::runtime_error("Invalid snow track file header");

		uint32_t expectedCrc = ReadU32(bytes.data() + bytes.size() - 4);
		uint32_t actualCrc = ComputeCRC32(bytes.data(), bytes.size() - 4);
		if (expectedCrc != actualCrc)
			throw std::runtime_error("Snow track checksum mismatch (corrupted data)");

		uint32_t version = ReadU32(bytes.data() + 4);
		if (version != s_TrackFormatVersion)
			throw std::runtime_error("Unsupported snow track binary format version");

		SnowFieldConfig config{};
		config.Resolution = ReadU32(bytes.data() + 8);
		config.WorldExtent = ReadF32(bytes.data() + 12);
		config.BaseDepth = ReadF32(bytes.data() + 16);
		config.MaxDepth = ReadF32(bytes.data() + 20);
		config.MinWalkableSpeedFactor = ReadF32(bytes.data() + 24);
		config.TrackHealRate = ReadF32(bytes.data() + 28);
		config.WindDriftStrength = ReadF32(bytes.data() + 32);
		ValidateConfig(config);

		uint64_t mutationCounter = ReadU64(bytes.data() + 36);
		size_t cellCount = static_cast<size_t>(config.Resolution) * config.Resolution;
		size_t expectedSize = headerBytes + cellCount * 5 * 4 + 4;
		if (bytes.size() != expectedSize)
			throw std::runtime_error("Snow track payload length does not match grid dimensions");

		std::vector<SnowCell> cells(cellCount);
		const uint8_t* ptr = bytes.data() + headerBytes;
		for (size_t i = 0; i < cellCount; i++)
		{
			SnowCell& cell = cells[i];
			cell.BaseDepth = ReadF32(ptr + 0);
			cell.Compression = ReadF32(ptr + 4);
			cell.Displacement = ReadF32(ptr + 8);
			cell.Ice = ReadF32(ptr + 12);
			cell.Moisture = ReadF32(ptr + 16);
			ptr += 20;
			if (!std::isfinite(cell.BaseDepth) || cell.BaseDepth < 0.0f || cell.BaseDepth > config.MaxDepth ||
				!std::isfinite(cell.Compression) || cell.Compression < 0.0f ||
				cell.Compression > cell.BaseDepth + 1e-4f || !std::isfinite(cell.Displacement) ||
				cell.Displacement < 0.0f || cell.Displacement > config.MaxDepth * 0.5f || !std::isfinite(cell.Ice) ||
				cell.Ice < 0.0f || cell.Ice > 1.0f || !std::isfinite(cell.Moisture) || cell.Moisture < 0.0f ||
				cell.Moisture > 1.0f)
				throw std::runtime_error("Snow track contains corrupted or out-of-range cell values");
			cell.Compression = std::min(cell.Compression, cell.BaseDepth);
		}

		m_Config = config;
		m_Cells = std::move(cells);
		m_MutationCounter = mutationCounter;
	}

	void SnowField::SaveTracks(const std::filesystem::path& path) const
	{
		auto bytes = SerializeBinary();
		std::string payload(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		AtomicWrite(path, payload);
	}

	void SnowField::LoadTracks(const std::filesystem::path& path)
	{
		if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path))
			throw std::runtime_error("Snow track file does not exist or is not a regular file: " + path.string());
		auto fileSize = std::filesystem::file_size(path);
		if (fileSize > 64 * 1024 * 1024)
			throw std::runtime_error("Snow track file exceeds 64 MiB limit");
		std::ifstream input(path, std::ios::binary);
		if (!input)
			throw std::runtime_error("Cannot open snow track file: " + path.string());
		std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
		if (fileSize > 0 && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(fileSize)))
			throw std::runtime_error("Failed to read snow track file: " + path.string());
		DeserializeBinary(bytes);
	}

	SnowField SnowField::Snapshot() const
	{
		return *this;
	}

	void SnowField::Restore(const SnowField& snapshot)
	{
		ValidateConfig(snapshot.m_Config);
		if (snapshot.m_Cells.size() != static_cast<size_t>(snapshot.m_Config.Resolution) * snapshot.m_Config.Resolution)
			throw std::invalid_argument("Invalid SnowField snapshot cell count");
		*this = snapshot;
	}

	uint32_t SnowField::GetActiveTrackCount() const
	{
		uint32_t count = 0;
		for (const auto& cell : m_Cells)
			if (cell.Compression > 0.01f)
				count++;
		return count;
	}

	float SnowField::GetAverageDepth() const
	{
		if (m_Cells.empty())
			return 0.0f;
		double total = 0.0;
		for (const auto& cell : m_Cells)
			total += static_cast<double>(
				std::clamp(cell.BaseDepth - cell.Compression + cell.Displacement, 0.0f, m_Config.MaxDepth));
		return static_cast<float>(total / static_cast<double>(m_Cells.size()));
	}

} // namespace Bellwether
