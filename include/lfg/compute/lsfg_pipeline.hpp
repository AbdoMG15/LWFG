#pragma once

#include "lfg/vk_common.hpp"
#include "lfg/vulkan_context.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace lfg::compute {

// Native Vulkan execution of Lossless Scaling's LSFG 3.1 frame generation pipeline
// powered by lsfg-vk's open-source (GPL-3.0) lsfg-vk-backend shaderchains and
// descriptor/shader management, while preserving LWFG's capture, pacing, and overlay.
class LsfgPipeline {
public:
    static constexpr uint32_t kOutputSlots = 2;

    struct OutputImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    explicit LsfgPipeline(const VulkanContext& vk, const std::string& explicitDllPath = "");
    ~LsfgPipeline();

    LsfgPipeline(const LsfgPipeline&) = delete;
    LsfgPipeline& operator=(const LsfgPipeline&) = delete;

    bool isAvailable() const { return available_; }
    bool isConfigured() const { return res_ != nullptr && outputs_[0].view != VK_NULL_HANDLE; }
    const std::string& dllPath() const { return dllPath_; }
    bool usingFp16() const { return useFp16_; }
    bool performanceMode() const { return perfMode_; }
    int flowScale() const { return flowScale_; }
    const OutputImage& output(uint32_t index) const { return outputs_[index % kOutputSlots]; }

    static std::filesystem::path resolveExplicitDllPath(const std::string& explicitPath);
    static std::filesystem::path findLosslessDll(const std::string& explicitPath = "");

    void configure(uint32_t width, uint32_t height, int flowScale, bool performanceMode,
                   VkImage slot0Image, VkImageView slot0View,
                   VkImage slot1Image, VkImageView slot1View);

    void recordPrepassOnly(VkCommandBuffer cmd, uint64_t frameIndex);
    void recordGeneratePass(VkCommandBuffer cmd, uint64_t frameIndex, float timestamp, uint32_t outputSlot);
    void record(VkCommandBuffer cmd, uint64_t frameIndex, uint32_t genCount);

private:
    struct BackendState;
    struct ResolutionState;

    const VulkanContext& vk_;
    bool available_ = false;
    bool useFp16_ = false;
    std::string dllPath_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    int flowScale_ = 100;
    bool perfMode_ = false;
    std::array<OutputImage, kOutputSlots> outputs_{};

    std::unique_ptr<BackendState> backend_;
    std::unique_ptr<ResolutionState> res_;
};

}  // namespace lfg::compute
