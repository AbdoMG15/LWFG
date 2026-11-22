#include "lfg/frame_generator.hpp"

#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace lfg {

FrameGenerator::FrameGenerator(const Options& options) : options_(options) {
    if (const char* d = std::getenv("LFG_DEDUP"); d && std::strcmp(d, "0") == 0) {
        dedupEnabled_ = false;
    }
    options_.multiplier = std::clamp(options_.multiplier, 2, kMaxMultiplier);
    options_.flowScale = std::clamp(options_.flowScale, 25, 100);
    if (options_.targetFps <= 0) {
        options_.targetFps = 120;
        targetFpsAutoInit_ = true;
    } else {
        options_.targetFps = std::clamp(options_.targetFps, 30, kMaxTargetFps);
    }
    options_.panelWidth = std::clamp(options_.panelWidth, 340, 1920);
    options_.panelHeight = std::clamp(options_.panelHeight, 380, 1440);

    initVulkanBackend(options_.gpuIndex, options_.nvidia ? GpuPreference::Nvidia : GpuPreference::Auto);

    std::string gpuLabel = vk_->selectedGpuInfo().shortLabel;
    controlPanel_ = std::make_unique<present::ControlPanel>(
        options_.displayIndex, options_.multiplier, options_.adaptive, options_.targetFps,
        options_.showFps, options_.toggleHotkey, gpuLabel,
        options_.panelWidth, options_.panelHeight, options_.flowScale, options_.performanceMode,
        options_.dllPath, options_.vsync);
    if (controlPanel_) {
        const bool lsfgOk = lsfg_ && lsfg_->isAvailable();
        controlPanel_->setLsfgActive(lsfgOk);
        if (lsfgOk) {
            controlPanel_->setDllPath(lsfg_->dllPath());
        } else {
            controlPanel_->setStatusNotice("SET LOSSLESS SCALING PATH IN SETTINGS");
        }
        options_.toggleHotkey = controlPanel_->hotkeyLabel();
    }
    saveSettings(options_);
}

FrameGenerator::~FrameGenerator() {
    if (controlPanel_) {
        options_.panelWidth = controlPanel_->windowWidth();
        options_.panelHeight = controlPanel_->windowHeight();
        options_.toggleHotkey = controlPanel_->hotkeyLabel();
        options_.flowScale = controlPanel_->flowScale();
        options_.performanceMode = controlPanel_->performanceMode();
        if (lsfg_ && lsfg_->isAvailable()) {
            options_.dllPath = lsfg_->dllPath();
        }
    }
    saveSettings(options_);
    stopCaptureSession();
    teardownVulkanBackend();
}

void FrameGenerator::initVulkanBackend(int gpuIndex, GpuPreference pref) {
    auto surfaceExts = present::OverlayWindow::requiredInstanceExtensions();
    vk_ = std::make_unique<VulkanContext>(surfaceExts, options_.validation, pref, gpuIndex);
    options_.gpuIndex = static_cast<int>(vk_->selectedGpuIndex());
    options_.nvidia = vk_->selectedGpuInfo().isNvidia;
    forceLinearCapture_ = false;
    forceShmCapture_ = false;
    directImportFailed_ = false;
    loggedStagingFallback_ = false;

    importer_ = std::make_unique<capture::DmaBufImageImporter>(*vk_);
    lsfg_ = std::make_unique<compute::LsfgPipeline>(*vk_, options_.dllPath);
    if (lsfg_->isAvailable()) {
        options_.dllPath = lsfg_->dllPath();
    }
    if (controlPanel_) {
        const bool lsfgOk = lsfg_ && lsfg_->isAvailable();
        controlPanel_->setLsfgActive(lsfgOk);
        if (lsfgOk) {
            controlPanel_->setDllPath(options_.dllPath);
        } else {
            controlPanel_->setStatusNotice("SET LOSSLESS SCALING PATH IN SETTINGS");
        }
    }
}

void FrameGenerator::teardownVulkanBackend() {
    if (vk_) {
        vk_->waitOneShotAsync();
        vkDeviceWaitIdle(vk_->device());
        clearPrefetchedFrame();
        resetPacerState(false);
        overlay_.reset();
        lsfg_.reset();
        importer_.reset();
        destroyThumbResources();
        destroyStagingBuffer();
        destroySlots();
        vk_.reset();
    }
    lastPresentedOutputSlot_ = -1;
    ++resourceEpoch_;
}

bool FrameGenerator::ensureLsfgAvailable() {
    if (lsfg_ && lsfg_->isAvailable()) {
        if (controlPanel_) controlPanel_->setLsfgActive(true);
        return true;
    }
    if (vk_) {
        lsfg_ = std::make_unique<compute::LsfgPipeline>(*vk_, options_.dllPath);
        if (lsfg_->isAvailable()) {
            options_.dllPath = lsfg_->dllPath();
            saveSettings(options_);
            if (controlPanel_) {
                controlPanel_->setDllPath(options_.dllPath);
                controlPanel_->setLsfgActive(true);
                controlPanel_->setStatusNotice("");
            }
            return true;
        }
    }
    std::fprintf(stderr,
                 "[lfg] ERROR: Lossless Scaling files not found! Install Lossless Scaling on Steam or set its location in the SETTINGS tab.\n");
    if (controlPanel_) {
        controlPanel_->setLsfgActive(false);
        controlPanel_->setStatusNotice("SET LOSSLESS SCALING PATH IN SETTINGS");
    }
    return false;
}

bool FrameGenerator::switchGpu(int targetGpuIndex) {
    int previousGpu = vk_ ? static_cast<int>(vk_->selectedGpuIndex()) : 0;
    bool wasGenerating = generating_;
    generating_ = false;
    if (controlPanel_) controlPanel_->setGenerating(false);

    stopCaptureSession();
    teardownVulkanBackend();

    try {
        initVulkanBackend(targetGpuIndex, GpuPreference::Auto);
        if (controlPanel_) {
            controlPanel_->setGpuLabel(vk_->selectedGpuInfo().shortLabel);
            if (lsfg_ && lsfg_->isAvailable()) {
                controlPanel_->setStatusNotice("");
            } else {
                controlPanel_->setStatusNotice("INSTALL LOSSLESS SCALING ON STEAM");
            }
        }
        std::fprintf(stderr, "[lfg] >>> Switched active GPU to %s <<<\n",
                     vk_->selectedGpuInfo().name.c_str());

        if (wasGenerating) {
            handleControlAction(present::ControlAction::Start);
        }
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[lfg] failed to switch to GPU [%d]: %s — reverting to GPU [%d]\n",
                     targetGpuIndex, e.what(), previousGpu);
        teardownVulkanBackend();
        initVulkanBackend(previousGpu, GpuPreference::Auto);
        if (controlPanel_) {
            controlPanel_->setGpuLabel(vk_->selectedGpuInfo().shortLabel);
            controlPanel_->setStatusNotice("GPU SWITCH FAILED - REVERTED");
        }
        return false;
    }
}

void FrameGenerator::destroySlots() {
    clearPendingUniqueFrame();
    for (auto& slot : slots_) {
        if (slot.view) vkDestroyImageView(vk_->device(), slot.view, nullptr);
        if (slot.image) vkDestroyImage(vk_->device(), slot.image, nullptr);
        if (slot.memory) vkFreeMemory(vk_->device(), slot.memory, nullptr);
        slot = FrameSlot{};
    }
    if (pendingSlot_.view) vkDestroyImageView(vk_->device(), pendingSlot_.view, nullptr);
    if (pendingSlot_.image) vkDestroyImage(vk_->device(), pendingSlot_.image, nullptr);
    if (pendingSlot_.memory) vkFreeMemory(vk_->device(), pendingSlot_.memory, nullptr);
    pendingSlot_ = FrameSlot{};
    frameWidth_ = 0;
    frameHeight_ = 0;
}

void FrameGenerator::ensureStagingBuffer(VkDeviceSize requiredBytes) {
    if (stagingBuffer_ != VK_NULL_HANDLE && stagingSize_ >= requiredBytes && stagingMapped_ != nullptr) {
        return;
    }
    destroyStagingBuffer();

    VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size = requiredBytes;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(vk_->device(), &bufInfo, nullptr, &stagingBuffer_));

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vk_->device(), stagingBuffer_, &memReqs);

    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = memReqs.size;
    alloc.memoryTypeIndex = vk_->findMemoryType(
        memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(vk_->device(), &alloc, nullptr, &stagingMemory_));
    VK_CHECK(vkBindBufferMemory(vk_->device(), stagingBuffer_, stagingMemory_, 0));
    VK_CHECK(vkMapMemory(vk_->device(), stagingMemory_, 0, requiredBytes, 0, &stagingMapped_));
    stagingSize_ = requiredBytes;
}

void FrameGenerator::destroyStagingBuffer() {
    if (!vk_ || vk_->device() == VK_NULL_HANDLE) return;
    if (stagingMemory_ != VK_NULL_HANDLE) {
        if (stagingMapped_) {
            vkUnmapMemory(vk_->device(), stagingMemory_);
            stagingMapped_ = nullptr;
        }
        vkFreeMemory(vk_->device(), stagingMemory_, nullptr);
        stagingMemory_ = VK_NULL_HANDLE;
    }
    if (stagingBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(vk_->device(), stagingBuffer_, nullptr);
        stagingBuffer_ = VK_NULL_HANDLE;
    }
    stagingSize_ = 0;
}

void FrameGenerator::ensureThumbResources() {
    if (thumbImage_ != VK_NULL_HANDLE && thumbMapped_ != nullptr) {
        return;
    }
    destroyThumbResources();

    try {
        VkImageCreateInfo imgInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        imgInfo.extent = {kThumbW, kThumbH, 1};
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(vk_->device(), &imgInfo, nullptr, &thumbImage_));

        VkMemoryRequirements imgReqs;
        vkGetImageMemoryRequirements(vk_->device(), thumbImage_, &imgReqs);
        thumbImageMemory_ = vk_->allocateMemory(imgReqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkBindImageMemory(vk_->device(), thumbImage_, thumbImageMemory_, 0));

        VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufInfo.size = kThumbBytes;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(vk_->device(), &bufInfo, nullptr, &thumbBuffer_));

        VkMemoryRequirements bufReqs;
        vkGetBufferMemoryRequirements(vk_->device(), thumbBuffer_, &bufReqs);

        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = bufReqs.size;
        alloc.memoryTypeIndex = vk_->findMemoryType(
            bufReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(vk_->device(), &alloc, nullptr, &thumbBufferMemory_));
        VK_CHECK(vkBindBufferMemory(vk_->device(), thumbBuffer_, thumbBufferMemory_, 0));

        void* mapped = nullptr;
        VK_CHECK(vkMapMemory(vk_->device(), thumbBufferMemory_, 0, kThumbBytes, 0, &mapped));
        thumbMapped_ = static_cast<uint8_t*>(mapped);
    } catch (...) {
        destroyThumbResources();
        throw;
    }
}

void FrameGenerator::destroyThumbResources() {
    hasLastUniqueThumb_ = false;
    if (!vk_ || vk_->device() == VK_NULL_HANDLE) return;
    if (thumbBufferMemory_ != VK_NULL_HANDLE) {
        if (thumbMapped_) {
            vkUnmapMemory(vk_->device(), thumbBufferMemory_);
            thumbMapped_ = nullptr;
        }
        vkFreeMemory(vk_->device(), thumbBufferMemory_, nullptr);
        thumbBufferMemory_ = VK_NULL_HANDLE;
    }
    if (thumbBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(vk_->device(), thumbBuffer_, nullptr);
        thumbBuffer_ = VK_NULL_HANDLE;
    }
    if (thumbImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(vk_->device(), thumbImage_, nullptr);
        thumbImage_ = VK_NULL_HANDLE;
    }
    if (thumbImageMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(vk_->device(), thumbImageMemory_, nullptr);
        thumbImageMemory_ = VK_NULL_HANDLE;
    }
}

void FrameGenerator::recordThumbDownsample(VkCommandBuffer cmd, VkImage srcImage,
                                           uint32_t srcW, uint32_t srcH) {
    if (!dedupEnabled_ || thumbImage_ == VK_NULL_HANDLE || thumbBuffer_ == VK_NULL_HANDLE ||
        srcW == 0 || srcH == 0) {
        VkImageMemoryBarrier2 toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toGeneral.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
        toGeneral.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toGeneral.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.image = srcImage;
        toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toGeneral;
        vkCmdPipelineBarrier2(cmd, &dep);
        return;
    }

    VkImageMemoryBarrier2 preThumb[2]{};
    preThumb[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    preThumb[0].srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
    preThumb[0].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    preThumb[0].dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    preThumb[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    preThumb[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    preThumb[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    preThumb[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preThumb[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preThumb[0].image = srcImage;
    preThumb[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    preThumb[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    preThumb[1].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    preThumb[1].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    preThumb[1].dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    preThumb[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    preThumb[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    preThumb[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    preThumb[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preThumb[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preThumb[1].image = thumbImage_;
    preThumb[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo depPre{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depPre.imageMemoryBarrierCount = 2;
    depPre.pImageMemoryBarriers = preThumb;
    vkCmdPipelineBarrier2(cmd, &depPre);

    VkImageBlit thumbBlit{};
    thumbBlit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    thumbBlit.srcOffsets[0] = {0, 0, 0};
    thumbBlit.srcOffsets[1] = {static_cast<int32_t>(srcW), static_cast<int32_t>(srcH), 1};
    thumbBlit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    thumbBlit.dstOffsets[0] = {0, 0, 0};
    thumbBlit.dstOffsets[1] = {static_cast<int32_t>(kThumbW), static_cast<int32_t>(kThumbH), 1};
    vkCmdBlitImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, thumbImage_,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &thumbBlit, VK_FILTER_LINEAR);

    VkImageMemoryBarrier2 postBlit[2]{};
    postBlit[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    postBlit[0].srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    postBlit[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    postBlit[0].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    postBlit[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    postBlit[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    postBlit[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    postBlit[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    postBlit[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    postBlit[0].image = srcImage;
    postBlit[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    postBlit[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    postBlit[1].srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    postBlit[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    postBlit[1].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    postBlit[1].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    postBlit[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    postBlit[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    postBlit[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    postBlit[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    postBlit[1].image = thumbImage_;
    postBlit[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo depPostBlit{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depPostBlit.imageMemoryBarrierCount = 2;
    depPostBlit.pImageMemoryBarriers = postBlit;
    vkCmdPipelineBarrier2(cmd, &depPostBlit);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = kThumbW;
    region.bufferImageHeight = kThumbH;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {kThumbW, kThumbH, 1};
    vkCmdCopyImageToBuffer(cmd, thumbImage_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           thumbBuffer_, 1, &region);

    VkMemoryBarrier2 hostVisible{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    hostVisible.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    hostVisible.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    hostVisible.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    hostVisible.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo depHost{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depHost.memoryBarrierCount = 1;
    depHost.pMemoryBarriers = &hostVisible;
    vkCmdPipelineBarrier2(cmd, &depHost);
}

void FrameGenerator::commitCurrentThumb() {
    if (!thumbMapped_) return;
    std::memcpy(lastUniqueThumb_.data(), thumbMapped_, static_cast<size_t>(kThumbBytes));
    hasLastUniqueThumb_ = true;
}

bool FrameGenerator::isCurrentThumbDuplicate() const {
    if (!dedupEnabled_ || !hasLastUniqueThumb_ || !thumbMapped_) {
        return false;
    }

    uint64_t interiorSum = 0;
    uint32_t interiorNonZero = 0;
    int interiorMax = 0;
    uint32_t borderNonZero = 0;
    int borderMax = 0;

    for (uint32_t y = 0; y < kThumbH; ++y) {
        const bool inInteriorY = (y >= 2 && y < 32);
        for (uint32_t x = 0; x < kThumbW; ++x) {
            const size_t idx = (static_cast<size_t>(y) * kThumbW + x) * 4u;
            const int d0 = std::abs(static_cast<int>(thumbMapped_[idx + 0]) -
                                    static_cast<int>(lastUniqueThumb_[idx + 0]));
            const int d1 = std::abs(static_cast<int>(thumbMapped_[idx + 1]) -
                                    static_cast<int>(lastUniqueThumb_[idx + 1]));
            const int d2 = std::abs(static_cast<int>(thumbMapped_[idx + 2]) -
                                    static_cast<int>(lastUniqueThumb_[idx + 2]));
            const int d = std::max({d0, d1, d2});

            if (inInteriorY && x >= 2 && x < 62) {
                interiorSum += static_cast<uint64_t>(d);
                interiorMax = std::max(interiorMax, d);
                if (d >= 1) ++interiorNonZero;
            } else {
                borderMax = std::max(borderMax, d);
                if (d >= 1) ++borderNonZero;
            }
        }
    }

    return (interiorNonZero <= 3 && interiorSum <= 4 && interiorMax <= 2) &&
           (borderNonZero <= 6 && borderMax <= 16);
}

void FrameGenerator::recordUniqueFrameTiming(std::chrono::steady_clock::time_point arrivalTime,
                                             double fallbackPeriodMs, double fallbackJitterMs,
                                             double& outPeriodMs, double& outJitterMs) {
    if (hasLastUniqueArrival_) {
        const double deltaMs =
            std::chrono::duration<double, std::milli>(arrivalTime - lastUniqueArrival_).count();
        if (deltaMs >= 2.5 && deltaMs <= 160.0) {
            if (hasSmoothedUniquePeriod_ &&
                std::abs(deltaMs - smoothedUniquePeriodMs_) > smoothedUniquePeriodMs_ * 0.25) {
                if (++uniqueRegimeShiftCount_ >= 4) {
                    uniqueDeltaCount_ = 0;
                    uniqueDeltaHead_ = 0;
                    uniqueRegimeShiftCount_ = 0;
                }
            } else {
                uniqueRegimeShiftCount_ = 0;
            }

            uniqueDeltas_[uniqueDeltaHead_] = deltaMs;
            uniqueDeltaHead_ = (uniqueDeltaHead_ + 1) % uniqueDeltas_.size();
            if (uniqueDeltaCount_ < uniqueDeltas_.size()) ++uniqueDeltaCount_;

            std::array<double, 16> sorted{};
            const size_t n = std::min(uniqueDeltaCount_, sorted.size());
            for (size_t i = 0; i < n; ++i) sorted[i] = uniqueDeltas_[i];
            for (size_t i = 1; i < n; ++i) {
                const double key = sorted[i];
                size_t j = i;
                while (j > 0 && sorted[j - 1] > key) {
                    sorted[j] = sorted[j - 1];
                    --j;
                }
                sorted[j] = key;
            }

            const size_t trim = n / 5;
            const size_t lo = trim;
            const size_t hi = (n > trim * 2) ? (n - trim) : n;
            double sum = 0.0;
            for (size_t i = lo; i < hi; ++i) sum += sorted[i];
            const double trimmedMean = sum / static_cast<double>(std::max<size_t>(1, hi - lo));

            const size_t p90Idx = (n > 1) ? ((n - 1) * 9 / 10) : 0;
            const double upperSpread = std::max(0.0, sorted[p90Idx] - trimmedMean);

            if (!hasSmoothedUniquePeriod_ || n <= 4) {
                smoothedUniquePeriodMs_ = trimmedMean;
                smoothedUniqueJitterMs_ = upperSpread;
                hasSmoothedUniquePeriod_ = true;
            } else {
                smoothedUniquePeriodMs_ = smoothedUniquePeriodMs_ * 0.65 + trimmedMean * 0.35;
                smoothedUniqueJitterMs_ = smoothedUniqueJitterMs_ * 0.75 + upperSpread * 0.25;
            }
        }
    }

    lastUniqueArrival_ = arrivalTime;
    hasLastUniqueArrival_ = true;

    outPeriodMs = hasSmoothedUniquePeriod_
                      ? std::clamp(smoothedUniquePeriodMs_, 2.5, 160.0)
                      : std::clamp(fallbackPeriodMs, 2.5, 160.0);
    outJitterMs = hasSmoothedUniquePeriod_
                      ? std::clamp(smoothedUniqueJitterMs_, 0.0, 40.0)
                      : std::clamp(fallbackJitterMs, 0.0, 40.0);
}

bool FrameGenerator::startCaptureSession(bool autoSelectActiveWindow) {
    stopCaptureSession();
    try {
        portal_ = std::make_unique<capture::PortalSession>();
        const bool monitor = options_.monitorCapture;
        if (autoSelectActiveWindow && !monitor) {
            std::fprintf(stderr, "[lfg] auto-selecting active top window for capture...\n");
        } else {
            std::fprintf(stderr, "[lfg] asking the desktop to share a %s — pick one in the dialog...\n",
                         monitor ? "monitor" : "window");
        }
        currentTarget_ = portal_->requestCapture(
            monitor ? capture::CaptureSource::Monitor : capture::CaptureSource::Window,
            autoSelectActiveWindow);
        std::fprintf(stderr, "[lfg] portal gave PipeWire node %u\n", currentTarget_.nodeId);

        capture_ = std::make_unique<capture::PipeWireCapture>(*importer_);
        bool linearOnly = vk_->isCrossGpuLikely() || !vk_->dmaBufSupported() || forceLinearCapture_;
        if (linearOnly) forceLinearCapture_ = true;
        directImportFailed_ = false;
        capture_->start(currentTarget_, linearOnly, forceShmCapture_);
        sessionActive_ = true;
        newestSlot_ = -1;
        lsfgFrameIndex_ = 0;
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[lfg] capture selection cancelled or failed: %s\n", e.what());
        if (controlPanel_) controlPanel_->setStatusNotice("CAPTURE FAILED - SELECT TARGET WINDOW");
        stopCaptureSession();
        return false;
    }
}

void FrameGenerator::clearPendingUniqueFrame() {
    pendingUniqueValid_ = false;
    pendingUniqueCount_ = 0;
    pendingUniquePeriodMs_ = 16.6667;
    pendingUniqueJitterMs_ = 0.0;
    pendingUniqueArrival_ = {};
    pendingUniqueStart_ = {};
}

void FrameGenerator::clearPrefetchedFrame() {
    prefetchedValid_ = false;
    prefetchedSlot_ = -1;
    prefetchedFidx_ = 0;
    prefetchedJitterMs_ = 0.0;
    prefetchedArrival_ = {};
    prefetchedStart_ = {};
}

void FrameGenerator::resetPacerState(bool keepTimingEma) {
    clearPendingUniqueFrame();
    nextPresentTime_ = {};
    lastActualPresent_ = {};
    lastStepMs_ = 0.0;
    adaptivePhaseMs_ = 0.0;
    paceMinMs_ = 1e9;
    paceMaxMs_ = 0.0;
    paceSumMs_ = 0.0;
    paceCount_ = 0;
    hasLastUniqueThumb_ = false;
    hasLastUniqueArrival_ = false;
    lastUniqueArrival_ = {};
    uniqueDeltaCount_ = 0;
    uniqueDeltaHead_ = 0;
    uniqueRegimeShiftCount_ = 0;
    hasSmoothedUniquePeriod_ = false;
    smoothedUniquePeriodMs_ = 16.6667;
    smoothedUniqueJitterMs_ = 0.0;
    dupsDroppedInWindow_ = 0;
    if (!keepTimingEma) {
        firstPresentEmaMs_ = 0.0;
        prepassEmaMs_ = 0.0;
        genPassEmaMs_ = 0.0;
    }
}

void FrameGenerator::stopCaptureSession() {
    if (vk_) vk_->waitOneShotAsync();
    clearPrefetchedFrame();
    if (capture_) {
        capture_->stop();
        capture_.reset();
    }
    if (portal_) {
        portal_->close();
        portal_.reset();
    }
    sessionActive_ = false;
    newestSlot_ = -1;
    lsfgFrameIndex_ = 0;
    resetPacerState(false);
    lastPresentedOutputSlot_ = -1;
    adaptiveGpuCapMult_ = static_cast<double>(kMaxAdaptiveMultiplier);
    ++resourceEpoch_;
}

void FrameGenerator::handleControlAction(present::ControlAction action) {
    if (action == present::ControlAction::None) return;

    if (action == present::ControlAction::Toggle) {
        action = generating_ ? present::ControlAction::Stop : present::ControlAction::Start;
    }

    switch (action) {
        case present::ControlAction::Start: {
            if (!ensureLsfgAvailable()) {
                generating_ = false;
                if (controlPanel_) controlPanel_->setGenerating(false);
                break;
            }
            if (!sessionActive_) {
                if (!startCaptureSession(true)) {
                    generating_ = false;
                    if (controlPanel_) controlPanel_->setGenerating(false);
                    break;
                }
            }
            if (vk_) vk_->waitOneShotAsync();
            clearPrefetchedFrame();
            resetPacerState(false);
            generating_ = true;
            newestSlot_ = -1;
            lsfgFrameIndex_ = 0;
            effectiveMultiplier_ = 0;
            adaptiveGpuCapMult_ = static_cast<double>(kMaxAdaptiveMultiplier);
            lastPresentedOutputSlot_ = -1;
            needsTargetAlign_ = true;
            // Do not map the overlay yet: it is mapped once the first captured frame is ready to be
            // presented, so a slow/failed capture never covers the game with a black window.
            if (controlPanel_) controlPanel_->setGenerating(true);
            if (options_.adaptive) {
                std::fprintf(stderr, "[lfg] >>> Frame Generation STARTED (Adaptive -> %d FPS on %s) <<<\n",
                             options_.targetFps, vk_->selectedGpuInfo().shortLabel.c_str());
            } else {
                std::fprintf(stderr, "[lfg] >>> Frame Generation STARTED (%dx mode on %s) <<<\n",
                             options_.multiplier, vk_->selectedGpuInfo().shortLabel.c_str());
            }
            break;
        }
        case present::ControlAction::Stop: {
            generating_ = false;
            newestSlot_ = -1;
            clearPrefetchedFrame();
            resetPacerState(false);
            if (overlay_) overlay_->hide();
            stopCaptureSession();
            if (controlPanel_) controlPanel_->setGenerating(false);
            std::fprintf(stderr, "[lfg] >>> Frame Generation STOPPED (Streaming ended) <<<\n");
            break;
        }
        case present::ControlAction::SetMode2X: {
            options_.adaptive = false;
            options_.multiplier = 2;
            effectiveMultiplier_ = 0;
            adaptivePhaseMs_ = 0.0;
            adaptiveGpuCapMult_ = static_cast<double>(kMaxAdaptiveMultiplier);
            if (controlPanel_) {
                controlPanel_->setMode(false, 2);
                controlPanel_->setStatusNotice("");
            }
            saveSettings(options_);
            std::fprintf(stderr, "[lfg] >>> Mode switched to Fixed 2X <<<\n");
            break;
        }
        case present::ControlAction::SetMode3X: {
            options_.adaptive = false;
            options_.multiplier = 3;
            effectiveMultiplier_ = 0;
            adaptivePhaseMs_ = 0.0;
            adaptiveGpuCapMult_ = static_cast<double>(kMaxAdaptiveMultiplier);
            if (controlPanel_) {
                controlPanel_->setMode(false, 3);
                controlPanel_->setStatusNotice("");
            }
            saveSettings(options_);
            std::fprintf(stderr, "[lfg] >>> Mode switched to Fixed 3X <<<\n");
            break;
        }
        case present::ControlAction::SetMode4X: {
            options_.adaptive = false;
            options_.multiplier = 4;
            effectiveMultiplier_ = 0;
            adaptivePhaseMs_ = 0.0;
            adaptiveGpuCapMult_ = static_cast<double>(kMaxAdaptiveMultiplier);
            if (controlPanel_) {
                controlPanel_->setMode(false, 4);
                controlPanel_->setStatusNotice("");
            }
            saveSettings(options_);
            std::fprintf(stderr, "[lfg] >>> Mode switched to Fixed 4X <<<\n");
            break;
        }
        case present::ControlAction::SetModeAdaptive: {
            options_.adaptive = true;
            effectiveMultiplier_ = 0;
            adaptivePhaseMs_ = 0.0;
            adaptiveGpuCapMult_ = static_cast<double>(kMaxAdaptiveMultiplier);
            if (controlPanel_) {
                controlPanel_->setMode(true, options_.multiplier);
                controlPanel_->setStatusNotice("");
            }
            saveSettings(options_);
            std::fprintf(stderr, "[lfg] >>> Mode switched to ADAPTIVE (Target: %d FPS) <<<\n",
                         options_.targetFps);
            break;
        }
        case present::ControlAction::CycleMode: {
            // Cycle: 2X -> 3X -> 4X -> ADAPTIVE -> 2X
            if (!options_.adaptive && options_.multiplier == 2) {
                options_.multiplier = 3;
            } else if (!options_.adaptive && options_.multiplier == 3) {
                options_.multiplier = 4;
            } else if (!options_.adaptive && options_.multiplier >= 4) {
                options_.adaptive = true;
            } else {
                options_.adaptive = false;
                options_.multiplier = 2;
            }
            effectiveMultiplier_ = 0;
            adaptivePhaseMs_ = 0.0;
            adaptiveGpuCapMult_ = static_cast<double>(kMaxAdaptiveMultiplier);
            if (controlPanel_) {
                controlPanel_->setMode(options_.adaptive, options_.multiplier);
                controlPanel_->setStatusNotice("");
            }
            saveSettings(options_);
            if (options_.adaptive) {
                std::fprintf(stderr, "[lfg] >>> Mode switched to ADAPTIVE (Target: %d FPS) <<<\n",
                             options_.targetFps);
            } else {
                std::fprintf(stderr, "[lfg] >>> Mode switched to Fixed %dX <<<\n", options_.multiplier);
            }
            break;
        }
        case present::ControlAction::TargetFpsDown: {
            options_.adaptive = true;
            targetFpsAutoInit_ = false;
            int step = (options_.targetFps > 240) ? 20 : 10;
            options_.targetFps = std::clamp(options_.targetFps - step, 30, kMaxTargetFps);
            adaptivePhaseMs_ = 0.0;
            adaptiveGpuCapMult_ = static_cast<double>(kMaxAdaptiveMultiplier);
            if (controlPanel_) {
                controlPanel_->setMode(true, options_.multiplier);
                controlPanel_->setTargetFps(options_.targetFps);
            }
            saveSettings(options_);
            std::fprintf(stderr, "[lfg] >>> Adaptive Target FPS set to %d <<<\n", options_.targetFps);
            break;
        }
        case present::ControlAction::TargetFpsUp: {
            options_.adaptive = true;
            targetFpsAutoInit_ = false;
            int step = (options_.targetFps >= 240) ? 20 : 10;
            options_.targetFps = std::clamp(options_.targetFps + step, 30, kMaxTargetFps);
            adaptivePhaseMs_ = 0.0;
            adaptiveGpuCapMult_ = static_cast<double>(kMaxAdaptiveMultiplier);
            if (controlPanel_) {
                controlPanel_->setMode(true, options_.multiplier);
                controlPanel_->setTargetFps(options_.targetFps);
            }
            saveSettings(options_);
            std::fprintf(stderr, "[lfg] >>> Adaptive Target FPS set to %d <<<\n", options_.targetFps);
            break;
        }
        case present::ControlAction::CycleTargetFps: {
            options_.adaptive = true;
            targetFpsAutoInit_ = false;
            std::vector<int> presets = {60, 90, 120, 144, 165, 180, 240, 300, 360, 480, 500, 600, 720, 1000};
            if (overlay_ && overlay_->refreshRateHz() > 30.0) {
                int monHz = static_cast<int>(std::lround(overlay_->refreshRateHz()));
                if (std::find(presets.begin(), presets.end(), monHz) == presets.end()) {
                    presets.push_back(monHz);
                    std::sort(presets.begin(), presets.end());
                }
            }
            int nextTarget = presets[0];
            for (int p : presets) {
                if (p > options_.targetFps) {
                    nextTarget = p;
                    break;
                }
            }
            options_.targetFps = nextTarget;
            adaptivePhaseMs_ = 0.0;
            adaptiveGpuCapMult_ = static_cast<double>(kMaxAdaptiveMultiplier);
            if (controlPanel_) {
                controlPanel_->setMode(true, options_.multiplier);
                controlPanel_->setTargetFps(options_.targetFps);
            }
            saveSettings(options_);
            std::fprintf(stderr, "[lfg] >>> Adaptive Target FPS set to %d <<<\n", options_.targetFps);
            break;
        }
        case present::ControlAction::FlowScaleDown: {
            options_.flowScale = std::clamp(((options_.flowScale - 5) / 5) * 5, 25, 100);
            if (controlPanel_) controlPanel_->setFlowScale(options_.flowScale);
            configureLsfgResources();
            saveSettings(options_);
            std::fprintf(stderr, "[lfg] >>> Flow Scale set to %d%% <<<\n", options_.flowScale);
            break;
        }
        case present::ControlAction::FlowScaleUp: {
            options_.flowScale = std::clamp(((options_.flowScale + 5) / 5) * 5, 25, 100);
            if (controlPanel_) controlPanel_->setFlowScale(options_.flowScale);
            configureLsfgResources();
            saveSettings(options_);
            std::fprintf(stderr, "[lfg] >>> Flow Scale set to %d%% <<<\n", options_.flowScale);
            break;
        }
        case present::ControlAction::FlowScaleChanged: {
            if (controlPanel_) {
                options_.flowScale = std::clamp(controlPanel_->flowScale(), 25, 100);
            }
            configureLsfgResources();
            saveSettings(options_);
            std::fprintf(stderr, "[lfg] >>> Flow Scale set to %d%% <<<\n", options_.flowScale);
            break;
        }
        case present::ControlAction::TogglePerformanceMode: {
            options_.performanceMode = !options_.performanceMode;
            if (controlPanel_) controlPanel_->setPerformanceMode(options_.performanceMode);
            configureLsfgResources();
            saveSettings(options_);
            std::fprintf(stderr, "[lfg] >>> LSFG Performance Mode: %s <<<\n",
                         options_.performanceMode ? "ON (Performance)" : "OFF (Quality)");
            break;
        }
        case present::ControlAction::CycleGpu: {
            if (!vk_ || vk_->availableGpus().size() <= 1) {
                if (controlPanel_) controlPanel_->setStatusNotice("ONLY 1 VULKAN GPU DETECTED");
                break;
            }
            int nextGpu = static_cast<int>((vk_->selectedGpuIndex() + 1) % vk_->availableGpus().size());
            if (switchGpu(nextGpu)) {
                saveSettings(options_);
            }
            break;
        }
        case present::ControlAction::SelectWindow: {
            if (!ensureLsfgAvailable()) {
                generating_ = false;
                if (controlPanel_) controlPanel_->setGenerating(false);
                break;
            }
            if (overlay_) overlay_->hide();
            generating_ = false;
            if (controlPanel_) controlPanel_->setGenerating(false);
            if (startCaptureSession(false)) {
                generating_ = true;
                needsTargetAlign_ = true;
                if (overlay_) overlay_->show();
                if (controlPanel_) controlPanel_->setGenerating(true);
                std::fprintf(stderr, "[lfg] >>> Switched target window & STARTED <<<\n");
            }
            break;
        }
        case present::ControlAction::CycleDisplay: {
            int maxMonitors = overlay_ ? static_cast<int>(overlay_->monitorCount()) : 2;
            if (maxMonitors < 1) maxMonitors = 2;
            options_.displayIndex++;
            if (options_.displayIndex >= maxMonitors) options_.displayIndex = -1;
            if (overlay_) {
                present::TargetRect req{currentTarget_.x, currentTarget_.y, frameWidth_, frameHeight_};
                overlay_->setPreferredDisplay(options_.displayIndex, req);
                overlay_->alignWindowedTarget(frameWidth_, frameHeight_);
                if (targetFpsAutoInit_ && overlay_->refreshRateHz() > 30.0) {
                    options_.targetFps = std::clamp(static_cast<int>(std::lround(overlay_->refreshRateHz())), 30, kMaxTargetFps);
                    if (controlPanel_) controlPanel_->setTargetFps(options_.targetFps);
                }
            }
            if (controlPanel_) controlPanel_->setDisplayMode(options_.displayIndex);
            saveSettings(options_);
            break;
        }
        case present::ControlAction::ToggleFps: {
            options_.showFps = !options_.showFps;
            if (overlay_) overlay_->setShowFps(options_.showFps);
            if (controlPanel_) controlPanel_->setShowFps(options_.showFps);
            saveSettings(options_);
            break;
        }
        case present::ControlAction::ToggleVsync: {
            options_.vsync = !options_.vsync;
            adaptiveGpuCapMult_ = static_cast<double>(kMaxAdaptiveMultiplier);
            if (overlay_) {
                if (vk_) vk_->waitOneShotAsync();
                clearPrefetchedFrame();
                resetPacerState(true);
                lastPresentedOutputSlot_ = -1;
                ++resourceEpoch_;
                overlay_->setVsync(options_.vsync);
            }
            if (controlPanel_) controlPanel_->setVsync(options_.vsync);
            saveSettings(options_);
            std::fprintf(stderr, "[lfg] >>> VSync: %s <<<\n",
                         options_.vsync ? "ON (FIFO - monitor Hz cap)"
                                        : "OFF (IMMEDIATE/MAILBOX - uncapped FPS)");
            break;
        }
        case present::ControlAction::DllPathChanged: {
            std::string manualInput = controlPanel_ ? controlPanel_->dllPath() : options_.dllPath;
            auto resolved = compute::LsfgPipeline::resolveExplicitDllPath(manualInput);
            if (resolved.empty()) {
                std::fprintf(stderr,
                             "[lfg] Lossless.dll not found at user-specified location: '%s'\n",
                             manualInput.c_str());
                if (controlPanel_) {
                    controlPanel_->setStatusNotice("LOSSLESS.DLL NOT FOUND AT THAT LOCATION");
                }
                break;
            }
            options_.dllPath = resolved.string();
            if (vk_) {
                vk_->waitOneShotAsync();
                clearPrefetchedFrame();
                resetPacerState(false);
                vkDeviceWaitIdle(vk_->device());
                lastPresentedOutputSlot_ = -1;
                ++resourceEpoch_;
                lsfg_ = std::make_unique<compute::LsfgPipeline>(*vk_, options_.dllPath);
            }
            if (lsfg_ && lsfg_->isAvailable()) {
                options_.dllPath = lsfg_->dllPath();
                saveSettings(options_);
                configureLsfgResources();
                if (controlPanel_) {
                    controlPanel_->setDllPath(options_.dllPath);
                    controlPanel_->setLsfgActive(true);
                    controlPanel_->setStatusNotice("LOSSLESS SCALING FILES LOADED!");
                }
                std::fprintf(stderr, "[lfg] >>> Loaded Lossless Scaling files from: %s <<<\n",
                             options_.dllPath.c_str());
            } else {
                if (controlPanel_) {
                    controlPanel_->setLsfgActive(false);
                    controlPanel_->setStatusNotice("INVALID LOSSLESS.DLL - FAILED TO LOAD");
                }
            }
            break;
        }
        case present::ControlAction::AutoDetectDllPath: {
            auto found = compute::LsfgPipeline::findLosslessDll("");
            if (found.empty()) {
                std::fprintf(stderr, "[lfg] Auto-detect could not find Lossless Scaling in Steam libraries.\n");
                if (controlPanel_) {
                    controlPanel_->setStatusNotice("NOT FOUND IN STEAM - BROWSE OR TYPE PATH");
                }
                break;
            }
            options_.dllPath = found.string();
            if (vk_) {
                vk_->waitOneShotAsync();
                clearPrefetchedFrame();
                resetPacerState(false);
                vkDeviceWaitIdle(vk_->device());
                lastPresentedOutputSlot_ = -1;
                ++resourceEpoch_;
                lsfg_ = std::make_unique<compute::LsfgPipeline>(*vk_, options_.dllPath);
            }
            if (lsfg_ && lsfg_->isAvailable()) {
                options_.dllPath = lsfg_->dllPath();
                saveSettings(options_);
                configureLsfgResources();
                if (controlPanel_) {
                    controlPanel_->setDllPath(options_.dllPath);
                    controlPanel_->setLsfgActive(true);
                    controlPanel_->setStatusNotice("STEAM LOSSLESS SCALING FOUND & LOADED!");
                }
                std::fprintf(stderr, "[lfg] >>> Auto-detected Lossless Scaling in Steam: %s <<<\n",
                             options_.dllPath.c_str());
            } else {
                if (controlPanel_) {
                    controlPanel_->setLsfgActive(false);
                    controlPanel_->setStatusNotice("FAILED TO LOAD LOSSLESS.DLL SHADERS");
                }
            }
            break;
        }
        case present::ControlAction::Quit: {
            running_ = false;
            break;
        }
        default:
            break;
    }
}

bool FrameGenerator::ensureResources(uint32_t width, uint32_t height, const capture::CaptureTarget& target) {
    if (width == frameWidth_ && height == frameHeight_ &&
        slots_[0].image != VK_NULL_HANDLE &&
        overlay_ != nullptr &&
        lsfg_ && lsfg_->isConfigured()) {
        return false;
    }

    vk_->waitOneShotAsync();
    clearPrefetchedFrame();
    vkDeviceWaitIdle(vk_->device());
    resetPacerState(true);
    lastPresentedOutputSlot_ = -1;
    adaptiveGpuCapMult_ = static_cast<double>(kMaxAdaptiveMultiplier);
    ++resourceEpoch_;
    destroySlots();

    try {
        for (auto& slot : slots_) {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = VK_FORMAT_B8G8R8A8_UNORM;
            info.extent = {width, height, 1};
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VK_CHECK(vkCreateImage(vk_->device(), &info, nullptr, &slot.image));

            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(vk_->device(), slot.image, &req);
            slot.memory = vk_->allocateMemory(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VK_CHECK(vkBindImageMemory(vk_->device(), slot.image, slot.memory, 0));

            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = slot.image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VK_CHECK(vkCreateImageView(vk_->device(), &viewInfo, nullptr, &slot.view));
        }

        {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = VK_FORMAT_B8G8R8A8_UNORM;
            info.extent = {width, height, 1};
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VK_CHECK(vkCreateImage(vk_->device(), &info, nullptr, &pendingSlot_.image));

            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(vk_->device(), pendingSlot_.image, &req);
            pendingSlot_.memory = vk_->allocateMemory(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VK_CHECK(vkBindImageMemory(vk_->device(), pendingSlot_.image, pendingSlot_.memory, 0));
        }

        // Transition slots and pending staging image to VK_IMAGE_LAYOUT_GENERAL upfront so LSFG descriptor bindings are valid immediately
        {
            VkCommandBuffer cmd = vk_->beginOneShotCommands(vk_->graphicsCommandPool());
            VkImageMemoryBarrier barriers[3]{};
            VkImage imgs[3] = {slots_[0].image, slots_[1].image, pendingSlot_.image};
            for (int i = 0; i < 3; ++i) {
                barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barriers[i].srcAccessMask = 0;
                barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
                barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barriers[i].image = imgs[i];
                barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            }
            vkCmdPipelineBarrier(cmd,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 3, barriers);
            vk_->endOneShotCommands(vk_->graphicsCommandPool(), vk_->graphicsPresentQueue(), cmd);
        }

        frameWidth_ = width;
        frameHeight_ = height;

        ensureThumbResources();
        configureLsfgResources();

        if (!overlay_) {
            present::TargetRect requestedRect{target.x, target.y, width, height};
            overlay_ = std::make_unique<present::OverlayWindow>(
                *vk_, requestedRect, options_.displayIndex, options_.showFps, options_.vsync);
            if (targetFpsAutoInit_ && overlay_->refreshRateHz() > 30.0) {
                options_.targetFps = std::clamp(static_cast<int>(std::lround(overlay_->refreshRateHz())), 30, kMaxTargetFps);
                if (controlPanel_) controlPanel_->setTargetFps(options_.targetFps);
                saveSettings(options_);
            }
        } else {
            present::TargetRect requestedRect{target.x, target.y, width, height};
            overlay_->setPreferredDisplay(options_.displayIndex, requestedRect);
        }

        overlay_->alignWindowedTarget(width, height);
        needsTargetAlign_ = false;
        if (!generating_ && overlay_) overlay_->hide();
        return true;
    } catch (...) {
        destroySlots();
        throw;
    }
}

void FrameGenerator::configureLsfgResources() {
    if (!vk_ || !lsfg_ || !lsfg_->isAvailable() ||
        frameWidth_ == 0 || frameHeight_ == 0 || slots_[0].view == VK_NULL_HANDLE) {
        return;
    }
    options_.flowScale = std::clamp(options_.flowScale, 25, 100);
    vk_->waitOneShotAsync();
    clearPrefetchedFrame();
    vkDeviceWaitIdle(vk_->device());
    resetPacerState(true);
    lastPresentedOutputSlot_ = -1;
    adaptiveGpuCapMult_ = static_cast<double>(kMaxAdaptiveMultiplier);
    ++resourceEpoch_;
    try {
        lsfg_->configure(frameWidth_, frameHeight_, options_.flowScale, options_.performanceMode,
                         slots_[0].view, slots_[1].view);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[lsfg] warning: pipeline allocation failed (%s) — retrying in Performance Mode (50%% flow)...\n",
                     e.what());
        options_.performanceMode = true;
        options_.flowScale = std::min(options_.flowScale, 50);
        if (controlPanel_) {
            controlPanel_->setPerformanceMode(options_.performanceMode);
            controlPanel_->setFlowScale(options_.flowScale);
        }
        lsfg_->configure(frameWidth_, frameHeight_, options_.flowScale, options_.performanceMode,
                         slots_[0].view, slots_[1].view);
    }
    newestSlot_ = -1;
    lsfgFrameIndex_ = 0;
    std::fprintf(stderr,
                 "[lsfg] Configured LSFG 3.1 pipeline: %ux%u | Flow Scale %d%% | %s Mode\n",
                 frameWidth_, frameHeight_, options_.flowScale,
                 options_.performanceMode ? "Performance" : "Quality");
}

namespace {

void recordImageCopyToSlot(VkCommandBuffer cmd, VkImage srcImage, VkImage dstImage,
                           uint32_t width, uint32_t height) {
    VkImageMemoryBarrier2 preCopy[2]{};
    preCopy[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    preCopy[0].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    preCopy[0].srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    preCopy[0].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    preCopy[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    preCopy[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    preCopy[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    preCopy[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preCopy[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preCopy[0].image = srcImage;
    preCopy[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    preCopy[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    preCopy[1].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    preCopy[1].srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    preCopy[1].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    preCopy[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    preCopy[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    preCopy[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    preCopy[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preCopy[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preCopy[1].image = dstImage;
    preCopy[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo depPre{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depPre.imageMemoryBarrierCount = 2;
    depPre.pImageMemoryBarriers = preCopy;
    vkCmdPipelineBarrier2(cmd, &depPre);

    VkImageCopy imgCopy{};
    imgCopy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    imgCopy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    imgCopy.extent = {width, height, 1};
    vkCmdCopyImage(cmd,
                   srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &imgCopy);

    VkImageMemoryBarrier2 postCopy[2]{};
    postCopy[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    postCopy[0].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    postCopy[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    postCopy[0].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    postCopy[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    postCopy[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    postCopy[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    postCopy[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    postCopy[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    postCopy[0].image = srcImage;
    postCopy[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    postCopy[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    postCopy[1].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    postCopy[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    postCopy[1].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    postCopy[1].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    postCopy[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    postCopy[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    postCopy[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    postCopy[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    postCopy[1].image = dstImage;
    postCopy[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo depPost{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depPost.imageMemoryBarrierCount = 2;
    depPost.pImageMemoryBarriers = postCopy;
    vkCmdPipelineBarrier2(cmd, &depPost);
}

void syncDmaBufRead(int fd, bool start) {
    if (fd < 0) return;
    if (start) {
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        ::poll(&pfd, 1, 8);
    }
    struct dma_buf_sync sync{};
    sync.flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_READ;
    ::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

}  // namespace

void FrameGenerator::uploadViaStagingBuffer(const capture::CaptureBuffer& buffer, FrameSlot& slot) {
    if (buffer.width == 0 || buffer.height == 0 || buffer.planes.empty()) {
        throw std::runtime_error("uploadViaStagingBuffer: invalid buffer dimensions or planes");
    }
    if (buffer.hasExplicitModifier && buffer.modifier != 0) {
        throw std::runtime_error("uploadViaStagingBuffer: cannot CPU-map non-linear DRM modifier");
    }

    const uint32_t w = buffer.width;
    const uint32_t h = buffer.height;
    const uint32_t srcStride = buffer.planes[0].stride;
    const uint32_t srcOffset = buffer.planes[0].offset;
    const uint32_t dstRowBytes = w * 4u;
    const VkDeviceSize totalDstBytes = static_cast<VkDeviceSize>(dstRowBytes) * h;

    if (srcStride < dstRowBytes) {
        throw std::runtime_error("uploadViaStagingBuffer: invalid source stride");
    }

    const uint8_t* mappedBase = nullptr;
    size_t mappedLen = 0;
    bool didMmap = false;

    if (buffer.mappedData != nullptr) {
        mappedBase = static_cast<const uint8_t*>(buffer.mappedData);
    } else {
        int fd = buffer.planes[0].fd;
        if (fd < 0) throw std::runtime_error("uploadViaStagingBuffer: invalid fd");
        size_t minNeeded = static_cast<size_t>(srcOffset) + static_cast<size_t>(srcStride) * (h - 1) + dstRowBytes;
        off_t fdEnd = ::lseek(fd, 0, SEEK_END);
        if (fdEnd > 0 && static_cast<size_t>(fdEnd) >= minNeeded) {
            mappedLen = static_cast<size_t>(fdEnd);
        } else {
            mappedLen = std::max<size_t>(minNeeded, buffer.dataSize);
        }
        void* ptr = ::mmap(nullptr, mappedLen, PROT_READ, MAP_SHARED, fd, 0);
        if ((ptr == MAP_FAILED || !ptr) && mappedLen > minNeeded) {
            mappedLen = minNeeded;
            ptr = ::mmap(nullptr, mappedLen, PROT_READ, MAP_SHARED, fd, 0);
        }
        if (ptr == MAP_FAILED || !ptr) {
            throw std::runtime_error("uploadViaStagingBuffer: mmap failed on capture buffer");
        }
        mappedBase = static_cast<const uint8_t*>(ptr);
        didMmap = true;
    }

    try {
        ensureStagingBuffer(totalDstBytes);
        const uint8_t* srcRow = mappedBase + srcOffset;
        uint8_t* dstRow = static_cast<uint8_t*>(stagingMapped_);
        const bool swizzleRgbaToBgra = (buffer.format == VK_FORMAT_R8G8B8A8_UNORM);

        if (!swizzleRgbaToBgra && srcStride == dstRowBytes) {
            std::memcpy(dstRow, srcRow, static_cast<size_t>(totalDstBytes));
        } else if (!swizzleRgbaToBgra) {
            for (uint32_t y = 0; y < h; ++y) {
                std::memcpy(dstRow + static_cast<size_t>(y) * dstRowBytes,
                            srcRow + static_cast<size_t>(y) * srcStride,
                            dstRowBytes);
            }
        } else {
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t* s = srcRow + static_cast<size_t>(y) * srcStride;
                uint8_t* d = dstRow + static_cast<size_t>(y) * dstRowBytes;
                for (uint32_t x = 0; x < w; ++x) {
                    d[x * 4 + 0] = s[x * 4 + 2];
                    d[x * 4 + 1] = s[x * 4 + 1];
                    d[x * 4 + 2] = s[x * 4 + 0];
                    d[x * 4 + 3] = 255;
                }
            }
        }
    } catch (...) {
        if (didMmap) ::munmap(const_cast<uint8_t*>(mappedBase), mappedLen);
        throw;
    }
    if (didMmap) ::munmap(const_cast<uint8_t*>(mappedBase), mappedLen);

    VkCommandBuffer cmd = vk_->beginOneShotCommands(vk_->graphicsCommandPool());

    VkImageMemoryBarrier2 toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toDst.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    toDst.srcAccessMask = VK_ACCESS_2_NONE;
    toDst.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = slot.image;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep1.imageMemoryBarrierCount = 1;
    dep1.pImageMemoryBarriers = &toDst;
    vkCmdPipelineBarrier2(cmd, &dep1);

    VkBufferImageCopy copyRegion{};
    copyRegion.bufferOffset = 0;
    copyRegion.bufferRowLength = w;
    copyRegion.bufferImageHeight = h;
    copyRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.imageOffset = {0, 0, 0};
    copyRegion.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuffer_, slot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

    recordThumbDownsample(cmd, slot.image, w, h);

    vk_->endOneShotCommands(vk_->graphicsCommandPool(), vk_->graphicsPresentQueue(), cmd);
}

void FrameGenerator::copyIntoSlot(const capture::CaptureBuffer& buffer, FrameSlot& slot) {
    const int primaryFd = buffer.planes.empty() ? -1 : buffer.planes[0].fd;
    if (buffer.isDmaBuf) {
        syncDmaBufRead(primaryFd, true);
    }

    const bool canTryDirectImport = buffer.isDmaBuf && vk_->dmaBufSupported() &&
                                    buffer.mappedData == nullptr && !directImportFailed_ &&
                                    (!vk_->isCrossGpuLikely() || !buffer.hasExplicitModifier || buffer.modifier == 0);
    if (canTryDirectImport) {
        try {
            capture::ImportedFrame imported = importer_->import(buffer.width, buffer.height, buffer.format,
                                                                buffer.hasExplicitModifier, buffer.modifier,
                                                                buffer.planes);
            try {
                VkCommandBuffer cmd = vk_->beginOneShotCommands(vk_->graphicsCommandPool());

                VkImageMemoryBarrier2 barriers[2]{};
                barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                barriers[0].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
                barriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barriers[0].srcQueueFamilyIndex = vk_->foreignQueueSupported()
                                                      ? VK_QUEUE_FAMILY_FOREIGN_EXT
                                                      : VK_QUEUE_FAMILY_IGNORED;
                barriers[0].dstQueueFamilyIndex = vk_->foreignQueueSupported()
                                                      ? vk_->queueFamilies().graphicsPresent
                                                      : VK_QUEUE_FAMILY_IGNORED;
                barriers[0].image = imported.image;
                barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

                barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                barriers[1].srcAccessMask = VK_ACCESS_2_NONE;
                barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
                barriers[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barriers[1].image = slot.image;
                barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

                VkDependencyInfo dep1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep1.imageMemoryBarrierCount = 2;
                dep1.pImageMemoryBarriers = barriers;
                vkCmdPipelineBarrier2(cmd, &dep1);

                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.srcOffsets[1] = {static_cast<int32_t>(buffer.width), static_cast<int32_t>(buffer.height), 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.dstOffsets[1] = {static_cast<int32_t>(buffer.width), static_cast<int32_t>(buffer.height), 1};
                vkCmdBlitImage(cmd, imported.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

                recordThumbDownsample(cmd, slot.image, buffer.width, buffer.height);

                vk_->endOneShotCommands(vk_->graphicsCommandPool(), vk_->graphicsPresentQueue(), cmd);
            } catch (...) {
                importer_->release(imported);
                if (buffer.isDmaBuf) syncDmaBufRead(primaryFd, false);
                throw;
            }
            importer_->release(imported);
            if (buffer.isDmaBuf) syncDmaBufRead(primaryFd, false);
            return;
        } catch (const std::exception& e) {
            directImportFailed_ = true;
            if (!loggedStagingFallback_) {
                loggedStagingFallback_ = true;
                std::fprintf(stderr,
                             "[lfg] direct DMA-BUF import unavailable on %s (%s) — using cross-GPU staging upload\n",
                             vk_->selectedGpuInfo().name.c_str(), e.what());
            }
        }
    }

    try {
        uploadViaStagingBuffer(buffer, slot);
    } catch (...) {
        if (buffer.isDmaBuf) syncDmaBufRead(primaryFd, false);
        throw;
    }
    if (buffer.isDmaBuf) syncDmaBufRead(primaryFd, false);
}

bool FrameGenerator::waitUntilTime(std::chrono::steady_clock::time_point targetTime,
                                   bool allowCaptureDrain, bool allowPrefetch,
                                   int currentSlot, uint64_t nextFidx) {
    using Clock = std::chrono::steady_clock;

    while (running_ && generating_) {
        auto now = Clock::now();
        if (now >= targetTime) break;

        auto remUs = std::chrono::duration_cast<std::chrono::microseconds>(targetTime - now).count();

        if (allowCaptureDrain && !prefetchedValid_ && remUs >= 1500 &&
            capture_ && capture_->hasPendingFrame() &&
            vk_ && vk_->pollOneShotAsyncComplete()) {
            pollAndClassifyPendingCapture();
            now = Clock::now();
            if (now >= targetTime) break;
            remUs = std::chrono::duration_cast<std::chrono::microseconds>(targetTime - now).count();
        }

        if (allowPrefetch && pendingUniqueValid_ && !prefetchedValid_ && remUs >= 1500 &&
            vk_ && vk_->pollOneShotAsyncComplete()) {
            promotePendingToPrefetch(currentSlot, nextFidx);
            now = Clock::now();
            if (now >= targetTime) break;
            remUs = std::chrono::duration_cast<std::chrono::microseconds>(targetTime - now).count();
        }

        if (remUs > 1500 &&
            (lastUiPollTime_.time_since_epoch().count() == 0 ||
             now - lastUiPollTime_ >= std::chrono::milliseconds(16))) {
            lastUiPollTime_ = now;
            int sigAct = pendingSignalAction_.exchange(0);
            if (sigAct == 1) handleControlAction(present::ControlAction::Start);
            else if (sigAct == 2) handleControlAction(present::ControlAction::Stop);
            else if (sigAct == 3) handleControlAction(present::ControlAction::Toggle);
            else if (sigAct == 4 && controlPanel_) controlPanel_->showFromTray();

            if (controlPanel_) {
                present::ControlAction act = controlPanel_->pollEvents();
                if (controlPanel_->consumeConfigChanged()) {
                    options_.panelWidth = controlPanel_->windowWidth();
                    options_.panelHeight = controlPanel_->windowHeight();
                    options_.toggleHotkey = controlPanel_->hotkeyLabel();
                    saveSettings(options_);
                }
                if (act != present::ControlAction::None) {
                    handleControlAction(act);
                }
            }
            if (!generating_ || !running_) return false;
            now = Clock::now();
            if (now >= targetTime) break;
            remUs = std::chrono::duration_cast<std::chrono::microseconds>(targetTime - now).count();
        }

        if (remUs > 700) {
            std::this_thread::sleep_for(std::chrono::microseconds(std::min<int64_t>(remUs - 500, 1500)));
        } else {
            // High-precision spin-yield for the final sub-millisecond window so frame presentation
            // hits exact VBlank / pacing intervals without OS scheduler sleep overshoot.
            std::this_thread::yield();
        }
    }
    return running_ && generating_;
}

bool FrameGenerator::pollAndClassifyPendingCapture() {
    if (!generating_ || !running_ || !capture_ || !vk_ || !lsfg_ ||
        !lsfg_->isAvailable() || !lsfg_->isConfigured()) {
        return false;
    }
    if (pendingSlot_.image == VK_NULL_HANDLE || vk_->isOneShotAsyncPending()) {
        return false;
    }

    capture::CaptureBuffer nextBuf;
    if (!capture_->tryTakeBuffer(nextBuf)) return false;

    if (!generating_ || !running_ || nextBuf.width != frameWidth_ || nextBuf.height != frameHeight_) {
        capture_->releaseBuffer(nextBuf);
        return false;
    }

    const auto pfStart = std::chrono::steady_clock::now();
    try {
        copyIntoSlot(nextBuf, pendingSlot_);
    } catch (...) {
        capture_->releaseBuffer(nextBuf);
        return false;
    }
    const double rawPeriodMs = nextBuf.producerPeriodMs;
    const double rawJitterMs = nextBuf.producerJitterMs;
    const auto arrival = (nextBuf.arrivalTime.time_since_epoch().count() != 0) ? nextBuf.arrivalTime : pfStart;
    capture_->releaseBuffer(nextBuf);

    if (isCurrentThumbDuplicate()) {
        ++dupsDroppedInWindow_;
        return false;
    }
    commitCurrentThumb();

    double periodMs = 16.6667;
    double jitterMs = 0.0;
    recordUniqueFrameTiming(arrival, rawPeriodMs, rawJitterMs, periodMs, jitterMs);

    pendingUniqueValid_ = true;
    ++pendingUniqueCount_;
    pendingUniquePeriodMs_ = periodMs;
    pendingUniqueJitterMs_ = jitterMs;
    pendingUniqueArrival_ = arrival;
    pendingUniqueStart_ = pfStart;
    return true;
}

bool FrameGenerator::promotePendingToPrefetch(int currentSlot, uint64_t nextFidx) {
    if (prefetchedValid_ || !pendingUniqueValid_ || currentSlot < 0 ||
        !generating_ || !running_ || !vk_ || !lsfg_ ||
        !lsfg_->isAvailable() || !lsfg_->isConfigured() ||
        pendingSlot_.image == VK_NULL_HANDLE || vk_->isOneShotAsyncPending()) {
        return false;
    }
    const int nextSlot = (currentSlot == 0) ? 1 : 0;
    if (slots_[nextSlot].image == VK_NULL_HANDLE) return false;

    uint64_t fidx = nextFidx;
    if ((fidx & 1u) != static_cast<uint64_t>(nextSlot)) {
        ++fidx;
    }

    VkCommandBuffer preCmd = vk_->beginOneShotCommands(vk_->graphicsCommandPool());
    recordImageCopyToSlot(preCmd, pendingSlot_.image, slots_[nextSlot].image, frameWidth_, frameHeight_);
    lsfg_->recordPrepassOnly(preCmd, fidx);
    vk_->submitOneShotAsync(vk_->graphicsPresentQueue(), preCmd);

    prefetchedValid_ = true;
    prefetchedSlot_ = nextSlot;
    prefetchedFidx_ = fidx;
    prefetchedPeriodMs_ = pendingUniquePeriodMs_;
    prefetchedJitterMs_ = pendingUniqueJitterMs_;
    prefetchedArrival_ = pendingUniqueArrival_;
    prefetchedStart_ = pendingUniqueStart_;
    clearPendingUniqueFrame();
    return true;
}

void FrameGenerator::copyPendingIntoSlot(FrameSlot& dstSlot) {
    VkCommandBuffer copyCmd = vk_->beginOneShotCommands(vk_->graphicsCommandPool());
    recordImageCopyToSlot(copyCmd, pendingSlot_.image, dstSlot.image, frameWidth_, frameHeight_);
    vk_->endOneShotCommands(vk_->graphicsCommandPool(), vk_->graphicsPresentQueue(), copyCmd);
}

void FrameGenerator::run() {
    ::prctl(PR_SET_TIMERSLACK, 10000UL, 0, 0, 0);
    std::fprintf(stderr,
        "[lfg] Control Panel ready. Start/Stop Shortcut: %s | Active GPU: %s\n",
        controlPanel_ ? controlPanel_->hotkeyLabel().c_str() : "ALT+F9",
        vk_->selectedGpuInfo().name.c_str());

    if (options_.autoStart) {
        handleControlAction(present::ControlAction::Start);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options_.runSeconds);
    if (options_.runSeconds > 0)
        std::fprintf(stderr, "[lfg] auto-exit in %d s\n", options_.runSeconds);

    int consecutiveFailures = 0;
    uint64_t cycles = 0;

    auto fpsWindowStart = std::chrono::steady_clock::now();
    auto termLogStart = fpsWindowStart;
    auto lastRealFrameTime = std::chrono::steady_clock::time_point{};
    uint32_t capturedInWindow = 0;
    uint32_t presentedInWindow = 0;

    auto recordCycleStats = [&](int presented, double logPeriodMs, bool emitTermLog) {
        ++cycles;
        ++capturedInWindow;
        presentedInWindow += static_cast<uint32_t>(std::max(0, presented));

        auto afterCycle = std::chrono::steady_clock::now();
        double windowSec = std::chrono::duration<double>(afterCycle - fpsWindowStart).count();
        if (windowSec >= 0.25) {
            float capFps = static_cast<float>(capturedInWindow / windowSec);
            float outFps = static_cast<float>(presentedInWindow / windowSec);
            if (overlay_) overlay_->setFps(capFps, outFps);
            if (controlPanel_) {
                controlPanel_->setFps(static_cast<int>(std::lround(capFps)),
                                      static_cast<int>(std::lround(outFps)));
            }

            if (emitTermLog && std::chrono::duration<double>(afterCycle - termLogStart).count() >= 1.0) {
                const double hz = overlay_ ? overlay_->refreshRateHz() : 0.0;
                const double avgPace = paceCount_ > 0 ? (paceSumMs_ / static_cast<double>(paceCount_)) : 0.0;
                const double minPace = paceCount_ > 0 ? paceMinMs_ : 0.0;
                const double maxPace = paceCount_ > 0 ? paceMaxMs_ : 0.0;
                if (options_.adaptive) {
                    std::fprintf(stderr, "[lfg] ADAPTIVE (target %d, %dx, flow %d%%) FPS (%s): %.1f -> %.1f "
                                 "(source period %.1f ms, display %.0f Hz, gpu synth %.1f ms, pace %.2f/%.2f/%.2f ms, dups %u)\n",
                                 options_.targetFps, effectiveMultiplier_ > 0 ? effectiveMultiplier_ : 1,
                                 options_.flowScale, vk_->selectedGpuInfo().shortLabel.c_str(),
                                 capFps, outFps, logPeriodMs, hz, firstPresentEmaMs_,
                                 minPace, avgPace, maxPace, dupsDroppedInWindow_);
                } else {
                    std::fprintf(stderr, "[lfg] %dX (flow %d%%) FPS (%s): %.1f -> %.1f "
                                 "(source period %.1f ms, display %.0f Hz, gpu synth %.1f ms, pace %.2f/%.2f/%.2f ms, dups %u)\n",
                                 effectiveMultiplier_ > 0 ? effectiveMultiplier_ : options_.multiplier,
                                 options_.flowScale, vk_->selectedGpuInfo().shortLabel.c_str(), capFps, outFps,
                                 logPeriodMs, hz, firstPresentEmaMs_, minPace, avgPace, maxPace,
                                 dupsDroppedInWindow_);
                }
                paceMinMs_ = 1e9;
                paceMaxMs_ = 0.0;
                paceSumMs_ = 0.0;
                paceCount_ = 0;
                dupsDroppedInWindow_ = 0;
                termLogStart = afterCycle;
            }
            fpsWindowStart = afterCycle;
            capturedInWindow = 0;
            presentedInWindow = 0;
        }
    };

    while (running_) {
        if (options_.runSeconds > 0 && std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "[lfg] time limit reached\n");
            break;
        }

        int sigAct = pendingSignalAction_.exchange(0);
        if (sigAct == 1) handleControlAction(present::ControlAction::Start);
        else if (sigAct == 2) handleControlAction(present::ControlAction::Stop);
        else if (sigAct == 3) handleControlAction(present::ControlAction::Toggle);
        else if (sigAct == 4 && controlPanel_) controlPanel_->showFromTray();

        if (controlPanel_) {
            present::ControlAction guiAct = controlPanel_->pollEvents();
            lastUiPollTime_ = std::chrono::steady_clock::now();
            if (controlPanel_->consumeConfigChanged()) {
                options_.panelWidth = controlPanel_->windowWidth();
                options_.panelHeight = controlPanel_->windowHeight();
                options_.toggleHotkey = controlPanel_->hotkeyLabel();
                saveSettings(options_);
            }
            if (guiAct != present::ControlAction::None) {
                handleControlAction(guiAct);
            }
        }
        if (!running_) break;

        if (!sessionActive_ || !capture_) {
            if (vk_) vk_->waitOneShotAsync();
            clearPrefetchedFrame();
            clearPendingUniqueFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (prefetchedValid_) {
            const int pfSlot = prefetchedSlot_;
            const uint64_t pfFidx = prefetchedFidx_;
            const double pfPeriodMs = prefetchedPeriodMs_;
            const double pfJitterMs = prefetchedJitterMs_;
            const auto pfArrival = prefetchedArrival_;
            const auto pfStart = prefetchedStart_;
            clearPrefetchedFrame();

            if (generating_ && pfSlot >= 0 && lsfg_ && lsfg_->isAvailable() && lsfg_->isConfigured()) {
                newestSlot_ = pfSlot;
                lsfgFrameIndex_ = pfFidx;
                lastRealFrameTime = pfStart;
                if (overlay_) overlay_->show();

                const int presented = renderCycle(slots_[newestSlot_], pfStart, pfArrival, pfPeriodMs, pfJitterMs, true);
                recordCycleStats(presented, pfPeriodMs, true);
                continue;
            } else if (vk_) {
                vk_->waitOneShotAsync();
            }
        } else if (pendingUniqueValid_) {
            const double pendingPeriodMs = pendingUniquePeriodMs_;
            const double pendingJitterMs = pendingUniqueJitterMs_;
            const auto pendingArrival = pendingUniqueArrival_;
            const auto pendingStart = pendingUniqueStart_;
            clearPendingUniqueFrame();

            if (generating_ && lsfg_ && lsfg_->isAvailable() && lsfg_->isConfigured() &&
                pendingSlot_.image != VK_NULL_HANDLE) {
                const int slotIndex = (newestSlot_ == 0) ? 1 : 0;
                copyPendingIntoSlot(slots_[slotIndex]);
                newestSlot_ = slotIndex;
                lastRealFrameTime = pendingStart;
                if ((lsfgFrameIndex_ & 1u) != static_cast<uint64_t>(newestSlot_)) {
                    ++lsfgFrameIndex_;
                }
                if (overlay_) overlay_->show();

                const int presented =
                    renderCycle(slots_[newestSlot_], pendingStart, pendingArrival, pendingPeriodMs, pendingJitterMs, false);
                recordCycleStats(presented, pendingPeriodMs, true);
                continue;
            }
        }

        capture::CaptureBuffer buffer;
        const capture::WaitStatus status = capture_->waitForBuffer(16, buffer);
        if (status == capture::WaitStatus::Failed) {
            std::string reason = capture_->failureMessage();
            std::fprintf(stderr, "[lfg] PipeWire stream ended: %s\n", reason.c_str());
            if (generating_) {
                if (!forceLinearCapture_) {
                    std::fprintf(stderr, "[lfg] retrying PipeWire stream in linear cross-GPU mode...\n");
                    forceLinearCapture_ = true;
                    try {
                        capture_->stop();
                        capture_ = std::make_unique<capture::PipeWireCapture>(*importer_);
                        capture_->start(currentTarget_, true, false);
                        continue;
                    } catch (...) {}
                } else if (!forceShmCapture_) {
                    std::fprintf(stderr, "[lfg] retrying PipeWire stream in shared-memory (MemFd) mode...\n");
                    forceShmCapture_ = true;
                    try {
                        capture_->stop();
                        capture_ = std::make_unique<capture::PipeWireCapture>(*importer_);
                        capture_->start(currentTarget_, true, true);
                        continue;
                    } catch (...) {}
                }
            }
            if (controlPanel_) controlPanel_->setStatusNotice("STREAM ENDED - PRESS START");
            handleControlAction(present::ControlAction::Stop);
            stopCaptureSession();
            continue;
        }
        if (status == capture::WaitStatus::Timeout) {
            if (!generating_ || newestSlot_ < 0 || !lsfg_ || !lsfg_->isAvailable() ||
                frameWidth_ == 0 || frameHeight_ == 0) {
                continue;
            }
            const auto computeStart = std::chrono::steady_clock::now();
            if (lastRealFrameTime.time_since_epoch().count() != 0 &&
                std::chrono::duration<double, std::milli>(computeStart - lastRealFrameTime).count() < 250.0) {
                continue;
            }
            // When a browser or window is steady / not sending new PipeWire frames, keep applying
            // Lossless Scaling's LSFG 3.1 pipeline across the latest captured frame at 60 Hz cadence.
            if (overlay_) {
                overlay_->waitForIdle();
                lastPresentedOutputSlot_ = -1;
            }
            const int srcSlot = newestSlot_;
            const int dstSlot = (srcSlot == 0) ? 1 : 0;

            VkCommandBuffer copyCmd = vk_->beginOneShotCommands(vk_->graphicsCommandPool());
            recordImageCopyToSlot(copyCmd, slots_[srcSlot].image, slots_[dstSlot].image, frameWidth_, frameHeight_);
            vk_->endOneShotCommands(vk_->graphicsCommandPool(), vk_->graphicsPresentQueue(), copyCmd);

            newestSlot_ = dstSlot;
            if ((lsfgFrameIndex_ & 1u) != static_cast<uint64_t>(newestSlot_)) {
                ++lsfgFrameIndex_;
            }

            const double steadyPeriodMs = 16.6667;
            const int presented = renderCycle(slots_[newestSlot_], computeStart, computeStart, steadyPeriodMs, 0.0, false);
            recordCycleStats(presented, steadyPeriodMs, false);
            continue;
        }

        if (!generating_ || !lsfg_ || !lsfg_->isAvailable()) {
            capture_->releaseBuffer(buffer);
            continue;
        }

        const auto computeStart = std::chrono::steady_clock::now();
        const auto arrivalTime = (buffer.arrivalTime.time_since_epoch().count() != 0) ? buffer.arrivalTime : computeStart;
        double producerPeriodMs = std::clamp(buffer.producerPeriodMs, 2.5, 160.0);
        double producerJitterMs = std::clamp(buffer.producerJitterMs, 0.0, 40.0);

        bool resourcesReady = false;
        try {
            if (ensureResources(buffer.width, buffer.height, currentTarget_)) {
                newestSlot_ = -1;
                resetPacerState(true);
            } else if (needsTargetAlign_) {
                if (overlay_) {
                    present::TargetRect requestedRect{currentTarget_.x, currentTarget_.y, buffer.width, buffer.height};
                    overlay_->setPreferredDisplay(options_.displayIndex, requestedRect);
                    overlay_->alignWindowedTarget(buffer.width, buffer.height);
                }
                needsTargetAlign_ = false;
            }
            resourcesReady = true;
            resourceFailures_ = 0;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[lfg] resource allocation error: %s\n", e.what());
        }

        if (!resourcesReady) {
            capture_->releaseBuffer(buffer);
            if (overlay_) overlay_->hide();
            if (++resourceFailures_ >= 5) {
                resourceFailures_ = 0;
                std::fprintf(stderr, "[lfg] not enough GPU memory for LSFG on %s — stopping frame generation\n",
                             vk_->selectedGpuInfo().name.c_str());
                if (controlPanel_) controlPanel_->setStatusNotice("NOT ENOUGH VRAM - LOWER GAME SETTINGS");
                handleControlAction(present::ControlAction::Stop);
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        bool copied = false;
        int copiedSlot = -1;
        try {
            const int slotIndex = (newestSlot_ == 0) ? 1 : 0;
            copyIntoSlot(buffer, slots_[slotIndex]);
            copiedSlot = slotIndex;
            copied = true;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[lfg] could not import/copy a captured frame: %s\n", e.what());
        }
        capture_->releaseBuffer(buffer);

        if (!copied) {
            if (!forceLinearCapture_) {
                std::fprintf(stderr, "[lfg] switching capture stream to linear cross-GPU mode for %s...\n",
                             vk_->selectedGpuInfo().name.c_str());
                forceLinearCapture_ = true;
                try {
                    capture_->stop();
                    capture_ = std::make_unique<capture::PipeWireCapture>(*importer_);
                    capture_->start(currentTarget_, true, false);
                    consecutiveFailures = 0;
                    continue;
                } catch (...) {}
            }
            if (!forceShmCapture_) {
                std::fprintf(stderr, "[lfg] switching capture stream to shared-memory (MemFd) mode for %s...\n",
                             vk_->selectedGpuInfo().name.c_str());
                forceLinearCapture_ = true;
                forceShmCapture_ = true;
                directImportFailed_ = true;
                try {
                    capture_->stop();
                    capture_ = std::make_unique<capture::PipeWireCapture>(*importer_);
                    capture_->start(currentTarget_, true, true);
                    consecutiveFailures = 0;
                    continue;
                } catch (...) {}
            }
            if (++consecutiveFailures >= 15) {
                consecutiveFailures = 0;
                try {
                    capture_->stop();
                    capture_ = std::make_unique<capture::PipeWireCapture>(*importer_);
                    capture_->start(currentTarget_, true, true);
                    continue;
                } catch (...) {
                    if (newestSlot_ < 0) {
                        if (controlPanel_) controlPanel_->setStatusNotice("CAPTURE IMPORT FAILED");
                        handleControlAction(present::ControlAction::Stop);
                    }
                }
            }
            continue;
        }
        consecutiveFailures = 0;

        if (newestSlot_ >= 0 && isCurrentThumbDuplicate()) {
            const double sinceLastMs =
                (lastRealFrameTime.time_since_epoch().count() != 0)
                    ? std::chrono::duration<double, std::milli>(computeStart - lastRealFrameTime).count()
                    : 0.0;
            if (sinceLastMs < 250.0) {
                ++dupsDroppedInWindow_;
                continue;
            }
            // Genuinely static/paused scene (>= 250 ms): keep presenting at 60 Hz keepalive
            // without polluting the unique-frame period/jitter estimator.
            newestSlot_ = copiedSlot;
            commitCurrentThumb();
            lastRealFrameTime = computeStart;
            producerPeriodMs = 16.6667;
            producerJitterMs = 0.0;
        } else {
            newestSlot_ = copiedSlot;
            commitCurrentThumb();
            lastRealFrameTime = computeStart;
            recordUniqueFrameTiming(arrivalTime, producerPeriodMs, producerJitterMs,
                                    producerPeriodMs, producerJitterMs);
        }

        if (overlay_ && generating_) overlay_->show();

        const int previous = (newestSlot_ == 0) ? 1 : 0;
        if (slots_[previous].image == VK_NULL_HANDLE || cycles == 0) {
            if (cycles == 0 && newestSlot_ == 0) {
                std::fprintf(stderr, "[lfg] receiving frames (%ux%u) — waiting for frame 2 to start LSFG 3.1\n",
                             frameWidth_, frameHeight_);
                lsfgFrameIndex_ = static_cast<uint64_t>(newestSlot_);
                VkCommandBuffer preCmd = vk_->beginOneShotCommands(vk_->graphicsCommandPool());
                lsfg_->recordPrepassOnly(preCmd, lsfgFrameIndex_++);
                vk_->endOneShotCommands(vk_->graphicsCommandPool(), vk_->graphicsPresentQueue(), preCmd);
                if (overlay_) {
                    overlay_->waitForIdle();
                    if (overlay_->presentImage(slots_[newestSlot_].image, VK_IMAGE_LAYOUT_GENERAL, frameWidth_, frameHeight_)) {
                        const auto now = std::chrono::steady_clock::now();
                        lastActualPresent_ = now;
                        const double initStep = producerPeriodMs * 0.5;
                        lastStepMs_ = initStep;
                        nextPresentTime_ = now + std::chrono::microseconds(static_cast<int64_t>(initStep * 1000.0));
                    }
                    lastPresentedOutputSlot_ = -1;
                }
                fpsWindowStart = std::chrono::steady_clock::now();
                termLogStart = fpsWindowStart;
                capturedInWindow = 0;
                presentedInWindow = 0;
                continue;
            }
        }

        if ((lsfgFrameIndex_ & 1u) != static_cast<uint64_t>(newestSlot_)) {
            ++lsfgFrameIndex_;
        }

        const int presented = renderCycle(slots_[newestSlot_], computeStart, arrivalTime, producerPeriodMs, producerJitterMs, false);
        recordCycleStats(presented, producerPeriodMs, true);
    }

    std::fprintf(stderr, "[lfg] stopped after %llu cycle(s)\n", static_cast<unsigned long long>(cycles));
    stopCaptureSession();
}

int FrameGenerator::renderCycle(const FrameSlot& newest,
                                std::chrono::steady_clock::time_point computeStart,
                                std::chrono::steady_clock::time_point arrivalTime,
                                double framePeriodMs,
                                double producerJitterMs,
                                bool prepassAlreadySubmitted) {
    using Clock = std::chrono::steady_clock;
    auto toDuration = [](double ms) {
        return std::chrono::microseconds(static_cast<int64_t>(ms * 1000.0));
    };

    if (!overlay_ || !lsfg_ || !lsfg_->isAvailable() || !lsfg_->isConfigured()) return 0;

    struct PresentStep {
        float t = 1.0f;
        bool useNewest = false;
        bool usePrevious = false;
    };

    std::vector<PresentStep> steps;
    double baseStepMs = framePeriodMs;

    if (options_.adaptive) {
        const double targetFps = std::clamp(static_cast<double>(options_.targetFps), 30.0,
                                            static_cast<double>(kMaxTargetFps));
        const double targetStepMs = 1000.0 / targetFps;
        const double gpuCap = std::clamp(adaptiveGpuCapMult_, 2.0,
                                         static_cast<double>(kMaxAdaptiveMultiplier));
        baseStepMs = std::max(targetStepMs, framePeriodMs / gpuCap);

        if (framePeriodMs <= baseStepMs * 1.04) {
            adaptivePhaseMs_ = 0.0;
            effectiveMultiplier_ = 1;
            steps.push_back(PresentStep{1.0f, true, false});
            baseStepMs = framePeriodMs;
        } else {
            double tau = (adaptivePhaseMs_ > 0.0 && adaptivePhaseMs_ <= baseStepMs)
                             ? adaptivePhaseMs_
                             : baseStepMs;
            while (tau <= framePeriodMs + 1e-4 &&
                   static_cast<int>(steps.size()) < kMaxAdaptiveMultiplier) {
                const float t = static_cast<float>(tau / framePeriodMs);
                if (t <= 0.05f) {
                    steps.push_back(PresentStep{0.0f, false, true});
                } else if (t >= 0.95f) {
                    steps.push_back(PresentStep{1.0f, true, false});
                } else {
                    steps.push_back(PresentStep{t, false, false});
                }
                tau += baseStepMs;
            }
            if (steps.empty()) {
                steps.push_back(PresentStep{1.0f, true, false});
            }
            adaptivePhaseMs_ = std::clamp(tau - framePeriodMs, 1e-3, baseStepMs);
            effectiveMultiplier_ = static_cast<int>(steps.size());
        }
    } else {
        const int mult = std::clamp(options_.multiplier, 2, kMaxMultiplier);
        effectiveMultiplier_ = mult;
        baseStepMs = framePeriodMs / static_cast<double>(mult);
        steps.reserve(static_cast<size_t>(mult));
        for (int k = 1; k < mult; ++k) {
            steps.push_back(PresentStep{static_cast<float>(k) / static_cast<float>(mult), false, false});
        }
        steps.push_back(PresentStep{1.0f, true, false});
    }

    int totalPresents = static_cast<int>(steps.size());

    // Compute continuous PLL step duration and phase lead relative to real-frame arrival.
    const double estSynthMs = std::clamp(
        (firstPresentEmaMs_ > 0.0) ? firstPresentEmaMs_ : 3.5, 1.0, 10.0);
    const double jitterMarginMs = std::clamp(
        std::max(framePeriodMs * 0.10, producerJitterMs * 1.35 + 1.5),
        2.0, std::min(framePeriodMs * 0.45, 18.0));
    const double targetLeadMs = std::clamp(
        estSynthMs + 1.0 + jitterMarginMs,
        3.0, std::max(framePeriodMs * 0.65, 14.0));

    const auto arrivalAnchor =
        (arrivalTime.time_since_epoch().count() != 0 &&
         arrivalTime <= computeStart &&
         (computeStart - arrivalTime) < std::chrono::milliseconds(150))
            ? arrivalTime
            : computeStart;
    const auto idealFirstPresent = arrivalAnchor + toDuration(targetLeadMs);

    double stepMs = std::max(0.25, baseStepMs);
    {
        const auto nowInit = Clock::now();
        const bool hasNextAnchor =
            (nextPresentTime_.time_since_epoch().count() != 0) &&
            (std::abs(std::chrono::duration<double, std::milli>(nextPresentTime_ - nowInit).count()) < 120.0);

        if (!hasNextAnchor) {
            nextPresentTime_ = std::max(nowInit, idealFirstPresent);
        } else if (nextPresentTime_ < nowInit) {
            const auto minFloor = (lastActualPresent_.time_since_epoch().count() != 0)
                                      ? (lastActualPresent_ + toDuration(baseStepMs * 0.92))
                                      : nowInit;
            nextPresentTime_ = std::max(nowInit, minFloor);
        } else {
            const double phaseErrMs =
                std::chrono::duration<double, std::milli>(nextPresentTime_ - idealFirstPresent).count();
            const double maxErrMs = std::max(framePeriodMs * 1.8, 70.0);
            if (std::abs(phaseErrMs) > maxErrMs) {
                const auto minFloor = (lastActualPresent_.time_since_epoch().count() != 0)
                                          ? (lastActualPresent_ + toDuration(baseStepMs * 0.92))
                                          : nowInit;
                nextPresentTime_ = std::max({nowInit, minFloor, idealFirstPresent});
            } else {
                const double corrMs = std::clamp(-phaseErrMs * 0.18 / static_cast<double>(totalPresents),
                                                 -0.08 * baseStepMs, 0.08 * baseStepMs);
                stepMs = std::max(0.25, baseStepMs + corrMs);
            }
        }
    }

    struct AsyncGuard {
        FrameGenerator* self;
        bool keepOnExit = false;
        void wait() {
            if (self->vk_ && self->vk_->isOneShotAsyncPending()) {
                self->vk_->waitOneShotAsync();
            }
        }
        ~AsyncGuard() {
            if (!keepOnExit) wait();
        }
    } asyncCompute{this};

    const uint64_t fidx = lsfgFrameIndex_++;
    const uint64_t cycleEpoch = resourceEpoch_;
    bool prepassInFlight = false;
    bool genInFlight = false;
    uint32_t pendingGenSlot = 0u;
    auto firstSubmitTime = computeStart;
    bool firstGenMeasured = false;

    const bool firstIsGen = (!steps[0].useNewest && !steps[0].usePrevious);
    if (firstIsGen) {
        if (prepassAlreadySubmitted) {
            asyncCompute.wait();
        }
        const uint32_t firstSlot = (lastPresentedOutputSlot_ == 0) ? 1u : 0u;
        if (lastPresentedOutputSlot_ == static_cast<int>(firstSlot)) {
            overlay_->waitForIdle();
            lastPresentedOutputSlot_ = -1;
        }
        VkCommandBuffer firstCmd = vk_->beginOneShotCommands(vk_->graphicsCommandPool());
        if (!prepassAlreadySubmitted) {
            lsfg_->recordPrepassOnly(firstCmd, fidx);
        }
        lsfg_->recordGeneratePass(firstCmd, fidx, steps[0].t, firstSlot);
        firstSubmitTime = Clock::now();
        vk_->submitOneShotAsync(vk_->graphicsPresentQueue(), firstCmd);
        pendingGenSlot = firstSlot;
        genInFlight = true;
    } else if (!prepassAlreadySubmitted) {
        VkCommandBuffer preCmd = vk_->beginOneShotCommands(vk_->graphicsCommandPool());
        lsfg_->recordPrepassOnly(preCmd, fidx);
        firstSubmitTime = Clock::now();
        vk_->submitOneShotAsync(vk_->graphicsPresentQueue(), preCmd);
        prepassInFlight = true;
    } else {
        prepassInFlight = true;
    }

    int presented = 0;
    bool preemptedForGpu = false;
    bool preemptedForRate = false;
    for (int k = 0; k < totalPresents; ++k) {
        const auto targetPresentTime = nextPresentTime_;
        const bool isLast = (k == totalPresents - 1);
        const bool isGen = (!steps[k].useNewest && !steps[k].usePrevious);
        const bool allowPrefetch =
            (isLast && totalPresents > 1 && !prefetchedValid_ && !steps[k].usePrevious);

        if (Clock::now() < targetPresentTime) {
            if (!waitUntilTime(targetPresentTime, /*allowCaptureDrain=*/!prefetchedValid_,
                               allowPrefetch, newestSlot_, lsfgFrameIndex_)) {
                return presented;
            }
        }
        if (!running_ || !generating_ || resourceEpoch_ != cycleEpoch || !overlay_ || newestSlot_ < 0) {
            return presented;
        }

        if (isGen && genInFlight && !prefetchedValid_) {
            const bool alreadyDone = vk_->pollOneShotAsyncComplete();
            asyncCompute.wait();
            genInFlight = false;
            if (!firstGenMeasured && k == 0) {
                firstGenMeasured = true;
                if (!alreadyDone) {
                    const double synthMs = std::clamp(
                        std::chrono::duration<double, std::milli>(Clock::now() - firstSubmitTime).count(),
                        0.2, 10.0);
                    if (prepassAlreadySubmitted) {
                        genPassEmaMs_ = (genPassEmaMs_ <= 0.0) ? synthMs : (genPassEmaMs_ * 0.80 + synthMs * 0.20);
                        const double targetTotal = std::min(10.0, synthMs + 1.0);
                        firstPresentEmaMs_ =
                            (firstPresentEmaMs_ <= 0.0) ? targetTotal : (firstPresentEmaMs_ * 0.80 + targetTotal * 0.20);
                    } else {
                        firstPresentEmaMs_ =
                            (firstPresentEmaMs_ <= 0.0) ? synthMs : (firstPresentEmaMs_ * 0.80 + synthMs * 0.20);
                    }
                } else if (firstPresentEmaMs_ <= 0.0) {
                    firstPresentEmaMs_ = 2.5;
                } else if (firstPresentEmaMs_ > 3.0) {
                    firstPresentEmaMs_ = firstPresentEmaMs_ * 0.85 + 2.2 * 0.15;
                }
            }
        } else if (totalPresents == 1 && !prefetchedValid_ && prepassInFlight) {
            asyncCompute.wait();
            prepassInFlight = false;
        }

        bool ok = false;
        if (steps[k].useNewest) {
            ok = overlay_->presentImage(newest.image, VK_IMAGE_LAYOUT_GENERAL, frameWidth_, frameHeight_);
            if (ok) lastPresentedOutputSlot_ = -1;
        } else if (steps[k].usePrevious) {
            const int prevSlot = (newestSlot_ == 0) ? 1 : 0;
            ok = overlay_->presentImage(slots_[prevSlot].image, VK_IMAGE_LAYOUT_GENERAL, frameWidth_, frameHeight_);
            if (ok) lastPresentedOutputSlot_ = -1;
        } else {
            const auto& gen = lsfg_->output(pendingGenSlot);
            ok = overlay_->presentImage(gen.image, VK_IMAGE_LAYOUT_GENERAL, gen.width, gen.height);
            if (ok) lastPresentedOutputSlot_ = static_cast<int>(pendingGenSlot);
        }

        if (ok) {
            ++presented;
            const auto now = Clock::now();
            if (lastActualPresent_.time_since_epoch().count() != 0) {
                const double deltaMs = std::chrono::duration<double, std::milli>(now - lastActualPresent_).count();
                if (deltaMs < 200.0) {
                    paceMinMs_ = std::min(paceMinMs_, deltaMs);
                    paceMaxMs_ = std::max(paceMaxMs_, deltaMs);
                    paceSumMs_ += deltaMs;
                    ++paceCount_;
                }
            }
            lastActualPresent_ = now;

            const auto baseAnchor = (targetPresentTime < now - toDuration(stepMs * 0.10)) ? now : targetPresentTime;
            const auto nextTarget = baseAnchor + toDuration(stepMs);
            const auto minTarget = now + toDuration(stepMs * 0.92);
            const auto maxTarget = now + toDuration(stepMs * 1.12);
            nextPresentTime_ = std::clamp(nextTarget, minTarget, maxTarget);
            lastStepMs_ = stepMs;
        } else {
            overlay_->waitForIdle();
            lastPresentedOutputSlot_ = -1;
            nextPresentTime_ = Clock::now() + toDuration(stepMs);
        }

        // Prepare the next intermediate frame on the alternate ping-pong slot while `k` displays.
        if (k + 1 < totalPresents) {
            if (!prefetchedValid_ && capture_ && capture_->hasPendingFrame() &&
                vk_ && vk_->pollOneShotAsyncComplete()) {
                pollAndClassifyPendingCapture();
            }

            const double elapsedFromArrival =
                std::chrono::duration<double, std::milli>(Clock::now() - arrivalAnchor).count();
            const double livePeriodMs = hasSmoothedUniquePeriod_
                                            ? std::clamp(smoothedUniquePeriodMs_, 2.5, 160.0)
                                            : framePeriodMs;
            const int remainingAfter = (totalPresents - 1) - k;
            const bool subharmonicBehind =
                (pendingUniqueCount_ >= 2) ||
                (pendingUniqueValid_ && framePeriodMs > livePeriodMs * 1.25 &&
                 elapsedFromArrival >= livePeriodMs * 1.15 + targetLeadMs);
            const bool lateOverrun =
                (pendingUniqueValid_ && elapsedFromArrival >= framePeriodMs * 1.35 + targetLeadMs &&
                 remainingAfter >= 2);
            if (subharmonicBehind && remainingAfter >= 1) {
                preemptedForRate = true;
                steps.resize(static_cast<size_t>(k + 2));
                steps[k + 1] = PresentStep{1.0f, true, false};
                totalPresents = static_cast<int>(steps.size());
                continue;
            }
            if (lateOverrun) {
                preemptedForGpu = (firstPresentEmaMs_ > baseStepMs * 0.85);
                steps.resize(static_cast<size_t>(k + 2));
                steps[k + 1] = PresentStep{1.0f, true, false};
                totalPresents = static_cast<int>(steps.size());
                continue;
            }

            if (!steps[k + 1].useNewest && !steps[k + 1].usePrevious) {
                if (prepassInFlight) {
                    asyncCompute.wait();
                    prepassInFlight = false;
                }
                const uint32_t nextSlot = (lastPresentedOutputSlot_ == 0) ? 1u : 0u;
                if (lastPresentedOutputSlot_ == static_cast<int>(nextSlot)) {
                    overlay_->waitForIdle();
                    lastPresentedOutputSlot_ = -1;
                }
                VkCommandBuffer nextCmd = vk_->beginOneShotCommands(vk_->graphicsCommandPool());
                lsfg_->recordGeneratePass(nextCmd, fidx, steps[k + 1].t, nextSlot);
                vk_->submitOneShotAsync(vk_->graphicsPresentQueue(), nextCmd);
                pendingGenSlot = nextSlot;
                genInFlight = true;
            }
        }
    }

    const double totalCycleMs =
        std::chrono::duration<double, std::milli>(Clock::now() - computeStart).count();
    if (options_.adaptive) {
        if (preemptedForGpu) {
            const double fit = std::max(2.0, static_cast<double>(presented));
            const double cur = std::min(adaptiveGpuCapMult_, static_cast<double>(effectiveMultiplier_));
            adaptiveGpuCapMult_ = std::clamp(cur * 0.6 + fit * 0.4, 2.0, static_cast<double>(kMaxAdaptiveMultiplier));
        } else if (!preemptedForRate && totalCycleMs <= framePeriodMs * 1.10) {
            adaptiveGpuCapMult_ = std::min(static_cast<double>(kMaxAdaptiveMultiplier), adaptiveGpuCapMult_ + 0.5);
        }
    }

    asyncCompute.keepOnExit = prefetchedValid_;
    return presented;
}

}  // namespace lfg
