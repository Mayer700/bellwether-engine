#include "ScriptRuntime.h"

extern "C"
{
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

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
		return {"CreateEntity", "DestroyEntity", "Exists", "SetPosition",
				"GetPosition",	"GetName",		 "Rename", "SpawnPrefab"};
	}

	ScriptRuntime::ScriptRuntime(Scene& scene) : m_Scene(scene)
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
		double x = 0, y = 0, z = 0;
		char errorMessage[512] = {};
		// Lua argument checks occur before C++ objects with destructors enter scope.
		if (operation == 0 || operation == 7)
		{
			luaL_checktype(state, 1, LUA_TSTRING);
			text = lua_tostring(state, 1);
		}
		else
			id = luaL_checkinteger(state, 1);
		if (operation == 6)
		{
			luaL_checktype(state, 2, LUA_TSTRING);
			text = lua_tostring(state, 2);
		}
		if (operation == 3)
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

	bool ScriptRuntime::FinishCall(int result, const Scene& checkpoint, std::string& error)
	{
		lua_sethook(m_State, nullptr, 0, 0);
		if (result != LUA_OK)
		{
			const auto* message = lua_tostring(m_State, -1);
			error = message ? message : "Unknown script error";
			lua_pop(m_State, 1);
			m_Scene = checkpoint;
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
		lua_sethook(m_State, InstructionLimit, LUA_MASKCOUNT, 1000000);
		int result = luaL_loadbufferx(m_State, source.data(), source.size(), "SceneScript", "t");
		if (result == LUA_OK)
			result = lua_pcall(m_State, 0, 0, 0);
		return FinishCall(result, checkpoint, error);
	}

	bool ScriptRuntime::Update(double timestep, std::string& error)
	{
		if (m_Faulted || !std::isfinite(timestep) || timestep < 0 || timestep > 1)
		{
			error = "Invalid script update state or timestep";
			return false;
		}
		Scene checkpoint = m_Scene;
		lua_getglobal(m_State, "OnUpdate");
		if (lua_isnil(m_State, -1))
		{
			lua_pop(m_State, 1);
			error.clear();
			return true;
		}
		lua_pushnumber(m_State, timestep);
		lua_sethook(m_State, InstructionLimit, LUA_MASKCOUNT, 1000000);
		return FinishCall(lua_pcall(m_State, 1, 0, 0), checkpoint, error);
	}

} // namespace Bellwether
