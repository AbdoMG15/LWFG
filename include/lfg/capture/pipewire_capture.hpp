#pragma once

#include "lfg/capture/dmabuf_image_importer.hpp"
#include "lfg/capture/portal_session.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

namespace lfg::capture {

struct CaptureBuffer {
    pw_buffer* pwBuffer = nullptr;
    std::vector<DmaBufPlane> planes;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool isDmaBuf = true;
    bool hasExplicitModifier = false;
    uint64_t modifier = 0;
    const void* mappedData = nullptr;
    uint32_t dataSize = 0;
    uint32_t generation = 0;
    std::chrono::steady_clock::time_point arrivalTime{};
    double producerPeriodMs = 16.67;
    double producerJitterMs = 0.0;
};

enum class WaitStatus { Frame, Timeout, Failed };

class PipeWireCapture {
public:
    explicit PipeWireCapture(DmaBufImageImporter& importer);
    ~PipeWireCapture();

    PipeWireCapture(const PipeWireCapture&) = delete;
    PipeWireCapture& operator=(const PipeWireCapture&) = delete;

    void start(const CaptureTarget& target, bool linearModifierOnly = false, bool forceShmOnly = false);
    void stop();

    WaitStatus waitForBuffer(int timeoutMs, CaptureBuffer& out);
    bool tryTakeBuffer(CaptureBuffer& out);
    bool hasPendingFrame() const { return pendingReady_.load(std::memory_order_acquire); }
    void releaseBuffer(const CaptureBuffer& buffer);

    std::string failureMessage();

    static void onStateChanged(void* data, pw_stream_state old, pw_stream_state state, const char* error);
    static void onParamChanged(void* data, uint32_t id, const struct spa_pod* param);
    static void onProcess(void* data);
    static void onRemoveBuffer(void* data, struct pw_buffer* buffer);

private:
    void handleParamChanged(uint32_t id, const struct spa_pod* param);
    void handleProcess();
    void handleRemoveBuffer(struct pw_buffer* buffer);
    void buildEnumFormatParams(spa_pod_builder& b, std::vector<const spa_pod*>& outParams) const;

    DmaBufImageImporter& importer_;

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    spa_hook streamListener_{};

    bool linearModifierOnly_ = false;
    bool forceShmOnly_ = false;
    std::vector<uint64_t> modifiers_;

    VkFormat vkFormat_ = VK_FORMAT_UNDEFINED;
    bool formatNegotiated_ = false;
    bool hasExplicitModifier_ = false;
    uint64_t negotiatedModifier_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    int fixationAttempts_ = 0;

    std::chrono::steady_clock::time_point lastProducerFrame_{};
    bool hasLastProducerFrame_ = false;
    bool hasSmoothedPeriod_ = false;
    double smoothedProducerPeriodMs_ = 16.67;
    double smoothedProducerJitterMs_ = 0.0;
    std::array<double, 16> recentDeltas_{};
    size_t deltaCount_ = 0;
    size_t deltaHead_ = 0;
    int regimeShiftCount_ = 0;

    std::atomic<uint32_t> generation_{0};

    std::mutex mutex_;
    std::condition_variable cv_;
    bool pendingValid_ = false;
    std::atomic<bool> pendingReady_{false};
    CaptureBuffer pending_;
    bool failed_ = false;
    std::string failureMessage_;
};

}  // namespace lfg::capture
