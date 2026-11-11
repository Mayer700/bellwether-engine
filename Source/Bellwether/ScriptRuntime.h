#pragma once

#include "Scene.h"
#include "SnowField.h"

struct lua_State;

namespace Bellwether
{

	class ScriptRuntime
	{
	  public:
		explicit ScriptRuntime(Scene& scene, SnowField* snow = nullptr);
		~ScriptRuntime();
		ScriptRuntime(const ScriptRuntime&) = delete;
		ScriptRuntime& operator=(const ScriptRuntime&) = delete;
		bool Execute(const std::string& source, std::string& error);
		bool Update(double timestep, std::string& error);
		static std::vector<std::string> GetAPI();

	  private:
		static void* Allocate(void* userData, void* pointer, size_t oldSize, size_t newSize);
		static int Invoke(lua_State* state);
		bool FinishCall(int result, const Scene& checkpoint, const SnowField& snowCheckpoint, bool hasSnowCheckpoint,
						std::string& error);
		Scene& m_Scene;
		SnowField* m_Snow = nullptr;
		lua_State* m_State = nullptr;
		bool m_Faulted = false;
		size_t m_MemoryUsed = 0;
	};

} // namespace Bellwether
