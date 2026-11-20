#pragma once

#include "lfg/vk_common.hpp"
#include "lfg/vulkan_context.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace lfg::compute {

// Native Vulkan implementation of Lossless Scaling's LSFG 3.1 frame generation pipeline.
// Automatically locates Lossless.dll in the user's Steam library (or LSFG_DLL_PATH),
// extracts the embedded SPIR-V compute shaders (FP16 or FP32, Quality and Performance modes),
// and executes the full Mipmaps -> Alpha0/1 -> Beta0/1 -> Gamma0/1 -> Delta0/1 -> Generate
// multi-scale optical flow and frame synthesis pipeline on the active VulkanContext.
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
    bool isConfigured() const { return descriptorPool_ != VK_NULL_HANDLE && outputs_[0].view != VK_NULL_HANDLE; }
    const std::string& dllPath() const { return dllPath_; }
    bool usingFp16() const { return useFp16_; }
    bool performanceMode() const { return perfMode_; }
    int flowScale() const { return flowScale_; }
    const OutputImage& output(uint32_t index) const { return outputs_[index % kOutputSlots]; }

    static std::filesystem::path resolveExplicitDllPath(const std::string& explicitPath);
    static std::filesystem::path findLosslessDll(const std::string& explicitPath = "");

    void configure(uint32_t width, uint32_t height, int flowScale, bool performanceMode,
                   VkImageView slot0View, VkImageView slot1View);

    void recordPrepassOnly(VkCommandBuffer cmd, uint64_t frameIndex);
    void recordGeneratePass(VkCommandBuffer cmd, uint64_t frameIndex, float timestamp, uint32_t outputSlot);
    void record(VkCommandBuffer cmd, uint64_t frameIndex, uint32_t genCount);

private:
    struct ShaderKey {
        uint32_t id = 0;
        bool perf = false;
        bool operator==(const ShaderKey& o) const { return id == o.id && perf == o.perf; }
    };

    struct ShaderKeyHash {
        size_t operator()(const ShaderKey& k) const {
            return (static_cast<size_t>(k.id) << 1) ^ (k.perf ? 1u : 0u);
        }
    };

    struct CompiledShader {
        VkShaderModule module = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        uint32_t sampledCount = 0;
        uint32_t storageCount = 0;
        uint32_t bufferCount = 0;
        uint32_t samplerCount = 0;
    };

    struct InternalImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkExtent2D extent{0, 0};
        VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    };

    struct Alpha0Level {
        std::vector<InternalImage> temp0;
        std::vector<InternalImage> temp1;
        std::vector<InternalImage> images;
        std::array<CompiledShader, 3> shaders{};
        std::array<VkDescriptorSet, 3> sets{};
        VkExtent2D dispatch0{1, 1};
        VkExtent2D dispatch1{1, 1};
    };

    struct Alpha1Level {
        size_t temporal = 2;
        std::vector<std::vector<InternalImage>> images;
        CompiledShader shader{};
        std::vector<VkDescriptorSet> sets;
        VkExtent2D dispatch{1, 1};
    };

    struct Gamma0Level {
        size_t temporal = 2;
        std::array<InternalImage, 3> images{};
        CompiledShader shader{};
        std::vector<VkDescriptorSet> sets;
        VkExtent2D dispatch{1, 1};
    };

    struct Gamma1Level {
        std::vector<InternalImage> temp0;
        std::vector<InternalImage> temp1;
        InternalImage image{};  // VK_FORMAT_R16G16B16A16_SFLOAT
        std::array<CompiledShader, 4> shaders{};
        std::array<VkDescriptorSet, 4> sets{};
        VkExtent2D dispatch{1, 1};
    };

    struct Delta0Level {
        size_t temporal = 2;
        std::array<InternalImage, 3> images0{};
        std::vector<InternalImage> images1;
        CompiledShader shader0{};
        CompiledShader shader1{};
        std::vector<VkDescriptorSet> sets0;
        std::vector<VkDescriptorSet> sets1;
        VkExtent2D dispatch{1, 1};
    };

    struct Delta1Level {
        std::vector<InternalImage> temp0;
        std::vector<InternalImage> temp1;
        InternalImage image0{};  // VK_FORMAT_R16G16B16A16_SFLOAT
        InternalImage image1{};  // VK_FORMAT_R16G16B16A16_SFLOAT
        std::array<CompiledShader, 8> shaders{};
        std::array<VkDescriptorSet, 8> sets{};
        VkExtent2D dispatch{1, 1};
    };

    bool loadAndCompileShaders(const std::filesystem::path& dllFile);
    void savePipelineCache() const;
    void destroyShaders();
    void destroyResolutionResources();

    InternalImage createInternalImage(VkExtent2D extent, VkFormat format,
                                      std::vector<VkImage>& outToInit);
    void destroyInternalImage(InternalImage& img);

    VkDescriptorSet allocateAndWriteSet(
        const CompiledShader& shader,
        const std::vector<VkImageView>& sampledViews,
        const std::vector<VkImageView>& storageViews,
        const std::vector<VkSampler>& samplers,
        const std::vector<VkBuffer>& uniformBuffers);

    static void recordComputeBarrier(VkCommandBuffer cmd);
    static void dispatchShader(VkCommandBuffer cmd, const CompiledShader& shader,
                               VkDescriptorSet set, VkExtent2D extent);

    const VulkanContext& vk_;
    bool available_ = false;
    bool useFp16_ = false;
    std::string dllPath_;

    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
    std::unordered_map<ShaderKey, CompiledShader, ShaderKeyHash> compiledShaders_;

    VkSampler bnbSampler_ = VK_NULL_HANDLE;
    VkSampler bnwSampler_ = VK_NULL_HANDLE;
    VkSampler eabSampler_ = VK_NULL_HANDLE;

    VkBuffer defaultUbo_ = VK_NULL_HANDLE;
    VkDeviceMemory defaultUboMem_ = VK_NULL_HANDLE;
    void* defaultUboMapped_ = nullptr;

    VkBuffer passUbo_ = VK_NULL_HANDLE;
    VkDeviceMemory passUboMem_ = VK_NULL_HANDLE;

    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    int flowScale_ = 100;
    bool perfMode_ = false;
    VkImageView slot0View_ = VK_NULL_HANDLE;
    VkImageView slot1View_ = VK_NULL_HANDLE;
    std::array<OutputImage, kOutputSlots> outputs_{};

    InternalImage blackImage_{};

    // Mipmaps
    std::array<InternalImage, 7> mipmapImages_{};
    CompiledShader mipmapShader_{};
    std::array<VkDescriptorSet, 2> mipmapSets_{};
    VkExtent2D mipmapDispatch_{1, 1};

    // Alpha0 & Alpha1 (7 levels)
    std::array<Alpha0Level, 7> alpha0_{};
    std::array<Alpha1Level, 7> alpha1_{};

    // Beta0
    std::array<InternalImage, 2> beta0Images_{};
    CompiledShader beta0Shader_{};
    std::array<VkDescriptorSet, 3> beta0Sets_{};
    VkExtent2D beta0Dispatch_{1, 1};

    // Beta1
    std::array<InternalImage, 2> beta1Temp0_{};
    std::array<InternalImage, 2> beta1Temp1_{};
    std::array<InternalImage, 6> beta1Images_{};  // VK_FORMAT_R8_UNORM
    std::array<CompiledShader, 4> beta1Shaders_{};
    std::array<VkDescriptorSet, 4> beta1Sets_{};
    VkExtent2D beta1Dispatch0_{1, 1};
    VkExtent2D beta1Dispatch1_{1, 1};

    // Gamma0 & Gamma1 (7 levels)
    std::array<Gamma0Level, 7> gamma0_{};
    std::array<Gamma1Level, 7> gamma1_{};

    // Delta0 & Delta1 (3 levels)
    std::array<Delta0Level, 3> delta0_{};
    std::array<Delta1Level, 3> delta1_{};

    // Generate
    CompiledShader generateShader_{};
    std::array<std::array<VkDescriptorSet, kOutputSlots>, 2> generateSets_{};
    VkExtent2D generateDispatch_{1, 1};
};

}  // namespace lfg::compute
