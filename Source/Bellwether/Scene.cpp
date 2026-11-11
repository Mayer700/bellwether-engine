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

	void ValidateModelAssetPath(const std::string& asset)
	{
		if (asset.empty())
			return;
		if (asset.size() > 1024 || asset.front() == '/')
			throw std::invalid_argument("Model asset must be a relative path of at most 1024 UTF-8 bytes");
		for (size_t index = 0; index < asset.size();)
		{
			auto first = static_cast<unsigned char>(asset[index++]);
			if (first < 0x80)
			{
				if (first < 0x20 || first == 0x7f ||
					std::string("\\:\"<>|?*").find(static_cast<char>(first)) != std::string::npos)
					throw std::invalid_argument("Model asset contains a non-portable path character");
				continue;
			}
			uint32_t codepoint = 0;
			size_t continuation = 0;
			uint32_t minimum = 0;
			if (first >= 0xc2 && first <= 0xdf)
			{
				codepoint = first & 0x1f;
				continuation = 1;
				minimum = 0x80;
			}
			else if (first >= 0xe0 && first <= 0xef)
			{
				codepoint = first & 0x0f;
				continuation = 2;
				minimum = 0x800;
			}
			else if (first >= 0xf0 && first <= 0xf4)
			{
				codepoint = first & 0x07;
				continuation = 3;
				minimum = 0x10000;
			}
			else
				throw std::invalid_argument("Model asset must contain valid UTF-8");
			if (continuation > asset.size() - index)
				throw std::invalid_argument("Model asset contains truncated UTF-8");
			while (continuation--)
			{
				auto next = static_cast<unsigned char>(asset[index++]);
				if ((next & 0xc0) != 0x80)
					throw std::invalid_argument("Model asset must contain valid UTF-8");
				codepoint = (codepoint << 6) | (next & 0x3f);
			}
			if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
				throw std::invalid_argument("Model asset must contain valid UTF-8");
		}
		for (size_t begin = 0; begin < asset.size();)
		{
			auto end = asset.find('/', begin);
			if (end == std::string::npos)
				end = asset.size();
			auto segment = asset.substr(begin, end - begin);
			if (segment.empty() || segment.size() > 255 || segment == "." || segment == ".." || segment.back() == '.' ||
				segment.back() == ' ')
				throw std::invalid_argument("Model asset contains an invalid path segment");
			auto stem = segment.substr(0, segment.find('.'));
			for (auto& character : stem)
				if (character >= 'a' && character <= 'z')
					character -= 'a' - 'A';
			if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
				(stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '1' &&
				 stem[3] <= '9') ||
				(stem.size() == 5 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
				 static_cast<unsigned char>(stem[3]) == 0xc2 &&
				 (static_cast<unsigned char>(stem[4]) == 0xb9 || static_cast<unsigned char>(stem[4]) == 0xb2 ||
				  static_cast<unsigned char>(stem[4]) == 0xb3)))
				throw std::invalid_argument("Model asset contains a reserved device name");
			if (end == asset.size())
				break;
			begin = end + 1;
			if (begin == asset.size())
				throw std::invalid_argument("Model asset cannot end with a path separator");
		}
		auto extension =
			asset.substr(asset.find_last_of('.') == std::string::npos ? asset.size() : asset.find_last_of('.'));
		for (auto& character : extension)
			if (character >= 'A' && character <= 'Z')
				character += 'a' - 'A';
		if (extension != ".gltf" && extension != ".glb")
			throw std::invalid_argument("Model asset must reference a .gltf or .glb file");
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
		Json result = {{"Format", 1},
					   {"NextID", m_NextID},
					   {"Entities", Json::array()},
					   {"Renderer", m_RenderSettings.Serialize()}};
		for (const auto& [id, entity] : m_Entities)
		{
			ValidateModelAssetPath(entity.ModelAsset);
			result["Entities"].push_back({{"ID", id},
										  {"Name", entity.Name},
										  {"Transform",
										   {{"Translation", WriteVector(entity.Transform.Translation)},
											{"Rotation", WriteVector(entity.Transform.Rotation)},
											{"Scale", WriteVector(entity.Transform.Scale)}}},
										  {"Script", entity.Script}});
			if (!entity.ModelAsset.empty())
				result["Entities"].back()["Model"] = {{"Asset", entity.ModelAsset}};
		}
		return result;
	}

	static void ReadEntity(Entity& entity, const Json& data, bool allowID = true)
	{
		if (allowID)
			ValidateFields(data, {"ID", "Name", "Transform", "Script", "Model"});
		else
			ValidateFields(data, {"Name", "Transform", "Script", "Model"});
		if (!data.at("Name").is_string())
			throw std::invalid_argument("Entity name must be a string");
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
		entity.Script.clear();
		if (data.contains("Script"))
		{
			if (!data["Script"].is_string())
				throw std::invalid_argument("Script path must be a string");
			entity.Script = data["Script"].get<std::string>();
		}
		entity.ModelAsset.clear();
		if (data.contains("Model"))
		{
			ValidateFields(data.at("Model"), {"Asset"});
			if (!data.at("Model").at("Asset").is_string())
				throw std::invalid_argument("Model asset path must be a string");
			entity.ModelAsset = data.at("Model").at("Asset").get<std::string>();
			ValidateModelAssetPath(entity.ModelAsset);
		}
	}

	void Scene::Deserialize(const Json& data)
	{
		ValidateFields(data, {"Format", "NextID", "Entities", "Renderer"});
		if (data.at("Format") != 1 || !data.at("Entities").is_array())
			throw std::invalid_argument("Unsupported scene format");
		Scene candidate;
		if (data.contains("Renderer"))
			candidate.SetRenderSettings(RenderSettings::Deserialize(data["Renderer"]));
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
			const auto& nextValue = data["NextID"];
			if (!nextValue.is_number_integer() || nextValue.is_number_float())
				throw std::invalid_argument("Invalid next entity ID");
			EntityID parsedNext = 0;
			if (nextValue.is_number_unsigned())
			{
				auto value = nextValue.get<uint64_t>();
				if (value == 0 || value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
					throw std::invalid_argument("Invalid next entity ID");
				parsedNext = static_cast<EntityID>(value);
			}
			else
			{
				auto value = nextValue.get<int64_t>();
				if (value <= 0)
					throw std::invalid_argument("Invalid next entity ID");
				parsedNext = static_cast<EntityID>(value);
			}
			candidate.m_NextID = std::max(candidate.m_NextID, parsedNext);
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
			if (!command.is_object())
				throw std::invalid_argument("Scene command must be an object");
			if (!command.contains("Operation") || !command["Operation"].is_string())
				throw std::invalid_argument("Scene command requires a string Operation");
			auto operation = command["Operation"].get<std::string>();
			if (operation == "SetRendererSettings")
			{
				ValidateFields(command, {"Operation", "Settings"});
				if (!command.at("Settings").is_object())
					throw std::invalid_argument("Renderer settings command must contain an object");
				auto settings = candidate.GetRenderSettings().Serialize();
				for (const auto& [name, value] : command.at("Settings").items())
					settings[name] = value;
				candidate.SetRenderSettings(RenderSettings::Deserialize(settings));
			}
			else if (operation == "Create")
			{
				ValidateFields(command, {"Operation", "Name", "Translation"});
				if (!command.at("Name").is_string())
					throw std::invalid_argument("Entity name must be a string");
				auto id = candidate.CreateEntity(command.at("Name").get<std::string>());
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
				if (!entity.ModelAsset.empty())
					replacement["Model"] = {{"Asset", entity.ModelAsset}};
				ReadEntity(entity, replacement, false);
			}
			else if (operation == "SetModel")
			{
				ValidateFields(command, {"Operation", "ID", "Asset"});
				auto& entity = candidate.GetEntity(ReadEntityID(command.at("ID")));
				if (!command.at("Asset").is_string())
					throw std::invalid_argument("Model asset path must be a string");
				auto asset = command.at("Asset").get<std::string>();
				ValidateModelAssetPath(asset);
				entity.ModelAsset = std::move(asset);
			}
			else if (operation == "Rename")
			{
				ValidateFields(command, {"Operation", "ID", "Name"});
				auto& entity = candidate.GetEntity(ReadEntityID(command.at("ID")));
				if (!command.at("Name").is_string())
					throw std::invalid_argument("Entity name must be a string");
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
		if (!prefab.is_object() || !prefab.contains("Name") || !prefab["Name"].is_string())
			throw std::invalid_argument("Prefab requires a string Name");
		Scene candidate = *this;
		auto id = candidate.CreateEntity(prefab["Name"].get<std::string>());
		ReadEntity(candidate.GetEntity(id), prefab, false);
		*this = std::move(candidate);
		return id;
	}

	const RenderSettings& Scene::GetRenderSettings() const
	{
		return m_RenderSettings;
	}

	void Scene::SetRenderSettings(const RenderSettings& settings)
	{
		settings.Validate();
		m_RenderSettings = settings;
	}

} // namespace Bellwether
