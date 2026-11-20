#pragma once

#include "lfg/vulkan_context.hpp"

#include <vector>

namespace lfg::capture {

struct DmaBufPlane {
    int fd = -1;
    uint32_t offset = 0;
    uint32_t stride = 0;
};

struct ImportedFrame {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
};

class DmaBufImageImporter {
public:
    explicit DmaBufImageImporter(VulkanContext& vk) : vk_(vk) {}

    std::vector<uint64_t> queryImportableModifiers(VkFormat format) const;
    uint32_t queryModifierPlaneCount(VkFormat format, uint64_t drmModifier) const;

    ImportedFrame import(uint32_t width, uint32_t height, VkFormat format, bool hasExplicitModifier,
                          uint64_t drmModifier, const std::vector<DmaBufPlane>& planes);

    void release(ImportedFrame& frame);

private:
    VulkanContext& vk_;
};

}  // namespace lfg::capture
