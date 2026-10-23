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
		void SetSettings(const RenderSettings& settings);
		const RenderSettings& GetSettings() const;
		nvrhi::ITexture* GetHDRTexture() const;
		nvrhi::ITexture* GetShadowTexture(size_t cascade) const;
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
			glm::vec3 BoundsCenter{0};
			float BoundsRadius = 0;
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
		void ResizeTargets(uint32_t width, uint32_t height);
		void DrawGeometry(nvrhi::ICommandList* commands, nvrhi::IFramebuffer* framebuffer,
						  nvrhi::IGraphicsPipeline* pipeline, nvrhi::IBindingSet* bindings, uint32_t width,
						  uint32_t height, float range);
		struct FrameData
		{
			glm::mat4 ViewProjection;
			std::array<glm::mat4, 2> LightViewProjection;
			glm::vec4 CameraPosition;
			glm::vec4 SunDirection;
			glm::vec4 SunRadianceFog;
			glm::vec4 RenderParameters;
		};
		nvrhi::IDevice* m_Device;
		nvrhi::ShaderHandle m_VertexShader, m_FragmentShader;
		nvrhi::BindingLayoutHandle m_Layout;
		nvrhi::BindingSetHandle m_Bindings;
		nvrhi::InputLayoutHandle m_InputLayout;
		nvrhi::BufferHandle m_Camera;
		std::map<std::pair<nvrhi::Format, nvrhi::Format>, nvrhi::GraphicsPipelineHandle> m_Pipelines;
		nvrhi::ShaderHandle m_ShadowShader, m_FullscreenShader, m_TonemapShader;
		nvrhi::BindingLayoutHandle m_ShadowLayout, m_PostLayout;
		nvrhi::BindingSetHandle m_ShadowBindings, m_PostBindings;
		nvrhi::GraphicsPipelineHandle m_ShadowPipeline, m_LightingPipeline;
		nvrhi::SamplerHandle m_ShadowSampler, m_LinearSampler;
		std::array<nvrhi::TextureHandle, 2> m_ShadowMaps;
		std::array<nvrhi::FramebufferHandle, 2> m_ShadowFramebuffers;
		nvrhi::TextureHandle m_HDR, m_HDRDepth;
		nvrhi::FramebufferHandle m_HDRFramebuffer;
		uint32_t m_Width = 0, m_Height = 0;
		static constexpr uint32_t s_ShadowResolution = 2048;
		RenderSettings m_Settings;
		std::vector<Mesh> m_Meshes;
		std::vector<Instance> m_Instances;
	};

} // namespace Bellwether
