#pragma once

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <array>

namespace Bellwether
{
	using Json = nlohmann::json;
	struct RenderSettings
	{
		float Exposure = 1.0f;
		bool Shadows = true;
		float FogDensity = 0.003f;
		glm::vec3 SunDirection{-0.6f, 0.85f, -0.35f};
		glm::vec3 SunRadiance{4.0f, 3.3f, 2.5f};
		glm::vec3 CameraTarget{0, 1, 10};
		float CameraSize = 24.0f;

		void Validate() const;
		Json Serialize() const;
		static RenderSettings Deserialize(const Json& value);
		glm::mat4 GetViewProjection(float aspect) const;
		glm::vec3 GetCameraPosition() const;
		std::array<glm::mat4, 2> GetShadowMatrices(uint32_t resolution) const;
	};
} // namespace Bellwether
