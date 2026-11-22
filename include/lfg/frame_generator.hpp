#pragma once

#include "lfg/capture/dmabuf_image_importer.hpp"
#include "lfg/capture/pipewire_capture.hpp"
#include "lfg/capture/portal_session.hpp"
#include "lfg/compute/lsfg_pipeline.hpp"
#include "lfg/options.hpp"
#include "lfg/present/control_panel.hpp"
#include "lfg/present/overlay_window.hpp"
#include "lfg/vulkan_context.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace lfg {

class FrameGenerator {
public:
    explicit FrameGenerator(const Options& options);
    ~FrameGenerator();

    FrameGenerator(const FrameGenerator&) = delete;
    FrameGenerator& operator=(const FrameGenerator&) = delete;

    void run();
    void stop() { running_ = false; }
    void requestStartGenerating() { pendingSignalAction_.store(1); }
    void requestStopGenerating()  { pendingSignalAction_.store(2); }
    void requestToggleGenerating(){ pendingSignalAction_.store(3); }
    void requestShowWindow()      { pendingSignalAction_.store(4); }

private:
    struct FrameSlot {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    void initVulkanBackend(int gpuIndex, GpuPreference pref);
    void teardownVulkanBackend();
    bool switchGpu(int targetGpuIndex);
    bool ensureLsfgAvailable();

    bool startCaptureSession(bool autoSelectActiveWindow = true);
    void stopCaptureSession();
    void handleControlAction(present::ControlAction action);
    bool waitUntilTime(std::chrono::steady_clock::time_point targetTime,
                       bool allowPrefetch = false, int currentSlot = -1, uint64_t nextFidx = 0);
    bool tryPrefetchNextFrame(int currentSlot, uint64_t nextFidx);
    void clearPrefetchedFrame();
    void resetPacerState(bool keepTimingEma = false);

    bool ensureResources(uint32_t width, uint32_t height, const capture::CaptureTarget& target);
    void configureLsfgResources();
    void destroySlots();
    void ensureStagingBuffer(VkDeviceSize requiredBytes);
    void destroyStagingBuffer();
    void ensureThumbResources();
    void destroyThumbResources();
    void recordThumbDownsample(VkCommandBuffer cmd, VkImage srcImage, uint32_t srcW, uint32_t srcH);
    bool isCurrentThumbDuplicate() const;
    void commitCurrentThumb();
    void recordUniqueFrameTiming(std::chrono::steady_clock::time_point arrivalTime,
                                 double fallbackPeriodMs, double fallbackJitterMs,
                                 double& outPeriodMs, double& outJitterMs);
    void copyIntoSlot(const capture::CaptureBuffer& buffer, FrameSlot& slot);
    void uploadViaStagingBuffer(const capture::CaptureBuffer& buffer, FrameSlot& slot);
    // Returns the number of frames that were actually queued to the display this cycle.
    int renderCycle(const FrameSlot& newest,
                    std::chrono::steady_clock::time_point computeStart,
                    std::chrono::steady_clock::time_point arrivalTime,
                    double framePeriodMs,
                    double producerJitterMs,
                    bool prepassAlreadySubmitted = false);

    Options options_;
    bool targetFpsAutoInit_ = false;
    bool needsTargetAlign_ = false;
    int resourceFailures_ = 0;

    std::unique_ptr<VulkanContext> vk_;
    std::unique_ptr<capture::PortalSession> portal_;
    std::unique_ptr<capture::DmaBufImageImporter> importer_;
    std::unique_ptr<capture::PipeWireCapture> capture_;
    std::unique_ptr<compute::LsfgPipeline> lsfg_;
    std::unique_ptr<present::OverlayWindow> overlay_;
    std::unique_ptr<present::ControlPanel> controlPanel_;

    capture::CaptureTarget currentTarget_{};
    bool sessionActive_ = false;
    bool generating_ = false;
    int newestSlot_ = -1;
    uint64_t lsfgFrameIndex_ = 0;

    FrameSlot slots_[2];
    uint32_t frameWidth_ = 0;
    uint32_t frameHeight_ = 0;

    VkBuffer stagingBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
    void* stagingMapped_ = nullptr;
    VkDeviceSize stagingSize_ = 0;

    static constexpr uint32_t kThumbW = 64;
    static constexpr uint32_t kThumbH = 36;
    static constexpr VkDeviceSize kThumbBytes = static_cast<VkDeviceSize>(kThumbW) * kThumbH * 4u;
    VkImage thumbImage_ = VK_NULL_HANDLE;
    VkDeviceMemory thumbImageMemory_ = VK_NULL_HANDLE;
    VkBuffer thumbBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory thumbBufferMemory_ = VK_NULL_HANDLE;
    uint8_t* thumbMapped_ = nullptr;
    std::array<uint8_t, kThumbW * kThumbH * 4u> lastUniqueThumb_{};
    bool hasLastUniqueThumb_ = false;
    bool dedupEnabled_ = true;

    std::chrono::steady_clock::time_point lastUniqueArrival_{};
    bool hasLastUniqueArrival_ = false;
    std::array<double, 16> uniqueDeltas_{};
    size_t uniqueDeltaCount_ = 0;
    size_t uniqueDeltaHead_ = 0;
    int uniqueRegimeShiftCount_ = 0;
    bool hasSmoothedUniquePeriod_ = false;
    double smoothedUniquePeriodMs_ = 16.6667;
    double smoothedUniqueJitterMs_ = 0.0;
    uint32_t dupsDroppedInWindow_ = 0;

    bool forceLinearCapture_ = false;
    bool forceShmCapture_ = false;
    bool directImportFailed_ = false;
    bool loggedStagingFallback_ = false;

    // Display-aware & Adaptive pacing state (continuous PLL frame pacer).
    static constexpr int kMaxAdaptiveMultiplier = 42;
    int effectiveMultiplier_ = 0;     // multiplier actually used this cycle
    double adaptivePhaseMs_ = 0.0;    // continuous Adaptive cadence phase across real frames
    double adaptiveGpuCapMult_ = static_cast<double>(kMaxAdaptiveMultiplier);
    double firstPresentEmaMs_ = 0.0;  // smoothed GPU compute time per cycle
    double prepassEmaMs_ = 0.0;
    double genPassEmaMs_ = 0.0;
    std::chrono::steady_clock::time_point nextPresentTime_{};
    std::chrono::steady_clock::time_point lastActualPresent_{};
    double lastStepMs_ = 0.0;
    int lastPresentedOutputSlot_ = -1;
    uint64_t resourceEpoch_ = 0;
    std::chrono::steady_clock::time_point lastUiPollTime_{};

    // Overlapped next-frame prefetch & prepass state.
    bool prefetchedValid_ = false;
    int prefetchedSlot_ = -1;
    uint64_t prefetchedFidx_ = 0;
    double prefetchedPeriodMs_ = 16.6667;
    double prefetchedJitterMs_ = 0.0;
    std::chrono::steady_clock::time_point prefetchedArrival_{};
    std::chrono::steady_clock::time_point prefetchedStart_{};

    // 1-second output pacing telemetry (ms between consecutive presents).
    double paceMinMs_ = 1e9;
    double paceMaxMs_ = 0.0;
    double paceSumMs_ = 0.0;
    uint32_t paceCount_ = 0;

    std::atomic<bool> running_{true};
    std::atomic<int> pendingSignalAction_{0};
};

}  // namespace lfg
