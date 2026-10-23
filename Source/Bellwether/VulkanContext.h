#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include "RenderSettings.h"

namespace Bellwether
{

	class VulkanContext
	{
	  public:
		VulkanContext(int width, int height, const std::string& title, bool hidden = false);
		~VulkanContext();
		VulkanContext(const VulkanContext&) = delete;
		VulkanContext& operator=(const VulkanContext&) = delete;
		bool ShouldClose() const;
		bool RenderFrame();
		void Resize(int width, int height);
		void VerifyReadback(const std::filesystem::path& output);
		void LoadWorld(const std::filesystem::path& directory);
		void LoadModel(const std::filesystem::path& path);
		void LoadScene(const std::filesystem::path& path);
		void CaptureWorld(const std::filesystem::path& output);
		std::string GetDeviceName() const;
		unsigned GetValidationErrors() const;
		void Shutdown();
		void SetRenderSettings(const RenderSettings& settings);
		void VerifyRenderer(const std::filesystem::path& outputDirectory,
							const std::filesystem::path& fixtureDirectory);
		void VerifyModelRenderer(const std::filesystem::path& outputDirectory,
								 const std::filesystem::path& fixtureDirectory);

	  private:
		class Implementation;
		std::unique_ptr<Implementation> m_Implementation;
	};

} // namespace Bellwether
