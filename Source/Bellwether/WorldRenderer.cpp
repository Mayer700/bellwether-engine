#include "WorldRenderer.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>

namespace Bellwether
{

	static std::vector<char> ReadBinary(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if (!input || input.tellg() < 0 || input.tellg() > 256 * 1024 * 1024)
			throw std::runtime_error("Invalid or missing binary file: " + path.string());
		std::vector<char> data(static_cast<size_t>(input.tellg()));
		input.seekg(0);
		input.read(data.data(), static_cast<std::streamsize>(data.size()));
		if (!input)
			throw std::runtime_error("Truncated binary file: " + path.string());
		return data;
	}

	WorldRenderer::WorldRenderer(nvrhi::IDevice* device) : m_Device(device)
	{
		auto directory = std::filesystem::path(BELLWETHER_SHADER_DIRECTORY);
		auto vertex = ReadBinary(directory / "World.vert.spv");
		auto fragment = ReadBinary(directory / "World.frag.spv");
		m_VertexShader = device->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex),
											  vertex.data(), vertex.size());
		m_FragmentShader = device->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel),
												fragment.data(), fragment.size());
		nvrhi::VertexAttributeDesc attributes[] = {nvrhi::VertexAttributeDesc()
													   .setName("POSITION")
													   .setFormat(nvrhi::Format::RGB32_FLOAT)
													   .setOffset(0)
													   .setElementStride(32),
												   nvrhi::VertexAttributeDesc()
													   .setName("NORMAL")
													   .setFormat(nvrhi::Format::RGB32_FLOAT)
													   .setOffset(12)
													   .setElementStride(32),
												   nvrhi::VertexAttributeDesc()
													   .setName("TEXCOORD")
													   .setFormat(nvrhi::Format::RG32_FLOAT)
													   .setOffset(24)
													   .setElementStride(32)};
		m_InputLayout = device->createInputLayout(attributes, 3, m_VertexShader);
		m_Layout = device->createBindingLayout(nvrhi::BindingLayoutDesc()
												   .setVisibility(nvrhi::ShaderType::All)
												   .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
												   .addItem(nvrhi::BindingLayoutItem::PushConstants(1, 96)));
		m_Camera = device->createBuffer(nvrhi::BufferDesc()
											.setByteSize(80)
											.setIsConstantBuffer(true)
											.setIsVolatile(true)
											.setMaxVersions(16)
											.setDebugName("Camera"));
		m_Bindings = device->createBindingSet(nvrhi::BindingSetDesc()
												  .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_Camera))
												  .addItem(nvrhi::BindingSetItem::PushConstants(1, 96)),
											  m_Layout);
		if (!m_VertexShader || !m_FragmentShader || !m_Layout || !m_Camera || !m_Bindings || !m_InputLayout)
			throw std::runtime_error("World renderer resource initialization failed");
	}

	WorldRenderer::Mesh WorldRenderer::UploadMesh(nvrhi::ICommandList* commands, const std::vector<float>& vertices,
												  const std::vector<uint32_t>& indices)
	{
		Mesh result;
		result.IndexCount = static_cast<uint32_t>(indices.size());
		result.Vertices = m_Device->createBuffer(nvrhi::BufferDesc()
													 .setByteSize(vertices.size() * sizeof(float))
													 .setIsVertexBuffer(true)
													 .setInitialState(nvrhi::ResourceStates::VertexBuffer)
													 .setKeepInitialState(true)
													 .setDebugName("World Vertices"));
		result.Indices = m_Device->createBuffer(nvrhi::BufferDesc()
													.setByteSize(indices.size() * sizeof(uint32_t))
													.setIsIndexBuffer(true)
													.setInitialState(nvrhi::ResourceStates::IndexBuffer)
													.setKeepInitialState(true)
													.setDebugName("World Indices"));
		if (!result.Vertices || !result.Indices)
			throw std::runtime_error("Cannot allocate mesh buffers");
		commands->writeBuffer(result.Vertices, vertices.data(), vertices.size() * sizeof(float));
		commands->writeBuffer(result.Indices, indices.data(), indices.size() * sizeof(uint32_t));
		return result;
	}

	void WorldRenderer::Load(const std::filesystem::path& directory)
	{
		auto manifest = ReadJson(directory / "world.json");
		if (manifest.at("format") != 1)
			throw std::runtime_error("Unsupported world bridge format");
		std::vector<Mesh> meshes;
		std::vector<Instance> instances;
		auto commands = m_Device->createCommandList();
		commands->open();
		for (const auto& entry : manifest.at("meshes"))
		{
			auto relative = std::filesystem::path(entry.at("path").get<std::string>());
			if (relative.is_absolute() || relative.generic_string().find("..") != std::string::npos)
				throw std::invalid_argument("Mesh paths must stay within the content directory");
			auto data = ReadBinary(directory / relative);
			if (data.size() < 8)
				throw std::runtime_error("Truncated mesh header");
			uint32_t vertexCount = 0, indexCount = 0;
			std::memcpy(&vertexCount, data.data(), 4);
			std::memcpy(&indexCount, data.data() + 4, 4);
			if (!vertexCount || !indexCount || indexCount % 3 || vertexCount > 10000000 || indexCount > 30000000 ||
				data.size() != 8 + size_t(vertexCount) * 32 + size_t(indexCount) * 4)
				throw std::runtime_error("Invalid mesh sizes");
			std::vector<float> vertices(size_t(vertexCount) * 8);
			std::vector<uint32_t> indices(indexCount);
			std::memcpy(vertices.data(), data.data() + 8, vertices.size() * 4);
			std::memcpy(indices.data(), data.data() + 8 + vertices.size() * 4, indices.size() * 4);
			for (auto value : vertices)
				if (!std::isfinite(value))
					throw std::runtime_error("Non-finite mesh vertex data");
			for (auto index : indices)
				if (index >= vertexCount)
					throw std::runtime_error("Mesh index out of range");
			meshes.push_back(UploadMesh(commands, vertices, indices));
		}
		for (const auto& entry : manifest.at("draws"))
		{
			if (!entry.at("visible").get<bool>())
				continue;
			Instance instance;
			if (!entry.at("mesh").is_number_unsigned() && !entry.at("mesh").is_number_integer())
				throw std::runtime_error("Mesh instance index must be integral");
			instance.MeshIndex = entry.at("mesh");
			if (instance.MeshIndex >= meshes.size() || entry.at("matrix").size() != 16)
				throw std::runtime_error("Invalid mesh instance");
			for (int column = 0; column < 4; column++)
				for (int row = 0; row < 4; row++)
					instance.Transform[column][row] = entry.at("matrix")[column * 4 + row];
			for (int column = 0; column < 4; column++)
				for (int row = 0; row < 4; row++)
					if (!std::isfinite(instance.Transform[column][row]))
						throw std::runtime_error("Non-finite instance transform");
			if (std::abs(glm::determinant(instance.Transform)) < 0.00000001f)
				throw std::runtime_error("Singular instance transform");
			instance.AlbedoRoughness =
				glm::vec4(ReadVector(Json::array({entry.at("color")[0], entry.at("color")[1], entry.at("color")[2]})),
						  entry.at("roughness").get<float>());
			instance.EmissionMetallic = glm::vec4(ReadVector(entry.at("emission")), entry.at("metallic").get<float>());
			if (!std::isfinite(instance.AlbedoRoughness.w) || instance.AlbedoRoughness.w < 0 ||
				instance.AlbedoRoughness.w > 1 || !std::isfinite(instance.EmissionMetallic.w) ||
				instance.EmissionMetallic.w < 0 || instance.EmissionMetallic.w > 1)
				throw std::runtime_error("Invalid material roughness or metallic value");
			instances.push_back(instance);
		}
		int size = manifest.at("height_size");
		float extent = manifest.at("world_size");
		auto heightBytes = ReadBinary(directory / "height.f32");
		if (size < 2 || size > 8192 || heightBytes.size() != size_t(size) * size * 4 || !std::isfinite(extent) ||
			extent <= 0)
			throw std::runtime_error("Invalid heightfield");
		std::vector<float> heights(size_t(size) * size);
		std::memcpy(heights.data(), heightBytes.data(), heightBytes.size());
		for (auto value : heights)
			if (!std::isfinite(value))
				throw std::runtime_error("Non-finite terrain height");
		constexpr int gridSize = 513;
		std::vector<float> vertices;
		std::vector<uint32_t> indices;
		vertices.reserve(gridSize * gridSize * 8);
		auto sample = [&](int x, int z)
		{ return heights[size_t(std::clamp(z, 0, size - 1)) * size + std::clamp(x, 0, size - 1)]; };
		for (int z = 0; z < gridSize; z++)
		{
			for (int x = 0; x < gridSize; x++)
			{
				int hx = x * (size - 1) / (gridSize - 1), hz = z * (size - 1) / (gridSize - 1);
				auto normal = glm::normalize(glm::vec3(sample(hx - 1, hz) - sample(hx + 1, hz), 2 * extent / size,
													   sample(hx, hz - 1) - sample(hx, hz + 1)));
				vertices.insert(vertices.end(), {x * extent / (gridSize - 1) - extent / 2, sample(hx, hz),
												 z * extent / (gridSize - 1) - extent / 2, normal.x, normal.y, normal.z,
												 x / float(gridSize - 1), z / float(gridSize - 1)});
			}
		}
		for (uint32_t z = 0; z < gridSize - 1; z++)
			for (uint32_t x = 0; x < gridSize - 1; x++)
			{
				auto index = z * gridSize + x;
				indices.insert(indices.end(),
							   {index, index + gridSize, index + 1, index + 1, index + gridSize, index + gridSize + 1});
			}
		Instance terrain;
		terrain.MeshIndex = meshes.size();
		terrain.AlbedoRoughness = {.85f, .91f, .98f, .85f};
		meshes.push_back(UploadMesh(commands, vertices, indices));
		instances.push_back(terrain);
		commands->close();
		m_Device->executeCommandList(commands);
		m_Device->waitForIdle();
		m_Meshes = std::move(meshes);
		m_Instances = std::move(instances);
	}

	void WorldRenderer::Render(nvrhi::ICommandList* commands, nvrhi::IFramebuffer* framebuffer, uint32_t width,
							   uint32_t height)
	{
		if (!width || !height)
			throw std::invalid_argument("Render dimensions must be positive");
		const auto& framebufferInfo = framebuffer->getFramebufferInfo();
		if (framebufferInfo.colorFormats.size() != 1 || framebufferInfo.sampleCount != 1 ||
			framebufferInfo.depthFormat == nvrhi::Format::UNKNOWN)
			throw std::invalid_argument("World renderer requires one color target, one depth target and one sample");
		auto format = std::make_pair(framebufferInfo.colorFormats[0], framebufferInfo.depthFormat);
		if (!m_Pipelines.contains(format))
		{
			auto descriptor = nvrhi::GraphicsPipelineDesc()
								  .setVertexShader(m_VertexShader)
								  .setPixelShader(m_FragmentShader)
								  .setInputLayout(m_InputLayout)
								  .addBindingLayout(m_Layout);
			descriptor.renderState.rasterState.setCullMode(nvrhi::RasterCullMode::None);
			descriptor.renderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(true).setDepthFunc(
				nvrhi::ComparisonFunc::Less);
			m_Pipelines[format] = m_Device->createGraphicsPipeline(descriptor, framebuffer->getFramebufferInfo());
			if (!m_Pipelines[format])
				throw std::runtime_error("World graphics pipeline creation failed");
		}
		struct CameraData
		{
			glm::mat4 ViewProjection;
			glm::vec4 Position;
		};
		glm::vec3 target{0, 1, 10}, eye = target + glm::vec3(22, 30, 34);
		float aspect = float(width) / height;
		auto projection = glm::orthoRH_ZO(-12.f * aspect, 12.f * aspect, -12.f, 12.f, .1f, 400.f);
		// NVRHI already inverts the Vulkan viewport height; retain a conventional projection.
		CameraData camera{projection * glm::lookAtRH(eye, target, {0, 1, 0}), glm::vec4(eye, 1)};
		commands->writeBuffer(m_Camera, &camera, sizeof(camera));
		nvrhi::GraphicsState state;
		state.setPipeline(m_Pipelines.at(format))
			.setFramebuffer(framebuffer)
			.addBindingSet(m_Bindings)
			.setViewport(
				nvrhi::ViewportState().addViewportAndScissorRect(nvrhi::Viewport(float(width), float(height))));
		for (const auto& instance : m_Instances)
		{
			if (glm::length(glm::vec2(instance.Transform[3].x - target.x, instance.Transform[3].z - target.z)) > 80)
				continue;
			const auto& mesh = m_Meshes[instance.MeshIndex];
			state.vertexBuffers.resize(0);
			state.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(mesh.Vertices).setSlot(0).setOffset(0));
			state.setIndexBuffer(
				nvrhi::IndexBufferBinding().setBuffer(mesh.Indices).setFormat(nvrhi::Format::R32_UINT).setOffset(0));
			commands->setGraphicsState(state);
			struct ObjectData
			{
				glm::mat4 Transform;
				glm::vec4 AlbedoRoughness;
				glm::vec4 EmissionMetallic;
			};
			ObjectData object{instance.Transform, instance.AlbedoRoughness, instance.EmissionMetallic};
			static_assert(sizeof(ObjectData) == 96);
			commands->setPushConstants(&object, sizeof(object));
			commands->drawIndexed(nvrhi::DrawArguments().setVertexCount(mesh.IndexCount));
		}
	}

} // namespace Bellwether
