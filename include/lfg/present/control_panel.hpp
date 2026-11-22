#pragma once

#include <gio/gio.h>
#include <xcb/xcb.h>

#include <cstdint>
#include <string>
#include <vector>

namespace lfg::present {

enum class ControlAction {
    None,
    Start,
    Stop,
    Toggle,
    CycleMode,
    SetMode2X,
    SetMode3X,
    SetMode4X,
    SetModeAdaptive,
    TargetFpsDown,
    CycleTargetFps,
    TargetFpsUp,
    FlowScaleDown,
    FlowScaleUp,
    FlowScaleChanged,
    TogglePerformanceMode,
    CycleGpu,
    SelectWindow,
    CycleDisplay,
    ToggleFps,
    ToggleVsync,
    RebindHotkey,
    SwitchTabMain,
    SwitchTabSettings,
    EditDllPath,
    BrowseDllFolder,
    BrowseDllFile,
    PasteDllPath,
    ApplyDllPath,
    DllPathChanged,
    AutoDetectDllPath,
    MinimizeToTray,
    Quit,
};

class ControlPanel {
public:
    ControlPanel(int initialDisplay, int initialMultiplier, bool initialAdaptive, int initialTargetFps,
                 bool initialShowFps, const std::string& initialHotkey = "",
                 const std::string& initialGpuLabel = "[0] AUTO",
                 int initialWidth = 460, int initialHeight = 520,
                 int initialFlowScale = 100, bool initialPerformanceMode = false,
                 const std::string& initialDllPath = "",
                 bool initialVsync = false);
    ~ControlPanel();

    ControlPanel(const ControlPanel&) = delete;
    ControlPanel& operator=(const ControlPanel&) = delete;

    ControlAction pollEvents();

    void showFromTray();
    void hideToTray();
    void toggleTrayWindow();
    bool isWindowVisible() const { return windowVisible_; }

    void setGenerating(bool active);
    void setFps(int capFps, int outFps);
    void setDisplayMode(int displayIndex);
    void setMode(bool adaptive, int multiplier);
    void setTargetFps(int targetFps);
    void setFlowScale(int flowScale);
    int flowScale() const { return flowScale_; }
    void setPerformanceMode(bool perf);
    bool performanceMode() const { return performanceMode_; }
    void setLsfgActive(bool active);
    void setDllPath(const std::string& dllPath);
    const std::string& dllPath() const { return dllPath_; }
    void setGpuLabel(const std::string& gpuLabel);
    void setStatusNotice(const std::string& notice);
    void setShowFps(bool show);
    void setVsync(bool enabled);
    bool vsync() const { return vsync_; }
    void draw();

    const std::string& hotkeyLabel() const { return hotkeyLabel_; }
    int windowWidth() const { return static_cast<int>(winW_); }
    int windowHeight() const { return static_cast<int>(winH_); }
    bool consumeConfigChanged() {
        bool changed = configChanged_;
        configChanged_ = false;
        return changed;
    }

    // D-Bus callbacks for StatusNotifierItem & dbusmenu
    void onSniMethodCall(const char* method, GVariant* parameters, GDBusMethodInvocation* invocation);
    GVariant* onSniGetProperty(const char* property);
    void onMenuMethodCall(const char* method, GVariant* parameters, GDBusMethodInvocation* invocation);
    GVariant* onMenuGetProperty(const char* property);

private:
    struct Button {
        int16_t x, y;
        uint16_t w, h;
        std::string label;
        uint32_t bgColor;
        uint32_t borderColor;
        ControlAction action;
        bool activeHighlight = false;
        std::string tooltip;
    };

    void initSystemTray();
    void teardownSystemTray();
    void registerWithTrayWatcher();
    void emitTraySignals();
    GVariant* buildMenuLayoutVariant() const;

    void setupGlobalHotkeys(xcb_window_t root);
    void applyHotkeyString(const std::string& spec);
    void loadHotkeyConfig();
    void saveHotkeyConfig() const;

    xcb_keycode_t findKeycode(uint32_t keysym, xcb_keycode_t fallbackEvdev);
    uint32_t keycodeToKeysym(xcb_keycode_t kc) const;
    uint32_t keycodeToKeysymWithState(xcb_keycode_t kc, uint16_t state) const;
    static std::string keysymToName(uint32_t sym);
    static uint32_t nameToKeysym(const std::string& name);

    void recreateBackbuffer();
    void fillRect(int16_t x, int16_t y, uint16_t w, uint16_t h, uint32_t rgb);
    void drawRectBorder(int16_t x, int16_t y, uint16_t w, uint16_t h, uint32_t rgb);
    void drawText(int16_t x, int16_t y, const std::string& text, uint32_t rgb, int scale = 2);
    void rebuildButtons();
    bool updateFlowScaleFromMouseX(int16_t mx);
    bool isWindowMinimizedByWM() const;
    void setSkipTaskbar(bool skip);

    xcb_connection_t* conn_ = nullptr;
    xcb_screen_t* screen_ = nullptr;
    xcb_window_t window_ = 0;
    xcb_pixmap_t backbuf_ = 0;
    xcb_gcontext_t gc_ = 0;
    xcb_atom_t wmDeleteWindow_ = XCB_ATOM_NONE;
    xcb_atom_t wmStateAtom_ = XCB_ATOM_NONE;
    xcb_atom_t netWmStateAtom_ = XCB_ATOM_NONE;
    xcb_atom_t netWmStateHiddenAtom_ = XCB_ATOM_NONE;
    xcb_atom_t netWmStateSkipTaskbarAtom_ = XCB_ATOM_NONE;
    xcb_atom_t netWmStateSkipPagerAtom_ = XCB_ATOM_NONE;
    xcb_atom_t netActiveWindowAtom_ = XCB_ATOM_NONE;
    bool windowVisible_ = true;
    uint16_t winW_ = 460;
    uint16_t winH_ = 520;
    uint16_t backbufW_ = 0;
    uint16_t backbufH_ = 0;
    int hoverIndex_ = -1;
    bool configChanged_ = false;
    bool draggingFlowSlider_ = false;
    bool settingsTab_ = false;
    bool editingDllPath_ = false;

    // System Tray (StatusNotifierItem + dbusmenu over GDBus)
    GDBusConnection* dbusConn_ = nullptr;
    GDBusNodeInfo* sniNodeInfo_ = nullptr;
    GDBusNodeInfo* menuNodeInfo_ = nullptr;
    guint sniRegId_ = 0;
    guint menuRegId_ = 0;
    guint watcherSubId_ = 0;
    std::string sniBusName_;
    uint32_t menuRevision_ = 1;
    ControlAction pendingTrayAction_ = ControlAction::None;

    xcb_keycode_t toggleKeycode_ = 75;              // default F9
    uint16_t toggleMods_ = XCB_MOD_MASK_1;          // default Alt (Mod1)
    std::string hotkeyLabel_ = "ALT+F9";
    bool waitingForHotkey_ = false;
    bool isHotkeyHeld_ = false;
    uint32_t lastHotkeyPressTime_ = 0;
    uint32_t lastHotkeyReleaseTime_ = 0;

    xcb_keycode_t keyF12_ = 96;

    bool generating_ = false;
    int capFps_ = 0;
    int outFps_ = 0;
    int displayIndex_ = -1;
    int multiplier_ = 2;
    bool adaptive_ = false;
    int targetFps_ = 120;
    int flowScale_ = 100;
    bool performanceMode_ = false;
    bool lsfgActive_ = false;
    std::string dllPath_;
    std::string dllEditBuffer_;
    int16_t flowSliderX_ = 0;
    int16_t flowSliderY_ = 0;
    uint16_t flowSliderW_ = 0;
    uint16_t flowSliderH_ = 0;
    std::string gpuLabel_ = "[0] AUTO";
    std::string statusNotice_;
    bool showFps_ = true;
    bool vsync_ = false;

    std::vector<Button> buttons_;

    static constexpr uint16_t kMinWinW = 340;
    static constexpr uint16_t kMinWinH = 380;
};

}  // namespace lfg::present
