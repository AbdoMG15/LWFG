#pragma once

#include "lfg/vk_common.hpp"

#include <optional>
#include <string>
#include <vector>

namespace lfg {

struct DeviceExtensionFunctions {
    PFN_vkGetMemoryFdPropertiesKHR GetMemoryFdPropertiesKHR = nullptr;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT GetImageDrmFormatModifierPropertiesEXT = nullptr;
};

enum class GpuPreference {
    Auto,    // integrated GPU if there is one (it usually drives the display), else the first usable GPU
    Nvidia,  // first usable NVIDIA GPU (experimental: cross-GPU DMA-BUF import)
};

struct GpuDeviceInfo {
    size_t index = 0;
    std::string name;
    std::string shortLabel;
    VkPhysicalDeviceType deviceType = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    uint32_t vendorId = 0;
    bool isIntegrated = false;
    bool isNvidia = false;
};

struct QueueFamilies {
    uint32_t graphicsPresent = UINT32_MAX;
    uint32_t compute = UINT32_MAX;

    bool hasDedicatedCompute() const { return compute != graphicsPresent; }
};

class VulkanContext {
public:
    VulkanContext(const std::vector<const char*>& surfaceExtensions, bool enableValidation,
                  GpuPreference preference = GpuPreference::Auto, int explicitGpuIndex = -1);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkDevice device() const { return device_; }
    const QueueFamilies& queueFamilies() const { return queueFamilies_; }
    VkQueue graphicsPresentQueue() const { return graphicsPresentQueue_; }
    VkQueue computeQueue() const { return computeQueue_; }
    VkCommandPool graphicsCommandPool() const { return graphicsCommandPool_; }
    VkCommandPool computeCommandPool() const { return computeCommandPool_; }
    const DeviceExtensionFunctions& ext() const { return ext_; }
    bool fp16Supported() const { return fp16Supported_; }
    bool dmaBufSupported() const { return dmaBufSupported_; }
    bool drmModifierSupported() const { return drmModifierSupported_; }
    bool foreignQueueSupported() const { return foreignQueueSupported_; }

    const std::vector<GpuDeviceInfo>& availableGpus() const { return gpuList_; }
    size_t selectedGpuIndex() const { return selectedGpuIndex_; }
    const GpuDeviceInfo& selectedGpuInfo() const { return gpuList_[selectedGpuIndex_]; }
    bool isCrossGpuLikely() const { return crossGpuLikely_; }

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;
    VkDeviceMemory allocateMemory(const VkMemoryRequirements& req,
                                  VkMemoryPropertyFlags preferredProps) const;

    VkCommandBuffer beginOneShotCommands(VkCommandPool pool) const;
    void endOneShotCommands(VkCommandPool pool, VkQueue queue, VkCommandBuffer cmd) const;
    void submitOneShotAsync(VkQueue queue, VkCommandBuffer cmd) const;
    void waitOneShotAsync() const;
    bool pollOneShotAsyncComplete() const;
    bool isOneShotAsyncPending() const { return oneShotInUse_; }

private:
    void createInstance(const std::vector<const char*>& surfaceExtensions, bool enableValidation);
    void pickPhysicalDeviceAndQueues(GpuPreference preference, int explicitGpuIndex);
    void createLogicalDevice();
    void loadExtensionFunctions();
    void createCommandPools();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;

    QueueFamilies queueFamilies_;
    VkQueue graphicsPresentQueue_ = VK_NULL_HANDLE;
    VkQueue computeQueue_ = VK_NULL_HANDLE;

    VkCommandPool graphicsCommandPool_ = VK_NULL_HANDLE;
    VkCommandPool computeCommandPool_ = VK_NULL_HANDLE;

    // Reusable primary command buffer + fence for synchronous one-shot commands on graphicsCommandPool_
    // (eliminates per-frame vkAllocateCommandBuffers / vkCreateFence / vkDestroyFence overhead).
    mutable VkCommandBuffer oneShotCmd_ = VK_NULL_HANDLE;
    mutable VkFence oneShotFence_ = VK_NULL_HANDLE;
    mutable bool oneShotInUse_ = false;

    DeviceExtensionFunctions ext_;
    bool fp16Supported_ = false;
    bool dmaBufSupported_ = false;
    bool drmModifierSupported_ = false;
    bool foreignQueueSupported_ = false;

    std::vector<GpuDeviceInfo> gpuList_;
    size_t selectedGpuIndex_ = 0;
    bool crossGpuLikely_ = false;

    std::vector<const char*> enabledDeviceExtensions_;
};

}  // namespace lfg
