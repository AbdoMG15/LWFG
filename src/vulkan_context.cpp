#include "lfg/vulkan_context.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>

namespace lfg {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::cerr << "[validation] " << data->pMessage << "\n";
    }
    return VK_FALSE;
}

const std::vector<const char*> kCoreDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
};

const std::vector<const char*> kOptionalDmaBufExtensions = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
};

bool deviceSupportsExtensions(VkPhysicalDevice device, const std::vector<const char*>& required) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());

    for (const char* req : required) {
        bool found = false;
        for (const auto& ext : available) {
            if (std::strcmp(req, ext.extensionName) == 0) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

bool deviceSupportsExtension(VkPhysicalDevice device, const char* name) {
    return deviceSupportsExtensions(device, {name});
}

std::string buildShortGpuLabel(size_t idx, const VkPhysicalDeviceProperties& props) {
    std::string raw = props.deviceName;

    // 1. Extract optional driver/chip tag in parentheses (e.g. "(RADV REMBRANDT)") and remove (R)/(TM)
    std::string parenTag;
    size_t openParen = raw.find('(');
    while (openParen != std::string::npos) {
        size_t closeParen = raw.find(')', openParen);
        if (closeParen == std::string::npos) break;
        std::string inside = raw.substr(openParen + 1, closeParen - openParen - 1);
        if (inside != "R" && inside != "r" && inside != "TM" && inside != "tm") {
            parenTag = inside;
        }
        raw.erase(openParen, closeParen - openParen + 1);
        openParen = raw.find('(');
    }

    // 2. Strip verbose vendor/marketing boilerplate so the real GPU brand + model fits cleanly
    const char* kStripPhrases[] = {
        "Advanced Micro Devices, Inc.",
        "[AMD/ATI]",
        "Corporation",
        "GeForce ",
        "Laptop GPU",
        "Max-Q Design",
        " with Max-Q",
        "Series",
    };
    for (const char* phrase : kStripPhrases) {
        size_t pos = 0;
        size_t len = std::strlen(phrase);
        while ((pos = raw.find(phrase, pos)) != std::string::npos) {
            raw.erase(pos, len);
        }
    }

    // 3. Collapse whitespace and trim
    std::string cleaned;
    cleaned.reserve(raw.size());
    bool prevSpace = true;
    for (char c : raw) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!prevSpace) {
                cleaned.push_back(' ');
                prevSpace = true;
            }
        } else {
            cleaned.push_back(c);
            prevSpace = false;
        }
    }
    if (!cleaned.empty() && cleaned.back() == ' ') cleaned.pop_back();

    // 4. Uppercase for the bitmap UI font
    for (char& c : cleaned) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    for (char& c : parenTag) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }

    // 5. Clean trailing " GRAPHICS" and preserve specific GPU model or RADV chip codename
    const std::string kGraphicsSuffix = " GRAPHICS";
    if (cleaned.size() > kGraphicsSuffix.size() &&
        cleaned.compare(cleaned.size() - kGraphicsSuffix.size(), kGraphicsSuffix.size(), kGraphicsSuffix) == 0) {
        std::string withoutGraphics = cleaned.substr(0, cleaned.size() - kGraphicsSuffix.size());
        if (withoutGraphics == "AMD RADEON" || withoutGraphics == "AMD") {
            if (parenTag.rfind("RADV ", 0) == 0 && parenTag.size() > 5) {
                cleaned = withoutGraphics + " " + parenTag.substr(5);
            } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                cleaned = withoutGraphics + " IGPU";
            } else {
                cleaned = withoutGraphics;
            }
        } else {
            cleaned = withoutGraphics;
        }
    } else if ((cleaned == "AMD RADEON" || cleaned == "AMD") &&
               props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        if (parenTag.rfind("RADV ", 0) == 0 && parenTag.size() > 5) {
            cleaned += " " + parenTag.substr(5);
        } else {
            cleaned += " IGPU";
        }
    }

    if (cleaned.empty()) {
        cleaned = "GPU";
    }
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
        cleaned.find("IGPU") == std::string::npos) {
        cleaned += " IGPU";
    }

    return "[" + std::to_string(idx) + "] " + cleaned;
}

}  // namespace

VulkanContext::VulkanContext(const std::vector<const char*>& surfaceExtensions, bool enableValidation,
                             GpuPreference preference, int explicitGpuIndex) {
    createInstance(surfaceExtensions, enableValidation);
    pickPhysicalDeviceAndQueues(preference, explicitGpuIndex);
    createLogicalDevice();
    loadExtensionFunctions();
    createCommandPools();
}

VulkanContext::~VulkanContext() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (oneShotFence_) vkDestroyFence(device_, oneShotFence_, nullptr);
        if (graphicsCommandPool_) vkDestroyCommandPool(device_, graphicsCommandPool_, nullptr);
        if (computeCommandPool_ && computeCommandPool_ != graphicsCommandPool_)
            vkDestroyCommandPool(device_, computeCommandPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (debugMessenger_ != VK_NULL_HANDLE) {
        auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyFn) destroyFn(instance_, debugMessenger_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
}

void VulkanContext::createInstance(const std::vector<const char*>& surfaceExtensions, bool enableValidation) {
    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "lfg-linux";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 5);
    appInfo.pEngineName = "lfg-linux";
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> instanceExtensions = surfaceExtensions;
    instanceExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    if (enableValidation) instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    std::vector<const char*> layers;
    if (enableValidation) layers.push_back("VK_LAYER_KHRONOS_validation");

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
    createInfo.ppEnabledExtensionNames = instanceExtensions.data();
    createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    createInfo.ppEnabledLayerNames = layers.data();

    VK_CHECK(vkCreateInstance(&createInfo, nullptr, &instance_));

    if (enableValidation) {
        auto createFn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (createFn) {
            VkDebugUtilsMessengerCreateInfoEXT dbgInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            dbgInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                       VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dbgInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dbgInfo.pfnUserCallback = debugCallback;
            createFn(instance_, &dbgInfo, nullptr, &debugMessenger_);
        }
    }
}

void VulkanContext::pickPhysicalDeviceAndQueues(GpuPreference preference, int explicitGpuIndex) {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
    if (deviceCount == 0) throw std::runtime_error("No Vulkan-capable physical devices found");
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

    struct Candidate {
        VkPhysicalDevice device;
        VkPhysicalDeviceProperties props;
        uint32_t graphicsFamily;
        uint32_t computeFamily;
    };
    std::vector<Candidate> candidates;

    for (VkPhysicalDevice candidate : devices) {
        if (!deviceSupportsExtensions(candidate, kCoreDeviceExtensions)) continue;

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());

        std::optional<uint32_t> graphicsFamily;
        std::optional<uint32_t> dedicatedComputeFamily;
        for (uint32_t i = 0; i < familyCount; ++i) {
            const bool hasGraphics = families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT;
            const bool hasCompute = families[i].queueFlags & VK_QUEUE_COMPUTE_BIT;
            if (hasGraphics && !graphicsFamily) graphicsFamily = i;
            if (hasCompute && !hasGraphics && !dedicatedComputeFamily) dedicatedComputeFamily = i;
        }
        if (!graphicsFamily) continue;

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(candidate, &props);
        candidates.push_back({candidate, props, *graphicsFamily, dedicatedComputeFamily.value_or(*graphicsFamily)});
    }
    if (candidates.empty()) throw std::runtime_error("No physical device supports the required extensions/queues");

    // Filter out software/CPU rasterizers (e.g. llvmpipe) if at least one hardware GPU is available,
    // so cycling GPUs in the UI only switches between real hardware GPUs (e.g. iGPU <-> NVIDIA dGPU).
    bool hasHardwareGpu = false;
    for (const auto& c : candidates) {
        if (c.props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
            hasHardwareGpu = true;
            break;
        }
    }
    if (hasHardwareGpu) {
        candidates.erase(
            std::remove_if(candidates.begin(), candidates.end(),
                           [](const Candidate& c) { return c.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU; }),
            candidates.end());
    }

    // Ensure Integrated GPUs (e.g. AMD APU driving the desktop compositor) are always ordered
    // at index [0] and Discrete GPUs (e.g. NVIDIA dGPU) follow at [1].
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) {
                         auto rank = [](VkPhysicalDeviceType t) {
                             if (t == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) return 0;
                             if (t == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) return 1;
                             return 2;
                         };
                         return rank(a.props.deviceType) < rank(b.props.deviceType);
                     });

    // If multiple Vulkan ICDs are installed for the same physical card (e.g. RADV + AMDVLK,
    // or NVIDIA proprietary + NVK), deduplicate by (vendorID, deviceID, deviceType), preferring
    // the driver that supports VK_EXT_image_drm_format_modifier.
    std::vector<Candidate> uniqueCandidates;
    for (const auto& c : candidates) {
        bool merged = false;
        for (auto& u : uniqueCandidates) {
            if (u.props.vendorID == c.props.vendorID &&
                u.props.deviceID == c.props.deviceID &&
                u.props.deviceType == c.props.deviceType) {
                bool uHasMod = deviceSupportsExtension(u.device, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
                bool cHasMod = deviceSupportsExtension(c.device, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
                if (!uHasMod && cHasMod) {
                    u = c;
                }
                merged = true;
                break;
            }
        }
        if (!merged) uniqueCandidates.push_back(c);
    }
    candidates = std::move(uniqueCandidates);

    auto typeName = [](VkPhysicalDeviceType t) {
        switch (t) {
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
            case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
            default: return "other";
        }
    };

    gpuList_.clear();
    bool hasIntegrated = false;
    std::fprintf(stderr, "[vulkan] usable GPUs:\n");
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& p = candidates[i].props;
        std::fprintf(stderr, "  [%zu] %s (%s)\n", i, p.deviceName, typeName(p.deviceType));
        bool isIgpu = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
        if (isIgpu) hasIntegrated = true;
        gpuList_.push_back(GpuDeviceInfo{
            i,
            p.deviceName,
            buildShortGpuLabel(i, p),
            p.deviceType,
            p.vendorID,
            isIgpu,
            p.vendorID == 0x10DE
        });
    }

    size_t chosenIndex = 0;
    if (explicitGpuIndex >= 0 && static_cast<size_t>(explicitGpuIndex) < candidates.size()) {
        chosenIndex = static_cast<size_t>(explicitGpuIndex);
        std::fprintf(stderr, "[vulkan] explicitly selected GPU [%zu]\n", chosenIndex);
    } else if (const char* forcedIndex = std::getenv("LFG_GPU_INDEX")) {
        int envIdx = std::atoi(forcedIndex);
        if (envIdx >= 0 && static_cast<size_t>(envIdx) < candidates.size()) {
            chosenIndex = static_cast<size_t>(envIdx);
            std::fprintf(stderr, "[vulkan] LFG_GPU_INDEX set — forcing GPU [%zu]\n", chosenIndex);
        }
    } else if (preference == GpuPreference::Nvidia) {
        constexpr uint32_t kNvidiaVendorId = 0x10DE;
        bool found = false;
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (candidates[i].props.vendorID == kNvidiaVendorId) {
                chosenIndex = i;
                found = true;
                break;
            }
        }
        if (found) {
            std::fprintf(stderr, "[vulkan] using NVIDIA GPU [%zu]\n", chosenIndex);
        } else {
            std::fprintf(stderr, "[vulkan] NVIDIA GPU not found on this system — defaulting to GPU [0]\n");
            chosenIndex = 0;
        }
    } else {
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (candidates[i].props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                chosenIndex = i;
                break;
            }
        }
    }

    selectedGpuIndex_ = chosenIndex;
    const Candidate& chosen = candidates[chosenIndex];
    // If the system has multiple GPUs and the user picked a secondary/discrete GPU,
    // use linear/SHM cross-GPU capture negotiation.
    crossGpuLikely_ = (candidates.size() > 1) &&
                      (chosenIndex != 0 || (hasIntegrated && chosen.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU));

    std::fprintf(stderr, "[vulkan] active GPU: [%zu] %s%s\n",
                 chosenIndex, chosen.props.deviceName,
                 crossGpuLikely_ ? " (cross-GPU linear DMA-BUF mode)" : "");

    physicalDevice_ = chosen.device;
    queueFamilies_.graphicsPresent = chosen.graphicsFamily;
    queueFamilies_.compute = chosen.computeFamily;

    dmaBufSupported_ =
        deviceSupportsExtension(physicalDevice_, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
        deviceSupportsExtension(physicalDevice_, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    drmModifierSupported_ =
        dmaBufSupported_ &&
        deviceSupportsExtension(physicalDevice_, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    foreignQueueSupported_ =
        deviceSupportsExtension(physicalDevice_, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
}

void VulkanContext::createLogicalDevice() {
    std::set<uint32_t> uniqueFamilies = {queueFamilies_.graphicsPresent, queueFamilies_.compute};
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    // Use low queue priority (0.2) on the integrated APU so KWin's compositor always preempts
    // our LSFG compute dispatches. On discrete GPUs use a medium priority (0.5): 1.0 let LSFG
    // preempt the game running on the same GPU and dropped its base FPS (e.g. 50 -> 30).
    // Override with LFG_QUEUE_PRIORITY=0.0..1.0 for testing.
    float priority = selectedGpuInfo().isIntegrated ? 0.2f : 0.5f;
    if (const char* qp = std::getenv("LFG_QUEUE_PRIORITY")) {
        priority = std::clamp(static_cast<float>(std::atof(qp)), 0.0f, 1.0f);
    }
    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        queueInfos.push_back(qi);
    }

    enabledDeviceExtensions_ = kCoreDeviceExtensions;
    for (const char* optExt : kOptionalDmaBufExtensions) {
        if (deviceSupportsExtension(physicalDevice_, optExt)) {
            enabledDeviceExtensions_.push_back(optExt);
        }
    }

    VkPhysicalDeviceVulkan12Features supportedVk12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 supportedFeatures2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    supportedFeatures2.pNext = &supportedVk12;
    vkGetPhysicalDeviceFeatures2(physicalDevice_, &supportedFeatures2);
    fp16Supported_ = (supportedVk12.shaderFloat16 == VK_TRUE);

    VkPhysicalDeviceVulkan12Features vk12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    vk12.shaderFloat16 = fp16Supported_ ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceSynchronization2Features sync2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    sync2.synchronization2 = VK_TRUE;
    sync2.pNext = &vk12;

    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.features.shaderStorageImageExtendedFormats =
        supportedFeatures2.features.shaderStorageImageExtendedFormats;
    features2.features.shaderStorageImageWriteWithoutFormat =
        supportedFeatures2.features.shaderStorageImageWriteWithoutFormat;
    features2.pNext = &sync2;

    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &features2;
    deviceInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledDeviceExtensions_.size());
    deviceInfo.ppEnabledExtensionNames = enabledDeviceExtensions_.data();

    VK_CHECK(vkCreateDevice(physicalDevice_, &deviceInfo, nullptr, &device_));

    vkGetDeviceQueue(device_, queueFamilies_.graphicsPresent, 0, &graphicsPresentQueue_);
    vkGetDeviceQueue(device_, queueFamilies_.compute, 0, &computeQueue_);
}

void VulkanContext::loadExtensionFunctions() {
    ext_.GetMemoryFdPropertiesKHR = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
        vkGetDeviceProcAddr(device_, "vkGetMemoryFdPropertiesKHR"));
    ext_.GetImageDrmFormatModifierPropertiesEXT = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
        vkGetDeviceProcAddr(device_, "vkGetImageDrmFormatModifierPropertiesEXT"));
    if (!ext_.GetMemoryFdPropertiesKHR) {
        dmaBufSupported_ = false;
        drmModifierSupported_ = false;
    }
}

void VulkanContext::createCommandPools() {
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    poolInfo.queueFamilyIndex = queueFamilies_.graphicsPresent;
    VK_CHECK(vkCreateCommandPool(device_, &poolInfo, nullptr, &graphicsCommandPool_));

    if (queueFamilies_.hasDedicatedCompute()) {
        poolInfo.queueFamilyIndex = queueFamilies_.compute;
        VK_CHECK(vkCreateCommandPool(device_, &poolInfo, nullptr, &computeCommandPool_));
    } else {
        computeCommandPool_ = graphicsCommandPool_;
    }

    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = graphicsCommandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device_, &allocInfo, &oneShotCmd_));

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(device_, &fenceInfo, nullptr, &oneShotFence_));
}

uint32_t VulkanContext::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    throw std::runtime_error("No suitable Vulkan memory type found");
}

VkDeviceMemory VulkanContext::allocateMemory(const VkMemoryRequirements& req,
                                             VkMemoryPropertyFlags preferredProps) const {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;

    // 1. Try preferred memory properties first (e.g. DEVICE_LOCAL_BIT VRAM).
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & preferredProps) == preferredProps) {
            ai.memoryTypeIndex = i;
            VkDeviceMemory mem = VK_NULL_HANDLE;
            if (vkAllocateMemory(device_, &ai, nullptr, &mem) == VK_SUCCESS) {
                return mem;
            }
        }
    }

    // 2. No silent fallback to system RAM: GPU images placed in host memory are read over PCIe for
    //    every LSFG pass, which tanks both the game and the generated frame rate (0.1.2 regression).
    //    If VRAM is full we fail here and the caller retries with lighter settings instead.
    std::fprintf(stderr, "[vulkan] VRAM full: could not allocate %.1f MB of device-local memory\n",
                 static_cast<double>(req.size) / (1024.0 * 1024.0));
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, preferredProps);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &mem));
    return mem;
}

VkCommandBuffer VulkanContext::beginOneShotCommands(VkCommandPool pool) const {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (pool == graphicsCommandPool_ && oneShotCmd_ != VK_NULL_HANDLE && !oneShotInUse_) {
        oneShotInUse_ = true;
        VK_CHECK(vkResetCommandBuffer(oneShotCmd_, 0));
        cmd = oneShotCmd_;
    } else {
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device_, &allocInfo, &cmd));
    }

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));
    return cmd;
}

void VulkanContext::endOneShotCommands(VkCommandPool pool, VkQueue queue, VkCommandBuffer cmd) const {
    VK_CHECK(vkEndCommandBuffer(cmd));

    if (cmd == oneShotCmd_ && oneShotFence_ != VK_NULL_HANDLE) {
        VK_CHECK(vkResetFences(device_, 1, &oneShotFence_));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        VK_CHECK(vkQueueSubmit(queue, 1, &submit, oneShotFence_));
        VK_CHECK(vkWaitForFences(device_, 1, &oneShotFence_, VK_TRUE, UINT64_MAX));
        oneShotInUse_ = false;
        return;
    }

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK_CHECK(vkCreateFence(device_, &fenceInfo, nullptr, &fence));

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    VK_CHECK(vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX));

    vkDestroyFence(device_, fence, nullptr);
    vkFreeCommandBuffers(device_, pool, 1, &cmd);
}

void VulkanContext::submitOneShotAsync(VkQueue queue, VkCommandBuffer cmd) const {
    VK_CHECK(vkEndCommandBuffer(cmd));

    if (cmd == oneShotCmd_ && oneShotFence_ != VK_NULL_HANDLE) {
        VK_CHECK(vkResetFences(device_, 1, &oneShotFence_));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        VK_CHECK(vkQueueSubmit(queue, 1, &submit, oneShotFence_));
        oneShotInUse_ = true;
        return;
    }

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK_CHECK(vkCreateFence(device_, &fenceInfo, nullptr, &fence));

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    VK_CHECK(vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX));

    vkDestroyFence(device_, fence, nullptr);
    vkFreeCommandBuffers(device_, graphicsCommandPool_, 1, &cmd);
}

void VulkanContext::waitOneShotAsync() const {
    if (!oneShotInUse_ || oneShotFence_ == VK_NULL_HANDLE) return;
    VK_CHECK(vkWaitForFences(device_, 1, &oneShotFence_, VK_TRUE, UINT64_MAX));
    oneShotInUse_ = false;
}

}  // namespace lfg
