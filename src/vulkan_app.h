#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <stb_image.h>

#include "camera.h"
#include "common.h"
#include "terrain.h"
#include "weather.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

class VulkanApp {
public:
    void run()
    {
        initWindow();
        initVulkan();
        initImGui();
        mainLoop();
        cleanup();
    }

private:
    GLFWwindow* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    std::uint32_t graphicsFamily_ = 0;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_{};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline skyPipeline_ = VK_NULL_HANDLE;
    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;
    VkImage skyboxImage_ = VK_NULL_HANDLE;
    VkDeviceMemory skyboxMemory_ = VK_NULL_HANDLE;
    VkImageView skyboxView_ = VK_NULL_HANDLE;
    VkSampler skyboxSampler_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;
    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory_ = VK_NULL_HANDLE;
    VkBuffer indexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory_ = VK_NULL_HANDLE;
    std::array<VkBuffer, kMaxFramesInFlight> uniformBuffers_{};
    std::array<VkDeviceMemory, kMaxFramesInFlight> uniformMemories_{};
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kMaxFramesInFlight> descriptorSets_{};
    VkDescriptorPool imguiPool_ = VK_NULL_HANDLE;
    std::array<VkSemaphore, kMaxFramesInFlight> imageAvailable_{};
    std::array<VkSemaphore, kMaxFramesInFlight> renderFinished_{};
    std::array<VkFence, kMaxFramesInFlight> inFlight_{};
    std::size_t currentFrame_ = 0;
    Terrain terrain_;
    Camera camera_;
    bool terrainDirty_ = true;
    bool wireframe_ = false;
    bool showFlowParticles_ = true;
    bool erosionAnimating_ = false;
    float erosionSpeed_ = 0.35f;
    bool framebufferResized_ = false;

    // --- weather / microclimate ---
    Weather weather_;
    WeatherParams weatherParams_{};
    bool weatherRunning_ = false;
    int weatherPreset_ = 0;
    int weatherGridXZ_ = 88;
    int weatherGridY_ = 56;
    bool showClouds_ = true;
    bool showRain_ = true;
    bool showSliceH_ = true;     // horizontal (Y) plane
    bool showSliceV_ = true;     // vertical plane
    int sliceH_ = 14;            // Y index for the horizontal plane
    int vertAxis_ = 1;           // vertical plane normal: 0 = X, 1 = Z
    int sliceV_ = 44;            // index of the vertical plane along its normal axis
    bool showWindStreaks_ = true;
    int weatherField_ = static_cast<int>(WeatherField::VerticalWind);
    float rainErosionDrops_ = 9000.0f;
    // temporally-smoothed colour range per field (kills slice flicker)
    float fieldLo_[9] = {0};
    float fieldHi_[9] = {0};
    bool fieldRangeInit_[9] = {false};

    // --- volumetric clouds (GPU ray-march) ---
    bool cloudVolumetric_ = true;       // true: ray-march; false: legacy billboard splats
    float cloudDensity_ = 1500.0f;      // extinction per unit qc
    int cloudSteps_ = 48;               // primary march samples
    float cloudSunAbsorb_ = 1.0f;       // self-shadow strength
    float cloudCoverage_ = 0.0f;        // density floor (trims wisps)
    static constexpr int kCloudHeightRes = 192;                 // occlusion heightmap resolution
    static constexpr int kCloudMaxCells = 144 * 96 * 144;       // SSBO capacity (grid slider maxima)
    VkDescriptorSetLayout cloudSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout cloudPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline cloudPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool cloudPool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kMaxFramesInFlight> cloudSets_{};
    std::array<VkBuffer, kMaxFramesInFlight> cloudFieldBuffers_{};
    std::array<VkDeviceMemory, kMaxFramesInFlight> cloudFieldMemories_{};
    std::array<void*, kMaxFramesInFlight> cloudFieldMapped_{};
    VkBuffer cloudHeightBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory cloudHeightMemory_ = VK_NULL_HANDLE;
    void* cloudHeightMapped_ = nullptr;

    // --- field slice planes drawn as depth-tested 3D geometry (clean per-pixel terrain
    //     cutout, vs. the old per-vertex ImGui-overlay occlusion which was blocky) ---
    struct SliceVertex { glm::vec3 pos; glm::vec4 color; };
    static constexpr int kSliceMaxVerts = 2 * 144 * 144 * 6; // both planes, worst-case grid
    VkPipeline slicePipeline_ = VK_NULL_HANDLE;
    std::array<VkBuffer, kMaxFramesInFlight> sliceVertexBuffers_{};
    std::array<VkDeviceMemory, kMaxFramesInFlight> sliceVertexMemories_{};
    std::array<void*, kMaxFramesInFlight> sliceVertexMapped_{};
    std::array<std::uint32_t, kMaxFramesInFlight> sliceVertexCount_{};

    // --- day/night clock (drives the sun for both rendering and the weather solver) ---
    bool clockAuto_ = true;         // auto-advance vs. manual scrub
    float timeOfDay_ = 9.0f;        // hours, 0..24
    float dayLengthSec_ = 90.0f;    // real seconds per simulated day
    float season_ = 0.4f;           // -1 = winter, +1 = summer (solar declination scale)
    float latitudeDeg_ = 45.0f;

    void initWindow()
    {
        if (!glfwInit()) throw std::runtime_error("Failed to initialize GLFW");
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window_ = glfwCreateWindow(1440, 900, "Mountains - Vulkan Terrain", nullptr, nullptr);
        if (!window_) throw std::runtime_error("Failed to create window");
        camera_.updateFromOrbit();
        glfwSetWindowUserPointer(window_, this);
        glfwSetScrollCallback(window_, [](GLFWwindow* window, double xOffset, double yOffset) {
            auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
            if (app) app->onScroll(xOffset, yOffset);
        });
        glfwSetFramebufferSizeCallback(window_, [](GLFWwindow* window, int, int) {
            auto* app = static_cast<VulkanApp*>(glfwGetWindowUserPointer(window));
            if (app) app->framebufferResized_ = true;
        });
    }

    void initVulkan()
    {
        createInstance();
        checkVk(glfwCreateWindowSurface(instance_, window_, nullptr, &surface_), "Failed to create surface");
        pickPhysicalDevice();
        createDevice();
        createSwapchain();
        createRenderPass();
        createDescriptorSetLayout();
        createCloudDescriptorSetLayout();
        createPipeline();
        createCloudPipeline();
        createSlicePipeline();
        createSliceResources();
        createDepthResources();
        createFramebuffers();
        createCommandPool();
        createSkyboxResources();
        createTerrainBuffers();
        createUniformBuffers();
        createCloudResources();
        createDescriptorPool();
        createDescriptorSets();
        createCloudDescriptors();
        createCommandBuffers();
        createSyncObjects();
        initWeather();
        uploadCloudHeightmap();
    }

    void initWeather()
    {
        float baseY = terrain_.minSurfaceHeight() - 4.0f;
        float lidY = terrain_.maxSurfaceHeight() + 70.0f;
        weather_.configure(weatherGridXZ_, weatherGridY_, weatherGridXZ_, baseY, lidY);
        syncWeatherTerrain();
        weather_.reset(weatherParams_);
        sliceH_ = weather_.ny() / 4;
        sliceV_ = weather_.nz() / 2;
        weather_.startWorker();
    }

    void rebuildWeatherGrid()
    {
        bool wasRunning = weatherRunning_;
        float baseY = terrain_.minSurfaceHeight() - 4.0f;
        float lidY = terrain_.maxSurfaceHeight() + 70.0f;
        weather_.configure(weatherGridXZ_, weatherGridY_, weatherGridXZ_, baseY, lidY);
        syncWeatherTerrain();
        weather_.reset(weatherParams_);
        sliceH_ = glm::clamp(sliceH_, 0, weather_.ny() - 1);
        sliceV_ = glm::clamp(sliceV_, 0, vertAxisCount() - 1);
        weatherRunning_ = wasRunning;
    }

    void syncWeatherTerrain()
    {
        int nx = weather_.nx();
        int nz = weather_.nz();
        std::vector<float> cols(static_cast<std::size_t>(nx) * nz);
        for (int k = 0; k < nz; ++k) {
            for (int i = 0; i < nx; ++i) {
                float wx = -kTerrainWorldSize * 0.5f + (i + 0.5f) / nx * kTerrainWorldSize;
                float wz = -kTerrainWorldSize * 0.5f + (k + 0.5f) / nz * kTerrainWorldSize;
                cols[static_cast<std::size_t>(k) * nx + i] = terrain_.surfaceHeightAtWorld(wx, wz);
            }
        }
        weather_.setColumnHeights(cols);
    }

    int vertAxisCount() const { return vertAxis_ == 0 ? weather_.nx() : weather_.nz(); }

    void createInstance()
    {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Mountains";
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.pEngineName = "Mountains";
        appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.apiVersion = VK_API_VERSION_1_2;

        std::uint32_t glfwCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwCount);
        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwCount);
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        checkVk(vkCreateInstance(&createInfo, nullptr, &instance_), "Failed to create Vulkan instance");
    }

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device)
    {
        QueueFamilyIndices indices;
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
        for (std::uint32_t i = 0; i < count; ++i) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) indices.graphics = i;
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &present);
            if (present) indices.present = i;
            if (indices.complete()) break;
        }
        return indices;
    }

    SwapchainSupport querySwapchain(VkPhysicalDevice device)
    {
        SwapchainSupport support;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &support.caps);
        std::uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &count, nullptr);
        support.formats.resize(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &count, support.formats.data());
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &count, nullptr);
        support.presentModes.resize(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &count, support.presentModes.data());
        return support;
    }

    void pickPhysicalDevice()
    {
        std::uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        if (count == 0) throw std::runtime_error("No Vulkan physical devices found");
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance_, &count, devices.data());
        for (VkPhysicalDevice device : devices) {
            QueueFamilyIndices indices = findQueueFamilies(device);
            SwapchainSupport swap = querySwapchain(device);
            if (indices.complete() && !swap.formats.empty() && !swap.presentModes.empty()) {
                physicalDevice_ = device;
                graphicsFamily_ = *indices.graphics;
                VkPhysicalDeviceProperties props{};
                vkGetPhysicalDeviceProperties(device, &props);
                std::cout << "Vulkan GPU: " << props.deviceName << "\n";
                return;
            }
        }
        throw std::runtime_error("No suitable Vulkan device found");
    }

    void createDevice()
    {
        QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);
        std::set<std::uint32_t> uniqueFamilies{*indices.graphics, *indices.present};
        float priority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queues;
        for (std::uint32_t family : uniqueFamilies) {
            VkDeviceQueueCreateInfo queue{};
            queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue.queueFamilyIndex = family;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
            queues.push_back(queue);
        }
        const std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME, "VK_KHR_portability_subset"};
        VkPhysicalDeviceFeatures features{};
        features.fillModeNonSolid = VK_TRUE;
        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queues.size());
        createInfo.pQueueCreateInfos = queues.data();
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.pEnabledFeatures = &features;
        checkVk(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_), "Failed to create Vulkan device");
        vkGetDeviceQueue(device_, *indices.graphics, 0, &graphicsQueue_);
        vkGetDeviceQueue(device_, *indices.present, 0, &presentQueue_);
    }

    VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats)
    {
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return format;
        }
        return formats.front();
    }

    VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes)
    {
        for (VkPresentModeKHR mode : modes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) return mode;
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& caps)
    {
        if (caps.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) return caps.currentExtent;
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        return {
            std::clamp(static_cast<std::uint32_t>(width), caps.minImageExtent.width, caps.maxImageExtent.width),
            std::clamp(static_cast<std::uint32_t>(height), caps.minImageExtent.height, caps.maxImageExtent.height)
        };
    }

    void createSwapchain()
    {
        SwapchainSupport support = querySwapchain(physicalDevice_);
        VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(support.formats);
        VkPresentModeKHR presentMode = choosePresentMode(support.presentModes);
        swapchainExtent_ = chooseExtent(support.caps);
        std::uint32_t imageCount = support.caps.minImageCount + 1;
        if (support.caps.maxImageCount > 0) imageCount = std::min(imageCount, support.caps.maxImageCount);

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = surface_;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = swapchainExtent_;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);
        std::array<std::uint32_t, 2> families{*indices.graphics, *indices.present};
        if (indices.graphics != indices.present) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = families.data();
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }
        createInfo.preTransform = support.caps.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        checkVk(vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_), "Failed to create swapchain");
        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
        swapchainImages_.resize(imageCount);
        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());
        swapchainFormat_ = surfaceFormat.format;
        swapchainImageViews_.resize(swapchainImages_.size());
        for (std::size_t i = 0; i < swapchainImages_.size(); ++i) {
            swapchainImageViews_[i] = createImageView(swapchainImages_[i], swapchainFormat_, VK_IMAGE_ASPECT_COLOR_BIT);
        }
    }

    VkImageView createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect)
    {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = image;
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = format;
        createInfo.subresourceRange.aspectMask = aspect;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;
        VkImageView view = VK_NULL_HANDLE;
        checkVk(vkCreateImageView(device_, &createInfo, nullptr, &view), "Failed to create image view");
        return view;
    }

    void createRenderPass()
    {
        VkAttachmentDescription color{};
        color.format = swapchainFormat_;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentDescription depth{};
        depth.format = VK_FORMAT_D32_SFLOAT;
        depth.samples = VK_SAMPLE_COUNT_1_BIT;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        std::array<VkAttachmentDescription, 2> attachments{color, depth};
        VkRenderPassCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        createInfo.attachmentCount = static_cast<std::uint32_t>(attachments.size());
        createInfo.pAttachments = attachments.data();
        createInfo.subpassCount = 1;
        createInfo.pSubpasses = &subpass;
        createInfo.dependencyCount = 1;
        createInfo.pDependencies = &dependency;
        checkVk(vkCreateRenderPass(device_, &createInfo, nullptr, &renderPass_), "Failed to create render pass");
    }

    void createDescriptorSetLayout()
    {
        VkDescriptorSetLayoutBinding ubo{};
        ubo.binding = 0;
        ubo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ubo.descriptorCount = 1;
        ubo.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutBinding skybox{};
        skybox.binding = 1;
        skybox.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        skybox.descriptorCount = 1;
        skybox.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{ubo, skybox};
        VkDescriptorSetLayoutCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        createInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        createInfo.pBindings = bindings.data();
        checkVk(vkCreateDescriptorSetLayout(device_, &createInfo, nullptr, &descriptorSetLayout_), "Failed to create descriptor set layout");
    }

    VkShaderModule createShaderModule(const std::vector<char>& code)
    {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const std::uint32_t*>(code.data());
        VkShaderModule module = VK_NULL_HANDLE;
        checkVk(vkCreateShaderModule(device_, &createInfo, nullptr, &module), "Failed to create shader module");
        return module;
    }

    void createPipeline()
    {
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorSetLayout_;
        checkVk(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_), "Failed to create pipeline layout");
        createSkyPipeline();
        createTerrainPipeline();
    }

    void createTerrainPipeline()
    {
        VkShaderModule vert = createShaderModule(readBinaryFile(std::string(SHADER_BINARY_DIR) + "/terrain.vert.spv"));
        VkShaderModule frag = createShaderModule(readBinaryFile(std::string(SHADER_BINARY_DIR) + "/terrain.frag.spv"));
        VkPipelineShaderStageCreateInfo vertStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertStage.module = vert;
        vertStage.pName = "main";
        VkPipelineShaderStageCreateInfo fragStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragStage.module = frag;
        fragStage.pName = "main";
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{vertStage, fragStage};

        VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        std::array<VkVertexInputAttributeDescription, 4> attributes{{
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
            {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, hydro)}
        }};
        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;
        std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();
        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depth;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = pipelineLayout_;
        pipelineInfo.renderPass = renderPass_;
        pipelineInfo.subpass = 0;
        checkVk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_), "Failed to create graphics pipeline");
        vkDestroyShaderModule(device_, vert, nullptr);
        vkDestroyShaderModule(device_, frag, nullptr);
    }

    void createSkyPipeline()
    {
        VkShaderModule vert = createShaderModule(readBinaryFile(std::string(SHADER_BINARY_DIR) + "/sky.vert.spv"));
        VkShaderModule frag = createShaderModule(readBinaryFile(std::string(SHADER_BINARY_DIR) + "/sky.frag.spv"));
        VkPipelineShaderStageCreateInfo vertStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertStage.module = vert;
        vertStage.pName = "main";
        VkPipelineShaderStageCreateInfo fragStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragStage.module = frag;
        fragStage.pName = "main";
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{vertStage, fragStage};

        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = VK_FALSE;
        depth.depthWriteEnable = VK_FALSE;
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;
        std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();
        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depth;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = pipelineLayout_;
        pipelineInfo.renderPass = renderPass_;
        pipelineInfo.subpass = 0;
        checkVk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &skyPipeline_), "Failed to create sky pipeline");
        vkDestroyShaderModule(device_, vert, nullptr);
        vkDestroyShaderModule(device_, frag, nullptr);
    }

    // ---- volumetric cloud pass --------------------------------------------------------------
    void createCloudDescriptorSetLayout()
    {
        VkDescriptorSetLayoutBinding ubo{};
        ubo.binding = 0;
        ubo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ubo.descriptorCount = 1;
        ubo.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutBinding field{};
        field.binding = 1;
        field.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        field.descriptorCount = 1;
        field.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutBinding height{};
        height.binding = 2;
        height.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        height.descriptorCount = 1;
        height.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{ubo, field, height};
        VkDescriptorSetLayoutCreateInfo createInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        createInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        createInfo.pBindings = bindings.data();
        checkVk(vkCreateDescriptorSetLayout(device_, &createInfo, nullptr, &cloudSetLayout_), "Failed to create cloud descriptor layout");
    }

    void createCloudPipeline()
    {
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &cloudSetLayout_;
        checkVk(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &cloudPipelineLayout_), "Failed to create cloud pipeline layout");

        VkShaderModule vert = createShaderModule(readBinaryFile(std::string(SHADER_BINARY_DIR) + "/cloud.vert.spv"));
        VkShaderModule frag = createShaderModule(readBinaryFile(std::string(SHADER_BINARY_DIR) + "/cloud.frag.spv"));
        VkPipelineShaderStageCreateInfo vertStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertStage.module = vert;
        vertStage.pName = "main";
        VkPipelineShaderStageCreateInfo fragStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragStage.module = frag;
        fragStage.pName = "main";
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{vertStage, fragStage};

        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = VK_FALSE;
        depth.depthWriteEnable = VK_FALSE;
        // Premultiplied-alpha "over": out = src.rgb + dst.rgb * (1 - src.a).
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;
        std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();
        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depth;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = cloudPipelineLayout_;
        pipelineInfo.renderPass = renderPass_;
        pipelineInfo.subpass = 0;
        checkVk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &cloudPipeline_), "Failed to create cloud pipeline");
        vkDestroyShaderModule(device_, vert, nullptr);
        vkDestroyShaderModule(device_, frag, nullptr);
    }

    void createCloudResources()
    {
        VkDeviceSize fieldBytes = static_cast<VkDeviceSize>(kCloudMaxCells) * sizeof(float);
        for (int i = 0; i < kMaxFramesInFlight; ++i) {
            createBuffer(fieldBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         cloudFieldBuffers_[i], cloudFieldMemories_[i]);
            vkMapMemory(device_, cloudFieldMemories_[i], 0, fieldBytes, 0, &cloudFieldMapped_[i]);
            std::memset(cloudFieldMapped_[i], 0, fieldBytes);
        }
        VkDeviceSize hgtBytes = static_cast<VkDeviceSize>(kCloudHeightRes) * kCloudHeightRes * sizeof(float);
        createBuffer(hgtBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     cloudHeightBuffer_, cloudHeightMemory_);
        vkMapMemory(device_, cloudHeightMemory_, 0, hgtBytes, 0, &cloudHeightMapped_);
        std::memset(cloudHeightMapped_, 0, hgtBytes);
    }

    void createCloudDescriptors()
    {
        std::array<VkDescriptorPoolSize, 2> poolSizes{{
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxFramesInFlight},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kMaxFramesInFlight},
        }};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        poolInfo.maxSets = kMaxFramesInFlight;
        checkVk(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &cloudPool_), "Failed to create cloud descriptor pool");

        std::array<VkDescriptorSetLayout, kMaxFramesInFlight> layouts{};
        layouts.fill(cloudSetLayout_);
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = cloudPool_;
        alloc.descriptorSetCount = kMaxFramesInFlight;
        alloc.pSetLayouts = layouts.data();
        checkVk(vkAllocateDescriptorSets(device_, &alloc, cloudSets_.data()), "Failed to allocate cloud descriptor sets");
        for (int i = 0; i < kMaxFramesInFlight; ++i) {
            VkDescriptorBufferInfo uboInfo{uniformBuffers_[i], 0, sizeof(SceneUniforms)};
            VkDescriptorBufferInfo fieldInfo{cloudFieldBuffers_[i], 0, VK_WHOLE_SIZE};
            VkDescriptorBufferInfo hgtInfo{cloudHeightBuffer_, 0, VK_WHOLE_SIZE};
            std::array<VkWriteDescriptorSet, 3> writes{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = cloudSets_[i];
            writes[0].dstBinding = 0;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].descriptorCount = 1;
            writes[0].pBufferInfo = &uboInfo;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = cloudSets_[i];
            writes[1].dstBinding = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[1].descriptorCount = 1;
            writes[1].pBufferInfo = &fieldInfo;
            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = cloudSets_[i];
            writes[2].dstBinding = 2;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[2].descriptorCount = 1;
            writes[2].pBufferInfo = &hgtInfo;
            vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }

    // Rasterize the terrain surface height into the occlusion heightmap (world XZ grid).
    void uploadCloudHeightmap()
    {
        if (!cloudHeightMapped_) return;
        float* h = static_cast<float*>(cloudHeightMapped_);
        const float WS = kTerrainWorldSize;
        for (int z = 0; z < kCloudHeightRes; ++z) {
            float wz = -WS * 0.5f + (z / static_cast<float>(kCloudHeightRes - 1)) * WS;
            for (int x = 0; x < kCloudHeightRes; ++x) {
                float wx = -WS * 0.5f + (x / static_cast<float>(kCloudHeightRes - 1)) * WS;
                h[z * kCloudHeightRes + x] = terrain_.surfaceHeightAtWorld(wx, wz);
            }
        }
    }

    // Push the current cloud-water volume into this frame's storage buffer.
    void updateCloudData(std::uint32_t frame)
    {
        if (!weather_.ready() || !cloudFieldMapped_[frame]) return;
        int cells = weather_.nx() * weather_.ny() * weather_.nz();
        if (cells <= 0 || cells > kCloudMaxCells) return;
        weather_.copyCloudField(static_cast<float*>(cloudFieldMapped_[frame]));
    }

    // ---- depth-tested 3D field-slice planes ------------------------------------------------
    void createSlicePipeline()
    {
        VkShaderModule vert = createShaderModule(readBinaryFile(std::string(SHADER_BINARY_DIR) + "/slice.vert.spv"));
        VkShaderModule frag = createShaderModule(readBinaryFile(std::string(SHADER_BINARY_DIR) + "/slice.frag.spv"));
        VkPipelineShaderStageCreateInfo vertStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT; vertStage.module = vert; vertStage.pName = "main";
        VkPipelineShaderStageCreateInfo fragStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT; fragStage.module = frag; fragStage.pName = "main";
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{vertStage, fragStage};

        VkVertexInputBindingDescription binding{0, sizeof(SliceVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        std::array<VkVertexInputAttributeDescription, 2> attributes{{
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SliceVertex, pos)},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SliceVertex, color)},
        }};
        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1; viewportState.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        // Depth-test against the terrain (drawn earlier in the pass) for a clean per-pixel
        // cutout, but don't write depth so the translucent slice can't occlude later draws.
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_FALSE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1; blend.pAttachments = &blendAttachment;
        std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();
        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depth;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = pipelineLayout_; // reuses the scene UBO (view/proj)
        pipelineInfo.renderPass = renderPass_;
        pipelineInfo.subpass = 0;
        checkVk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &slicePipeline_), "Failed to create slice pipeline");
        vkDestroyShaderModule(device_, vert, nullptr);
        vkDestroyShaderModule(device_, frag, nullptr);
    }

    void createSliceResources()
    {
        VkDeviceSize bytes = static_cast<VkDeviceSize>(kSliceMaxVerts) * sizeof(SliceVertex);
        for (int i = 0; i < kMaxFramesInFlight; ++i) {
            createBuffer(bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         sliceVertexBuffers_[i], sliceVertexMemories_[i]);
            vkMapMemory(device_, sliceVertexMemories_[i], 0, bytes, 0, &sliceVertexMapped_[i]);
            sliceVertexCount_[i] = 0;
        }
    }

    // Generate this frame's slice triangles (world-space pos + field color) into the mapped
    // vertex buffer. The GPU then depth-tests them against the terrain for a clean cutout.
    void buildSliceGeometry(std::uint32_t frame)
    {
        sliceVertexCount_[frame] = 0;
        if (!weather_.ready() || !sliceVertexMapped_[frame]) return;
        if (!showSliceH_ && !showSliceV_) return;
        int nx = weather_.nx(), ny = weather_.ny(), nz = weather_.nz();
        WeatherField field = static_cast<WeatherField>(weatherField_);
        float lo, hi; stableColorRange(field, lo, hi);
        float range = std::max(1e-4f, hi - lo);
        auto* verts = static_cast<SliceVertex*>(sliceVertexMapped_[frame]);
        std::uint32_t n = 0;
        const float alpha = 0.6f;

        auto colorAt = [&](int i, int j, int k) {
            float t = (weather_.sample(field, i, j, k, weatherParams_) - lo) / range;
            t = glm::clamp(t, 0.0f, 1.0f);
            glm::vec3 c;
            if (t < 0.25f) c = glm::mix(glm::vec3(0.10f, 0.12f, 0.55f), glm::vec3(0.0f, 0.65f, 0.85f), t / 0.25f);
            else if (t < 0.5f) c = glm::mix(glm::vec3(0.0f, 0.65f, 0.85f), glm::vec3(0.15f, 0.80f, 0.20f), (t - 0.25f) / 0.25f);
            else if (t < 0.75f) c = glm::mix(glm::vec3(0.15f, 0.80f, 0.20f), glm::vec3(0.95f, 0.85f, 0.10f), (t - 0.5f) / 0.25f);
            else c = glm::mix(glm::vec3(0.95f, 0.85f, 0.10f), glm::vec3(0.90f, 0.15f, 0.10f), (t - 0.75f) / 0.25f);
            return glm::vec4(c, alpha);
        };

        // Emit one quad (2 triangles) per cell of the plane. The depth test against the
        // terrain (drawn earlier in the pass) clips per-pixel for a clean mountain cutout.
        auto emitQuad = [&](int i0, int j0, int k0, int i1, int j1, int k1,
                            int i2, int j2, int k2, int i3, int j3, int k3) {
            if (n + 6 > static_cast<std::uint32_t>(kSliceMaxVerts)) return;
            SliceVertex v0{weather_.cellCenter(i0, j0, k0), colorAt(i0, j0, k0)};
            SliceVertex v1{weather_.cellCenter(i1, j1, k1), colorAt(i1, j1, k1)};
            SliceVertex v2{weather_.cellCenter(i2, j2, k2), colorAt(i2, j2, k2)};
            SliceVertex v3{weather_.cellCenter(i3, j3, k3), colorAt(i3, j3, k3)};
            verts[n++] = v0; verts[n++] = v1; verts[n++] = v2;
            verts[n++] = v0; verts[n++] = v2; verts[n++] = v3;
        };

        if (showSliceH_) {
            int j = glm::clamp(sliceH_, 0, ny - 1);
            for (int k = 0; k < nz - 1; ++k)
                for (int i = 0; i < nx - 1; ++i)
                    emitQuad(i, j, k, i + 1, j, k, i + 1, j, k + 1, i, j, k + 1);
        }
        if (showSliceV_) {
            int s = glm::clamp(sliceV_, 0, vertAxisCount() - 1);
            if (vertAxis_ == 0) { // X-normal plane: spans (j,k)
                for (int k = 0; k < nz - 1; ++k)
                    for (int j = 0; j < ny - 1; ++j)
                        emitQuad(s, j, k, s, j + 1, k, s, j + 1, k + 1, s, j, k + 1);
            } else {              // Z-normal plane: spans (i,j)
                for (int i = 0; i < nx - 1; ++i)
                    for (int j = 0; j < ny - 1; ++j)
                        emitQuad(i, j, s, i + 1, j, s, i + 1, j + 1, s, i, j + 1, s);
            }
        }
        sliceVertexCount_[frame] = n;
    }

    std::uint32_t findMemoryType(std::uint32_t typeFilter, VkMemoryPropertyFlags properties)
    {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);
        for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            if ((typeFilter & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) return i;
        }
        throw std::runtime_error("No suitable memory type found");
    }

    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory)
    {
        VkBufferCreateInfo createInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        createInfo.size = size;
        createInfo.usage = usage;
        createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checkVk(vkCreateBuffer(device_, &createInfo, nullptr, &buffer), "Failed to create buffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, buffer, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits, properties);
        checkVk(vkAllocateMemory(device_, &alloc, nullptr, &memory), "Failed to allocate buffer memory");
        vkBindBufferMemory(device_, buffer, memory, 0);
    }

    VkCommandBuffer beginSingleTimeCommands()
    {
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandPool = commandPool_;
        alloc.commandBufferCount = 1;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        checkVk(vkAllocateCommandBuffers(device_, &alloc, &commandBuffer), "Failed to allocate one-shot command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checkVk(vkBeginCommandBuffer(commandBuffer, &begin), "Failed to begin one-shot command buffer");
        return commandBuffer;
    }

    void endSingleTimeCommands(VkCommandBuffer commandBuffer)
    {
        checkVk(vkEndCommandBuffer(commandBuffer), "Failed to end one-shot command buffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer;
        checkVk(vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE), "Failed to submit one-shot command buffer");
        vkQueueWaitIdle(graphicsQueue_);
        vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
    }

    void copyBufferToImage(VkBuffer buffer, VkImage image, std::uint32_t width, std::uint32_t height, std::uint32_t layers)
    {
        VkCommandBuffer commandBuffer = beginSingleTimeCommands();
        std::vector<VkBufferImageCopy> regions;
        regions.reserve(layers);
        VkDeviceSize layerSize = static_cast<VkDeviceSize>(width) * height * 4;
        for (std::uint32_t layer = 0; layer < layers; ++layer) {
            VkBufferImageCopy region{};
            region.bufferOffset = layerSize * layer;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = layer;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {width, height, 1};
            regions.push_back(region);
        }
        vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(regions.size()), regions.data());
        endSingleTimeCommands(commandBuffer);
    }

    void transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, std::uint32_t layers)
    {
        VkCommandBuffer commandBuffer = beginSingleTimeCommands();
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = layers;
        VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        endSingleTimeCommands(commandBuffer);
    }

    void createImage(std::uint32_t width, std::uint32_t height, VkFormat format, VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& memory)
    {
        VkImageCreateInfo createInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        createInfo.imageType = VK_IMAGE_TYPE_2D;
        createInfo.extent = {width, height, 1};
        createInfo.mipLevels = 1;
        createInfo.arrayLayers = 1;
        createInfo.format = format;
        createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        createInfo.usage = usage;
        createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checkVk(vkCreateImage(device_, &createInfo, nullptr, &image), "Failed to create image");
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device_, image, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checkVk(vkAllocateMemory(device_, &alloc, nullptr, &memory), "Failed to allocate image memory");
        vkBindImageMemory(device_, image, memory, 0);
    }

    void createSkyboxResources()
    {
        const std::array<std::string, 6> faces{
            "assets/skybox/envmap_miramar/miramar_rt.tga",
            "assets/skybox/envmap_miramar/miramar_lf.tga",
            "assets/skybox/envmap_miramar/miramar_up.tga",
            "assets/skybox/envmap_miramar/miramar_dn.tga",
            "assets/skybox/envmap_miramar/miramar_ft.tga",
            "assets/skybox/envmap_miramar/miramar_bk.tga",
        };
        int width = 0;
        int height = 0;
        int channels = 0;
        std::vector<unsigned char> pixels;
        for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
            const std::string& face = faces[faceIndex];
            int w = 0;
            int h = 0;
            int c = 0;
            stbi_uc* data = stbi_load(face.c_str(), &w, &h, &c, STBI_rgb_alpha);
            if (!data) throw std::runtime_error("Failed to load skybox face: " + face);
            if (pixels.empty()) {
                width = w;
                height = h;
                channels = 4;
                pixels.resize(static_cast<std::size_t>(width) * height * channels * faces.size());
            }
            if (w != width || h != height) {
                stbi_image_free(data);
                throw std::runtime_error("Skybox faces must have matching sizes");
            }
            std::memcpy(pixels.data() + static_cast<std::size_t>(width) * height * channels * faceIndex, data, static_cast<std::size_t>(width) * height * channels);
            stbi_image_free(data);
        }

        VkDeviceSize imageSize = static_cast<VkDeviceSize>(pixels.size());
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingMemory);
        void* mapped = nullptr;
        vkMapMemory(device_, stagingMemory, 0, imageSize, 0, &mapped);
        std::memcpy(mapped, pixels.data(), pixels.size());
        vkUnmapMemory(device_, stagingMemory);

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 6;
        imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checkVk(vkCreateImage(device_, &imageInfo, nullptr, &skyboxImage_), "Failed to create skybox image");
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device_, skyboxImage_, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checkVk(vkAllocateMemory(device_, &alloc, nullptr, &skyboxMemory_), "Failed to allocate skybox memory");
        vkBindImageMemory(device_, skyboxImage_, skyboxMemory_, 0);

        transitionImageLayout(skyboxImage_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6);
        copyBufferToImage(stagingBuffer, skyboxImage_, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 6);
        transitionImageLayout(skyboxImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 6);
        vkDestroyBuffer(device_, stagingBuffer, nullptr);
        vkFreeMemory(device_, stagingMemory, nullptr);

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = skyboxImage_;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 6;
        checkVk(vkCreateImageView(device_, &viewInfo, nullptr, &skyboxView_), "Failed to create skybox image view");

        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = 1.0f;
        checkVk(vkCreateSampler(device_, &sampler, nullptr, &skyboxSampler_), "Failed to create skybox sampler");
    }

    void createDepthResources()
    {
        createImage(swapchainExtent_.width, swapchainExtent_.height, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depthImage_, depthMemory_);
        depthView_ = createImageView(depthImage_, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    void createFramebuffers()
    {
        framebuffers_.resize(swapchainImageViews_.size());
        for (std::size_t i = 0; i < swapchainImageViews_.size(); ++i) {
            std::array<VkImageView, 2> attachments{swapchainImageViews_[i], depthView_};
            VkFramebufferCreateInfo createInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            createInfo.renderPass = renderPass_;
            createInfo.attachmentCount = static_cast<std::uint32_t>(attachments.size());
            createInfo.pAttachments = attachments.data();
            createInfo.width = swapchainExtent_.width;
            createInfo.height = swapchainExtent_.height;
            createInfo.layers = 1;
            checkVk(vkCreateFramebuffer(device_, &createInfo, nullptr, &framebuffers_[i]), "Failed to create framebuffer");
        }
    }

    void cleanupSwapchain()
    {
        for (VkFramebuffer framebuffer : framebuffers_) vkDestroyFramebuffer(device_, framebuffer, nullptr);
        framebuffers_.clear();
        if (depthView_) vkDestroyImageView(device_, depthView_, nullptr);
        if (depthImage_) vkDestroyImage(device_, depthImage_, nullptr);
        if (depthMemory_) vkFreeMemory(device_, depthMemory_, nullptr);
        depthView_ = VK_NULL_HANDLE;
        depthImage_ = VK_NULL_HANDLE;
        depthMemory_ = VK_NULL_HANDLE;
        for (VkImageView view : swapchainImageViews_) vkDestroyImageView(device_, view, nullptr);
        swapchainImageViews_.clear();
        swapchainImages_.clear();
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    void recreateSwapchain()
    {
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        while (width == 0 || height == 0) {
            glfwWaitEvents();
            glfwGetFramebufferSize(window_, &width, &height);
        }
        vkDeviceWaitIdle(device_);
        cleanupSwapchain();
        createSwapchain();
        createDepthResources();
        createFramebuffers();
        ImGui_ImplVulkan_SetMinImageCount(2);
        framebufferResized_ = false;
    }

    void createCommandPool()
    {
        VkCommandPoolCreateInfo createInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        createInfo.queueFamilyIndex = graphicsFamily_;
        checkVk(vkCreateCommandPool(device_, &createInfo, nullptr, &commandPool_), "Failed to create command pool");
    }

    void createTerrainBuffers()
    {
        createBuffer(sizeof(Vertex) * terrain_.vertices().size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, vertexBuffer_, vertexMemory_);
        createBuffer(sizeof(std::uint32_t) * terrain_.indices().size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, indexBuffer_, indexMemory_);
        uploadTerrain();
    }

    void uploadTerrain()
    {
        void* data = nullptr;
        vkMapMemory(device_, vertexMemory_, 0, VK_WHOLE_SIZE, 0, &data);
        std::memcpy(data, terrain_.vertices().data(), sizeof(Vertex) * terrain_.vertices().size());
        vkUnmapMemory(device_, vertexMemory_);
        vkMapMemory(device_, indexMemory_, 0, VK_WHOLE_SIZE, 0, &data);
        std::memcpy(data, terrain_.indices().data(), sizeof(std::uint32_t) * terrain_.indices().size());
        vkUnmapMemory(device_, indexMemory_);
        terrainDirty_ = false;
    }

    void createUniformBuffers()
    {
        for (int i = 0; i < kMaxFramesInFlight; ++i) {
            createBuffer(sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, uniformBuffers_[i], uniformMemories_[i]);
        }
    }

    void createDescriptorPool()
    {
        std::array<VkDescriptorPoolSize, 2> poolSizes{{
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxFramesInFlight},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxFramesInFlight},
        }};
        VkDescriptorPoolCreateInfo createInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        createInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
        createInfo.pPoolSizes = poolSizes.data();
        createInfo.maxSets = kMaxFramesInFlight;
        checkVk(vkCreateDescriptorPool(device_, &createInfo, nullptr, &descriptorPool_), "Failed to create descriptor pool");
    }

    void createDescriptorSets()
    {
        std::array<VkDescriptorSetLayout, kMaxFramesInFlight> layouts{};
        layouts.fill(descriptorSetLayout_);
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descriptorPool_;
        alloc.descriptorSetCount = kMaxFramesInFlight;
        alloc.pSetLayouts = layouts.data();
        checkVk(vkAllocateDescriptorSets(device_, &alloc, descriptorSets_.data()), "Failed to allocate descriptor sets");
        for (int i = 0; i < kMaxFramesInFlight; ++i) {
            VkDescriptorBufferInfo bufferInfo{uniformBuffers_[i], 0, sizeof(SceneUniforms)};
            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = skyboxView_;
            imageInfo.sampler = skyboxSampler_;
            std::array<VkWriteDescriptorSet, 2> writes{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = descriptorSets_[i];
            writes[0].dstBinding = 0;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].descriptorCount = 1;
            writes[0].pBufferInfo = &bufferInfo;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = descriptorSets_[i];
            writes[1].dstBinding = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].descriptorCount = 1;
            writes[1].pImageInfo = &imageInfo;
            vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }

    void createCommandBuffers()
    {
        commandBuffers_.resize(kMaxFramesInFlight);
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = commandPool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = static_cast<std::uint32_t>(commandBuffers_.size());
        checkVk(vkAllocateCommandBuffers(device_, &alloc, commandBuffers_.data()), "Failed to allocate command buffers");
    }

    void createSyncObjects()
    {
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (int i = 0; i < kMaxFramesInFlight; ++i) {
            checkVk(vkCreateSemaphore(device_, &semaphore, nullptr, &imageAvailable_[i]), "Failed to create semaphore");
            checkVk(vkCreateSemaphore(device_, &semaphore, nullptr, &renderFinished_[i]), "Failed to create semaphore");
            checkVk(vkCreateFence(device_, &fence, nullptr, &inFlight_[i]), "Failed to create fence");
        }
    }

    void initImGui()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 5.0f;
        style.FrameRounding = 4.0f;
        style.GrabRounding = 4.0f;
        style.PopupRounding = 5.0f;
        style.WindowBorderSize = 1.0f;
        ImVec4* colors = style.Colors;
        colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.063f, 0.070f, 0.88f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.045f, 0.050f, 0.056f, 0.82f);
        colors[ImGuiCol_PopupBg] = ImVec4(0.055f, 0.060f, 0.068f, 0.96f);
        colors[ImGuiCol_Border] = ImVec4(0.22f, 0.26f, 0.28f, 0.42f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.040f, 0.048f, 0.055f, 1.00f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.075f, 0.100f, 0.110f, 1.00f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.095f, 0.110f, 0.120f, 0.92f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.135f, 0.160f, 0.170f, 0.95f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.165f, 0.195f, 0.205f, 1.00f);
        colors[ImGuiCol_Button] = ImVec4(0.110f, 0.135f, 0.145f, 0.95f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.165f, 0.205f, 0.215f, 1.00f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.200f, 0.255f, 0.265f, 1.00f);
        colors[ImGuiCol_SliderGrab] = ImVec4(0.36f, 0.50f, 0.48f, 1.00f);
        colors[ImGuiCol_SliderGrabActive] = ImVec4(0.48f, 0.65f, 0.60f, 1.00f);
        colors[ImGuiCol_CheckMark] = ImVec4(0.50f, 0.72f, 0.66f, 1.00f);
        colors[ImGuiCol_Header] = ImVec4(0.130f, 0.160f, 0.170f, 0.88f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.180f, 0.225f, 0.235f, 0.95f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.220f, 0.280f, 0.285f, 1.00f);
        ImGui_ImplGlfw_InitForVulkan(window_, true);
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 64;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        checkVk(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &imguiPool_), "Failed to create ImGui descriptor pool");
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_2;
        info.Instance = instance_;
        info.PhysicalDevice = physicalDevice_;
        info.Device = device_;
        info.QueueFamily = graphicsFamily_;
        info.Queue = graphicsQueue_;
        info.DescriptorPool = imguiPool_;
        info.MinImageCount = 2;
        info.ImageCount = static_cast<std::uint32_t>(swapchainImages_.size());
        info.PipelineInfoMain.RenderPass = renderPass_;
        info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        ImGui_ImplVulkan_Init(&info);
    }

    void onScroll(double xOffset, double yOffset)
    {
        if (ImGui::GetIO().WantCaptureMouse && !camera_.orbiting && !camera_.panning) return;
        glm::vec3 forward = glm::normalize(camera_.target - camera_.position);
        glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
        float scale = camera_.distance * camera_.panSpeed * 0.055f;
        camera_.distance *= std::pow(0.86f, static_cast<float>(yOffset));
        camera_.distance = glm::clamp(camera_.distance, 16.0f, 260.0f);
        camera_.target += (-right * static_cast<float>(xOffset)) * scale;
        camera_.lastInput = "two-finger zoom";
        camera_.updateFromOrbit();
    }

    void processInput(float dt)
    {
        bool leftMouse = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        bool rightMouse = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        bool middleMouse = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
        bool shiftDown = glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(window_, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        bool canStartCameraDrag = !ImGui::GetIO().WantCaptureMouse || camera_.orbiting || camera_.panning;
        bool wantsPan = (middleMouse || (rightMouse && shiftDown)) && canStartCameraDrag;
        bool wantsOrbit = (leftMouse || (rightMouse && !shiftDown)) && canStartCameraDrag;
        if (wantsPan || wantsOrbit) {
            if (!camera_.orbiting && !camera_.panning) {
                glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                camera_.firstMouse = true;
            }
            camera_.orbiting = wantsOrbit;
            camera_.panning = wantsPan;
        } else if (camera_.orbiting || camera_.panning) {
            camera_.orbiting = false;
            camera_.panning = false;
            glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
        if (camera_.orbiting || camera_.panning) {
            double mouseX = 0.0;
            double mouseY = 0.0;
            glfwGetCursorPos(window_, &mouseX, &mouseY);
            if (camera_.firstMouse) {
                camera_.lastMouseX = mouseX;
                camera_.lastMouseY = mouseY;
                camera_.firstMouse = false;
            }
            float dx = static_cast<float>(mouseX - camera_.lastMouseX);
            float dy = static_cast<float>(camera_.lastMouseY - mouseY);
            camera_.lastMouseX = mouseX;
            camera_.lastMouseY = mouseY;
            if (camera_.orbiting) {
                camera_.yaw += dx * 0.10f;
                camera_.pitch = glm::clamp(camera_.pitch + dy * 0.10f, -86.0f, 86.0f);
                camera_.lastInput = "drag orbit";
            } else {
                glm::vec3 forward = glm::normalize(camera_.target - camera_.position);
                glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
                glm::vec3 up = glm::normalize(glm::cross(right, forward));
                float scale = camera_.distance * camera_.panSpeed * 0.004f;
                camera_.target += (-right * dx + up * dy) * scale;
                camera_.lastInput = "drag pan";
            }
            camera_.updateFromOrbit();
        }
        if (!ImGui::GetIO().WantTextInput) {
            glm::vec3 forward = glm::normalize(camera_.target - camera_.position);
            glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
            glm::vec3 planarForward = glm::normalize(glm::vec3(forward.x, 0.0f, forward.z));
            float velocity = camera_.distance * 0.42f * dt;
            if (glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) velocity *= 2.0f;
            if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS) camera_.target += planarForward * velocity;
            if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS) camera_.target -= planarForward * velocity;
            if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS) camera_.target -= right * velocity;
            if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS) camera_.target += right * velocity;
            if (glfwGetKey(window_, GLFW_KEY_SPACE) == GLFW_PRESS) camera_.target.y += velocity;
            if (glfwGetKey(window_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) camera_.target.y -= velocity;
            camera_.updateFromOrbit();
        }
    }

    // Sun direction from the day/night clock: a real solar arc from time-of-day, season
    // (declination) and latitude. Elevation goes negative at night, which shuts off solar
    // heating in the solver and darkens the sky in the renderer.
    glm::vec3 sunDirection() const
    {
        float hourAngle = glm::radians((timeOfDay_ - 12.0f) * 15.0f); // 0 at solar noon
        float decl = glm::radians(23.44f * season_);
        float lat = glm::radians(latitudeDeg_);
        float sinElev = std::sin(lat) * std::sin(decl) + std::cos(lat) * std::cos(decl) * std::cos(hourAngle);
        sinElev = glm::clamp(sinElev, -1.0f, 1.0f);
        float elev = std::asin(sinElev);
        float cosElev = std::cos(elev);
        // Azimuth measured from north; flip after noon so the sun travels east -> west.
        float cosAz = (cosElev > 1e-4f) ? (std::sin(decl) - std::sin(lat) * sinElev) / (std::cos(lat) * cosElev) : 0.0f;
        float az = std::acos(glm::clamp(cosAz, -1.0f, 1.0f));
        if (hourAngle > 0.0f) az = 2.0f * 3.14159265f - az;
        // World vector: x east, y up, z north-ish. Exact compass alignment isn't critical;
        // what matters is a smooth arc that dips below the horizon at night.
        return glm::normalize(glm::vec3(cosElev * std::sin(az), sinElev, -cosElev * std::cos(az)));
    }

    // Advance the clock by real elapsed time (wraps at 24 h). Dragging the slider overrides
    // the value directly, and auto-advance simply continues from wherever it was set.
    void advanceClock(float dtReal)
    {
        if (clockAuto_ && dayLengthSec_ > 0.1f) {
            timeOfDay_ += dtReal / dayLengthSec_ * 24.0f;
            timeOfDay_ = std::fmod(timeOfDay_, 24.0f);
            if (timeOfDay_ < 0.0f) timeOfDay_ += 24.0f;
        }
    }

    void updateUniformBuffer(std::uint32_t frame)
    {
        TerrainSettings& s = terrain_.settings();
        glm::vec3 sunDir = sunDirection();
        float sunWarmth = glm::smoothstep(0.0f, 0.55f, sunDir.y);
        glm::vec3 sunColor = glm::mix(glm::vec3(1.0f, 0.58f, 0.34f), glm::vec3(1.0f, 0.93f, 0.78f), sunWarmth);
        glm::vec3 fogColor = glm::mix(glm::vec3(0.30f, 0.36f, 0.42f), glm::vec3(0.42f, 0.56f, 0.68f), sunWarmth);
        SceneUniforms ubo{};
        ubo.view = camera_.view();
        ubo.proj = glm::perspective(glm::radians(58.0f), static_cast<float>(swapchainExtent_.width) / static_cast<float>(swapchainExtent_.height), 0.1f, 650.0f);
        ubo.proj[1][1] *= -1.0f;
        ubo.cameraPos = glm::vec4(camera_.position, 1.0f);
        ubo.sunDir = glm::vec4(sunDir, 0.0f);
        ubo.sunColor = glm::vec4(sunColor, 1.0f);
        ubo.fogColor = glm::vec4(fogColor, 1.0f);
        ubo.terrain = glm::vec4(s.heightScale, s.snowLevel, s.waterLevel, s.fogDensity);
        ubo.effects = glm::vec4(s.waterTint, s.sedimentTint, s.showWater ? 1.0f : 0.0f, s.showSediment ? 1.0f : 0.0f);
        // Volumetric cloud parameters (world ray reconstruction + the box the field lives in).
        ubo.invViewProj = glm::inverse(ubo.proj * ubo.view);
        if (weather_.ready()) {
            glm::vec3 lo = weather_.cellCenter(0, 0, 0);
            glm::vec3 hi = weather_.cellCenter(weather_.nx() - 1, weather_.ny() - 1, weather_.nz() - 1);
            ubo.volMin = glm::vec4(lo, 0.0f);
            ubo.volMax = glm::vec4(hi, 0.0f);
            ubo.cloudGrid = glm::vec4(static_cast<float>(weather_.nx()), static_cast<float>(weather_.ny()),
                                      static_cast<float>(weather_.nz()), static_cast<float>(kCloudHeightRes));
        }
        ubo.cloudParams = glm::vec4(cloudDensity_, static_cast<float>(cloudSteps_), cloudSunAbsorb_, cloudCoverage_);
        void* data = nullptr;
        vkMapMemory(device_, uniformMemories_[frame], 0, sizeof(ubo), 0, &data);
        std::memcpy(data, &ubo, sizeof(ubo));
        vkUnmapMemory(device_, uniformMemories_[frame]);
    }

    void updateErosionAnimation()
    {
        if (!erosionAnimating_) return;
        terrain_.stepLiveDroplets(erosionSpeed_);
        terrainDirty_ = true;
        if (!terrain_.hasActiveDroplets()) {
            erosionAnimating_ = false;
            terrain_.finishLiveErosion();
        }
    }

    void drawControls()
    {
        TerrainSettings editable = terrain_.settings();
        ImGui::SetNextWindowPos(ImVec2(18, 18), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330, 285), ImGuiCond_FirstUseEver);
        ImGui::Begin("Terrain");
        bool regenerate = false;
        regenerate |= ImGui::InputInt("Seed", &editable.seed);
        regenerate |= ImGui::SliderInt("Octaves", &editable.octaves, 1, 10);
        regenerate |= ImGui::SliderFloat("Noise frequency", &editable.frequency, 0.5f, 7.0f, "%.2f");
        regenerate |= ImGui::SliderFloat("Lacunarity", &editable.lacunarity, 1.4f, 3.2f, "%.2f");
        regenerate |= ImGui::SliderFloat("Persistence", &editable.persistence, 0.25f, 0.72f, "%.2f");
        regenerate |= ImGui::SliderFloat("Peak sharpness", &editable.peakSharpness, 0.80f, 2.60f, "%.2f");
        regenerate |= ImGui::SliderFloat("Height scale", &editable.heightScale, 12.0f, 78.0f, "%.1f");
        if (regenerate || ImGui::Button("Regenerate heightmap")) {
            erosionAnimating_ = false;
            terrain_.generate(editable);
            terrainDirty_ = true;
            // New terrain -> rebuild the weather box (base/lid follow terrain height)
            // and regenerate the airmass so clouds form over the new mountains.
            if (weather_.ready()) rebuildWeatherGrid();
        } else {
            terrain_.settings() = editable;
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(18, 318), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330, 292), ImGuiCond_FirstUseEver);
        ImGui::Begin("Erosion");
        ImGui::SliderInt("Drops", &terrain_.settings().erosionDrops, 1000, 45000);
        ImGui::SliderFloat("Radius", &terrain_.settings().erosionRadius, 1.0f, 5.0f, "%.1f");
        ImGui::SliderFloat("Inertia", &terrain_.settings().inertia, 0.0f, 0.92f, "%.2f");
        ImGui::SliderFloat("Capacity", &terrain_.settings().capacity, 1.0f, 10.0f, "%.1f");
        ImGui::SliderFloat("Evaporation", &terrain_.settings().evaporation, 0.005f, 0.12f, "%.3f");
        ImGui::Checkbox("Show particles", &showFlowParticles_);
        ImGui::SliderFloat("Simulation speed", &erosionSpeed_, 0.06f, 1.0f, "%.2f");
        if (erosionAnimating_) ImGui::BeginDisabled();
        if (ImGui::Button(erosionAnimating_ ? "Simulating" : "Start simulation")) {
            int drops = terrain_.settings().erosionDrops;
            terrain_.beginLiveErosion(drops);
            terrain_.spawnLiveDroplets(drops, drops);
            erosionAnimating_ = true;
            terrainDirty_ = true;
        }
        if (erosionAnimating_) ImGui::EndDisabled();
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(365, 18), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330, 270), ImGuiCond_FirstUseEver);
        ImGui::Begin("Look");
        ImGui::SliderFloat("Snow level", &terrain_.settings().snowLevel, 0.35f, 0.9f, "%.2f");
        ImGui::SliderFloat("Water line", &terrain_.settings().waterLevel, 0.03f, 0.42f, "%.2f");
        ImGui::SliderFloat("Water tint", &terrain_.settings().waterTint, 0.0f, 5.0f, "%.1f");
        ImGui::SliderFloat("Sediment tint", &terrain_.settings().sedimentTint, 0.0f, 5.0f, "%.1f");
        ImGui::Checkbox("Show water", &terrain_.settings().showWater);
        ImGui::Checkbox("Show sediment", &terrain_.settings().showSediment);
        ImGui::SliderFloat("Fog density", &terrain_.settings().fogDensity, 0.0f, 0.03f, "%.3f");
        ImGui::Checkbox("Wireframe", &wireframe_);

        ImGui::SeparatorText("Time of day & sky");
        ImGui::Checkbox("Auto-advance", &clockAuto_);
        ImGui::SliderFloat("Time of day", &timeOfDay_, 0.0f, 24.0f, "%.2f h");
        int hh = static_cast<int>(timeOfDay_);
        int mm = static_cast<int>((timeOfDay_ - hh) * 60.0f) % 60;
        float elevDeg = glm::degrees(std::asin(glm::clamp(sunDirection().y, -1.0f, 1.0f)));
        ImGui::Text("%02d:%02d   sun %+.0f deg %s", hh, mm, elevDeg, elevDeg > 0.0f ? "(day)" : "(night)");
        ImGui::SliderFloat("Day length", &dayLengthSec_, 15.0f, 600.0f, "%.0f s/day");
        ImGui::SliderFloat("Season", &season_, -1.0f, 1.0f, "%.2f (-1 winter / +1 summer)");
        ImGui::SliderFloat("Latitude", &latitudeDeg_, 0.0f, 66.0f, "%.0f deg");
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(365, 306), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330, 185), ImGuiCond_FirstUseEver);
        ImGui::Begin("Camera");
        ImGui::SliderFloat("Orbit distance", &camera_.distance, 16.0f, 260.0f, "%.0f");
        ImGui::SliderFloat("Pan speed", &camera_.panSpeed, 0.05f, 0.55f, "%.2f");
        ImGui::Text("Last input: %s", camera_.lastInput.c_str());
        if (ImGui::Button("Reset view")) {
            camera_.target = glm::vec3(0.0f, 16.0f, 0.0f);
            camera_.yaw = -90.0f;
            camera_.pitch = -18.0f;
            camera_.distance = 108.0f;
        }
        ImGui::TextWrapped("One-finger click-drag orbits. Two-finger vertical swipe zooms; horizontal swipe pans. Shift + right drag pans. WASD pans the target.");
        camera_.updateFromOrbit();
        ImGui::End();
    }

    bool projectToScreen(const glm::vec3& world, const glm::mat4& viewProj, ImVec2& screen) const
    {
        glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
        if (clip.w <= 0.05f) return false;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.x < -1.12f || ndc.x > 1.12f || ndc.y < -1.12f || ndc.y > 1.12f || ndc.z < -1.0f || ndc.z > 1.0f) return false;
        if (terrain_.occludesSegment(camera_.position, world)) return false;
        ImVec2 size = ImGui::GetIO().DisplaySize;
        screen.x = (ndc.x * 0.5f + 0.5f) * size.x;
        screen.y = (ndc.y * 0.5f + 0.5f) * size.y;
        return true;
    }

    void drawFlowOverlay()
    {
        if (!showFlowParticles_) return;
        const auto& particles = terrain_.particlePositions();
        if (particles.empty()) return;
        float aspect = static_cast<float>(std::max(1u, swapchainExtent_.width)) / static_cast<float>(std::max(1u, swapchainExtent_.height));
        glm::mat4 proj = glm::perspective(glm::radians(58.0f), aspect, 0.1f, 650.0f);
        proj[1][1] *= -1.0f;
        glm::mat4 viewProj = proj * camera_.view();
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        for (const glm::vec3& world : particles) {
            ImVec2 screen{};
            if (projectToScreen(world, viewProj, screen)) {
                drawList->AddRectFilled(ImVec2(screen.x - 1.05f, screen.y - 1.05f), ImVec2(screen.x + 1.05f, screen.y + 1.05f), IM_COL32(168, 226, 244, 172));
            }
        }
    }

    glm::mat4 sceneProj() const
    {
        float aspect = static_cast<float>(std::max(1u, swapchainExtent_.width)) / static_cast<float>(std::max(1u, swapchainExtent_.height));
        glm::mat4 proj = glm::perspective(glm::radians(58.0f), aspect, 0.1f, 650.0f);
        proj[1][1] *= -1.0f;
        return proj;
    }

    // Cheap projection for volumetric splats: in front of camera + roughly on-screen.
    bool projectVolume(const glm::vec3& world, const glm::mat4& viewProj, ImVec2& screen, float& depth) const
    {
        glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
        if (clip.w <= 0.05f) return false;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.x < -1.15f || ndc.x > 1.15f || ndc.y < -1.15f || ndc.y > 1.15f || ndc.z < 0.0f || ndc.z > 1.0f) return false;
        ImVec2 size = ImGui::GetIO().DisplaySize;
        screen.x = (ndc.x * 0.5f + 0.5f) * size.x;
        screen.y = (ndc.y * 0.5f + 0.5f) * size.y;
        depth = clip.w;
        return true;
    }

    // True if the terrain surface blocks the straight line from the camera to P. Marches at a
    // fixed world-space resolution (rather than a fixed step count) so that even distant points
    // are sampled finely enough that the ray can't skip clean over a ridge -- the coarse
    // 14-step version let slice planes and wind streaks bleed through the mountains.
    bool occludedByTerrain(const glm::vec3& P) const
    {
        glm::vec3 o = camera_.position;
        glm::vec3 d = P - o;
        float dist = glm::length(d);
        if (dist < 1e-3f) return false;
        d /= dist;
        const float halfW = kTerrainWorldSize * 0.5f + 2.0f;
        const float stepLen = 1.5f; // world units between samples
        int steps = glm::clamp(static_cast<int>(dist / stepLen), 12, 96);
        for (int s = 1; s < steps; ++s) {
            float t = dist * (static_cast<float>(s) / static_cast<float>(steps));
            glm::vec3 q = o + d * t;
            if (std::abs(q.x) > halfW || std::abs(q.z) > halfW) continue;
            if (q.y < terrain_.surfaceHeightAtWorld(q.x, q.z) - 0.25f) return true;
        }
        return false;
    }

    void updateWeather(float dt)
    {
        advanceClock(dt);
        if (!weather_.ready()) return;
        // The solver runs on its own thread; we only hand it inputs (including the live sun
        // from the clock) and always display the latest finished snapshot.
        weather_.setControls(weatherRunning_, weatherParams_, sunDirection());
        weather_.setViewFrame(-1);
    }

    void applyWeatherPreset(int preset)
    {
        WeatherParams p{};
        switch (preset) {
            case 1: // Dry wind
                p.windSpeed = 10.0f;
                p.windDirDeg = 18.0f;
                p.inflowRH = 0.34f;
                p.surfaceTempC = 19.0f;
                p.thetaLapse = 0.0065f;
                p.solarHeating = 0.012f;
                p.irCooling = 0.0030f;
                p.surfaceDrag = 0.65f;
                p.windNudge = 0.18f;
                p.gustStrength = 0.10f;
                p.turbulence = 0.06f;
                p.autoThresh = 0.0025f;
                p.timeScale = 55.0f;
                break;
            case 2: // Orographic cloud
                p.windSpeed = 8.5f;
                p.windDirDeg = 22.0f;
                p.inflowRH = 0.88f;
                p.surfaceTempC = 13.0f;
                p.thetaLapse = 0.0045f;
                p.solarHeating = 0.008f;
                p.irCooling = 0.0035f;
                p.surfaceDrag = 0.75f;
                p.windNudge = 0.12f;
                p.gustStrength = 0.14f;
                p.turbulence = 0.10f;
                p.autoconv = 0.0012f;
                p.autoThresh = 0.00035f;
                p.accretion = 1.8f;
                p.rainEvap = 0.35f;
                p.timeScale = 45.0f;
                break;
            case 3: // Storm buildup
                p.windSpeed = 5.5f;
                p.windDirDeg = -12.0f;
                p.inflowRH = 0.96f;
                p.surfaceTempC = 24.0f;
                p.thetaLapse = 0.0008f;
                p.solarHeating = 0.022f;
                p.irCooling = 0.0022f;
                p.surfaceDrag = 0.95f;
                p.windNudge = 0.07f;
                p.gustStrength = 0.26f;
                p.turbulence = 0.22f;
                p.autoconv = 0.0024f;
                p.autoThresh = 0.00025f;
                p.accretion = 3.6f;
                p.rainEvap = 0.18f;
                p.buoyancy = 1.45f;
                p.timeScale = 34.0f;
                break;
            case 4: // Rain shadow
                p.windSpeed = 12.0f;
                p.windDirDeg = 35.0f;
                p.inflowRH = 0.78f;
                p.surfaceTempC = 16.0f;
                p.thetaLapse = 0.0052f;
                p.solarHeating = 0.010f;
                p.irCooling = 0.0038f;
                p.surfaceDrag = 0.58f;
                p.windNudge = 0.22f;
                p.gustStrength = 0.18f;
                p.turbulence = 0.08f;
                p.autoconv = 0.0017f;
                p.autoThresh = 0.00045f;
                p.accretion = 2.8f;
                p.rainEvap = 0.42f;
                p.timeScale = 50.0f;
                break;
            case 5: // Valley fog
                p.windSpeed = 1.6f;
                p.windDirDeg = 0.0f;
                p.inflowRH = 0.94f;
                p.surfaceTempC = 5.0f;
                p.thetaLapse = 0.0075f;
                p.solarHeating = 0.002f;
                p.irCooling = 0.0060f;
                p.surfaceDrag = 1.25f;
                p.windNudge = 0.03f;
                p.gustStrength = 0.05f;
                p.turbulence = 0.04f;
                p.autoconv = 0.0007f;
                p.autoThresh = 0.0009f;
                p.accretion = 0.8f;
                p.rainEvap = 0.55f;
                p.buoyancy = 0.65f;
                p.timeScale = 30.0f;
                break;
            default:
                break;
        }
        weatherParams_ = p;
        weather_.reset(weatherParams_);
    }

    void drawWeatherControls()
    {
        ImGui::SetNextWindowPos(ImVec2(712, 18), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(346, 430), ImGuiCond_FirstUseEver);
        ImGui::Begin("Weather");
        ImGui::Text("sim time: %.0f s   grid %dx%dx%d", weather_.simTime(), weather_.nx(), weather_.ny(), weather_.nz());
        ImGui::Text("boxes: %d", weather_.nx() * weather_.ny() * weather_.nz());
        const char* presets[] = {"Default", "Dry wind", "Orographic cloud", "Storm buildup", "Rain shadow", "Valley fog"};
        if (ImGui::Combo("Preset", &weatherPreset_, presets, IM_ARRAYSIZE(presets))) applyWeatherPreset(weatherPreset_);
        if (ImGui::Button(weatherRunning_ ? "Pause" : "Run")) weatherRunning_ = !weatherRunning_;
        ImGui::SameLine();
        if (ImGui::Button("Reset")) { weather_.reset(weatherParams_); }
        ImGui::SameLine();
        if (ImGui::Button("Re-sync terrain")) syncWeatherTerrain();

        ImGui::SeparatorText("Airmass / inflow");
        ImGui::SliderFloat("Wind speed", &weatherParams_.windSpeed, 0.0f, 18.0f, "%.1f m/s");
        ImGui::SliderFloat("Wind dir", &weatherParams_.windDirDeg, -180.0f, 180.0f, "%.0f deg");
        ImGui::SliderFloat("Inflow RH", &weatherParams_.inflowRH, 0.05f, 1.0f, "%.2f");
        ImGui::SliderFloat("Surface temp", &weatherParams_.surfaceTempC, -10.0f, 35.0f, "%.1f C");
        ImGui::SliderFloat("Stability", &weatherParams_.thetaLapse, -0.002f, 0.012f, "%.4f K/m");

        ImGui::SeparatorText("Surface energy");
        ImGui::SliderFloat("Solar heating", &weatherParams_.solarHeating, 0.0f, 0.04f, "%.3f K/s");
        ImGui::SliderFloat("IR cooling", &weatherParams_.irCooling, 0.0f, 0.012f, "%.4f K/s");
        ImGui::SliderFloat("Surface drag", &weatherParams_.surfaceDrag, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("Wind nudge", &weatherParams_.windNudge, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Gusts", &weatherParams_.gustStrength, 0.0f, 0.6f, "%.2f");
        ImGui::SliderFloat("Turbulence", &weatherParams_.turbulence, 0.0f, 0.7f, "%.2f");

        ImGui::SeparatorText("Moisture");
        ImGui::SliderFloat("Rain fall speed", &weatherParams_.rainFall, 1.0f, 12.0f, "%.1f m/s");
        ImGui::SliderFloat("Autoconversion", &weatherParams_.autoconv, 0.0f, 0.006f, "%.4f");
        ImGui::SliderFloat("Cloud threshold", &weatherParams_.autoThresh, 0.0001f, 0.005f, "%.4f kg/kg");
        ImGui::SliderFloat("Accretion", &weatherParams_.accretion, 0.0f, 8.0f, "%.1f");
        ImGui::SliderFloat("Rain evap", &weatherParams_.rainEvap, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Buoyancy", &weatherParams_.buoyancy, 0.0f, 2.5f, "%.2f");

        ImGui::SeparatorText("Solver");
        ImGui::SliderInt("Pressure iters", &weatherParams_.pressureIters, 6, 60);
        ImGui::SliderFloat("Time scale", &weatherParams_.timeScale, 1.0f, 120.0f, "%.0f x");
        ImGui::SliderInt("Grid XZ", &weatherGridXZ_, 32, 144);
        ImGui::SliderInt("Grid Y", &weatherGridY_, 24, 96);
        if (ImGui::Button("Apply grid")) rebuildWeatherGrid();

        ImGui::SeparatorText("Couple to terrain");
        ImGui::SliderFloat("Erosion drops", &rainErosionDrops_, 1000.0f, 40000.0f, "%.0f");
        if (ImGui::Button("Rain carves terrain")) {
            terrain_.erodeWeighted(weather_.precipWeights(), weather_.nx(), weather_.nz(), static_cast<int>(rainErosionDrops_));
            terrainDirty_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear precip")) weather_.clearPrecipAccum();
        ImGui::End();

        drawSlicePanel();
    }

    void drawSlicePanel()
    {
        ImGui::SetNextWindowPos(ImVec2(712, 458), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(346, 300), ImGuiCond_FirstUseEver);
        ImGui::Begin("Slices & view");
        ImGui::Checkbox("Clouds", &showClouds_); ImGui::SameLine();
        ImGui::Checkbox("Rain", &showRain_); ImGui::SameLine();
        ImGui::Checkbox("Wind", &showWindStreaks_);

        ImGui::SeparatorText("Clouds");
        ImGui::Checkbox("Volumetric (GPU ray-march)", &cloudVolumetric_);
        if (cloudVolumetric_) {
            ImGui::SliderFloat("Density", &cloudDensity_, 100.0f, 6000.0f, "%.0f");
            ImGui::SliderInt("March steps", &cloudSteps_, 12, 128);
            ImGui::SliderFloat("Sun absorption", &cloudSunAbsorb_, 0.1f, 4.0f, "%.2f");
            ImGui::SliderFloat("Coverage trim", &cloudCoverage_, 0.0f, 0.5f, "%.3f");
        }

        const char* fields[] = {"Temperature", "Theta pert", "Vertical wind", "Wind speed", "Vapor", "Rel humidity", "Cloud", "Rain", "Vorticity"};
        ImGui::Combo("Field", &weatherField_, fields, IM_ARRAYSIZE(fields));

        ImGui::SeparatorText("Horizontal slice");
        ImGui::Checkbox("Show##h", &showSliceH_); ImGui::SameLine();
        ImGui::SliderInt("Height (Y)", &sliceH_, 0, std::max(0, weather_.ny() - 1));

        ImGui::SeparatorText("Vertical slice");
        ImGui::Checkbox("Show##v", &showSliceV_); ImGui::SameLine();
        const char* vaxes[] = {"X-normal", "Z-normal"};
        if (ImGui::Combo("Facing", &vertAxis_, vaxes, IM_ARRAYSIZE(vaxes)))
            sliceV_ = glm::clamp(sliceV_, 0, vertAxisCount() - 1);
        ImGui::SliderInt("Position", &sliceV_, 0, std::max(0, vertAxisCount() - 1));
        ImGui::End();
    }


    // Temporally-smoothed [lo,hi] per field so the slice colours don't flash as the
    // instantaneous min/max jitters frame to frame.
    void stableColorRange(WeatherField field, float& outLo, float& outHi)
    {
        int nx = weather_.nx(), ny = weather_.ny(), nz = weather_.nz();
        float lo = 1e30f, hi = -1e30f;
        auto visit = [&](int i, int j, int k) {
            if (weather_.isSolid(i, j, k)) return;
            float val = weather_.sample(field, i, j, k, weatherParams_);
            lo = std::min(lo, val); hi = std::max(hi, val);
        };
        if (showSliceH_) { int j = glm::clamp(sliceH_, 0, ny - 1); for (int k = 0; k < nz; ++k) for (int i = 0; i < nx; ++i) visit(i, j, k); }
        if (showSliceV_) {
            int s = glm::clamp(sliceV_, 0, vertAxisCount() - 1);
            if (vertAxis_ == 0) { for (int k = 0; k < nz; ++k) for (int j = 0; j < ny; ++j) visit(s, j, k); }
            else { for (int i = 0; i < nx; ++i) for (int j = 0; j < ny; ++j) visit(i, j, s); }
        }
        int f = static_cast<int>(field);
        if (hi < lo) { outLo = fieldLo_[f]; outHi = fieldHi_[f]; return; }
        if (!fieldRangeInit_[f]) { fieldLo_[f] = lo; fieldHi_[f] = hi; fieldRangeInit_[f] = true; }
        else {
            float a = 0.06f; // EMA-smooth the color range to kill per-frame flicker
            fieldLo_[f] += (lo - fieldLo_[f]) * a;
            fieldHi_[f] += (hi - fieldHi_[f]) * a;
        }
        outLo = fieldLo_[f];
        outHi = std::max(fieldLo_[f] + 1e-4f, fieldHi_[f]);
    }

    // A continuous, gouraud-shaded slice plane (no gaps). `cellAt(a,b)` maps plane
    // coordinates to grid cells; A*B is the node grid. Terrain occlusion and solid
    // cells fade the corner alpha so the plane is cut away cleanly behind mountains.
    template <class CellFn>
    void drawWindStreaksOnPlane(ImDrawList* dl, const glm::mat4& viewProj, int A, int B, int stride,
                                CellFn cellAt)
    {
        for (int b = 0; b < B; b += stride) {
            for (int a = 0; a < A; a += stride) {
                int i, j, k; cellAt(a, b, i, j, k);
                if (weather_.isSolid(i, j, k)) continue;
                glm::vec3 c = weather_.cellCenter(i, j, k);
                if (occludedByTerrain(c)) continue;
                glm::vec3 vel = weather_.velocity(i, j, k);
                glm::vec3 tip = c + vel * 0.9f;
                ImVec2 pa, pb; float da, db;
                if (!projectVolume(c, viewProj, pa, da) || !projectVolume(tip, viewProj, pb, db)) continue;
                dl->AddLine(pa, pb, IM_COL32(250, 250, 255, 150), 1.0f);
                dl->AddCircleFilled(pb, 1.4f, IM_COL32(255, 240, 200, 180));
            }
        }
    }

    void drawWeatherOverlay()
    {
        if (!weather_.ready()) return;
        glm::mat4 viewProj = sceneProj() * camera_.view();
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        int nx = weather_.nx(), ny = weather_.ny(), nz = weather_.nz();
        WeatherField field = static_cast<WeatherField>(weatherField_);

        // --- legacy billboard splat clouds (used only when the GPU ray-march is off) ---
        if (showClouds_ && !cloudVolumetric_) {
            float dxz = kTerrainWorldSize / static_cast<float>(nx);
            struct Puff { float depth; ImVec2 sc; float sz; float a; };
            static std::vector<Puff> puffs;
            puffs.clear();
            for (int k = 0; k < nz; ++k)
                for (int j = 0; j < ny; ++j)
                    for (int i = 0; i < nx; ++i) {
                        if (weather_.isSolid(i, j, k)) continue;
                        float qc = weather_.cloudAt(i, j, k);
                        if (qc <= 2.0e-5f) continue;
                        glm::vec3 c = weather_.cellCenter(i, j, k);
                        if (occludedByTerrain(c)) continue;
                        ImVec2 sc; float depth;
                        if (!projectVolume(c, viewProj, sc, depth)) continue;
                        float screenPerWorld = (ImGui::GetIO().DisplaySize.y * 0.5f) / (depth * 0.554f);
                        float sz = glm::clamp(dxz * screenPerWorld * 0.85f, 2.0f, 40.0f);
                        float a = glm::clamp(qc * 7000.0f, 0.05f, 0.55f);
                        puffs.push_back({depth, sc, sz, a});
                    }
            std::sort(puffs.begin(), puffs.end(), [](const Puff& x, const Puff& y) { return x.depth > y.depth; });
            for (const Puff& p : puffs) {
                int ao = static_cast<int>(p.a * 0.55f * 255.0f);
                int ai = static_cast<int>(p.a * 255.0f);
                dl->AddCircleFilled(p.sc, p.sz, IM_COL32(236, 240, 248, ao), 12);
                dl->AddCircleFilled(p.sc, p.sz * 0.55f, IM_COL32(250, 252, 255, ai), 10);
            }
        }

        // --- rain streaks ---
        if (showRain_) {
            int drawn = 0;
            for (int k = 0; k < nz && drawn < 9000; ++k)
                for (int j = 0; j < ny; ++j)
                    for (int i = 0; i < nx; ++i) {
                        if (weather_.isSolid(i, j, k)) continue;
                        float qr = weather_.rainAt(i, j, k);
                        if (qr <= 6.0e-5f) continue;
                        glm::vec3 c = weather_.cellCenter(i, j, k);
                        if (occludedByTerrain(c)) continue;
                        ImVec2 sc; float depth;
                        if (!projectVolume(c, viewProj, sc, depth)) continue;
                        float sz = glm::clamp(1100.0f / depth, 2.0f, 14.0f);
                        float a = glm::clamp(qr * 9000.0f, 0.10f, 0.7f);
                        dl->AddLine(ImVec2(sc.x, sc.y - sz * 0.6f), ImVec2(sc.x, sc.y + sz * 0.9f),
                                    IM_COL32(120, 150, 210, static_cast<int>(a * 255)), 1.4f);
                        ++drawn;
                    }
        }

        // --- wind streaks on whichever planes are shown ---
        if (showWindStreaks_) {
            int strideXZ = std::max(1, nx / 26);
            int strideY = std::max(1, ny / 20);
            if (showSliceH_) {
                int j = glm::clamp(sliceH_, 0, ny - 1);
                drawWindStreaksOnPlane(dl, viewProj, nx, nz, strideXZ,
                                       [&](int a, int b, int& i, int& jj, int& k) { i = a; jj = j; k = b; });
            }
            if (showSliceV_) {
                int s = glm::clamp(sliceV_, 0, vertAxisCount() - 1);
                if (vertAxis_ == 0)
                    drawWindStreaksOnPlane(dl, viewProj, ny, nz, strideY,
                                           [&](int a, int b, int& i, int& jj, int& k) { i = s; jj = a; k = b; });
                else
                    drawWindStreaksOnPlane(dl, viewProj, nx, ny, strideXZ,
                                           [&](int a, int b, int& i, int& jj, int& k) { i = a; jj = b; k = s; });
            }
        }
    }

    void recordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t imageIndex)
    {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        checkVk(vkBeginCommandBuffer(commandBuffer, &begin), "Failed to begin command buffer");
        std::array<VkClearValue, 2> clears{};
        clears[0].color = {{0.30f, 0.42f, 0.52f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo renderPass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        renderPass.renderPass = renderPass_;
        renderPass.framebuffer = framebuffers_[imageIndex];
        renderPass.renderArea.offset = {0, 0};
        renderPass.renderArea.extent = swapchainExtent_;
        renderPass.clearValueCount = static_cast<std::uint32_t>(clears.size());
        renderPass.pClearValues = clears.data();
        vkCmdBeginRenderPass(commandBuffer, &renderPass, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0.0f, 0.0f, static_cast<float>(swapchainExtent_.width), static_cast<float>(swapchainExtent_.height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, swapchainExtent_};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline_);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descriptorSets_[currentFrame_], 0, nullptr);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer_, &offset);
        vkCmdBindIndexBuffer(commandBuffer, indexBuffer_, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descriptorSets_[currentFrame_], 0, nullptr);
        vkCmdDrawIndexed(commandBuffer, static_cast<std::uint32_t>(terrain_.indices().size()), 1, 0, 0, 0);
        if (sliceVertexCount_[currentFrame_] > 0) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, slicePipeline_);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descriptorSets_[currentFrame_], 0, nullptr);
            VkDeviceSize sOff = 0;
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, &sliceVertexBuffers_[currentFrame_], &sOff);
            vkCmdDraw(commandBuffer, sliceVertexCount_[currentFrame_], 1, 0, 0);
        }
        // Volumetric clouds: fullscreen ray-march after terrain (reads terrain heightmap for
        // occlusion), before the ImGui overlay. Premultiplied-alpha blended over the scene.
        if (cloudVolumetric_ && showClouds_ && weather_.ready()) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, cloudPipeline_);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, cloudPipelineLayout_, 0, 1, &cloudSets_[currentFrame_], 0, nullptr);
            vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        }
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
        vkCmdEndRenderPass(commandBuffer);
        checkVk(vkEndCommandBuffer(commandBuffer), "Failed to record command buffer");
    }

    void drawFrame()
    {
        vkWaitForFences(device_, 1, &inFlight_[currentFrame_], VK_TRUE, UINT64_MAX);
        std::uint32_t imageIndex = 0;
        VkResult acquire = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, imageAvailable_[currentFrame_], VK_NULL_HANDLE, &imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapchain();
            return;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            checkVk(acquire, "Failed to acquire swapchain image");
        }
        vkResetFences(device_, 1, &inFlight_[currentFrame_]);
        if (terrainDirty_) { uploadTerrain(); syncWeatherTerrain(); uploadCloudHeightmap(); }
        updateUniformBuffer(static_cast<std::uint32_t>(currentFrame_));
        updateCloudData(static_cast<std::uint32_t>(currentFrame_));
        buildSliceGeometry(static_cast<std::uint32_t>(currentFrame_));
        vkResetCommandBuffer(commandBuffers_[currentFrame_], 0);
        recordCommandBuffer(commandBuffers_[currentFrame_], imageIndex);
        VkSemaphore waitSemaphores[] = {imageAvailable_[currentFrame_]};
        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        VkSemaphore signalSemaphores[] = {renderFinished_[currentFrame_]};
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = waitSemaphores;
        submit.pWaitDstStageMask = waitStages;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffers_[currentFrame_];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = signalSemaphores;
        checkVk(vkQueueSubmit(graphicsQueue_, 1, &submit, inFlight_[currentFrame_]), "Failed to submit frame");
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = signalSemaphores;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &imageIndex;
        VkResult presentResult = vkQueuePresentKHR(presentQueue_, &present);
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || framebufferResized_) {
            recreateSwapchain();
        } else if (presentResult != VK_SUCCESS) {
            checkVk(presentResult, "Failed to present swapchain image");
        }
        currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
    }

    void mainLoop()
    {
        auto previous = std::chrono::high_resolution_clock::now();
        while (!glfwWindowShouldClose(window_)) {
            auto now = std::chrono::high_resolution_clock::now();
            float dt = std::chrono::duration<float>(now - previous).count();
            previous = now;
            glfwPollEvents();
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            processInput(dt);
            updateErosionAnimation();
            updateWeather(dt);
            drawControls();
            drawWeatherControls();
            drawFlowOverlay();
            drawWeatherOverlay();
            ImGui::Render();
            drawFrame();
        }
        vkDeviceWaitIdle(device_);
    }

    void cleanup()
    {
        weather_.stopWorker();
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        for (int i = 0; i < kMaxFramesInFlight; ++i) {
            vkDestroySemaphore(device_, imageAvailable_[i], nullptr);
            vkDestroySemaphore(device_, renderFinished_[i], nullptr);
            vkDestroyFence(device_, inFlight_[i], nullptr);
            vkDestroyBuffer(device_, uniformBuffers_[i], nullptr);
            vkFreeMemory(device_, uniformMemories_[i], nullptr);
        }
        for (int i = 0; i < kMaxFramesInFlight; ++i) {
            if (cloudFieldBuffers_[i]) vkDestroyBuffer(device_, cloudFieldBuffers_[i], nullptr);
            if (cloudFieldMemories_[i]) vkFreeMemory(device_, cloudFieldMemories_[i], nullptr);
        }
        if (cloudHeightBuffer_) vkDestroyBuffer(device_, cloudHeightBuffer_, nullptr);
        if (cloudHeightMemory_) vkFreeMemory(device_, cloudHeightMemory_, nullptr);
        vkDestroyDescriptorPool(device_, imguiPool_, nullptr);
        vkDestroyDescriptorPool(device_, cloudPool_, nullptr);
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        vkDestroyBuffer(device_, indexBuffer_, nullptr);
        vkFreeMemory(device_, indexMemory_, nullptr);
        vkDestroyBuffer(device_, vertexBuffer_, nullptr);
        vkFreeMemory(device_, vertexMemory_, nullptr);
        vkDestroySampler(device_, skyboxSampler_, nullptr);
        vkDestroyImageView(device_, skyboxView_, nullptr);
        vkDestroyImage(device_, skyboxImage_, nullptr);
        vkFreeMemory(device_, skyboxMemory_, nullptr);
        cleanupSwapchain();
        vkDestroyPipeline(device_, slicePipeline_, nullptr);
        for (int i = 0; i < kMaxFramesInFlight; ++i) {
            if (sliceVertexBuffers_[i]) vkDestroyBuffer(device_, sliceVertexBuffers_[i], nullptr);
            if (sliceVertexMemories_[i]) vkFreeMemory(device_, sliceVertexMemories_[i], nullptr);
        }
        vkDestroyPipeline(device_, cloudPipeline_, nullptr);
        vkDestroyPipeline(device_, skyPipeline_, nullptr);
        vkDestroyPipeline(device_, pipeline_, nullptr);
        vkDestroyPipelineLayout(device_, cloudPipelineLayout_, nullptr);
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        vkDestroyDescriptorSetLayout(device_, cloudSetLayout_, nullptr);
        vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        vkDestroyInstance(instance_, nullptr);
        glfwDestroyWindow(window_);
        glfwTerminate();
    }
};
