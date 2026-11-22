#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace lfg {

inline constexpr int kMaxMultiplier = 20;
inline constexpr int kMaxTargetFps = 1000;

struct Options {
    bool validation = false;          // --validation     : enable the Khronos validation layer
    bool nvidia = false;              // --nvidia         : run on the NVIDIA GPU (experimental cross-GPU capture)
    int gpuIndex = -1;                // --gpu N          : select GPU index N (0, 1, ...; -1 = auto)
    bool monitorCapture = false;      // --monitor        : capture a whole monitor instead of one window
    int displayIndex = -1;            // --display N      : force overlay onto monitor index N (-1 = auto)
    int multiplier = 2;               // --multiplier N   : fixed frame generation multiplier (2..20)
    bool adaptive = false;            // --adaptive       : adaptive frame generation mode (aims for targetFps)
    int targetFps = 0;                // --target-fps N   : target FPS in adaptive mode (0 = auto-match monitor Hz or 120, max 1000)
    int flowScale = 100;              // --flow-scale N   : optical flow resolution scale % (25..100, default 100)
    bool performanceMode = false;     // --performance    : LSFG Performance Mode (lighter shader network)
    std::string dllPath = "";         // --dll PATH       : explicit path to Lossless.dll (auto-detected if empty)
    std::string toggleHotkey = "";    // --hotkey KEY     : custom start/stop toggle shortcut (e.g. "Alt+F9", "F8")
    bool showFps = true;              // --no-fps         : disable the on-screen FPS overlay
    bool vsync = false;               // --vsync/--no-vsync: lock presentation to monitor Hz (FIFO) vs uncapped
    bool autoStart = false;           // --autostart      : immediately open window picker and start on launch
    int runSeconds = 0;               // --seconds N      : auto-exit after N seconds (0 = run until quit)
    int panelWidth = 460;             // persisted Control Panel width
    int panelHeight = 520;            // persisted Control Panel height
};

inline std::filesystem::path settingsFilePath() {
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".config" / "lfg-linux" / "settings.conf";
    }
    return "/tmp/lfg-settings.conf";
}

inline void saveSettings(const Options& opts) {
    auto path = settingsFilePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    // Atomic staging write (inspired by app_processor / LosslessScaling safe persistence):
    // write to a temporary staging file first, verify stream integrity, and atomically swap.
    auto stagingPath = path;
    stagingPath += ".staging.tmp";
    {
        std::ofstream out(stagingPath, std::ios::trunc);
        if (!out.is_open()) return;

        out << "# LFG Linux persistent settings\n"
            << "gpu_index=" << opts.gpuIndex << "\n"
            << "nvidia=" << (opts.nvidia ? 1 : 0) << "\n"
            << "monitor_capture=" << (opts.monitorCapture ? 1 : 0) << "\n"
            << "display_index=" << opts.displayIndex << "\n"
            << "multiplier=" << std::clamp(opts.multiplier, 2, kMaxMultiplier) << "\n"
            << "adaptive=" << (opts.adaptive ? 1 : 0) << "\n"
            << "target_fps=" << opts.targetFps << "\n"
            << "flow_scale=" << std::clamp(opts.flowScale, 25, 100) << "\n"
            << "performance_mode=" << (opts.performanceMode ? 1 : 0) << "\n"
            << "dll_path=" << opts.dllPath << "\n"
            << "toggle_hotkey=" << opts.toggleHotkey << "\n"
            << "show_fps=" << (opts.showFps ? 1 : 0) << "\n"
            << "vsync=" << (opts.vsync ? 1 : 0) << "\n"
            << "panel_width=" << std::clamp(opts.panelWidth, 340, 1600) << "\n"
            << "panel_height=" << std::clamp(opts.panelHeight, 380, 1400) << "\n"
            << "# END_LFG_SETTINGS\n";
        out.flush();
        if (out.fail()) {
            out.close();
            std::filesystem::remove(stagingPath, ec);
            return;
        }
    }
    std::filesystem::rename(stagingPath, path, ec);
    if (ec) {
        std::filesystem::copy_file(stagingPath, path, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(stagingPath, ec);
    }
}

inline bool loadSavedSettings(Options& opts) {
    auto path = settingsFilePath();
    std::ifstream in(path);
    if (!in.is_open()) return false;

    Options parsed = opts;
    int validPairs = 0;
    bool malformedLine = false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            malformedLine = true;
            break;
        }
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "gpu_index")            { parsed.gpuIndex = std::atoi(val.c_str()); ++validPairs; }
        else if (key == "nvidia")          { parsed.nvidia = (std::atoi(val.c_str()) != 0); ++validPairs; }
        else if (key == "monitor_capture") { parsed.monitorCapture = (std::atoi(val.c_str()) != 0); ++validPairs; }
        else if (key == "display_index")   { parsed.displayIndex = std::atoi(val.c_str()); ++validPairs; }
        else if (key == "multiplier")      { parsed.multiplier = std::clamp(std::atoi(val.c_str()), 2, kMaxMultiplier); ++validPairs; }
        else if (key == "adaptive")        { parsed.adaptive = (std::atoi(val.c_str()) != 0); ++validPairs; }
        else if (key == "target_fps")      { parsed.targetFps = std::clamp(std::atoi(val.c_str()), 0, kMaxTargetFps); ++validPairs; }
        else if (key == "flow_scale")      { parsed.flowScale = std::clamp(std::atoi(val.c_str()), 25, 100); ++validPairs; }
        else if (key == "performance_mode"){ parsed.performanceMode = (std::atoi(val.c_str()) != 0); ++validPairs; }
        else if (key == "dll_path")        { if (!val.empty()) parsed.dllPath = val; ++validPairs; }
        else if (key == "toggle_hotkey")   { if (!val.empty()) parsed.toggleHotkey = val; ++validPairs; }
        else if (key == "show_fps")        { parsed.showFps = (std::atoi(val.c_str()) != 0); ++validPairs; }
        else if (key == "vsync")           { parsed.vsync = (std::atoi(val.c_str()) != 0); ++validPairs; }
        else if (key == "panel_width")     { parsed.panelWidth = std::clamp(std::atoi(val.c_str()), 340, 1600); ++validPairs; }
        else if (key == "panel_height")    { parsed.panelHeight = std::clamp(std::atoi(val.c_str()), 380, 1400); ++validPairs; }
    }
    in.close();

    // Corruption recovery (mirrors LosslessScaling.dll CorruptedSettingsBackup + CreateSettings):
    if (malformedLine || validPairs == 0) {
        std::error_code ec;
        auto backupPath = path.parent_path() / "CorruptedSettingsBackup.conf";
        std::filesystem::copy_file(path, backupPath, std::filesystem::copy_options::overwrite_existing, ec);
        saveSettings(opts);
        return false;
    }

    opts = parsed;
    return true;
}

}  // namespace lfg
