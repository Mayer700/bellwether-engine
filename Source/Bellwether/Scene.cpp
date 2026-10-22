#include "Scene.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <stdexcept>

namespace Bellwether
{

	static Json WriteVector(const glm::vec3& value)
	{
		return Json::array({value.x, value.y, value.z});
	}

	static void ValidateFields(const Json& object, std::initializer_list<const char*> fields)
	{
		if (!object.is_object())
			throw std::invalid_argument("Expected a JSON object");
		for (const auto& [name, value] : object.items())
		{
			if (std::none_of(fields.begin(), fields.end(), [&](const char* field) { return name == field; }))
				throw std::invalid_argument("Unsupported field: " + name);
		}
	}

	static EntityID ReadEntityID(const Json& value)
	{
		if (!value.is_number_integer())
			throw std::invalid_argument("Entity ID must be an integer");
		if (value.is_number_unsigned())
		{
			auto id = value.get<EntityID>();
			if (!id || id >= static_cast<EntityID>(std::numeric_limits<int64_t>::max()))
				throw std::invalid_argument("Entity ID is outside the supported range");
			return id;
		}
		auto id = value.get<int64_t>();
		if (id <= 0 || id >= std::numeric_limits<int64_t>::max())
			throw std::invalid_argument("Entity ID is outside the supported range");
		return static_cast<EntityID>(id);
	}

	glm::vec3 ReadVector(const Json& value)
	{
		if (!value.is_array() || value.size() != 3)
			throw std::invalid_argument("Expected a three-component vector");
		glm::vec3 result;
		for (int index = 0; index < 3; index++)
		{
			if (!value[index].is_number())
				throw std::invalid_argument("Vector components must be numeric");
			result[index] = value[index].get<float>();
			if (!std::isfinite(result[index]))
				throw std::invalid_argument("Vector components must be finite");
		}
		return result;
	}

	glm::mat4 TransformComponent::GetMatrix() const
	{
		auto matrix = glm::translate(glm::mat4(1.0f), Translation);
		matrix = glm::rotate(matrix, Rotation.z, {0, 0, 1});
		matrix = glm::rotate(matrix, Rotation.y, {0, 1, 0});
		matrix = glm::rotate(matrix, Rotation.x, {1, 0, 0});
		return glm::scale(matrix, Scale);
	}

	EntityID Scene::CreateEntity(const std::string& name)
	{
		if (m_Entities.size() >= 100000)
			throw std::overflow_error("Scene entity budget exceeded (100000)");
		if (name.empty() || name.size() > 1024)
			throw std::invalid_argument("Entity name must contain 1 to 1024 bytes");
		if (m_NextID >= static_cast<EntityID>(std::numeric_limits<int64_t>::max()))
			throw std::overflow_error("Scene entity IDs exhausted");
		Entity entity;
		entity.ID = m_NextID++;
		entity.Name = name;
		m_Entities.emplace(entity.ID, entity);
		return entity.ID;
	}

	void Scene::DestroyEntity(EntityID id)
	{
		if (!m_Entities.erase(id))
			throw std::out_of_range("Entity does not exist: " + std::to_string(id));
	}

	bool Scene::Exists(EntityID id) const
	{
		return m_Entities.contains(id);
	}

	Entity& Scene::GetEntity(EntityID id)
	{
		return m_Entities.at(id);
	}

	const Entity& Scene::GetEntity(EntityID id) const
	{
		return m_Entities.at(id);
	}

	std::vector<EntityID> Scene::GetEntities() const
	{
		std::vector<EntityID> result;
		result.reserve(m_Entities.size());
		for (const auto& [id, entity] : m_Entities)
			result.push_back(id);
		return result;
	}

	Json Scene::Serialize() const
	{
		Json result = {{"Format", 1}, {"NextID", m_NextID}, {"Entities", Json::array()}};
		for (const auto& [id, entity] : m_Entities)
		{
			result["Entities"].push_back({{"ID", id},
										  {"Name", entity.Name},
										  {"Transform",
										   {{"Translation", WriteVector(entity.Transform.Translation)},
											{"Rotation", WriteVector(entity.Transform.Rotation)},
											{"Scale", WriteVector(entity.Transform.Scale)}}},
										  {"Script", entity.Script}});
		}
		return result;
	}

	static void ReadEntity(Entity& entity, const Json& data)
	{
		ValidateFields(data, {"ID", "Name", "Transform", "Script"});
		entity.Name = data.at("Name").get<std::string>();
		if (entity.Name.empty() || entity.Name.size() > 1024)
			throw std::invalid_argument("Invalid entity name");
		const auto& transform = data.at("Transform");
		ValidateFields(transform, {"Translation", "Rotation", "Scale"});
		entity.Transform.Translation = ReadVector(transform.at("Translation"));
		entity.Transform.Rotation = ReadVector(transform.at("Rotation"));
		entity.Transform.Scale = ReadVector(transform.at("Scale"));
		if (glm::any(glm::lessThanEqual(glm::abs(entity.Transform.Scale), glm::vec3(0.00001f))))
			throw std::invalid_argument("Transform scale must be invertible");
		entity.Script = data.value("Script", "");
	}

	void Scene::Deserialize(const Json& data)
	{
		ValidateFields(data, {"Format", "NextID", "Entities"});
		if (data.at("Format") != 1 || !data.at("Entities").is_array())
			throw std::invalid_argument("Unsupported scene format");
		Scene candidate;
		if (data.at("Entities").size() > 100000)
			throw std::invalid_argument("Scene entity budget exceeded (100000)");
		for (const auto& entry : data.at("Entities"))
		{
			auto id = ReadEntityID(entry.at("ID"));
			if (candidate.Exists(id))
				throw std::invalid_argument("Invalid or duplicate entity ID");
			Entity entity;
			entity.ID = static_cast<EntityID>(id);
			ReadEntity(entity, entry);
			candidate.m_Entities.emplace(entity.ID, entity);
			candidate.m_NextID = std::max(candidate.m_NextID, entity.ID + 1);
		}
		if (data.contains("NextID"))
		{
			if (!data["NextID"].is_number_integer() || data["NextID"].get<int64_t>() <= 0)
				throw std::invalid_argument("Invalid next entity ID");
			candidate.m_NextID = std::max(candidate.m_NextID, data["NextID"].get<EntityID>());
		}
		*this = std::move(candidate);
	}

	void Scene::ApplyCommands(const Json& commands)
	{
		if (!commands.is_array())
			throw std::invalid_argument("Command batch must be an array");
		Scene candidate = *this;
		for (const auto& command : commands)
		{
			auto operation = command.at("Operation").get<std::string>();
			if (operation == "Create")
			{
				ValidateFields(command, {"Operation", "Name", "Translation"});
				auto id = candidate.CreateEntity(command.at("Name"));
				if (command.contains("Translation"))
					candidate.GetEntity(id).Transform.Translation = ReadVector(command["Translation"]);
			}
			else if (operation == "Destroy")
			{
				ValidateFields(command, {"Operation", "ID"});
				candidate.DestroyEntity(ReadEntityID(command.at("ID")));
			}
			else if (operation == "SetTransform")
			{
				ValidateFields(command, {"Operation", "ID", "Transform"});
				auto& entity = candidate.GetEntity(ReadEntityID(command.at("ID")));
				Json replacement = {
					{"Name", entity.Name}, {"Transform", command.at("Transform")}, {"Script", entity.Script}};
				ReadEntity(entity, replacement);
			}
			else if (operation == "Rename")
			{
				ValidateFields(command, {"Operation", "ID", "Name"});
				auto& entity = candidate.GetEntity(ReadEntityID(command.at("ID")));
				auto name = command.at("Name").get<std::string>();
				if (name.empty() || name.size() > 1024)
					throw std::invalid_argument("Invalid entity name");
				entity.Name = name;
			}
			else
				throw std::invalid_argument("Unknown scene command: " + operation);
		}
		*this = std::move(candidate);
	}

	void Scene::Load(const std::filesystem::path& path)
	{
		Deserialize(ReadJson(path));
	}

	void Scene::Save(const std::filesystem::path& path) const
	{
		AtomicWrite(path, Serialize().dump(2));
	}

	EntityID Scene::SpawnPrefab(const Json& prefab)
	{
		Scene candidate = *this;
		auto id = candidate.CreateEntity(prefab.at("Name"));
		ReadEntity(candidate.GetEntity(id), prefab);
		*this = std::move(candidate);
		return id;
	}

} // namespace Bellwether
