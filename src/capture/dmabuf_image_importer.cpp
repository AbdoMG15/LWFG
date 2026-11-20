#include "lfg/capture/dmabuf_image_importer.hpp"

#include <unistd.h>

namespace lfg::capture {

std::vector<uint64_t> DmaBufImageImporter::queryImportableModifiers(VkFormat format) const {
    if (!vk_.drmModifierSupported()) return {};

    VkDrmFormatModifierPropertiesListEXT modList{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 props2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    props2.pNext = &modList;

    vkGetPhysicalDeviceFormatProperties2(vk_.physicalDevice(), format, &props2);
    if (modList.drmFormatModifierCount == 0) return {};

    std::vector<VkDrmFormatModifierPropertiesEXT> mods(modList.drmFormatModifierCount);
    modList.pDrmFormatModifierProperties = mods.data();
    vkGetPhysicalDeviceFormatProperties2(vk_.physicalDevice(), format, &props2);

    std::vector<uint64_t> usable;
    for (const auto& m : mods) {
        constexpr VkFormatFeatureFlags kRequired =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_BLIT_SRC_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        if ((m.drmFormatModifierTilingFeatures & kRequired) == kRequired &&
            m.drmFormatModifierPlaneCount == 1 &&
            !(m.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_DISJOINT_BIT)) {
            usable.push_back(m.drmFormatModifier);
        }
    }
    return usable;
}

uint32_t DmaBufImageImporter::queryModifierPlaneCount(VkFormat format, uint64_t drmModifier) const {
    if (!vk_.drmModifierSupported()) return 1;

    VkDrmFormatModifierPropertiesListEXT modList{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 props2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    props2.pNext = &modList;

    vkGetPhysicalDeviceFormatProperties2(vk_.physicalDevice(), format, &props2);
    if (modList.drmFormatModifierCount == 0) return 1;

    std::vector<VkDrmFormatModifierPropertiesEXT> mods(modList.drmFormatModifierCount);
    modList.pDrmFormatModifierProperties = mods.data();
    vkGetPhysicalDeviceFormatProperties2(vk_.physicalDevice(), format, &props2);

    for (const auto& m : mods) {
        if (m.drmFormatModifier == drmModifier) return m.drmFormatModifierPlaneCount;
    }
    return 1;
}

ImportedFrame DmaBufImageImporter::import(uint32_t width, uint32_t height, VkFormat format,
                                           bool hasExplicitModifier, uint64_t drmModifier,
                                           const std::vector<DmaBufPlane>& planes) {
    if (!vk_.dmaBufSupported()) {
        throw std::runtime_error("DmaBufImageImporter::import: DMA-BUF extension not supported on this GPU");
    }
    if (planes.empty() || planes.size() > 4 || planes[0].fd < 0) {
        throw std::runtime_error("DmaBufImageImporter::import: invalid plane count or fd");
    }

    ImportedFrame result;
    result.width = width;
    result.height = height;

    const bool useModifierExt = hasExplicitModifier && vk_.drmModifierSupported();

    std::vector<VkSubresourceLayout> planeLayouts(planes.size());
    for (size_t i = 0; i < planes.size(); ++i) {
        planeLayouts[i] = VkSubresourceLayout{};
        planeLayouts[i].offset = planes[i].offset;
        planeLayouts[i].rowPitch = planes[i].stride;
        planeLayouts[i].size = 0;
        planeLayouts[i].arrayPitch = 0;
        planeLayouts[i].depthPitch = 0;
    }

    VkImageDrmFormatModifierExplicitCreateInfoEXT modifierInfo{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    modifierInfo.drmFormatModifier = drmModifier;
    modifierInfo.drmFormatModifierPlaneCount = static_cast<uint32_t>(planeLayouts.size());
    modifierInfo.pPlaneLayouts = planeLayouts.data();

    VkExternalMemoryImageCreateInfo externalInfo{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    if (useModifierExt) externalInfo.pNext = &modifierInfo;

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.pNext = &externalInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = useModifierExt ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VK_CHECK(vkCreateImage(vk_.device(), &imageInfo, nullptr, &result.image));

    VkMemoryFdPropertiesKHR fdProps{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    uint32_t fdTypeBits = UINT32_MAX;
    if (vk_.ext().GetMemoryFdPropertiesKHR) {
        VkResult fdPropsResult = vk_.ext().GetMemoryFdPropertiesKHR(
            vk_.device(), VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, planes[0].fd, &fdProps);
        if (fdPropsResult == VK_SUCCESS && fdProps.memoryTypeBits != 0) {
            fdTypeBits = fdProps.memoryTypeBits;
        }
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(vk_.device(), result.image, &memReqs);

    uint32_t candidateBits = (memReqs.memoryTypeBits & fdTypeBits);
    if (candidateBits == 0) candidateBits = memReqs.memoryTypeBits;
    uint32_t typeIndex = vk_.findMemoryType(candidateBits, 0);

    auto tryAlloc = [&](bool dedicated) -> VkResult {
        int importFd = ::dup(planes[0].fd);
        if (importFd < 0) return VK_ERROR_INVALID_EXTERNAL_HANDLE;

        VkImportMemoryFdInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
        importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        importInfo.fd = importFd;

        VkMemoryDedicatedAllocateInfo dedicatedInfo{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicatedInfo.image = result.image;
        dedicatedInfo.pNext = &importInfo;

        VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.pNext = dedicated ? static_cast<const void*>(&dedicatedInfo)
                                    : static_cast<const void*>(&importInfo);
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = typeIndex;

        VkResult r = vkAllocateMemory(vk_.device(), &allocInfo, nullptr, &result.memory);
        if (r != VK_SUCCESS) {
            ::close(importFd);
        }
        return r;
    };

    // Always try with VkMemoryDedicatedAllocateInfo first (required by Mesa RADV for zero-copy DRM modifiers).
    VkResult allocResult = tryAlloc(true);
    if (allocResult != VK_SUCCESS) {
        allocResult = tryAlloc(false);
    }
    if (allocResult != VK_SUCCESS) {
        vkDestroyImage(vk_.device(), result.image, nullptr);
        result.image = VK_NULL_HANDLE;
        VK_CHECK(allocResult);
    }

    VkBindImageMemoryInfo bindInfo{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};
    bindInfo.image = result.image;
    bindInfo.memory = result.memory;
    VkResult bindRes = vkBindImageMemory2(vk_.device(), 1, &bindInfo);
    if (bindRes != VK_SUCCESS) {
        vkFreeMemory(vk_.device(), result.memory, nullptr);
        vkDestroyImage(vk_.device(), result.image, nullptr);
        result.memory = VK_NULL_HANDLE;
        result.image = VK_NULL_HANDLE;
        VK_CHECK(bindRes);
    }

    return result;
}

void DmaBufImageImporter::release(ImportedFrame& frame) {
    if (frame.view) vkDestroyImageView(vk_.device(), frame.view, nullptr);
    if (frame.image) vkDestroyImage(vk_.device(), frame.image, nullptr);
    if (frame.memory) vkFreeMemory(vk_.device(), frame.memory, nullptr);
    frame = ImportedFrame{};
}

}  // namespace lfg::capture
