#pragma once

#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <vector>

namespace Bellwether
{

	struct SnowFieldConfig
	{
		uint32_t Resolution = 128;
		float WorldExtent = 384.0f;
		float BaseDepth = 0.45f;
		float MaxDepth = 2.0f;
		float MinWalkableSpeedFactor = 0.48f;
		float TrackHealRate = 0.08f;
		float WindDriftStrength = 0.25f;
		bool operator==(const SnowFieldConfig&) const = default;
	};

	struct SnowCell
	{
		float BaseDepth = 0.0f;
		float Compression = 0.0f;
		float Displacement = 0.0f;
		float Ice = 0.0f;
		float Moisture = 0.0f;
		bool operator==(const SnowCell&) const = default;
	};

	struct SnowSample
	{
		float BaseDepth = 0.0f;
		float EffectiveDepth = 0.0f;
		float Coverage = 0.0f;
		float Compression = 0.0f;
		float Displacement = 0.0f;
		float Ice = 0.0f;
		float Moisture = 0.0f;
		float SpeedFactor = 1.0f;
		float FrictionFactor = 1.0f;
	};

	class SnowField
	{
	  public:
		SnowField();
		explicit SnowField(const SnowFieldConfig& config);

		static void ValidateConfig(const SnowFieldConfig& config);
		void Reset(const SnowFieldConfig& config);
		void InitializeFromHeightfield(const std::vector<float>& heights, uint32_t heightGridSize,
									   float slopeCutoff = 0.68f, float timberlineHeight = -1000.0f);
		void LoadExportedBytes(const std::vector<uint8_t>& depthBytes, const std::vector<uint8_t>& iceBytes,
							   uint32_t gridSize);

		bool StampCircle(glm::vec2 worldXZ, float radius, float pressureDepth, float rimFactor = 0.35f);
		bool StampSegment(glm::vec2 startXZ, glm::vec2 endXZ, float radius, float pressureDepth,
						  float rimFactor = 0.35f);
		bool ApplyHeatSource(glm::vec2 worldXZ, float radius, float heatPower, float dt);
		void StepWeather(float dt, float snowfallRate, glm::vec2 windVelocity, float ambientCelsius = -6.0f);

		SnowSample Sample(glm::vec2 worldXZ) const;
		const SnowCell& GetCell(uint32_t x, uint32_t z) const;
		void SetCell(uint32_t x, uint32_t z, const SnowCell& cell);

		// Packs normalized GPU snow state per texel:
		// R = EffectiveDepth / MaxDepth, G = Compression / MaxDepth, B = Ice, A = Coverage
		void PackRGBA8(std::vector<uint8_t>& outPixels) const;

		std::vector<uint8_t> SerializeBinary() const;
		void DeserializeBinary(const std::vector<uint8_t>& bytes);
		void SaveTracks(const std::filesystem::path& path) const;
		void LoadTracks(const std::filesystem::path& path);

		SnowField Snapshot() const;
		void Restore(const SnowField& snapshot);

		const SnowFieldConfig& GetConfig() const
		{
			return m_Config;
		}
		uint32_t GetResolution() const
		{
			return m_Config.Resolution;
		}
		float GetCellWorldSize() const;
		uint32_t GetActiveTrackCount() const;
		float GetAverageDepth() const;
		uint64_t GetMutationCounter() const
		{
			return m_MutationCounter;
		}

		bool operator==(const SnowField&) const = default;

	  private:
		SnowFieldConfig m_Config;
		std::vector<SnowCell> m_Cells;
		uint64_t m_MutationCounter = 0;
	};

} // namespace Bellwether
