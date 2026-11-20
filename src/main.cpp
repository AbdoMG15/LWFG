#include "lfg/frame_generator.hpp"
#include "lfg/options.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/file.h>
#include <unistd.h>

namespace {

constexpr const char* kPidFile = "/tmp/lfg-linux.pid";
constexpr const char* kLockFile = "/tmp/lfg-linux.lock";
int g_lockFd = -1;
lfg::FrameGenerator* g_generator = nullptr;

void handleStopSignal(int) {
    if (g_generator) g_generator->stop();
}

void handleUsr1Signal(int) {
    if (g_generator) g_generator->requestStartGenerating();
}

void handleUsr2Signal(int) {
    if (g_generator) g_generator->requestStopGenerating();
}

void handleHupSignal(int) {
    if (g_generator) g_generator->requestToggleGenerating();
}

void handleContSignal(int) {
    if (g_generator) g_generator->requestShowWindow();
}

pid_t readRunningPid() {
    std::ifstream in(kPidFile);
    pid_t pid = 0;
    if (in >> pid && pid > 0 && pid != ::getpid()) {
        if (::kill(pid, 0) == 0) return pid;
    }
    return 0;
}

bool acquireSingleInstanceLock() {
    g_lockFd = ::open(kLockFile, O_CREAT | O_RDWR, 0666);
    if (g_lockFd < 0) return true;
    if (::flock(g_lockFd, LOCK_EX | LOCK_NB) != 0) {
        ::close(g_lockFd);
        g_lockFd = -1;
        return false;
    }
    return true;
}

void writePidFile() {
    std::ofstream out(kPidFile, std::ios::trunc);
    out << ::getpid() << "\n";
}

void removePidFile() {
    ::unlink(kPidFile);
    if (g_lockFd >= 0) {
        ::flock(g_lockFd, LOCK_UN);
        ::close(g_lockFd);
        g_lockFd = -1;
    }
}

void printUsage(const char* program) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  --toggle         toggle frame generation Start/Stop on the running instance\n"
        "  --start          start generating frames on the running instance\n"
        "  --stop           stop/pause generating frames on the running instance\n"
        "  --quit           close the running LFG instance\n"
        "  --multiplier N   fixed frame generation multiplier: 2..20 (default: 2)\n"
        "  --adaptive       enable Adaptive Frame Generation mode (aims for --target-fps)\n"
        "  --target-fps N   target framerate for Adaptive mode (30..1000, default: monitor Hz or 120)\n"
        "  --flow-scale N   optical flow resolution scale percentage: 25..100 (default: 100)\n"
        "  --performance    enable Lossless Scaling LSFG Performance Mode (lighter shader network)\n"
        "  --dll PATH       explicit path to Lossless.dll (auto-detected from Steam if omitted)\n"
        "  --hotkey COMBO   set custom Start/Stop shortcut (e.g. Alt+F9, F8, Ctrl+F10)\n"
        "  --autostart      immediately open the window picker and start generating on launch\n"
        "  --display N      place overlay on monitor N (0, 1, ...; default: auto-detect single monitor)\n"
        "  --no-fps         hide the top-left on-screen FPS counter\n"
        "  --vsync          lock overlay presentation to monitor Hz (FIFO VSync)\n"
        "  --no-vsync       uncap overlay presentation past monitor Hz (IMMEDIATE/MAILBOX, default)\n"
        "  --gpu N          select Vulkan GPU index N (0, 1, ...; see startup list)\n"
        "  --nvidia         run on the NVIDIA GPU (same as selecting the NVIDIA GPU in GUI)\n"
        "  --monitor        capture a whole monitor instead of one window\n"
        "  --seconds N      exit automatically after N seconds (default 0 = run until closed)\n"
        "  --validation     enable the Vulkan validation layer\n"
        "  --help           show this text\n",
        program);
}

}  // namespace

int main(int argc, char** argv) {
    ::setenv("DISABLE_LSFGVK", "1", 0);
    // Clear any inherited PRIME / Optimus offload variables so Vulkan sees both the
    // AMD APU and NVIDIA dGPU and never silently hijacks the APU device onto the dGPU.
    ::unsetenv("__VK_LAYER_NV_optimus");
    ::unsetenv("__NV_PRIME_RENDER_OFFLOAD");
    ::unsetenv("DRI_PRIME");

    lfg::Options options;
    const bool hadSavedSettings = lfg::loadSavedSettings(options);

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--start") {
            if (pid_t pid = readRunningPid()) {
                ::kill(pid, SIGUSR1);
                std::fprintf(stderr, "[lfg] sent START signal to running LFG instance (PID %d)\n", pid);
                return 0;
            }
            options.autoStart = true;
        } else if (arg == "--stop") {
            if (pid_t pid = readRunningPid()) {
                ::kill(pid, SIGUSR2);
                std::fprintf(stderr, "[lfg] sent STOP signal to running LFG instance (PID %d)\n", pid);
            } else {
                std::fprintf(stderr, "[lfg] no running LFG instance found\n");
            }
            return 0;
        } else if (arg == "--toggle") {
            if (pid_t pid = readRunningPid()) {
                ::kill(pid, SIGHUP);
                std::fprintf(stderr, "[lfg] sent TOGGLE signal to running LFG instance (PID %d)\n", pid);
                return 0;
            }
            options.autoStart = true;
        } else if (arg == "--quit") {
            if (pid_t pid = readRunningPid()) {
                ::kill(pid, SIGTERM);
                std::fprintf(stderr, "[lfg] sent QUIT signal to running LFG instance (PID %d)\n", pid);
            }
            return 0;
        } else if (arg == "--multiplier" && i + 1 < argc) {
            int m = std::atoi(argv[++i]);
            options.multiplier = std::clamp(m, 2, lfg::kMaxMultiplier);
            options.adaptive = false;
        } else if (arg == "--adaptive") {
            options.adaptive = true;
        } else if (arg == "--target-fps" && i + 1 < argc) {
            options.targetFps = std::atoi(argv[++i]);
            options.adaptive = true;
        } else if (arg == "--flow-scale" && i + 1 < argc) {
            options.flowScale = std::clamp(std::atoi(argv[++i]), 25, 100);
        } else if (arg == "--performance") {
            options.performanceMode = true;
        } else if (arg == "--dll" && i + 1 < argc) {
            options.dllPath = argv[++i];
        } else if (arg == "--hotkey" && i + 1 < argc) {
            options.toggleHotkey = argv[++i];
        } else if (arg == "--autostart") {
            options.autoStart = true;
        } else if (arg == "--gpu" && i + 1 < argc) {
            options.gpuIndex = std::atoi(argv[++i]);
            options.nvidia = false;
        } else if (arg == "--nvidia") {
            options.nvidia = true;
            options.gpuIndex = -1;
        } else if (arg == "--monitor") {
            options.monitorCapture = true;
        } else if (arg == "--validation") {
            options.validation = true;
        } else if (arg == "--no-fps") {
            options.showFps = false;
        } else if (arg == "--vsync") {
            options.vsync = true;
        } else if (arg == "--no-vsync") {
            options.vsync = false;
        } else if (arg == "--display" && i + 1 < argc) {
            options.displayIndex = std::atoi(argv[++i]);
        } else if (arg == "--seconds" && i + 1 < argc) {
            options.runSeconds = std::atoi(argv[++i]);
            if (options.runSeconds < 0) options.runSeconds = 0;
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "Unknown or incomplete option: %s\n\n", arg.c_str());
            printUsage(argv[0]);
            return 2;
        }
    }

    if (!acquireSingleInstanceLock() || readRunningPid() != 0) {
        pid_t existing = readRunningPid();
        std::fprintf(stderr, "[lfg] already running (PID %d) — restoring window from system tray\n", existing);
        if (existing > 0) ::kill(existing, SIGCONT);
        return 0;
    }

    writePidFile();
    std::fprintf(stderr, "[lwfg] LWFG (Linux Windowed Frame Generation) — version 0.1.5 (Lossless Scaling LSFG 3.1 Steam Engine)\n");
    if (hadSavedSettings) {
        std::fprintf(stderr, "[lfg] loaded saved settings from %s\n", lfg::settingsFilePath().string().c_str());
    }

    try {
        lfg::FrameGenerator generator(options);
        g_generator = &generator;
        std::signal(SIGINT, handleStopSignal);
        std::signal(SIGTERM, handleStopSignal);
        std::signal(SIGUSR1, handleUsr1Signal);
        std::signal(SIGUSR2, handleUsr2Signal);
        std::signal(SIGHUP, handleHupSignal);
        std::signal(SIGCONT, handleContSignal);

        generator.run();
        g_generator = nullptr;
    } catch (const std::exception& e) {
        removePidFile();
        std::fprintf(stderr, "[lfg] fatal: %s\n", e.what());
        return 1;
    }

    removePidFile();
    return 0;
}
