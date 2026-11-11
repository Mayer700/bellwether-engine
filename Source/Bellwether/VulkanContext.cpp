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
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
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
		float PreviewTime = 0.0f;
		SnowField InteractiveSnow;
		bool HasInteractiveSnow = false;
		bool SpaceWasDown = false;
		std::unique_ptr<SnowSandbox> Sandbox;
		uint64_t LastSandboxMutation = 0;
		bool MouseLeftDown = false;
		bool HasPrevMouseWorldXZ = false;
		glm::vec2 PrevMouseWorldXZ{0.0f, 0.0f};
		glm::vec2 PrevSpaceXZ{0.0f, 0.0f};
		bool LaunchBoulderNext = false;
		bool KeyFWasDown = false;
		bool KeyGWasDown = false;
		bool KeyEnterWasDown = false;
		bool KeyTWasDown = false;
		bool KeyRWasDown = false;
		bool KeyHWasDown = false;
		bool Key3WasDown = false;
		bool Key4WasDown = false;
		bool Key5WasDown = false;
		bool Key6WasDown = false;
		std::array<bool, 4> KeyFnWasDown{false, false, false, false};
		float TitleUpdateAccumulator = 0.0f;

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
				Sandbox.reset();
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
			if (Swapchain)
			{
				Device.destroySwapchainKHR(Swapchain);
				Swapchain = nullptr;
			}
			auto capabilities = PhysicalDevice.getSurfaceCapabilitiesKHR(Surface);
			int width = 0, height = 0;
			glfwGetFramebufferSize(Window, &width, &height);
			Extent = capabilities.currentExtent;
			if (Extent.width == std::numeric_limits<uint32_t>::max())
				Extent = vk::Extent2D(std::clamp(static_cast<uint32_t>(width), capabilities.minImageExtent.width,
												 capabilities.maxImageExtent.width),
									  std::clamp(static_cast<uint32_t>(height), capabilities.minImageExtent.height,
												 capabilities.maxImageExtent.height));
			if (!Extent.width || !Extent.height)
				return;
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
			vk::SwapchainCreateInfoKHR descriptor(
				{}, Surface, imageCount, selected->format, selected->colorSpace, Extent, 1,
				vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst,
				vk::SharingMode::eExclusive, 0, nullptr, capabilities.currentTransform,
				vk::CompositeAlphaFlagBitsKHR::eOpaque, vk::PresentModeKHR::eFifo, true, nullptr);
			Swapchain = Device.createSwapchainKHR(descriptor);
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
			if (!Swapchain || static_cast<uint32_t>(width) != Extent.width ||
				static_cast<uint32_t>(height) != Extent.height)
			{
				CreateSwapchain();
				if (!Swapchain)
					return false;
			}
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

	namespace
	{
		void ResetInteractiveInputState(bool& hasInteractiveSnow, float& previewTime, bool& spaceWasDown,
										bool& mouseLeftDown, bool& hasPrevMouseWorldXZ, float& titleAccum)
		{
			hasInteractiveSnow = false;
			previewTime = 0.0f;
			spaceWasDown = false;
			mouseLeftDown = false;
			hasPrevMouseWorldXZ = false;
			titleAccum = 0.0f;
		}

		void SyncSandboxDynamicInstances(WorldRenderer& world, const SnowSandbox& sandbox)
		{
			for (const auto& inst : sandbox.GetDynamicInstances())
				world.SetInstanceTransform(inst.PlacementIndex, inst.Transform, inst.Visible);
		}

		void UpdateSandboxWindowTitle(GLFWwindow* window, const SnowSandbox& sandbox, glm::vec2 probeXZ)
		{
			if (!window)
				return;
			const SnowSample sample = sandbox.SampleAt(probeXZ);
			std::ostringstream title;
			title << std::fixed << std::setprecision(2) << "Bellwether Snow Sandbox | [" << sandbox.GetScenarioName()
				  << "] Depth:" << sample.EffectiveDepth << "m Ice:" << sample.Ice << " Moist:" << sample.Moisture << " "
				  << std::setprecision(1) << sandbox.GetAmbientCelsius()
				  << "C | F1-F4/T:Scenario LMB:Carve RMB/M:Melt F:Freeze G:Launch H:Help";
			glfwSetWindowTitle(window, title.str().c_str());
		}

		void PrintSandboxControls(const SnowSandbox& sandbox, glm::vec2 probeXZ)
		{
			const SnowSample sample = sandbox.SampleAt(probeXZ);
			std::cout << "\n=== Bellwether Real-Time Snow Sandbox ===\n"
					  << "Scenario      : " << sandbox.GetScenarioName() << " (F1:Tracks F2:Melt F3:Blizzard F4:Sparkle, T:Next, R:Reset)\n"
					  << "Probe (" << probeXZ.x << ", " << probeXZ.y << ") : Depth=" << sample.EffectiveDepth
					  << "m Moisture=" << sample.Moisture << " Ice=" << sample.Ice
					  << " Compression=" << sample.Compression << " Temp=" << sandbox.GetAmbientCelsius() << "C\n"
					  << "Navigation    : WASD/Arrows pan camera, Q/E zoom\n"
					  << "Snow Tools    : Left-Mouse drag/Space carve tracks | Right-Mouse/M melt snow | F cold-snap freeze\n"
					  << "Weather & Sim : 1/2 snowfall intensity | 3/4 wind preset | 5/6 sparkle/SSS | G/Enter launch sled/boulder\n"
					  << "=========================================\n";
		}
	} // namespace

	void VulkanContext::LoadWorld(const std::filesystem::path& directory)
	{
		if (!m_Implementation->Renderer)
			throw std::logic_error("Cannot load a world after context shutdown");
		auto candidate = std::make_unique<WorldRenderer>(m_Implementation->Renderer);
		candidate->Load(directory);
		std::cout << "WORLD_INSTANCES " << candidate->GetDrawCount() << '\n';
		m_Implementation->Sandbox.reset();
		m_Implementation->World = std::move(candidate);
		ResetInteractiveInputState(m_Implementation->HasInteractiveSnow, m_Implementation->PreviewTime,
								   m_Implementation->SpaceWasDown, m_Implementation->MouseLeftDown,
								   m_Implementation->HasPrevMouseWorldXZ, m_Implementation->TitleUpdateAccumulator);
	}

	void VulkanContext::LoadModel(const std::filesystem::path& path)
	{
		if (!m_Implementation->Renderer)
			throw std::logic_error("Cannot load a model after context shutdown");
		auto candidate = std::make_unique<WorldRenderer>(m_Implementation->Renderer);
		candidate->LoadModel(path);
		std::cout << "MODEL_INSTANCES " << candidate->GetDrawCount() << '\n';
		m_Implementation->Sandbox.reset();
		m_Implementation->World = std::move(candidate);
		ResetInteractiveInputState(m_Implementation->HasInteractiveSnow, m_Implementation->PreviewTime,
								   m_Implementation->SpaceWasDown, m_Implementation->MouseLeftDown,
								   m_Implementation->HasPrevMouseWorldXZ, m_Implementation->TitleUpdateAccumulator);
	}

	void VulkanContext::LoadScene(const std::filesystem::path& path)
	{
		if (!m_Implementation->Renderer)
			throw std::logic_error("Cannot load a scene after context shutdown");
		Scene scene;
		scene.Load(path);
		auto candidate = std::make_unique<WorldRenderer>(m_Implementation->Renderer);
		candidate->LoadScene(scene, std::filesystem::absolute(path).parent_path());
		std::cout << "SCENE_MODEL_INSTANCES " << candidate->GetDrawCount() << '\n';
		m_Implementation->Sandbox.reset();
		m_Implementation->World = std::move(candidate);
		ResetInteractiveInputState(m_Implementation->HasInteractiveSnow, m_Implementation->PreviewTime,
								   m_Implementation->SpaceWasDown, m_Implementation->MouseLeftDown,
								   m_Implementation->HasPrevMouseWorldXZ, m_Implementation->TitleUpdateAccumulator);
	}

	void VulkanContext::LoadSnowSandbox(SnowTestScenario scenario)
	{
		if (!m_Implementation->Renderer)
			throw std::logic_error("Cannot load snow sandbox after context shutdown");
		auto sandbox = std::make_unique<SnowSandbox>(scenario);
		auto candidate = std::make_unique<WorldRenderer>(m_Implementation->Renderer);
		candidate->LoadAsset(sandbox->GetRenderAsset(), sandbox->GetRenderSettings(), &sandbox->GetSnowField());
		SyncSandboxDynamicInstances(*candidate, *sandbox);
		std::cout << "SNOW_SANDBOX_LOADED scenario=" << sandbox->GetScenarioName()
				  << " instances=" << candidate->GetDrawCount() << '\n';
		m_Implementation->Sandbox = std::move(sandbox);
		m_Implementation->World = std::move(candidate);
		ResetInteractiveInputState(m_Implementation->HasInteractiveSnow, m_Implementation->PreviewTime,
								   m_Implementation->SpaceWasDown, m_Implementation->MouseLeftDown,
								   m_Implementation->HasPrevMouseWorldXZ, m_Implementation->TitleUpdateAccumulator);
		m_Implementation->LastSandboxMutation = m_Implementation->Sandbox->GetSnowMutationCounter();
		UpdateSandboxWindowTitle(m_Implementation->Window, *m_Implementation->Sandbox, {0.0f, 4.0f});
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
		auto encodeBackground = [&](float value)
		{
			value *= implementation.World->GetSettings().Exposure;
			float tone =
				std::clamp((value * (2.51f * value + .03f)) / (value * (2.43f * value + .59f) + .14f), 0.f, 1.f);
			return int(std::lround(std::pow(tone, 1 / 2.2f) * 255));
		};
		const std::array<int, 3> background{encodeBackground(.055f), encodeBackground(.09f), encodeBackground(.14f)};
		for (uint32_t y = 0; y < height; y++)
			for (uint32_t x = 0; x < width; x++)
			{
				auto* pixel = pixels + y * rowPitch + x * 4;
				bool geometry = std::abs(int(pixel[0]) - background[0]) > 3 ||
								std::abs(int(pixel[1]) - background[1]) > 3 ||
								std::abs(int(pixel[2]) - background[2]) > 3;
				geometryPixels += geometry;
				foregroundPixels += geometry && (unsigned(pixel[0]) + pixel[1] + pixel[2]) / 3 < 190;
				file.write(reinterpret_cast<char*>(pixel), 3);
			}
		renderer->unmapStagingTexture(staging);
		if (!file || geometryPixels < 5000)
			throw std::runtime_error(
				"World capture has insufficient pixels distinct from its tone-mapped background: " +
				std::to_string(geometryPixels));
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
		if (m_Implementation->Sandbox)
			m_Implementation->Sandbox->SetRenderSettings(settings);
		m_Implementation->World->SetSettings(settings);
	}

	void VulkanContext::UpdateSnowField(const SnowField& snow)
	{
		if (!m_Implementation->World)
			throw std::logic_error("Load a world before updating the snowfield");
		m_Implementation->World->UpdateSnowField(snow);
		m_Implementation->InteractiveSnow = snow;
		m_Implementation->HasInteractiveSnow = true;
	}

	void VulkanContext::ClearSnowField()
	{
		if (!m_Implementation->World)
			throw std::logic_error("Load a world before clearing the snowfield");
		m_Implementation->World->ClearSnowField();
		m_Implementation->HasInteractiveSnow = false;
	}

	void VulkanContext::SetTimeSeconds(float seconds)
	{
		if (!m_Implementation->World)
			throw std::logic_error("Load a world before updating time");
		m_Implementation->World->SetTimeSeconds(seconds);
	}

	void VulkanContext::StepInteractivePreview(float deltaSeconds)
	{
		auto& impl = *m_Implementation;
		if (!impl.Window || !impl.World || !std::isfinite(deltaSeconds))
			return;
		const float dt = std::clamp(deltaSeconds, 0.0f, 0.1f);
		impl.PreviewTime += dt;
		impl.World->SetTimeSeconds(impl.PreviewTime);

		auto keyDown = [&](int key) { return glfwGetKey(impl.Window, key) == GLFW_PRESS; };
		auto edgePressed = [&](int key, bool& prev)
		{
			const bool down = keyDown(key);
			const bool triggered = down && !prev;
			prev = down;
			return triggered;
		};
		RenderSettings settings = impl.World->GetSettings();
		bool settingsChanged = false;
		const float panSpeed = std::max(4.0f, settings.CameraSize * 0.75f);
		if (keyDown(GLFW_KEY_W) || keyDown(GLFW_KEY_UP))
		{
			settings.CameraTarget.z = std::clamp(settings.CameraTarget.z - panSpeed * dt, -99000.0f, 99000.0f);
			settingsChanged = true;
		}
		if (keyDown(GLFW_KEY_S) || keyDown(GLFW_KEY_DOWN))
		{
			settings.CameraTarget.z = std::clamp(settings.CameraTarget.z + panSpeed * dt, -99000.0f, 99000.0f);
			settingsChanged = true;
		}
		if (keyDown(GLFW_KEY_A) || keyDown(GLFW_KEY_LEFT))
		{
			settings.CameraTarget.x = std::clamp(settings.CameraTarget.x - panSpeed * dt, -99000.0f, 99000.0f);
			settingsChanged = true;
		}
		if (keyDown(GLFW_KEY_D) || keyDown(GLFW_KEY_RIGHT))
		{
			settings.CameraTarget.x = std::clamp(settings.CameraTarget.x + panSpeed * dt, -99000.0f, 99000.0f);
			settingsChanged = true;
		}
		if (keyDown(GLFW_KEY_Q))
		{
			settings.CameraSize = std::clamp(settings.CameraSize * (1.0f + 0.8f * dt), 4.0f, 200.0f);
			settingsChanged = true;
		}
		if (keyDown(GLFW_KEY_E))
		{
			settings.CameraSize = std::clamp(settings.CameraSize * (1.0f - 0.8f * dt), 4.0f, 200.0f);
			settingsChanged = true;
		}
		if (keyDown(GLFW_KEY_1))
		{
			settings.SnowfallIntensity = std::clamp(settings.SnowfallIntensity - 0.8f * dt, 0.0f, 1.0f);
			settingsChanged = true;
		}
		if (keyDown(GLFW_KEY_2))
		{
			settings.SnowfallIntensity = std::clamp(settings.SnowfallIntensity + 0.8f * dt, 0.0f, 1.0f);
			settingsChanged = true;
		}
		if (settingsChanged)
		{
			settings.Validate();
			impl.World->SetSettings(settings);
			if (impl.Sandbox)
				impl.Sandbox->SetRenderSettings(settings);
		}

		if (impl.Sandbox)
		{
			auto& sandbox = *impl.Sandbox;
			std::optional<SnowTestScenario> nextScenario;
			if (edgePressed(GLFW_KEY_F1, impl.KeyFnWasDown[0]))
				nextScenario = SnowTestScenario::TracksAndPhysics;
			if (edgePressed(GLFW_KEY_F2, impl.KeyFnWasDown[1]))
				nextScenario = SnowTestScenario::ThermalMeltAndIce;
			if (edgePressed(GLFW_KEY_F3, impl.KeyFnWasDown[2]))
				nextScenario = SnowTestScenario::BlizzardDrift;
			if (edgePressed(GLFW_KEY_F4, impl.KeyFnWasDown[3]))
				nextScenario = SnowTestScenario::SunnySparkle;
			if (edgePressed(GLFW_KEY_T, impl.KeyTWasDown))
			{
				const int idx = (static_cast<int>(sandbox.GetScenario()) + 1) % 4;
				nextScenario = static_cast<SnowTestScenario>(idx);
			}
			if (edgePressed(GLFW_KEY_R, impl.KeyRWasDown))
				nextScenario = sandbox.GetScenario();

			if (nextScenario.has_value())
			{
				sandbox.ResetScenario(*nextScenario);
				impl.World->LoadAsset(sandbox.GetRenderAsset(), sandbox.GetRenderSettings(), &sandbox.GetSnowField());
				SyncSandboxDynamicInstances(*impl.World, sandbox);
				impl.LastSandboxMutation = sandbox.GetSnowMutationCounter();
				impl.PreviewTime = sandbox.GetElapsedSeconds();
				impl.World->SetTimeSeconds(impl.PreviewTime);
				impl.HasPrevMouseWorldXZ = false;
				const glm::vec2 centerXZ(sandbox.GetRenderSettings().CameraTarget.x,
										 sandbox.GetRenderSettings().CameraTarget.z);
				sandbox.SetProbePosition(centerXZ);
				UpdateSandboxWindowTitle(impl.Window, sandbox, centerXZ);
				std::cout << "SNOW_SANDBOX_SCENARIO " << sandbox.GetScenarioName() << '\n';
				return;
			}

			if (edgePressed(GLFW_KEY_3, impl.Key3WasDown))
			{
				sandbox.CycleWindPreset(-1);
				impl.World->SetSettings(sandbox.GetRenderSettings());
			}
			if (edgePressed(GLFW_KEY_4, impl.Key4WasDown))
			{
				sandbox.CycleWindPreset(1);
				impl.World->SetSettings(sandbox.GetRenderSettings());
			}
			if (edgePressed(GLFW_KEY_5, impl.Key5WasDown))
			{
				sandbox.CycleSparklePreset(-1);
				impl.World->SetSettings(sandbox.GetRenderSettings());
			}
			if (edgePressed(GLFW_KEY_6, impl.Key6WasDown))
			{
				sandbox.CycleSparklePreset(1);
				impl.World->SetSettings(sandbox.GetRenderSettings());
			}

			const glm::vec2 camXZ(settings.CameraTarget.x, settings.CameraTarget.z);
			glm::vec2 activeProbeXZ = camXZ;
			const bool focused = glfwGetWindowAttrib(impl.Window, GLFW_FOCUSED) != 0;

			if (focused)
			{
				int winW = 0, winH = 0;
				glfwGetWindowSize(impl.Window, &winW, &winH);
				double cursorX = 0.0, cursorY = 0.0;
				glfwGetCursorPos(impl.Window, &cursorX, &cursorY);
				if (winW > 0 && winH > 0 && cursorX >= 0.0 && cursorX < static_cast<double>(winW) &&
					cursorY >= 0.0 && cursorY < static_cast<double>(winH))
				{
					const RenderSettings activeSettings = impl.World->GetSettings();
					const float aspect = static_cast<float>(winW) / static_cast<float>(winH);
					const float ndcX = static_cast<float>(cursorX / static_cast<double>(winW)) * 2.0f - 1.0f;
					const float ndcY = 1.0f - static_cast<float>(cursorY / static_cast<double>(winH)) * 2.0f;
					const glm::mat4 invVP = glm::inverse(activeSettings.GetViewProjection(aspect));
					const glm::vec4 nearH = invVP * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
					const glm::vec4 farH = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
					if (std::abs(nearH.w) > 1e-6f && std::abs(farH.w) > 1e-6f)
					{
						const glm::vec3 nearW = glm::vec3(nearH) / nearH.w;
						const glm::vec3 farW = glm::vec3(farH) / farH.w;
						if (const auto hit = sandbox.RaycastGround(nearW, farW - nearW))
						{
							activeProbeXZ = {hit->x, hit->z};
							const bool leftDown = glfwGetMouseButton(impl.Window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
							const bool rightDown =
								glfwGetMouseButton(impl.Window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
							if (leftDown)
							{
								if (impl.MouseLeftDown && impl.HasPrevMouseWorldXZ)
									sandbox.StampStroke(impl.PrevMouseWorldXZ, activeProbeXZ, 1.45f, 0.32f);
								else
									sandbox.StampAt(activeProbeXZ, 1.65f, 0.34f);
								impl.MouseLeftDown = true;
								impl.PrevMouseWorldXZ = activeProbeXZ;
								impl.HasPrevMouseWorldXZ = true;
							}
							else
							{
								impl.MouseLeftDown = false;
								impl.HasPrevMouseWorldXZ = false;
							}
							if (rightDown)
								sandbox.MeltAt(activeProbeXZ, 2.8f, 1.6f, dt);
						}
						else
						{
							impl.MouseLeftDown = false;
							impl.HasPrevMouseWorldXZ = false;
						}
					}
				}
				else
				{
					impl.MouseLeftDown = false;
					impl.HasPrevMouseWorldXZ = false;
				}
			}
			else
			{
				impl.MouseLeftDown = false;
				impl.HasPrevMouseWorldXZ = false;
			}

			sandbox.SetProbePosition(activeProbeXZ);
			sandbox.SetProbeVisible(true);

			const bool spaceDown = keyDown(GLFW_KEY_SPACE);
			if (spaceDown && !impl.SpaceWasDown)
			{
				sandbox.StampAt(activeProbeXZ, 1.75f, 0.35f);
				impl.PrevSpaceXZ = activeProbeXZ;
			}
			else if (spaceDown && impl.SpaceWasDown && glm::length(activeProbeXZ - impl.PrevSpaceXZ) > 0.1f)
			{
				sandbox.StampStroke(impl.PrevSpaceXZ, activeProbeXZ, 1.45f, 0.32f);
				impl.PrevSpaceXZ = activeProbeXZ;
			}
			impl.SpaceWasDown = spaceDown;

			if (keyDown(GLFW_KEY_M))
				sandbox.MeltAt(activeProbeXZ, 2.8f, 1.6f, dt);

			if (edgePressed(GLFW_KEY_F, impl.KeyFWasDown))
				sandbox.TriggerColdSnapFreeze(3.0f);

			const bool launchG = edgePressed(GLFW_KEY_G, impl.KeyGWasDown);
			const bool launchEnter = edgePressed(GLFW_KEY_ENTER, impl.KeyEnterWasDown);
			if (launchG || launchEnter)
			{
				const bool boulder = impl.LaunchBoulderNext;
				impl.LaunchBoulderNext = !impl.LaunchBoulderNext;
				sandbox.LaunchSled({activeProbeXZ.x - 8.5f, activeProbeXZ.y}, {11.5f, 0.0f}, boulder);
			}

			if (edgePressed(GLFW_KEY_H, impl.KeyHWasDown))
				PrintSandboxControls(sandbox, activeProbeXZ);

			sandbox.Step(dt);
			impl.PreviewTime = sandbox.GetElapsedSeconds();
			impl.World->SetTimeSeconds(impl.PreviewTime);

			const uint64_t mutation = sandbox.GetSnowMutationCounter();
			if (mutation != impl.LastSandboxMutation)
			{
				impl.World->UpdateSnowField(sandbox.GetSnowField());
				impl.LastSandboxMutation = mutation;
			}
			SyncSandboxDynamicInstances(*impl.World, sandbox);

			impl.TitleUpdateAccumulator += dt;
			if (impl.TitleUpdateAccumulator >= 0.25f)
			{
				impl.TitleUpdateAccumulator = 0.0f;
				UpdateSandboxWindowTitle(impl.Window, sandbox, activeProbeXZ);
			}
			return;
		}

		const bool spaceDown = keyDown(GLFW_KEY_SPACE);
		if (!impl.HasInteractiveSnow && impl.World->HasSnowField())
		{
			impl.InteractiveSnow = impl.World->GetSnowField();
			impl.HasInteractiveSnow = true;
		}
		bool snowChanged = false;
		if (spaceDown && !impl.SpaceWasDown)
		{
			if (!impl.HasInteractiveSnow)
			{
				SnowFieldConfig config;
				config.Resolution = 128;
				config.WorldExtent = 64.0f;
				config.BaseDepth = 0.45f;
				config.MaxDepth = 1.5f;
				impl.InteractiveSnow.Reset(config);
				impl.HasInteractiveSnow = true;
			}
			impl.InteractiveSnow.StampCircle(glm::vec2(settings.CameraTarget.x, settings.CameraTarget.z), 1.6f, 0.30f);
			snowChanged = true;
		}
		impl.SpaceWasDown = spaceDown;
		if (impl.HasInteractiveSnow && settings.SnowfallIntensity > 0.01f)
		{
			const uint64_t mutationBefore = impl.InteractiveSnow.GetMutationCounter();
			impl.InteractiveSnow.StepWeather(dt, settings.SnowfallIntensity * 0.15f,
											 glm::vec2(settings.WindVelocity.x, settings.WindVelocity.z));
			if (impl.InteractiveSnow.GetMutationCounter() != mutationBefore)
				snowChanged = true;
		}
		if (snowChanged)
			impl.World->UpdateSnowField(impl.InteractiveSnow);
	}

	void VulkanContext::VerifyRenderer(const std::filesystem::path& outputDirectory,
									   const std::filesystem::path& fixtureDirectory)
	{
		if (!m_Implementation->World)
			throw std::logic_error("Load the renderer fixture before verification");
		Bellwether::VerifyRenderer(m_Implementation->Renderer, *m_Implementation->World, outputDirectory,
								   fixtureDirectory);
	}

	void VulkanContext::VerifySnowRenderer(const std::filesystem::path& outputDirectory,
										   const std::filesystem::path& fixtureDirectory)
	{
		if (!m_Implementation->World)
			throw std::logic_error("Load the renderer fixture before snow verification");
		Bellwether::VerifySnowRenderer(m_Implementation->Renderer, *m_Implementation->World, outputDirectory,
									   fixtureDirectory);
	}

	void VulkanContext::VerifySnowSandbox(const std::filesystem::path& outputDirectory)
	{
		if (!m_Implementation->World)
			throw std::logic_error("Load the snow sandbox before verification");
		Bellwether::VerifySnowSandbox(m_Implementation->Renderer, *m_Implementation->World, outputDirectory);
	}

	void VulkanContext::VerifyModelRenderer(const std::filesystem::path& outputDirectory,
											const std::filesystem::path& fixtureDirectory)
	{
		if (!m_Implementation->World)
			throw std::logic_error("Load the model fixture before verification");
		Bellwether::VerifyModelRenderer(m_Implementation->Renderer, *m_Implementation->World, outputDirectory,
										fixtureDirectory);
	}

} // namespace Bellwether
