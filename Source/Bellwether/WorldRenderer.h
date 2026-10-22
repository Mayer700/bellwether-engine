#pragma once

#include "Scene.h"
#include <nvrhi/nvrhi.h>

namespace Bellwether
{

	class WorldRenderer
	{
	  public:
		explicit WorldRenderer(nvrhi::IDevice* device);
		void Load(const std::filesystem::path& directory);
		void Render(nvrhi::ICommandList* commands, nvrhi::IFramebuffer* framebuffer, uint32_t width, uint32_t height);
		size_t GetDrawCount() const
		{
			return m_Instances.size();
		}

	  private:
		struct Mesh
		{
			nvrhi::BufferHandle Vertices;
			nvrhi::BufferHandle Indices;
			uint32_t IndexCount = 0;
		};
		struct Instance
		{
			size_t MeshIndex = 0;
			glm::mat4 Transform{1};
			glm::vec4 AlbedoRoughness{1};
			glm::vec4 EmissionMetallic{0};
		};
		Mesh UploadMesh(nvrhi::ICommandList* commands, const std::vector<float>& vertices,
						const std::vector<uint32_t>& indices);
		nvrhi::IDevice* m_Device;
		nvrhi::ShaderHandle m_VertexShader, m_FragmentShader;
		nvrhi::BindingLayoutHandle m_Layout;
		nvrhi::BindingSetHandle m_Bindings;
		nvrhi::InputLayoutHandle m_InputLayout;
		nvrhi::BufferHandle m_Camera;
		std::map<std::pair<nvrhi::Format, nvrhi::Format>, nvrhi::GraphicsPipelineHandle> m_Pipelines;
		std::vector<Mesh> m_Meshes;
		std::vector<Instance> m_Instances;
	};

} // namespace Bellwether
