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
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace lfg {

FrameGenerator::FrameGenerator(const Options& options) : options_(options) {
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
        overlay_.reset();
        lsfg_.reset();
        importer_.reset();
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
    for (auto& slot : slots_) {
        if (slot.view) vkDestroyImageView(vk_->device(), slot.view, nullptr);
        if (slot.image) vkDestroyImage(vk_->device(), slot.image, nullptr);
        if (slot.memory) vkFreeMemory(vk_->device(), slot.memory, nullptr);
        slot = FrameSlot{};
    }
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

void FrameGenerator::stopCaptureSession() {
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
    lastPresentTime_ = {};
    lastStepMs_ = 0.0;
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
            generating_ = true;
            newestSlot_ = -1;
            lsfgFrameIndex_ = 0;
            effectiveMultiplier_ = 0;
            adaptiveAccum_ = 0.0;
            adaptiveGpuCapMult_ = static_cast<double>(kMaxAdaptiveMultiplier);
            firstPresentEmaMs_ = 0.0;
            lastPresentTime_ = {};
            lastStepMs_ = 0.0;
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
            lastPresentTime_ = {};
            lastStepMs_ = 0.0;
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
            adaptiveAccum_ = 0.0;
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
            adaptiveAccum_ = 0.0;
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
            adaptiveAccum_ = 0.0;
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
            adaptiveAccum_ = 0.0;
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
            adaptiveAccum_ = 0.0;
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
            adaptiveAccum_ = 0.0;
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
            adaptiveAccum_ = 0.0;
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
            adaptiveAccum_ = 0.0;
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
                vkDeviceWaitIdle(vk_->device());
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
                vkDeviceWaitIdle(vk_->device());
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
    vkDeviceWaitIdle(vk_->device());
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

        // Transition both slots to VK_IMAGE_LAYOUT_GENERAL upfront so LSFG descriptor bindings are valid immediately
        {
            VkCommandBuffer cmd = vk_->beginOneShotCommands(vk_->graphicsCommandPool());
            VkImageMemoryBarrier barriers[2]{};
            for (int i = 0; i < 2; ++i) {
                barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barriers[i].srcAccessMask = 0;
                barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
                barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barriers[i].image = slots_[i].image;
                barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            }
            vkCmdPipelineBarrier(cmd,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 2, barriers);
            vk_->endOneShotCommands(vk_->graphicsCommandPool(), vk_->graphicsPresentQueue(), cmd);
        }

        frameWidth_ = width;
        frameHeight_ = height;

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
    vkDeviceWaitIdle(vk_->device());
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

    VkImageMemoryBarrier2 toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toGeneral.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toGeneral.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toGeneral.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    toGeneral.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.image = slot.image;
    toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep2.imageMemoryBarrierCount = 1;
    dep2.pImageMemoryBarriers = &toGeneral;
    vkCmdPipelineBarrier2(cmd, &dep2);

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

                VkImageMemoryBarrier2 toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                toGeneral.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
                toGeneral.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                toGeneral.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                toGeneral.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toGeneral.image = slot.image;
                toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep2.imageMemoryBarrierCount = 1;
                dep2.pImageMemoryBarriers = &toGeneral;
                vkCmdPipelineBarrier2(cmd, &dep2);

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

bool FrameGenerator::waitUntilTime(std::chrono::steady_clock::time_point targetTime) {
    using Clock = std::chrono::steady_clock;

    while (running_ && generating_) {
        auto now = Clock::now();
        if (now >= targetTime) break;

        if (lastUiPollTime_.time_since_epoch().count() == 0 ||
            now - lastUiPollTime_ >= std::chrono::milliseconds(8)) {
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
        }

        auto remUs = std::chrono::duration_cast<std::chrono::microseconds>(targetTime - now).count();
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
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
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
                std::chrono::duration<double, std::milli>(computeStart - lastRealFrameTime).count() < 90.0) {
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
            preCopy[0].image = slots_[srcSlot].image;
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
            preCopy[1].image = slots_[dstSlot].image;
            preCopy[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

            VkDependencyInfo depPre{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            depPre.imageMemoryBarrierCount = 2;
            depPre.pImageMemoryBarriers = preCopy;
            vkCmdPipelineBarrier2(copyCmd, &depPre);

            VkImageCopy imgCopy{};
            imgCopy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            imgCopy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            imgCopy.extent = {frameWidth_, frameHeight_, 1};
            vkCmdCopyImage(copyCmd,
                           slots_[srcSlot].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           slots_[dstSlot].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
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
            postCopy[0].image = slots_[srcSlot].image;
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
            postCopy[1].image = slots_[dstSlot].image;
            postCopy[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

            VkDependencyInfo depPost{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            depPost.imageMemoryBarrierCount = 2;
            depPost.pImageMemoryBarriers = postCopy;
            vkCmdPipelineBarrier2(copyCmd, &depPost);
            vk_->endOneShotCommands(vk_->graphicsCommandPool(), vk_->graphicsPresentQueue(), copyCmd);

            newestSlot_ = dstSlot;
            if ((lsfgFrameIndex_ & 1u) != static_cast<uint64_t>(newestSlot_)) {
                ++lsfgFrameIndex_;
            }

            const double steadyPeriodMs = 16.6667;
            const int presented = renderCycle(slots_[newestSlot_], computeStart, steadyPeriodMs, 0.0);
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
                fpsWindowStart = afterCycle;
                capturedInWindow = 0;
                presentedInWindow = 0;
            }
            continue;
        }

        if (!generating_ || !lsfg_ || !lsfg_->isAvailable()) {
            capture_->releaseBuffer(buffer);
            continue;
        }

        const auto computeStart = std::chrono::steady_clock::now();
        lastRealFrameTime = computeStart;
        double queueLagMs = 0.0;
        if (buffer.arrivalTime.time_since_epoch().count() != 0 &&
            buffer.arrivalTime <= computeStart &&
            computeStart - buffer.arrivalTime < std::chrono::milliseconds(85)) {
            queueLagMs = std::chrono::duration<double, std::milli>(computeStart - buffer.arrivalTime).count();
        }
        double producerPeriodMs = std::clamp(buffer.producerPeriodMs, 4.0, 100.0);

        bool resourcesReady = false;
        try {
            if (ensureResources(buffer.width, buffer.height, currentTarget_)) {
                newestSlot_ = -1;
                lastPresentTime_ = {};
                lastStepMs_ = 0.0;
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
        try {
            const int slotIndex = (newestSlot_ == 0) ? 1 : 0;
            copyIntoSlot(buffer, slots_[slotIndex]);
            newestSlot_ = slotIndex;
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
                        lastPresentTime_ = std::chrono::steady_clock::now();
                        lastStepMs_ = producerPeriodMs * 0.5;
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

        const int presented = renderCycle(slots_[newestSlot_], computeStart, producerPeriodMs, queueLagMs);
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

            if (std::chrono::duration<double>(afterCycle - termLogStart).count() >= 1.0) {
                const double hz = overlay_ ? overlay_->refreshRateHz() : 0.0;
                if (options_.adaptive) {
                    std::fprintf(stderr, "[lfg] ADAPTIVE (target %d, %dx, flow %d%%) FPS (%s): %.1f -> %.1f (source period %.1f ms, "
                                 "display %.0f Hz, gpu synth %.1f ms)\n",
                                 options_.targetFps, effectiveMultiplier_ > 0 ? effectiveMultiplier_ : 1,
                                 options_.flowScale, vk_->selectedGpuInfo().shortLabel.c_str(),
                                 capFps, outFps, producerPeriodMs, hz, firstPresentEmaMs_);
                } else {
                    std::fprintf(stderr, "[lfg] %dX (flow %d%%) FPS (%s): %.1f -> %.1f (source period %.1f ms, "
                                 "display %.0f Hz, gpu synth %.1f ms)\n",
                                 effectiveMultiplier_ > 0 ? effectiveMultiplier_ : options_.multiplier,
                                 options_.flowScale, vk_->selectedGpuInfo().shortLabel.c_str(), capFps, outFps,
                                 producerPeriodMs, hz, firstPresentEmaMs_);
                }
                termLogStart = afterCycle;
            }
            fpsWindowStart = afterCycle;
            capturedInWindow = 0;
            presentedInWindow = 0;
        }
    }

    std::fprintf(stderr, "[lfg] stopped after %llu cycle(s)\n", static_cast<unsigned long long>(cycles));
    stopCaptureSession();
}

int FrameGenerator::computeEffectiveMultiplier(double framePeriodMs) {
    // 1. Adaptive Mode: dynamically vary per-cycle multiplier (1..42) via fractional accumulator
    //    so average output framerate locks onto `options_.targetFps` (e.g. 24 -> 500+ FPS).
    if (options_.adaptive) {
        double targetFps = std::clamp(static_cast<double>(options_.targetFps), 30.0, static_cast<double>(kMaxTargetFps));
        double targetPeriodMs = 1000.0 / targetFps;
        double gpuCap = std::clamp(adaptiveGpuCapMult_, 2.0, static_cast<double>(kMaxAdaptiveMultiplier));
        double desiredRatio = std::clamp(framePeriodMs / targetPeriodMs, 1.0, gpuCap);

        if (desiredRatio <= 1.05) {
            adaptiveAccum_ = 0.0;
            effectiveMultiplier_ = 1;
            return 1;
        }

        double total = desiredRatio + adaptiveAccum_;
        int mult = std::clamp(static_cast<int>(std::floor(total + 1e-4)), 1, kMaxAdaptiveMultiplier);
        adaptiveAccum_ = std::clamp(total - static_cast<double>(mult), 0.0, 0.9999);
        effectiveMultiplier_ = mult;
        return mult;
    }

    // 2. Fixed Multiplier Mode (2X..20X):
    //    Always honor the user's selected multiplier so both generated and real frames are produced.
    const int requested = std::clamp(options_.multiplier, 2, kMaxMultiplier);
    effectiveMultiplier_ = requested;
    return requested;
}

int FrameGenerator::renderCycle(const FrameSlot& newest,
                                std::chrono::steady_clock::time_point computeStart,
                                double framePeriodMs, double queueLagMs) {
    using Clock = std::chrono::steady_clock;
    auto toDuration = [](double ms) {
        return std::chrono::microseconds(static_cast<int64_t>(ms * 1000.0));
    };

    if (!overlay_ || !lsfg_ || !lsfg_->isAvailable() || !lsfg_->isConfigured()) return 0;
    const int mult = computeEffectiveMultiplier(framePeriodMs);

    if (mult <= 1) {
        VkCommandBuffer preCmd = vk_->beginOneShotCommands(vk_->graphicsCommandPool());
        lsfg_->recordPrepassOnly(preCmd, lsfgFrameIndex_++);
        vk_->endOneShotCommands(vk_->graphicsCommandPool(), vk_->graphicsPresentQueue(), preCmd);
        if (!overlay_ || newestSlot_ < 0) return 0;
        bool ok = overlay_->presentImage(newest.image, VK_IMAGE_LAYOUT_GENERAL, frameWidth_, frameHeight_);
        lastPresentedOutputSlot_ = -1;
        if (ok) {
            lastPresentTime_ = Clock::now();
            lastStepMs_ = framePeriodMs;
        }
        return ok ? 1 : 0;
    }

    struct AsyncGuard {
        FrameGenerator* self;
        void wait() {
            if (self->vk_ && self->vk_->isOneShotAsyncPending()) {
                self->vk_->waitOneShotAsync();
            }
        }
        ~AsyncGuard() { wait(); }
    } asyncCompute{this};

    const uint64_t fidx = lsfgFrameIndex_++;
    const uint64_t cycleEpoch = resourceEpoch_;
    const int genCount = mult - 1;
    const int totalPresents = genCount + 1;

    // Kick off Prepass + first intermediate frame (slot 0) asynchronously while the previous real frame displays.
    if (lastPresentedOutputSlot_ == 0) {
        overlay_->waitForIdle();
        lastPresentedOutputSlot_ = -1;
    }
    {
        VkCommandBuffer firstCmd = vk_->beginOneShotCommands(vk_->graphicsCommandPool());
        lsfg_->recordPrepassOnly(firstCmd, fidx);
        const float t0 = 1.0f / static_cast<float>(totalPresents);
        lsfg_->recordGeneratePass(firstCmd, fidx, t0, 0u);
        vk_->submitOneShotAsync(vk_->graphicsPresentQueue(), firstCmd);
    }

    // Phase-Locked Loop (PLL) Frame Pacer:
    // Divide the source frame period evenly across all (genCount + 1) output frames with sub-ms support:
    //   lastRealPresent -> +stepMs -> Gen_1 -> +stepMs -> ... -> Gen_K -> +stepMs -> Real_cur
    const double idealStepMs = framePeriodMs / static_cast<double>(totalPresents);
    const double pllTrimMs = std::clamp((queueLagMs - 0.8) / static_cast<double>(totalPresents + 1),
                                        0.0, idealStepMs * 0.18);
    const double stepMs = std::max(0.25, idealStepMs - pllTrimMs);
    const double cycleTargetMs = std::max(1.0, framePeriodMs - pllTrimMs * static_cast<double>(totalPresents));

    int presented = 0;
    bool preempted = false;
    for (int k = 0; k < totalPresents; ++k) {
        const bool isReal = (k == totalPresents - 1);
        const auto nowBeforeWait = Clock::now();
        const double elapsedCycleMs =
            std::chrono::duration<double, std::milli>(nowBeforeWait - computeStart).count();
        const double remainingCycleMs = std::max(0.0, cycleTargetMs - elapsedCycleMs);
        const double budgetStepMs = remainingCycleMs / static_cast<double>(totalPresents - k);
        const double waitIntervalMs =
            (k == 0)
                ? ((lastStepMs_ > 0.0) ? std::min(lastStepMs_, stepMs) : stepMs)
                : std::max(0.25, std::min(stepMs, budgetStepMs));

        const bool hasAnchor =
            (lastPresentTime_.time_since_epoch().count() != 0) &&
            (std::chrono::duration<double, std::milli>(nowBeforeWait - lastPresentTime_).count() < 120.0);
        const auto targetPresentTime =
            hasAnchor ? (lastPresentTime_ + toDuration(waitIntervalMs)) : nowBeforeWait;

        if (hasAnchor && nowBeforeWait < targetPresentTime) {
            if (!waitUntilTime(targetPresentTime)) return presented;
        }
        if (!running_ || !generating_ || resourceEpoch_ != cycleEpoch || !overlay_ || newestSlot_ < 0) {
            return presented;
        }

        if (!isReal) {
            asyncCompute.wait();
            if (k == 0) {
                const double firstGenMs =
                    std::chrono::duration<double, std::milli>(Clock::now() - computeStart).count();
                firstPresentEmaMs_ =
                    (firstPresentEmaMs_ <= 0.0) ? firstGenMs : firstPresentEmaMs_ * 0.9 + firstGenMs * 0.1;
            }
        }

        bool ok;
        if (isReal) {
            ok = overlay_->presentImage(newest.image, VK_IMAGE_LAYOUT_GENERAL, frameWidth_, frameHeight_);
            if (ok) lastPresentedOutputSlot_ = -1;
        } else {
            const uint32_t outSlot = static_cast<uint32_t>(k) & 1u;
            const auto& gen = lsfg_->output(outSlot);
            ok = overlay_->presentImage(gen.image, VK_IMAGE_LAYOUT_GENERAL, gen.width, gen.height);
            if (ok) lastPresentedOutputSlot_ = static_cast<int>(outSlot);
        }

        if (ok) {
            ++presented;
            const auto now = Clock::now();
            const auto maxDrift = std::chrono::microseconds(
                std::max<int64_t>(1500, static_cast<int64_t>(stepMs * 1000.0)));
            if (now > targetPresentTime && (now - targetPresentTime) < maxDrift) {
                lastPresentTime_ = targetPresentTime;
            } else {
                lastPresentTime_ = now;
            }
            lastStepMs_ = stepMs;
        } else {
            overlay_->waitForIdle();
            lastPresentedOutputSlot_ = -1;
        }

        // Synthesize the next intermediate frame on the alternate ping-pong slot while `k` displays!
        if (!isReal && (k + 1) < (totalPresents - 1)) {
            const double nowCycleMs =
                std::chrono::duration<double, std::milli>(Clock::now() - computeStart).count();
            const bool pendingReal = (capture_ && capture_->hasPendingFrame());
            if ((pendingReal && nowCycleMs >= framePeriodMs * 1.02) || nowCycleMs >= framePeriodMs * 1.25) {
                preempted = true;
                k = totalPresents - 2;  // next loop iteration presents the current Real frame immediately
                continue;
            }

            const float tNext = static_cast<float>(k + 2) / static_cast<float>(totalPresents);
            const uint32_t nextSlot = static_cast<uint32_t>(k + 1) & 1u;
            if (lastPresentedOutputSlot_ == static_cast<int>(nextSlot)) {
                overlay_->waitForIdle();
                lastPresentedOutputSlot_ = -1;
            }
            VkCommandBuffer nextCmd = vk_->beginOneShotCommands(vk_->graphicsCommandPool());
            lsfg_->recordGeneratePass(nextCmd, fidx, tNext, nextSlot);
            vk_->submitOneShotAsync(vk_->graphicsPresentQueue(), nextCmd);
        }
    }

    const double totalCycleMs =
        std::chrono::duration<double, std::milli>(Clock::now() - computeStart).count();
    if (options_.adaptive) {
        if (preempted) {
            const double fit = std::max(2.0, static_cast<double>(presented));
            const double cur = std::min(adaptiveGpuCapMult_, static_cast<double>(mult));
            adaptiveGpuCapMult_ = std::clamp(cur * 0.6 + fit * 0.4, 2.0, static_cast<double>(kMaxAdaptiveMultiplier));
        } else if (totalCycleMs <= framePeriodMs * 1.06) {
            adaptiveGpuCapMult_ = std::min(static_cast<double>(kMaxAdaptiveMultiplier), adaptiveGpuCapMult_ + 0.5);
        }
    }

    return presented;
}

}  // namespace lfg
