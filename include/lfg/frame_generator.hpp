#pragma once

#include "lfg/capture/dmabuf_image_importer.hpp"
#include "lfg/capture/pipewire_capture.hpp"
#include "lfg/capture/portal_session.hpp"
#include "lfg/compute/lsfg_pipeline.hpp"
#include "lfg/options.hpp"
#include "lfg/present/control_panel.hpp"
#include "lfg/present/overlay_window.hpp"
#include "lfg/vulkan_context.hpp"

#include <atomic>
#include <chrono>
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
    bool waitUntilTime(std::chrono::steady_clock::time_point targetTime);

    bool ensureResources(uint32_t width, uint32_t height, const capture::CaptureTarget& target);
    void configureLsfgResources();
    void destroySlots();
    void ensureStagingBuffer(VkDeviceSize requiredBytes);
    void destroyStagingBuffer();
    void copyIntoSlot(const capture::CaptureBuffer& buffer, FrameSlot& slot);
    void uploadViaStagingBuffer(const capture::CaptureBuffer& buffer, FrameSlot& slot);
    // Returns the number of frames that were actually queued to the display this cycle.
    int renderCycle(const FrameSlot& newest,
                    std::chrono::steady_clock::time_point computeStart,
                    double framePeriodMs, double queueLagMs);
    int computeEffectiveMultiplier(double framePeriodMs);

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
    bool forceLinearCapture_ = false;
    bool forceShmCapture_ = false;
    bool directImportFailed_ = false;
    bool loggedStagingFallback_ = false;

    // Display-aware & Adaptive pacing state (PLL frame pacer).
    static constexpr int kMaxAdaptiveMultiplier = 42;
    int effectiveMultiplier_ = 0;     // multiplier actually used this cycle
    double adaptiveAccum_ = 0.0;      // fractional cadence accumulator for Adaptive FG
    double adaptiveGpuCapMult_ = static_cast<double>(kMaxAdaptiveMultiplier);
    double firstPresentEmaMs_ = 0.0;  // smoothed GPU compute time per cycle
    std::chrono::steady_clock::time_point lastPresentTime_{};
    double lastStepMs_ = 0.0;
    int lastPresentedOutputSlot_ = -1;
    uint64_t resourceEpoch_ = 0;
    std::chrono::steady_clock::time_point lastUiPollTime_{};

    std::atomic<bool> running_{true};
    std::atomic<int> pendingSignalAction_{0};
};

}  // namespace lfg
