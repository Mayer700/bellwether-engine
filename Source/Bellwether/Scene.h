#pragma once

#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Bellwether
{

	using EntityID = uint64_t;
	using Json = nlohmann::json;

	struct TransformComponent
	{
		glm::vec3 Translation{0.0f};
		glm::vec3 Rotation{0.0f};
		glm::vec3 Scale{1.0f};
		glm::mat4 GetMatrix() const;
	};

	struct Entity
	{
		EntityID ID = 0;
		std::string Name;
		TransformComponent Transform;
		std::string Script;
	};

	class Scene
	{
	  public:
		EntityID CreateEntity(const std::string& name);
		void DestroyEntity(EntityID id);
		bool Exists(EntityID id) const;
		Entity& GetEntity(EntityID id);
		const Entity& GetEntity(EntityID id) const;
		std::vector<EntityID> GetEntities() const;
		Json Serialize() const;
		void Deserialize(const Json& data);
		void ApplyCommands(const Json& commands);
		void Load(const std::filesystem::path& path);
		void Save(const std::filesystem::path& path) const;
		EntityID SpawnPrefab(const Json& prefab);

	  private:
		EntityID m_NextID = 1;
		std::map<EntityID, Entity> m_Entities;
	};

	void AtomicWrite(const std::filesystem::path& path, const std::string& contents);
	Json ReadJson(const std::filesystem::path& path);
	glm::vec3 ReadVector(const Json& value);

} // namespace Bellwether
