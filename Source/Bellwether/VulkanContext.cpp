#include "VulkanContext.h"
#include "WorldRenderer.h"
#include "RendererVerification.h"

#define GLFW_INCLUDE_NONE
#include <vulkan/vulkan.hpp>
#include <GLFW/glfw3.h>
#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace Bellwether
{

	class VulkanContext::Implementation : public nvrhi::IMessageCallback
	{
	  public:
		GLFWwindow* Window = nullptr;
		vk::Instance Instance;
		vk::SurfaceKHR Surface;
		vk::PhysicalDevice PhysicalDevice;
		vk::Device Device;
		vk::Queue Queue;
		uint32_t QueueFamily = 0;
		vk::SwapchainKHR Swapchain;
		vk::Extent2D Extent;
		nvrhi::vulkan::DeviceHandle Backend;
		nvrhi::DeviceHandle Renderer;
		std::vector<nvrhi::TextureHandle> SwapImages;
		std::vector<nvrhi::FramebufferHandle> Framebuffers;
		nvrhi::TextureHandle Depth;
		std::unique_ptr<WorldRenderer> World;
		std::vector<vk::Semaphore> PresentSemaphores;
		std::array<vk::Semaphore, 2> AcquireSemaphores;
		std::array<nvrhi::EventQueryHandle, 2> FrameEvents;
		std::array<nvrhi::CommandListHandle, 2> Commands;
		std::array<bool, 2> Submitted{false, false};
		uint32_t FrameIndex = 0;
		unsigned Errors = 0;
		std::string DeviceName;

		void message(nvrhi::MessageSeverity severity, const char* messageText) override
		{
			if (severity == nvrhi::MessageSeverity::Error || severity == nvrhi::MessageSeverity::Fatal)
				Errors++;
			std::cerr << "NVRHI: " << messageText << '\n';
		}

		~Implementation()
		{
			Shutdown();
		}

		void Shutdown()
		{
			if (Device)
			{
				Device.waitIdle();
				Commands = {};
				World.reset();
				FrameEvents = {};
				Framebuffers.clear();
				Depth = nullptr;
				SwapImages.clear();
				if (Renderer)
					Renderer->runGarbageCollection();
				Renderer = nullptr;
				Backend = nullptr;
				for (auto semaphore : PresentSemaphores)
					Device.destroySemaphore(semaphore);
				for (auto semaphore : AcquireSemaphores)
					if (semaphore)
						Device.destroySemaphore(semaphore);
				if (Swapchain)
					Device.destroySwapchainKHR(Swapchain);
				Device.destroy();
				Device = nullptr;
			}
			if (Surface)
			{
				Instance.destroySurfaceKHR(Surface);
				Surface = nullptr;
			}
			if (Instance)
			{
				Instance.destroy();
				Instance = nullptr;
			}
			if (Window)
			{
				glfwDestroyWindow(Window);
				Window = nullptr;
			}
			glfwTerminate();
		}

		void Initialize(int width, int height, const std::string& title, bool hidden)
		{
			glfwSetErrorCallback([](int code, const char* description)
								 { std::cerr << "GLFW " << code << ": " << description << '\n'; });
			if (!glfwInit())
				throw std::runtime_error("GLFW initialization failed");
			if (!glfwVulkanSupported())
				throw std::runtime_error("A Vulkan loader and compatible driver are required");
			glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
			glfwWindowHint(GLFW_VISIBLE, hidden ? GLFW_FALSE : GLFW_TRUE);
			Window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
			if (!Window)
				throw std::runtime_error("Cannot create engine window");
			auto getInstanceProcAddress = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
				glfwGetInstanceProcAddress(nullptr, "vkGetInstanceProcAddr"));
			if (!getInstanceProcAddress)
				throw std::runtime_error("Cannot load Vulkan entry points");
			VULKAN_HPP_DEFAULT_DISPATCHER.init(getInstanceProcAddress);
			uint32_t extensionCount = 0;
			auto requiredExtensions = glfwGetRequiredInstanceExtensions(&extensionCount);
			std::vector<const char*> extensions(requiredExtensions, requiredExtensions + extensionCount);
			vk::InstanceCreateFlags instanceFlags;
#if defined(__APPLE__)
			extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
			instanceFlags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
#endif
			vk::ApplicationInfo application("Bellwether Engine", VK_MAKE_VERSION(0, 1, 0), "Bellwether",
											VK_MAKE_VERSION(0, 1, 0), VK_API_VERSION_1_3);
			vk::InstanceCreateInfo instanceInfo(instanceFlags, &application, 0, nullptr,
												static_cast<uint32_t>(extensions.size()), extensions.data());
			Instance = vk::createInstance(instanceInfo);
			VULKAN_HPP_DEFAULT_DISPATCHER.init(Instance);
			VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
			if (glfwCreateWindowSurface(Instance, Window, nullptr, &rawSurface) != VK_SUCCESS)
				throw std::runtime_error("Cannot create Vulkan presentation surface");
			Surface = rawSurface;
			int bestScore = -1;
			for (auto candidate : Instance.enumeratePhysicalDevices())
			{
				auto properties = candidate.getProperties();
				if (properties.apiVersion < VK_API_VERSION_1_3)
					continue;
				vk::PhysicalDeviceVulkan13Features features13;
				vk::PhysicalDeviceVulkan12Features features12;
				features12.pNext = &features13;
				vk::PhysicalDeviceFeatures2 features;
				features.pNext = &features12;
				candidate.getFeatures2(&features);
				if (!features12.timelineSemaphore || !features13.synchronization2 || !features13.dynamicRendering)
					continue;
				auto families = candidate.getQueueFamilyProperties();
				for (uint32_t index = 0; index < families.size(); index++)
				{
					if (!(families[index].queueFlags & vk::QueueFlagBits::eGraphics) ||
						!candidate.getSurfaceSupportKHR(index, Surface))
						continue;
					int score = properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu ? 100 : 10;
					if (score > bestScore)
					{
						bestScore = score;
						PhysicalDevice = candidate;
						QueueFamily = index;
						DeviceName = properties.deviceName.data();
					}
				}
			}
			if (!PhysicalDevice)
				throw std::runtime_error(
					"No Vulkan 1.3 graphics/present device with timeline semaphores and synchronization2");
			vk::PhysicalDeviceVulkan13Features features13;
			features13.synchronization2 = true;
			features13.dynamicRendering = true;
			vk::PhysicalDeviceVulkan12Features features12;
			features12.timelineSemaphore = true;
			features12.pNext = &features13;
			float priority = 1;
			vk::DeviceQueueCreateInfo queueInfo({}, QueueFamily, 1, &priority);
			std::vector<const char*> deviceExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
#if defined(__APPLE__)
			deviceExtensions.push_back("VK_KHR_portability_subset");
#endif
			vk::DeviceCreateInfo deviceInfo({}, 1, &queueInfo, 0, nullptr,
											static_cast<uint32_t>(deviceExtensions.size()), deviceExtensions.data());
			deviceInfo.pNext = &features12;
			Device = PhysicalDevice.createDevice(deviceInfo);
			VULKAN_HPP_DEFAULT_DISPATCHER.init(Device);
			Queue = Device.getQueue(QueueFamily, 0);
			nvrhi::vulkan::DeviceDesc descriptor{};
			descriptor.errorCB = this;
			descriptor.instance = Instance;
			descriptor.physicalDevice = PhysicalDevice;
			descriptor.device = Device;
			descriptor.graphicsQueue = Queue;
			descriptor.graphicsQueueIndex = static_cast<int>(QueueFamily);
			descriptor.instanceExtensions = extensions.data();
			descriptor.numInstanceExtensions = extensions.size();
			descriptor.deviceExtensions = deviceExtensions.data();
			descriptor.numDeviceExtensions = deviceExtensions.size();
			Backend = nvrhi::vulkan::createDevice(descriptor);
			if (!Backend)
				throw std::runtime_error("NVRHI Vulkan device creation failed");
			Renderer = nvrhi::validation::createValidationLayer(Backend);
			for (size_t index = 0; index < 2; index++)
			{
				AcquireSemaphores[index] = Device.createSemaphore({});
				FrameEvents[index] = Renderer->createEventQuery();
				Commands[index] = Renderer->createCommandList();
			}
			CreateSwapchain();
		}

		void CreateSwapchain()
		{
			Renderer->waitForIdle();
			Commands = {};
			Framebuffers.clear();
			Depth = nullptr;
			SwapImages.clear();
			Renderer->runGarbageCollection();
			for (auto semaphore : PresentSemaphores)
				Device.destroySemaphore(semaphore);
			PresentSemaphores.clear();
			auto capabilities = PhysicalDevice.getSurfaceCapabilitiesKHR(Surface);
			int width = 0, height = 0;
			glfwGetFramebufferSize(Window, &width, &height);
			Extent = capabilities.currentExtent;
			if (Extent.width == std::numeric_limits<uint32_t>::max())
				Extent = vk::Extent2D(std::clamp(static_cast<uint32_t>(width), capabilities.minImageExtent.width,
												 capabilities.maxImageExtent.width),
									  std::clamp(static_cast<uint32_t>(height), capabilities.minImageExtent.height,
												 capabilities.maxImageExtent.height));
			auto formats = PhysicalDevice.getSurfaceFormatsKHR(Surface);
			auto selected = std::find_if(formats.begin(), formats.end(),
										 [](const auto& format)
										 {
											 return format.format == vk::Format::eB8G8R8A8Unorm &&
													format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear;
										 });
			if (selected == formats.end())
				throw std::runtime_error("Required BGRA8_UNORM swapchain format unavailable");
			uint32_t imageCount = std::max(2u, capabilities.minImageCount + 1);
			if (capabilities.maxImageCount)
				imageCount = std::min(imageCount, capabilities.maxImageCount);
			auto oldSwapchain = Swapchain;
			vk::SwapchainCreateInfoKHR descriptor(
				{}, Surface, imageCount, selected->format, selected->colorSpace, Extent, 1,
				vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst,
				vk::SharingMode::eExclusive, 0, nullptr, capabilities.currentTransform,
				vk::CompositeAlphaFlagBitsKHR::eOpaque, vk::PresentModeKHR::eFifo, true, oldSwapchain);
			Swapchain = Device.createSwapchainKHR(descriptor);
			if (oldSwapchain)
				Device.destroySwapchainKHR(oldSwapchain);
			Depth = Renderer->createTexture(nvrhi::TextureDesc()
												.setWidth(Extent.width)
												.setHeight(Extent.height)
												.setFormat(nvrhi::Format::D32)
												.setIsRenderTarget(true)
												.setInitialState(nvrhi::ResourceStates::DepthWrite)
												.setKeepInitialState(true)
												.setDebugName("Scene Depth"));
			for (auto image : Device.getSwapchainImagesKHR(Swapchain))
			{
				auto texture = nvrhi::TextureDesc()
								   .setWidth(Extent.width)
								   .setHeight(Extent.height)
								   .setFormat(nvrhi::Format::BGRA8_UNORM)
								   .setIsRenderTarget(true)
								   .setInitialState(nvrhi::ResourceStates::Present)
								   .setKeepInitialState(true)
								   .setDebugName("Swapchain");
				SwapImages.push_back(Renderer->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image,
																			nvrhi::Object(image), texture));
				Framebuffers.push_back(Renderer->createFramebuffer(
					nvrhi::FramebufferDesc().addColorAttachment(SwapImages.back()).setDepthAttachment(Depth)));
				PresentSemaphores.push_back(Device.createSemaphore({}));
			}
			for (auto& commands : Commands)
				commands = Renderer->createCommandList();
			for (auto& query : FrameEvents)
				Renderer->resetEventQuery(query);
			Submitted = {false, false};
		}

		bool RenderFrame()
		{
			if (!Window)
				throw std::logic_error("Cannot render after context shutdown");
			glfwPollEvents();
			int width = 0, height = 0;
			glfwGetFramebufferSize(Window, &width, &height);
			if (!width || !height)
			{
				glfwWaitEventsTimeout(0.05);
				return false;
			}
			if (static_cast<uint32_t>(width) != Extent.width || static_cast<uint32_t>(height) != Extent.height)
				CreateSwapchain();
			auto index = FrameIndex % 2;
			if (Submitted[index])
			{
				Renderer->waitEventQuery(FrameEvents[index]);
				Renderer->resetEventQuery(FrameEvents[index]);
			}
			uint32_t imageIndex = 0;
			auto acquired =
				Device.acquireNextImageKHR(Swapchain, 1000000000, AcquireSemaphores[index], nullptr, &imageIndex);
			if (acquired == vk::Result::eErrorOutOfDateKHR)
			{
				CreateSwapchain();
				return false;
			}
			if (acquired == vk::Result::eTimeout || acquired == vk::Result::eNotReady)
				return false;
			if (acquired != vk::Result::eSuccess && acquired != vk::Result::eSuboptimalKHR)
				throw std::runtime_error("Swapchain acquire failed: " + vk::to_string(acquired));
			Backend->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, AcquireSemaphores[index], 0);
			auto& commands = Commands[index];
			commands->open();
			// Common maps to Vulkan UNDEFINED: discard the acquired image's previous contents.
			commands->beginTrackingTextureState(SwapImages[imageIndex], nvrhi::AllSubresources,
												nvrhi::ResourceStates::Common);
			commands->clearTextureFloat(SwapImages[imageIndex], nvrhi::AllSubresources,
										nvrhi::Color(0.055f, 0.09f, 0.14f, 1));
			if (World)
			{
				commands->clearDepthStencilTexture(Depth, nvrhi::AllSubresources, true, 1, false, 0);
				World->Render(commands, Framebuffers[imageIndex], Extent.width, Extent.height);
			}
			commands->setTextureState(SwapImages[imageIndex], nvrhi::AllSubresources, nvrhi::ResourceStates::Present);
			commands->commitBarriers();
			commands->close();
			Backend->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, PresentSemaphores[imageIndex], 0);
			Renderer->executeCommandList(commands);
			Renderer->setEventQuery(FrameEvents[index], nvrhi::CommandQueue::Graphics);
			Submitted[index] = true;
			auto semaphore = PresentSemaphores[imageIndex];
			vk::PresentInfoKHR presentInfo(1, &semaphore, 1, &Swapchain, &imageIndex);
			auto presented = Queue.presentKHR(presentInfo);
			if (presented == vk::Result::eErrorOutOfDateKHR || presented == vk::Result::eSuboptimalKHR ||
				acquired == vk::Result::eSuboptimalKHR)
				CreateSwapchain();
			else if (presented != vk::Result::eSuccess)
				throw std::runtime_error("Swapchain present failed: " + vk::to_string(presented));
			Renderer->runGarbageCollection();
			FrameIndex++;
			return true;
		}
	};

	VulkanContext::VulkanContext(int width, int height, const std::string& title, bool hidden)
		: m_Implementation(std::make_unique<Implementation>())
	{
		m_Implementation->Initialize(width, height, title, hidden);
	}

	VulkanContext::~VulkanContext() = default;

	bool VulkanContext::ShouldClose() const
	{
		return !m_Implementation->Window || glfwWindowShouldClose(m_Implementation->Window);
	}

	bool VulkanContext::RenderFrame()
	{
		return m_Implementation->RenderFrame();
	}

	void VulkanContext::Resize(int width, int height)
	{
		if (width <= 0 || height <= 0)
			throw std::invalid_argument("Resize dimensions must be positive");
		if (!m_Implementation->Window)
			throw std::logic_error("Cannot resize after context shutdown");
		glfwSetWindowSize(m_Implementation->Window, width, height);
	}

	void VulkanContext::VerifyReadback(const std::filesystem::path& output)
	{
		if (!m_Implementation->Renderer)
			throw std::logic_error("Cannot read back after context shutdown");
		if (!output.parent_path().empty())
			std::filesystem::create_directories(output.parent_path());
		auto& implementation = *m_Implementation;
		auto& renderer = implementation.Renderer;
		auto description = nvrhi::TextureDesc()
							   .setWidth(63)
							   .setHeight(47)
							   .setFormat(nvrhi::Format::RGBA8_UNORM)
							   .setIsRenderTarget(true)
							   .setInitialState(nvrhi::ResourceStates::CopySource)
							   .setKeepInitialState(true)
							   .setDebugName("Readback Probe");
		auto texture = renderer->createTexture(description);
		auto staging = renderer->createStagingTexture(description, nvrhi::CpuAccessMode::Read);
		auto commands = renderer->createCommandList();
		commands->open();
		commands->clearTextureFloat(texture, nvrhi::AllSubresources, nvrhi::Color(0.125f, 0.25f, 0.5f, 1));
		commands->copyTexture(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice());
		commands->close();
		renderer->executeCommandList(commands);
		renderer->waitForIdle();
		size_t rowPitch = 0;
		auto* pixels = static_cast<unsigned char*>(
			renderer->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
		if (!pixels)
			throw std::runtime_error("GPU readback mapping failed");
		std::ofstream file(output, std::ios::binary);
		file << "P6\n63 47\n255\n";
		bool matched = true;
		for (size_t y = 0; y < 47; y++)
		{
			for (size_t x = 0; x < 63; x++)
			{
				auto* pixel = pixels + y * rowPitch + x * 4;
				matched &= std::abs(int(pixel[0]) - 32) <= 1 && std::abs(int(pixel[1]) - 64) <= 1 &&
						   std::abs(int(pixel[2]) - 128) <= 1 && pixel[3] == 255;
				file.write(reinterpret_cast<char*>(pixel), 3);
			}
		}
		renderer->unmapStagingTexture(staging);
		if (!file || !matched)
			throw std::runtime_error("GPU readback probe did not match expected pixels");
	}

	void VulkanContext::LoadWorld(const std::filesystem::path& directory)
	{
		if (!m_Implementation->Renderer)
			throw std::logic_error("Cannot load a world after context shutdown");
		auto candidate = std::make_unique<WorldRenderer>(m_Implementation->Renderer);
		candidate->Load(directory);
		std::cout << "WORLD_INSTANCES " << candidate->GetDrawCount() << '\n';
		m_Implementation->World = std::move(candidate);
	}

	void VulkanContext::CaptureWorld(const std::filesystem::path& output)
	{
		if (!output.parent_path().empty())
			std::filesystem::create_directories(output.parent_path());
		auto& implementation = *m_Implementation;
		if (!implementation.World)
			throw std::runtime_error("No world loaded for capture");
		auto& renderer = implementation.Renderer;
		constexpr uint32_t width = 960, height = 600;
		auto description = nvrhi::TextureDesc()
							   .setWidth(width)
							   .setHeight(height)
							   .setFormat(nvrhi::Format::RGBA8_UNORM)
							   .setIsRenderTarget(true)
							   .setInitialState(nvrhi::ResourceStates::CopySource)
							   .setKeepInitialState(true);
		auto color = renderer->createTexture(description);
		auto depth = renderer->createTexture(nvrhi::TextureDesc()
												 .setWidth(width)
												 .setHeight(height)
												 .setFormat(nvrhi::Format::D32)
												 .setIsRenderTarget(true)
												 .setInitialState(nvrhi::ResourceStates::DepthWrite)
												 .setKeepInitialState(true));
		auto framebuffer =
			renderer->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(color).setDepthAttachment(depth));
		auto staging = renderer->createStagingTexture(description, nvrhi::CpuAccessMode::Read);
		auto commands = renderer->createCommandList();
		commands->open();
		commands->clearTextureFloat(color, nvrhi::AllSubresources, nvrhi::Color(0.055f, 0.09f, 0.14f, 1));
		commands->clearDepthStencilTexture(depth, nvrhi::AllSubresources, true, 1, false, 0);
		implementation.World->Render(commands, framebuffer, width, height);
		commands->copyTexture(staging, nvrhi::TextureSlice(), color, nvrhi::TextureSlice());
		commands->close();
		renderer->executeCommandList(commands);
		renderer->waitForIdle();
		size_t rowPitch = 0, geometryPixels = 0, foregroundPixels = 0;
		auto* pixels = static_cast<unsigned char*>(
			renderer->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
		if (!pixels)
			throw std::runtime_error("Cannot map world capture");
		std::ofstream file(output, std::ios::binary);
		file << "P6\n" << width << " " << height << "\n255\n";
		for (uint32_t y = 0; y < height; y++)
			for (uint32_t x = 0; x < width; x++)
			{
				auto* pixel = pixels + y * rowPitch + x * 4;
				geometryPixels += pixel[0] > 40 || pixel[1] > 45 || pixel[2] > 55;
				foregroundPixels += (pixel[0] > 40 || pixel[1] > 45 || pixel[2] > 55) &&
									(unsigned(pixel[0]) + pixel[1] + pixel[2]) / 3 < 160;
				file.write(reinterpret_cast<char*>(pixel), 3);
			}
		renderer->unmapStagingTexture(staging);
		if (!file || geometryPixels < 5000 || foregroundPixels < 3000)
			throw std::runtime_error("World capture has insufficient rendered geometry");
		std::cout << "WORLD_CAPTURE " << geometryPixels << " geometry pixels, " << foregroundPixels
				  << " foreground pixels\n";
	}

	std::string VulkanContext::GetDeviceName() const
	{
		return m_Implementation->DeviceName;
	}

	unsigned VulkanContext::GetValidationErrors() const
	{
		return m_Implementation->Errors;
	}

	void VulkanContext::Shutdown()
	{
		m_Implementation->Shutdown();
	}

	void VulkanContext::SetRenderSettings(const RenderSettings& settings)
	{
		if (!m_Implementation->World)
			throw std::logic_error("Load a world before applying render settings");
		m_Implementation->World->SetSettings(settings);
	}

	void VulkanContext::VerifyRenderer(const std::filesystem::path& outputDirectory,
									   const std::filesystem::path& fixtureDirectory)
	{
		if (!m_Implementation->World)
			throw std::logic_error("Load the renderer fixture before verification");
		Bellwether::VerifyRenderer(m_Implementation->Renderer, *m_Implementation->World, outputDirectory,
								   fixtureDirectory);
	}

} // namespace Bellwether
