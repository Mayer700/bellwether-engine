#include "ScriptRuntime.h"

extern "C"
{
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace Bellwether
{

	static void InstructionLimit(lua_State* state, lua_Debug*)
	{
		luaL_error(state, "Script instruction budget exceeded");
	}

	void* ScriptRuntime::Allocate(void* userData, void* pointer, size_t oldSize, size_t newSize)
	{
		auto* runtime = static_cast<ScriptRuntime*>(userData);
		if (!pointer)
			oldSize = 0;
		if (!newSize)
		{
			std::free(pointer);
			runtime->m_MemoryUsed -= oldSize;
			return nullptr;
		}
		constexpr size_t memoryLimit = 16 * 1024 * 1024;
		if (newSize > memoryLimit || runtime->m_MemoryUsed - oldSize > memoryLimit - newSize)
			return nullptr;
		auto* result = std::realloc(pointer, newSize);
		if (result)
			runtime->m_MemoryUsed = runtime->m_MemoryUsed - oldSize + newSize;
		return result;
	}

	std::vector<std::string> ScriptRuntime::GetAPI()
	{
		return {"CreateEntity",
				"DestroyEntity",
				"Exists",
				"SetPosition",
				"GetPosition",
				"GetName",
				"Rename",
				"SpawnPrefab",
				"SetRotation",
				"GetRotation",
				"SetScale",
				"GetScale",
				"GetEntities",
				"GetSnowDepth",
				"GetSnowCompression",
				"GetSnowSpeedFactor",
				"StampSnow",
				"MeltSnow",
				"SetWind",
				"GetWind",
				"SetSnowfall",
				"GetSnowIce",
				"GetSnowFriction",
				"StampSnowSegment",
				"StepSnowWeather"};
	}

	ScriptRuntime::ScriptRuntime(Scene& scene, SnowField* snow) : m_Scene(scene), m_Snow(snow)
	{
		m_State = lua_newstate(Allocate, this);
		if (!m_State)
			throw std::runtime_error("Cannot allocate Lua runtime");
		luaL_requiref(m_State, "_G", luaopen_base, 1);
		lua_pop(m_State, 1);
		luaL_requiref(m_State, LUA_MATHLIBNAME, luaopen_math, 1);
		lua_pop(m_State, 1);
		luaL_requiref(m_State, LUA_STRLIBNAME, luaopen_string, 1);
		lua_pop(m_State, 1);
		luaL_requiref(m_State, LUA_TABLIBNAME, luaopen_table, 1);
		lua_pop(m_State, 1);
		// Source strings are explicit inputs. Scripts cannot open files or processes.
		// Protected calls are host-owned so scripts cannot swallow instruction-budget errors.
		for (const auto* name : {"dofile", "loadfile", "load", "collectgarbage", "pcall", "xpcall"})
		{
			lua_pushnil(m_State);
			lua_setglobal(m_State, name);
		}
		lua_newtable(m_State);
		auto api = GetAPI();
		for (size_t index = 0; index < api.size(); index++)
		{
			lua_pushlightuserdata(m_State, this);
			lua_pushinteger(m_State, static_cast<lua_Integer>(index));
			lua_pushcclosure(m_State, Invoke, 2);
			lua_setfield(m_State, -2, api[index].c_str());
		}
		lua_setglobal(m_State, "Engine");
	}

	ScriptRuntime::~ScriptRuntime()
	{
		if (m_State)
			lua_close(m_State);
	}

	int ScriptRuntime::Invoke(lua_State* state)
	{
		auto* runtime = static_cast<ScriptRuntime*>(lua_touserdata(state, lua_upvalueindex(1)));
		auto operation = lua_tointeger(state, lua_upvalueindex(2));
		const char* text = nullptr;
		lua_Integer id = 0;
		double x = 0, y = 0, z = 0, w = 0, extra = 0, x2 = 0, z2 = 0;
		bool hasY = false, hasZ = false, hasW = false;
		char errorMessage[512] = {};
		// Lua argument checks occur before C++ objects with destructors enter scope.
		if (operation == 0 || operation == 7)
		{
			luaL_checktype(state, 1, LUA_TSTRING);
			text = lua_tostring(state, 1);
		}
		else if (operation == 12 || operation == 19)
		{
			// No arguments required.
		}
		else if ((operation >= 13 && operation <= 15) || operation == 21 || operation == 22)
		{
			x = luaL_checknumber(state, 1);
			z = luaL_checknumber(state, 2);
		}
		else if (operation == 16 || operation == 17)
		{
			x = luaL_checknumber(state, 1);
			z = luaL_checknumber(state, 2);
			y = luaL_checknumber(state, 3);
			w = luaL_checknumber(state, 4);
			extra = lua_isnoneornil(state, 5) ? (operation == 16 ? 0.35 : 1.0) : luaL_checknumber(state, 5);
		}
		else if (operation == 18)
		{
			x = luaL_checknumber(state, 1);
			y = luaL_checknumber(state, 2);
			z = luaL_checknumber(state, 3);
		}
		else if (operation == 20)
		{
			x = luaL_checknumber(state, 1);
			hasY = !lua_isnoneornil(state, 2);
			if (hasY)
				y = luaL_checknumber(state, 2);
			hasZ = !lua_isnoneornil(state, 3);
			if (hasZ)
				z = luaL_checknumber(state, 3);
			hasW = !lua_isnoneornil(state, 4);
			if (hasW)
				w = luaL_checknumber(state, 4);
		}
		else if (operation == 23)
		{
			x = luaL_checknumber(state, 1);
			z = luaL_checknumber(state, 2);
			x2 = luaL_checknumber(state, 3);
			z2 = luaL_checknumber(state, 4);
			y = luaL_checknumber(state, 5);
			w = luaL_checknumber(state, 6);
			extra = lua_isnoneornil(state, 7) ? 0.35 : luaL_checknumber(state, 7);
		}
		else if (operation == 24)
		{
			x = luaL_checknumber(state, 1);
			y = luaL_checknumber(state, 2);
			if (!lua_isnoneornil(state, 3) || !lua_isnoneornil(state, 4))
			{
				hasY = true;
				z = luaL_checknumber(state, 3);
				w = luaL_checknumber(state, 4);
			}
			extra = lua_isnoneornil(state, 5) ? -6.0 : luaL_checknumber(state, 5);
		}
		else
			id = luaL_checkinteger(state, 1);
		if (operation == 6)
		{
			luaL_checktype(state, 2, LUA_TSTRING);
			text = lua_tostring(state, 2);
		}
		if (operation == 3 || operation == 8 || operation == 10)
		{
			x = luaL_checknumber(state, 2);
			y = luaL_checknumber(state, 3);
			z = luaL_checknumber(state, 4);
		}
		try
		{
			auto& scene = runtime->m_Scene;
			if (id < 0)
				throw std::invalid_argument("Entity IDs must be positive");
			auto entityID = static_cast<EntityID>(id);
			auto requireSnow = [&]() -> SnowField&
			{
				if (!runtime->m_Snow)
					throw std::logic_error("No snowfield attached to script runtime");
				return *runtime->m_Snow;
			};
			auto requireXZ = [&](double wx, double wz) -> glm::vec2
			{
				if (!std::isfinite(wx) || !std::isfinite(wz) || std::abs(wx) > 1e6 || std::abs(wz) > 1e6)
					throw std::invalid_argument("Snow coordinates must be finite and within world bounds");
				return glm::vec2(static_cast<float>(wx), static_cast<float>(wz));
			};
			switch (operation)
			{
			case 0:
				lua_pushinteger(state, static_cast<lua_Integer>(scene.CreateEntity(text)));
				return 1;
			case 1:
				scene.DestroyEntity(entityID);
				return 0;
			case 2:
				lua_pushboolean(state, scene.Exists(entityID));
				return 1;
			case 3:
				if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || std::abs(x) > 1e8 ||
					std::abs(y) > 1e8 || std::abs(z) > 1e8)
					throw std::invalid_argument("Position must be finite and within world bounds");
				scene.GetEntity(entityID).Transform.Translation = glm::vec3(x, y, z);
				return 0;
			case 4:
			{
				const auto& position = scene.GetEntity(entityID).Transform.Translation;
				lua_pushnumber(state, position.x);
				lua_pushnumber(state, position.y);
				lua_pushnumber(state, position.z);
				return 3;
			}
			case 5:
				lua_pushstring(state, scene.GetEntity(entityID).Name.c_str());
				return 1;
			case 6:
				if (!text[0] || std::char_traits<char>::length(text) > 1024)
					throw std::invalid_argument("Invalid entity name");
				scene.GetEntity(entityID).Name = text;
				return 0;
			case 7:
				lua_pushinteger(state, static_cast<lua_Integer>(scene.SpawnPrefab(Json::parse(text))));
				return 1;
			case 8:
				if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || std::abs(x) > 1e6 ||
					std::abs(y) > 1e6 || std::abs(z) > 1e6)
					throw std::invalid_argument("Rotation must be finite and within bounds");
				scene.GetEntity(entityID).Transform.Rotation = glm::vec3(x, y, z);
				return 0;
			case 9:
			{
				const auto& rotation = scene.GetEntity(entityID).Transform.Rotation;
				lua_pushnumber(state, rotation.x);
				lua_pushnumber(state, rotation.y);
				lua_pushnumber(state, rotation.z);
				return 3;
			}
			case 10:
				if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || x < 1e-4 || y < 1e-4 || z < 1e-4 ||
					x > 1e4 || y > 1e4 || z > 1e4)
					throw std::invalid_argument("Scale must be finite, positive, and within bounds");
				scene.GetEntity(entityID).Transform.Scale = glm::vec3(x, y, z);
				return 0;
			case 11:
			{
				const auto& scale = scene.GetEntity(entityID).Transform.Scale;
				lua_pushnumber(state, scale.x);
				lua_pushnumber(state, scale.y);
				lua_pushnumber(state, scale.z);
				return 3;
			}
			case 12:
			{
				size_t count = 0;
				{
					const auto ids = scene.GetEntities();
					count = ids.size();
				}
				auto* buffer =
					static_cast<EntityID*>(lua_newuserdatauv(state, std::max<size_t>(1, count) * sizeof(EntityID), 0));
				{
					const auto ids = scene.GetEntities();
					count = std::min(count, ids.size());
					std::copy_n(ids.data(), count, buffer);
				}
				lua_createtable(state, static_cast<int>(count), 0);
				for (size_t index = 0; index < count; index++)
				{
					lua_pushinteger(state, static_cast<lua_Integer>(buffer[index]));
					lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
				}
				lua_remove(state, -2);
				return 1;
			}
			case 13:
				lua_pushnumber(state, requireSnow().Sample(requireXZ(x, z)).EffectiveDepth);
				return 1;
			case 14:
				lua_pushnumber(state, requireSnow().Sample(requireXZ(x, z)).Compression);
				return 1;
			case 15:
				lua_pushnumber(state, requireSnow().Sample(requireXZ(x, z)).SpeedFactor);
				return 1;
			case 16:
			{
				if (!std::isfinite(y) || y <= 0.0 || y > 100.0 || !std::isfinite(w) || w < 0.0 || w > 50.0 ||
					!std::isfinite(extra) || extra < 0.0 || extra > 1.0)
					throw std::invalid_argument("Invalid snow stamp radius, depth, or rim factor");
				bool stamped = requireSnow().StampCircle(requireXZ(x, z), static_cast<float>(y), static_cast<float>(w),
														 static_cast<float>(extra));
				lua_pushboolean(state, stamped);
				return 1;
			}
			case 17:
			{
				if (!std::isfinite(y) || y <= 0.0 || y > 100.0 || !std::isfinite(w) || w < 0.0 || w > 1000.0 ||
					!std::isfinite(extra) || extra <= 0.0 || extra > 10.0)
					throw std::invalid_argument("Invalid thermal snow melt arguments");
				bool melted = requireSnow().ApplyHeatSource(requireXZ(x, z), static_cast<float>(y),
															static_cast<float>(w), static_cast<float>(extra));
				lua_pushboolean(state, melted);
				return 1;
			}
			case 18:
			{
				if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
					throw std::invalid_argument("Wind velocity must be finite");
				RenderSettings candidate = scene.GetRenderSettings();
				candidate.WindVelocity = glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
				candidate.Validate();
				scene.SetRenderSettings(candidate);
				return 0;
			}
			case 19:
			{
				const auto& wind = scene.GetRenderSettings().WindVelocity;
				lua_pushnumber(state, wind.x);
				lua_pushnumber(state, wind.y);
				lua_pushnumber(state, wind.z);
				return 3;
			}
			case 20:
			{
				if (!std::isfinite(x) || (hasY && !std::isfinite(y)) || (hasZ && !std::isfinite(z)) ||
					(hasW && !std::isfinite(w)))
					throw std::invalid_argument("Snowfall parameters must be finite");
				RenderSettings candidate = scene.GetRenderSettings();
				candidate.SnowfallIntensity = static_cast<float>(x);
				if (hasY)
					candidate.SnowCoverage = static_cast<float>(y);
				if (hasZ)
					candidate.SnowSparkle = static_cast<float>(z);
				if (hasW)
					candidate.SnowSubsurface = static_cast<float>(w);
				candidate.Validate();
				scene.SetRenderSettings(candidate);
				return 0;
			}
			case 21:
				lua_pushnumber(state, requireSnow().Sample(requireXZ(x, z)).Ice);
				return 1;
			case 22:
				lua_pushnumber(state, requireSnow().Sample(requireXZ(x, z)).FrictionFactor);
				return 1;
			case 23:
			{
				if (!std::isfinite(y) || y <= 0.0 || y > 100.0 || !std::isfinite(w) || w < 0.0 || w > 50.0 ||
					!std::isfinite(extra) || extra < 0.0 || extra > 1.0)
					throw std::invalid_argument("Invalid snow segment stamp radius, depth, or rim factor");
				bool stamped = requireSnow().StampSegment(requireXZ(x, z), requireXZ(x2, z2), static_cast<float>(y),
														  static_cast<float>(w), static_cast<float>(extra));
				lua_pushboolean(state, stamped);
				return 1;
			}
			case 24:
			{
				const auto& sceneWind = scene.GetRenderSettings().WindVelocity;
				glm::vec2 wind = hasY ? glm::vec2(static_cast<float>(z), static_cast<float>(w))
									  : glm::vec2(sceneWind.x, sceneWind.z);
				requireSnow().StepWeather(static_cast<float>(x), static_cast<float>(y), wind,
										  static_cast<float>(extra));
				return 0;
			}
			}
		}
		catch (const std::exception& exception)
		{
			std::snprintf(errorMessage, sizeof(errorMessage), "%s", exception.what());
		}
		// Leave the C++ exception scope before a Lua allocation/error can longjmp.
		lua_pushstring(state, errorMessage);
		return lua_error(state);
	}

	bool ScriptRuntime::FinishCall(int result, const Scene& checkpoint, const std::optional<SnowField>& snowCheckpoint,
								   std::string& error)
	{
		lua_sethook(m_State, nullptr, 0, 0);
		if (result != LUA_OK)
		{
			const auto* message = lua_tostring(m_State, -1);
			error = message ? message : "Unknown script error";
			lua_pop(m_State, 1);
			m_Scene = checkpoint;
			if (snowCheckpoint.has_value() && m_Snow)
				m_Snow->Restore(*snowCheckpoint);
			m_Faulted = true;
			return false;
		}
		error.clear();
		return true;
	}

	bool ScriptRuntime::Execute(const std::string& source, std::string& error)
	{
		if (m_Faulted)
		{
			error = "Script runtime faulted; create a fresh instance before resuming";
			return false;
		}
		Scene checkpoint = m_Scene;
		std::optional<SnowField> snowCheckpoint;
		if (m_Snow)
			snowCheckpoint = m_Snow->Snapshot();
		lua_sethook(m_State, InstructionLimit, LUA_MASKCOUNT, 1000000);
		int result = luaL_loadbufferx(m_State, source.data(), source.size(), "SceneScript", "t");
		if (result == LUA_OK)
			result = lua_pcall(m_State, 0, 0, 0);
		return FinishCall(result, checkpoint, snowCheckpoint, error);
	}

	bool ScriptRuntime::Update(double timestep, std::string& error)
	{
		if (m_Faulted || !std::isfinite(timestep) || timestep < 0 || timestep > 1)
		{
			error = "Invalid script update state or timestep";
			return false;
		}
		Scene checkpoint = m_Scene;
		std::optional<SnowField> snowCheckpoint;
		if (m_Snow)
			snowCheckpoint = m_Snow->Snapshot();
		lua_getglobal(m_State, "OnUpdate");
		if (lua_isnil(m_State, -1))
		{
			lua_pop(m_State, 1);
			error.clear();
			return true;
		}
		lua_pushnumber(m_State, timestep);
		lua_sethook(m_State, InstructionLimit, LUA_MASKCOUNT, 1000000);
		return FinishCall(lua_pcall(m_State, 1, 0, 0), checkpoint, snowCheckpoint, error);
	}

} // namespace Bellwether
