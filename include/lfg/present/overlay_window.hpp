#pragma once

#include "lfg/vulkan_context.hpp"

#include <xcb/xcb.h>

#include <cstdint>
#include <vector>

namespace lfg::present {

struct TargetRect {
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
};

class OverlayWindow {
public:
    OverlayWindow(VulkanContext& vk, const TargetRect& rect, int preferredDisplay = -1,
                  bool showFps = true, bool vsync = false);
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    static std::vector<const char*> requiredInstanceExtensions();

    void show();
    void hide();
    bool isVisible() const { return mapped_; }

    void setShowFps(bool show) { showFps_ = show; }
    bool showFps() const { return showFps_; }

    void setVsync(bool enabled);
    bool vsync() const { return vsync_; }

    void setPreferredDisplay(int displayIndex, const TargetRect& fallbackRect);
    int preferredDisplay() const { return preferredDisplay_; }
    size_t monitorCount() const { return monitors_.size(); }

    // Centers the target windowed application on the active monitor so its real input region
    // aligns with the 1:1 unstretched window image surrounded by black bars.
    void alignWindowedTarget(uint32_t capWidth, uint32_t capHeight);

    void updateTargetRect(const TargetRect& rect);
    void setFps(float captureFps, float outputFps);
    // Returns true only if the image was actually queued for presentation (so callers can count
    // REAL presented frames instead of assuming every generated frame reached the screen).
    bool presentImage(VkImage image, VkImageLayout srcLayout, uint32_t width, uint32_t height);
    void waitForIdle();

    VkExtent2D extent() const { return swapchainExtent_; }

    // Refresh rate of the monitor the overlay is on (RandR), or LFG_REFRESH_HZ if set.
    // 0 = unknown. Frame generation can never show more distinct frames than this.
    double refreshRateHz() const { return refreshHz_; }
    double refreshPeriodMs() const { return refreshHz_ > 1.0 ? 1000.0 / refreshHz_ : 0.0; }

private:
    TargetRect resolveTargetMonitor(const TargetRect& requested, int preferredDisplay);
    void createXcbWindow(const TargetRect& rect, int preferredDisplay);
    void makeClickThroughAndBorderless();
    void createSurfaceAndSwapchain();
    void recreateSwapchain();
    void destroySwapchain();

    void createHudBuffer();
    void destroyHudBuffer();
    void renderHudBitmap(int capFps, int outFps);

    VulkanContext& vk_;

    xcb_connection_t* connection_ = nullptr;
    xcb_window_t window_ = 0;
    int screenNumber_ = 0;
    bool mapped_ = false;
    int preferredDisplay_ = -1;
    TargetRect monitorRect_{};
    std::vector<TargetRect> monitors_;
    std::vector<double> monitorHz_;
    double refreshHz_ = 0.0;
    bool vsync_ = false;
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;

    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D swapchainExtent_{};
    VkFormat swapchainFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    std::vector<VkImage> swapchainImages_;

    std::vector<VkSemaphore> imageAvailable_;
    std::vector<VkSemaphore> renderFinished_;
    uint32_t syncIndex_ = 0;
    VkFence inFlightFence_ = VK_NULL_HANDLE;
    // Last blit rectangle per swapchain image: each image keeps its own contents, so each one needs
    // its own black clear when the captured size or position changes.
    std::vector<TargetRect> imageBlitRect_;
    int32_t targetDstX_ = 0;
    int32_t targetDstY_ = 0;
    bool hasTargetPos_ = false;
    VkCommandBuffer blitCmd_ = VK_NULL_HANDLE;

    bool showFps_ = true;
    static constexpr uint32_t kHudWidth = 168;
    static constexpr uint32_t kHudHeight = 22;
    VkBuffer hudBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory hudMemory_ = VK_NULL_HANDLE;
    uint32_t* hudMapped_ = nullptr;
    int lastCapFps_ = -1;
    int lastOutFps_ = -1;
    std::vector<VkBufferImageCopy> hudCopies_;
};

}  // namespace lfg::present
