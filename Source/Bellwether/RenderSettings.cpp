#include "RenderSettings.h"
#include "Scene.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Bellwether
{
	static bool Finite(glm::vec3 value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
	}

	void RenderSettings::Validate() const
	{
		if (!std::isfinite(Exposure) || Exposure < 0.05f || Exposure > 8.0f)
			throw std::invalid_argument("Exposure must be between 0.05 and 8");
		if (!std::isfinite(FogDensity) || FogDensity < 0 || FogDensity > 0.1f)
			throw std::invalid_argument("Fog density must be between 0 and 0.1");
		if (!Finite(SunDirection) || glm::any(glm::greaterThan(glm::abs(SunDirection), glm::vec3(10000))) ||
			glm::length(SunDirection) < 0.01f || glm::normalize(SunDirection).y <= 0.01f)
			throw std::invalid_argument("Sun direction must be finite, nonzero and above the horizon");
		if (!Finite(SunRadiance) || glm::any(glm::lessThan(SunRadiance, glm::vec3(0))) ||
			glm::any(glm::greaterThan(SunRadiance, glm::vec3(100))))
			throw std::invalid_argument("Sun radiance must be between 0 and 100");
		if (!Finite(CameraTarget) || glm::any(glm::greaterThan(glm::abs(CameraTarget), glm::vec3(100000))))
			throw std::invalid_argument("Camera target must be finite and within the supported world bounds");
		if (!std::isfinite(CameraSize) || CameraSize < 4 || CameraSize > 200)
			throw std::invalid_argument("Camera size must be between 4 and 200");
		if (!std::isfinite(SnowCoverage) || SnowCoverage < 0.0f || SnowCoverage > 1.0f)
			throw std::invalid_argument("Snow coverage must be between 0 and 1");
		if (!std::isfinite(SnowfallIntensity) || SnowfallIntensity < 0.0f || SnowfallIntensity > 1.0f)
			throw std::invalid_argument("Snowfall intensity must be between 0 and 1");
		if (!Finite(WindVelocity) || glm::any(glm::greaterThan(glm::abs(WindVelocity), glm::vec3(100.0f))))
			throw std::invalid_argument("Wind velocity components must be finite and within [-100, 100]");
		if (!std::isfinite(SnowSparkle) || SnowSparkle < 0.0f || SnowSparkle > 2.0f)
			throw std::invalid_argument("Snow sparkle must be between 0 and 2");
		if (!std::isfinite(SnowSubsurface) || SnowSubsurface < 0.0f || SnowSubsurface > 2.0f)
			throw std::invalid_argument("Snow subsurface scattering must be between 0 and 2");
	}

	Json RenderSettings::Serialize() const
	{
		Validate();
		auto vector = [](glm::vec3 value) { return Json::array({value.x, value.y, value.z}); };
		return {{"Exposure", Exposure},
				{"Shadows", Shadows},
				{"FogDensity", FogDensity},
				{"SunDirection", vector(SunDirection)},
				{"SunRadiance", vector(SunRadiance)},
				{"CameraTarget", vector(CameraTarget)},
				{"CameraSize", CameraSize},
				{"SnowCoverage", SnowCoverage},
				{"SnowfallIntensity", SnowfallIntensity},
				{"WindVelocity", vector(WindVelocity)},
				{"SnowSparkle", SnowSparkle},
				{"SnowSubsurface", SnowSubsurface}};
	}

	RenderSettings RenderSettings::Deserialize(const Json& value)
	{
		if (!value.is_object())
			throw std::invalid_argument("Render settings must be an object");
		const std::array<std::string, 12> fields{"Exposure",		  "Shadows",	  "FogDensity",	 "SunDirection",
												 "SunRadiance",		  "CameraTarget", "CameraSize",	 "SnowCoverage",
												 "SnowfallIntensity", "WindVelocity", "SnowSparkle", "SnowSubsurface"};
		for (const auto& [name, item] : value.items())
			if (std::find(fields.begin(), fields.end(), name) == fields.end())
				throw std::invalid_argument("Unsupported renderer setting: " + name);
		RenderSettings settings;
		for (auto [name, destination] : {std::pair{"Exposure", &settings.Exposure},
										 {"FogDensity", &settings.FogDensity},
										 {"CameraSize", &settings.CameraSize},
										 {"SnowCoverage", &settings.SnowCoverage},
										 {"SnowfallIntensity", &settings.SnowfallIntensity},
										 {"SnowSparkle", &settings.SnowSparkle},
										 {"SnowSubsurface", &settings.SnowSubsurface}})
		{
			if (!value.contains(name))
				continue;
			if (!value[name].is_number())
				throw std::invalid_argument(std::string(name) + " must be numeric");
			*destination = value[name].get<float>();
		}
		if (value.contains("Shadows"))
		{
			if (!value["Shadows"].is_boolean())
				throw std::invalid_argument("Shadows must be boolean");
			settings.Shadows = value["Shadows"].get<bool>();
		}
		if (value.contains("SunDirection"))
			settings.SunDirection = ReadVector(value["SunDirection"]);
		if (value.contains("SunRadiance"))
			settings.SunRadiance = ReadVector(value["SunRadiance"]);
		if (value.contains("CameraTarget"))
			settings.CameraTarget = ReadVector(value["CameraTarget"]);
		if (value.contains("WindVelocity"))
			settings.WindVelocity = ReadVector(value["WindVelocity"]);
		settings.Validate();
		return settings;
	}

	glm::vec3 RenderSettings::GetCameraPosition() const
	{
		Validate();
		return CameraTarget + glm::vec3(22, 30, 34);
	}

	glm::mat4 RenderSettings::GetViewProjection(float aspect) const
	{
		Validate();
		if (!std::isfinite(aspect) || aspect <= 0)
			throw std::invalid_argument("Camera aspect must be positive and finite");
		float halfSize = CameraSize * 0.5f;
		return glm::orthoRH_ZO(-halfSize * aspect, halfSize * aspect, -halfSize, halfSize, 0.1f, 400.0f) *
			   glm::lookAtRH(GetCameraPosition(), CameraTarget, {0, 1, 0});
	}

	std::array<glm::mat4, 2> RenderSettings::GetShadowMatrices(uint32_t resolution) const
	{
		Validate();
		if (resolution < 64 || resolution > 8192)
			throw std::invalid_argument("Shadow resolution must be between 64 and 8192");
		auto direction = glm::normalize(SunDirection);
		auto up = std::abs(direction.y) > 0.98f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
		auto view = glm::lookAtRH(CameraTarget + direction * 120.0f, CameraTarget, up);
		std::array<glm::mat4, 2> result;
		for (size_t cascade = 0; cascade < result.size(); cascade++)
		{
			float halfSize = cascade == 0 ? 24.0f : 90.0f;
			auto projection = glm::orthoRH_ZO(-halfSize, halfSize, -halfSize, halfSize, 1.0f, 300.0f);
			auto origin = projection * view * glm::vec4(0, 0, 0, 1);
			glm::vec2 texels = glm::vec2(origin) * (resolution * 0.5f);
			auto offset = (glm::round(texels) - texels) * (2.0f / resolution);
			projection[3][0] += offset.x;
			projection[3][1] += offset.y;
			result[cascade] = projection * view;
		}
		return result;
	}
} // namespace Bellwether
