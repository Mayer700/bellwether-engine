#pragma once

#include <filesystem>
#include <memory>
#include <string>

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
		void CaptureWorld(const std::filesystem::path& output);
		std::string GetDeviceName() const;
		unsigned GetValidationErrors() const;
		void Shutdown();

	  private:
		class Implementation;
		std::unique_ptr<Implementation> m_Implementation;
	};

} // namespace Bellwether
