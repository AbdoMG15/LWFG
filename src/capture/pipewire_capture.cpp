#include "lfg/capture/pipewire_capture.hpp"

#include <spa/param/param.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/utils/defs.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace lfg::capture {

namespace {

constexpr spa_video_format kSpaFormatCandidates[] = {
    SPA_VIDEO_FORMAT_BGRA,
    SPA_VIDEO_FORMAT_BGRx,
    SPA_VIDEO_FORMAT_RGBA,
    SPA_VIDEO_FORMAT_RGBx,
};
constexpr uint64_t kDrmModLinear = 0;

VkFormat spaFormatToVk(spa_video_format f) {
    switch (f) {
        case SPA_VIDEO_FORMAT_BGRA:
        case SPA_VIDEO_FORMAT_BGRx:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case SPA_VIDEO_FORMAT_RGBA:
        case SPA_VIDEO_FORMAT_RGBx:
            return VK_FORMAT_R8G8B8A8_UNORM;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

struct LoopLock {
    explicit LoopLock(pw_thread_loop* l) : loop(l) { pw_thread_loop_lock(loop); }
    ~LoopLock() { pw_thread_loop_unlock(loop); }
    pw_thread_loop* loop;
};

const pw_stream_events kStreamEvents = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = PipeWireCapture::onStateChanged;
    events.param_changed = PipeWireCapture::onParamChanged;
    events.process = PipeWireCapture::onProcess;
    events.remove_buffer = PipeWireCapture::onRemoveBuffer;
    return events;
}();

}  // namespace

PipeWireCapture::PipeWireCapture(DmaBufImageImporter& importer) : importer_(importer) {
    pw_init(nullptr, nullptr);
}

PipeWireCapture::~PipeWireCapture() {
    stop();
    pw_deinit();
}

void PipeWireCapture::buildEnumFormatParams(spa_pod_builder& b, std::vector<const spa_pod*>& outParams) const {
    auto addSizeAndRate = [&]() {
        spa_rectangle defSize = SPA_RECTANGLE(width_, height_);
        spa_rectangle minSize = SPA_RECTANGLE(1, 1);
        spa_rectangle maxSize = SPA_RECTANGLE(8192, 4320);
        spa_fraction defRate = SPA_FRACTION(120, 1);
        spa_fraction minRate = SPA_FRACTION(0, 1);
        spa_fraction maxRate = SPA_FRACTION(1000, 1);
        spa_pod_builder_add(&b,
            SPA_FORMAT_VIDEO_size,      SPA_POD_CHOICE_RANGE_Rectangle(&defSize, &minSize, &maxSize),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&defRate, &minRate, &maxRate),
            0);
    };

    for (spa_video_format candidate : kSpaFormatCandidates) {
        if (!modifiers_.empty()) {
            spa_pod_frame objFrame, choiceFrame;
            spa_pod_builder_push_object(&b, &objFrame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
            spa_pod_builder_add(&b,
                SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
                SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                SPA_FORMAT_VIDEO_format, SPA_POD_Id(candidate),
                0);
            spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_modifier,
                                 SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
            spa_pod_builder_push_choice(&b, &choiceFrame, SPA_CHOICE_Enum, 0);
            spa_pod_builder_long(&b, static_cast<int64_t>(modifiers_[0]));
            for (uint64_t m : modifiers_) spa_pod_builder_long(&b, static_cast<int64_t>(m));
            spa_pod_builder_pop(&b, &choiceFrame);
            addSizeAndRate();
            if (auto* pod = static_cast<const spa_pod*>(spa_pod_builder_pop(&b, &objFrame))) {
                outParams.push_back(pod);
            }
        }
        {
            spa_pod_frame objFrame;
            spa_pod_builder_push_object(&b, &objFrame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
            spa_pod_builder_add(&b,
                SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
                SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                SPA_FORMAT_VIDEO_format, SPA_POD_Id(candidate),
                0);
            addSizeAndRate();
            if (auto* pod = static_cast<const spa_pod*>(spa_pod_builder_pop(&b, &objFrame))) {
                outParams.push_back(pod);
            }
        }
    }
}

void PipeWireCapture::start(const CaptureTarget& target, bool linearModifierOnly, bool forceShmOnly) {
    linearModifierOnly_ = linearModifierOnly || forceShmOnly;
    forceShmOnly_ = forceShmOnly;
    hasLastProducerFrame_ = false;
    hasSmoothedPeriod_ = false;
    smoothedProducerPeriodMs_ = 16.67;
    pendingReady_.store(false, std::memory_order_release);
    fixationAttempts_ = 0;
    width_ = static_cast<uint32_t>(target.width > 0 ? target.width : 1920);
    height_ = static_cast<uint32_t>(target.height > 0 ? target.height : 1080);

    if (forceShmOnly_) {
        modifiers_.clear();
    } else {
        modifiers_ = importer_.queryImportableModifiers(VK_FORMAT_B8G8R8A8_UNORM);
        if (linearModifierOnly_) {
            bool hasLinear = std::find(modifiers_.begin(), modifiers_.end(), kDrmModLinear) != modifiers_.end();
            if (hasLinear) {
                modifiers_ = {kDrmModLinear};
            } else {
                modifiers_.clear();
            }
        }
    }

    loop_ = pw_thread_loop_new("lfg-capture", nullptr);
    if (!loop_) throw std::runtime_error("pw_thread_loop_new failed");

    if (pw_thread_loop_start(loop_) < 0) throw std::runtime_error("pw_thread_loop_start failed");

    LoopLock lock(loop_);

    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_) throw std::runtime_error("pw_context_new failed");

    if (target.pipewireFd >= 0) {
        core_ = pw_context_connect_fd(context_, ::dup(target.pipewireFd), nullptr, 0);
    } else {
        core_ = pw_context_connect(context_, nullptr, 0);
    }
    if (!core_) throw std::runtime_error("pw_context_connect failed");

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        nullptr);

    stream_ = pw_stream_new(core_, "lfg-linux-capture", props);
    if (!stream_) throw std::runtime_error("pw_stream_new failed");

    pw_stream_add_listener(stream_, &streamListener_, &kStreamEvents, this);

    std::vector<uint8_t> buffer(65536);
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer.data(), static_cast<uint32_t>(buffer.size()));
    std::vector<const spa_pod*> formatParams;
    buildEnumFormatParams(b, formatParams);

    std::fprintf(stderr, "[pipewire] connecting to portal node %u: %zu format alternative(s), %zu modifier(s)\n",
                 target.nodeId, formatParams.size(), modifiers_.size());

    int connectResult = pw_stream_connect(
        stream_, PW_DIRECTION_INPUT, target.nodeId,
        PW_STREAM_FLAG_AUTOCONNECT,
        formatParams.data(), static_cast<uint32_t>(formatParams.size()));
    if (connectResult < 0) throw std::runtime_error("pw_stream_connect failed");
}

void PipeWireCapture::stop() {
    pendingReady_.store(false, std::memory_order_release);
    if (loop_) {
        pw_thread_loop_stop(loop_);
        if (stream_) pw_stream_destroy(stream_);
        if (core_) pw_core_disconnect(core_);
        if (context_) pw_context_destroy(context_);
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
        stream_ = nullptr;
        core_ = nullptr;
        context_ = nullptr;
    }
}

WaitStatus PipeWireCapture::waitForBuffer(int timeoutMs, CaptureBuffer& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return pendingValid_ || failed_; });
    if (failed_) return WaitStatus::Failed;
    if (!pendingValid_) return WaitStatus::Timeout;
    pendingValid_ = false;
    pendingReady_.store(false, std::memory_order_release);
    if (pending_.generation != generation_.load()) return WaitStatus::Timeout;
    out = pending_;
    return WaitStatus::Frame;
}

void PipeWireCapture::releaseBuffer(const CaptureBuffer& buffer) {
    if (!loop_ || !buffer.pwBuffer) return;
    LoopLock lock(loop_);
    if (stream_ && buffer.generation == generation_.load()) pw_stream_queue_buffer(stream_, buffer.pwBuffer);
}

std::string PipeWireCapture::failureMessage() {
    std::lock_guard<std::mutex> lock(mutex_);
    return failureMessage_;
}

void PipeWireCapture::onStateChanged(void* data, pw_stream_state old, pw_stream_state state, const char* error) {
    auto* self = static_cast<PipeWireCapture*>(data);
    std::fprintf(stderr, "[pipewire] stream state: %s -> %s%s%s\n", pw_stream_state_as_string(old),
                 pw_stream_state_as_string(state), error ? " : " : "", error ? error : "");
    const bool disconnected = state == PW_STREAM_STATE_UNCONNECTED && old != PW_STREAM_STATE_UNCONNECTED;
    if (state == PW_STREAM_STATE_ERROR || disconnected) {
        std::lock_guard<std::mutex> lock(self->mutex_);
        self->failed_ = true;
        self->failureMessage_ = error ? error : "the stream was disconnected";
        self->cv_.notify_all();
    }
}

void PipeWireCapture::onParamChanged(void* data, uint32_t id, const struct spa_pod* param) {
    static_cast<PipeWireCapture*>(data)->handleParamChanged(id, param);
}

void PipeWireCapture::onProcess(void* data) {
    static_cast<PipeWireCapture*>(data)->handleProcess();
}

void PipeWireCapture::onRemoveBuffer(void* data, struct pw_buffer* buffer) {
    static_cast<PipeWireCapture*>(data)->handleRemoveBuffer(buffer);
}

void PipeWireCapture::handleRemoveBuffer(struct pw_buffer* buffer) {
    generation_.fetch_add(1);
    std::lock_guard<std::mutex> lock(mutex_);
    if (pendingValid_ && pending_.pwBuffer == buffer) {
        pendingValid_ = false;
        pendingReady_.store(false, std::memory_order_release);
    }
}

void PipeWireCapture::handleParamChanged(uint32_t id, const struct spa_pod* param) {
    if (id != SPA_PARAM_Format) return;

    formatNegotiated_ = false;
    if (!param) return;

    uint32_t mediaType = 0, mediaSubtype = 0;
    if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0) return;
    if (mediaType != SPA_MEDIA_TYPE_video || mediaSubtype != SPA_MEDIA_SUBTYPE_raw) return;

    spa_video_info_raw info{};
    if (spa_format_video_raw_parse(param, &info) < 0) return;

    std::vector<uint8_t> buffer(65536);
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer.data(), static_cast<uint32_t>(buffer.size()));
    std::vector<const spa_pod*> params;

    const spa_pod_prop* modProp = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier);
    if (modProp && (modProp->flags & SPA_POD_PROP_FLAG_DONT_FIXATE) && fixationAttempts_ < 8) {
        ++fixationAttempts_;
        uint32_t nValues = 0;
        uint32_t choiceType = 0;
        const spa_pod* valuesPod = spa_pod_get_values(&modProp->value, &nValues, &choiceType);
        uint64_t chosenMod = info.modifier;
        bool foundMatch = false;
        if (valuesPod && SPA_POD_TYPE(valuesPod) == SPA_TYPE_Long && nValues > 0) {
            const auto* modList = static_cast<const int64_t*>(SPA_POD_BODY(valuesPod));
            chosenMod = static_cast<uint64_t>(modList[0]);
            for (uint32_t i = 0; i < nValues; ++i) {
                uint64_t candidateMod = static_cast<uint64_t>(modList[i]);
                if (std::find(modifiers_.begin(), modifiers_.end(), candidateMod) != modifiers_.end()) {
                    chosenMod = candidateMod;
                    foundMatch = true;
                    break;
                }
            }
        }

        spa_rectangle defSize = (info.size.width > 0 && info.size.height > 0)
                                    ? info.size
                                    : SPA_RECTANGLE(width_, height_);
        spa_rectangle minSize = SPA_RECTANGLE(1, 1);
        spa_rectangle maxSize = SPA_RECTANGLE(8192, 4320);
        spa_fraction defRate = (info.framerate.denom > 0 && info.framerate.num > 0)
                                   ? info.framerate
                                   : SPA_FRACTION(120, 1);
        spa_fraction minRate = SPA_FRACTION(0, 1);
        spa_fraction maxRate = SPA_FRACTION(1000, 1);

        if ((foundMatch || !linearModifierOnly_) && !forceShmOnly_) {
            spa_pod_frame objFrame;
            spa_pod_builder_push_object(&b, &objFrame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
            spa_pod_builder_add(&b,
                SPA_FORMAT_mediaType,       SPA_POD_Id(SPA_MEDIA_TYPE_video),
                SPA_FORMAT_mediaSubtype,    SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                SPA_FORMAT_VIDEO_format,    SPA_POD_Id(info.format),
                SPA_FORMAT_VIDEO_size,      SPA_POD_CHOICE_RANGE_Rectangle(&defSize, &minSize, &maxSize),
                SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&defRate, &minRate, &maxRate),
                0);
            spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
            spa_pod_builder_long(&b, static_cast<int64_t>(chosenMod));
            if (auto* fixatedPod = static_cast<const spa_pod*>(spa_pod_builder_pop(&b, &objFrame))) {
                params.push_back(fixatedPod);
            }
            buildEnumFormatParams(b, params);
        } else {
            // In cross-GPU mode (e.g. NVIDIA dGPU with iGPU compositor), if the compositor didn't offer
            // linear DRM modifier 0, fall back to modifier-less format so KWin supplies linear/SHM buffers.
            modifiers_.clear();
            buildEnumFormatParams(b, params);
        }

        pw_stream_update_params(stream_, params.data(), static_cast<uint32_t>(params.size()));
        return;
    }

    hasExplicitModifier_ = (info.flags & SPA_VIDEO_FLAG_MODIFIER) != 0;
    negotiatedModifier_ = hasExplicitModifier_ ? info.modifier : 0;

    // Never accept a non-linear modifier when operating in cross-GPU or SHM-only mode;
    // renegotiate modifier-less shared memory (MemFd/MemPtr) immediately.
    if ((linearModifierOnly_ || forceShmOnly_) && hasExplicitModifier_ &&
        (forceShmOnly_ || negotiatedModifier_ != kDrmModLinear) && fixationAttempts_ < 8) {
        ++fixationAttempts_;
        modifiers_.clear();
        buildEnumFormatParams(b, params);
        pw_stream_update_params(stream_, params.data(), static_cast<uint32_t>(params.size()));
        return;
    }

    fixationAttempts_ = 0;
    width_ = info.size.width;
    height_ = info.size.height;
    vkFormat_ = spaFormatToVk(info.format);

    char modifierText[40] = "none (implicit/linear)";
    if (hasExplicitModifier_)
        std::snprintf(modifierText, sizeof(modifierText), "0x%llx",
                      static_cast<unsigned long long>(negotiatedModifier_));
    std::fprintf(stderr, "[pipewire] negotiated format=%d size=%ux%u modifier=%s\n",
                 static_cast<int>(info.format), width_, height_, modifierText);

    if (vkFormat_ == VK_FORMAT_UNDEFINED) {
        std::fprintf(stderr, "[pipewire] unsupported pixel format %d\n", static_cast<int>(info.format));
        return;
    }

    formatNegotiated_ = true;

    uint32_t blocks = hasExplicitModifier_
                          ? importer_.queryModifierPlaneCount(vkFormat_, negotiatedModifier_)
                          : 1u;

    const bool allowDmaBuf = !forceShmOnly_ &&
                             (!linearModifierOnly_ || (hasExplicitModifier_ && negotiatedModifier_ == kDrmModLinear));
    const int32_t allowedDataTypes = allowDmaBuf
        ? ((1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))
        : ((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr));
    const spa_pod* bufferParam = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(6, 3, 8),
        SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(static_cast<int32_t>(blocks)),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(allowedDataTypes)));
    params.push_back(bufferParam);
    pw_stream_update_params(stream_, params.data(), static_cast<uint32_t>(params.size()));
}

void PipeWireCapture::handleProcess() {
    pw_buffer* latest = nullptr;
    while (pw_buffer* b = pw_stream_dequeue_buffer(stream_)) {
        if (latest) pw_stream_queue_buffer(stream_, latest);
        latest = b;
    }
    if (!latest) return;

    spa_buffer* sb = latest->buffer;
    if (!formatNegotiated_ || sb->n_datas < 1 || !sb->datas[0].chunk ||
        sb->datas[0].chunk->size == 0 || sb->datas[0].chunk->stride == 0 ||
        (sb->datas[0].chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
        pw_stream_queue_buffer(stream_, latest);
        return;
    }

    const uint32_t dataType = sb->datas[0].type;
    const bool isDmaBuf = (dataType == SPA_DATA_DmaBuf);
    const bool isMemFd  = (dataType == SPA_DATA_MemFd);
    const bool isMemPtr = (dataType == SPA_DATA_MemPtr);
    if (!isDmaBuf && !isMemFd && !isMemPtr) {
        pw_stream_queue_buffer(stream_, latest);
        return;
    }
    if ((isDmaBuf || isMemFd) && sb->datas[0].fd < 0) {
        pw_stream_queue_buffer(stream_, latest);
        return;
    }
    if (isMemPtr && !sb->datas[0].data) {
        pw_stream_queue_buffer(stream_, latest);
        return;
    }

    CaptureBuffer cb;
    cb.pwBuffer = latest;
    cb.isDmaBuf = isDmaBuf;
    cb.mappedData = isMemPtr ? sb->datas[0].data : nullptr;
    const uint32_t mapOff0 = isMemFd ? sb->datas[0].mapoffset : 0u;
    cb.dataSize = mapOff0 + std::max(sb->datas[0].maxsize,
                                     sb->datas[0].chunk->offset + sb->datas[0].chunk->size);
    cb.planes.reserve(sb->n_datas);
    for (uint32_t i = 0; i < sb->n_datas && i < 4; ++i) {
        if (!sb->datas[i].chunk || (i == 0 && sb->datas[i].chunk->stride == 0)) break;
        if (i > 0 && sb->datas[i].chunk->stride == 0 && sb->datas[i].chunk->size == 0) break;
        DmaBufPlane plane;
        plane.fd = static_cast<int>(sb->datas[i].fd >= 0 ? sb->datas[i].fd : sb->datas[0].fd);
        const uint32_t baseMapOffset = isMemFd ? sb->datas[i].mapoffset : 0u;
        plane.offset = baseMapOffset + sb->datas[i].chunk->offset;
        plane.stride = static_cast<uint32_t>(sb->datas[i].chunk->stride);
        cb.planes.push_back(plane);
    }
    if (cb.planes.empty()) {
        pw_stream_queue_buffer(stream_, latest);
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (hasLastProducerFrame_) {
        double deltaMs = std::chrono::duration<double, std::milli>(now - lastProducerFrame_).count();
        // Measure the true producer interval on the PipeWire thread (isolated from main-thread sleeps)
        if (deltaMs >= 3.5 && deltaMs <= 120.0) {
            if (!hasSmoothedPeriod_) {
                smoothedProducerPeriodMs_ = deltaMs;
                hasSmoothedPeriod_ = true;
            } else {
                smoothedProducerPeriodMs_ = smoothedProducerPeriodMs_ * 0.82 + deltaMs * 0.18;
            }
        }
    }
    lastProducerFrame_ = now;
    hasLastProducerFrame_ = true;

    cb.width = width_;
    cb.height = height_;
    cb.format = vkFormat_;
    cb.hasExplicitModifier = hasExplicitModifier_;
    cb.modifier = negotiatedModifier_;
    cb.generation = generation_.load();
    cb.arrivalTime = now;
    cb.producerPeriodMs = smoothedProducerPeriodMs_;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pendingValid_) pw_stream_queue_buffer(stream_, pending_.pwBuffer);
        pending_ = cb;
        pendingValid_ = true;
        pendingReady_.store(true, std::memory_order_release);
    }
    cv_.notify_one();
}

}  // namespace lfg::capture
