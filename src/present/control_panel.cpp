#include "lfg/present/control_panel.hpp"
#include "lfg/present/app_icon.hpp"
#include "lfg/options.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace lfg::present {

namespace {

const char* kSniIntrospectionXml = R"XML(
<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="i" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="OverlayIconName" type="s" access="read"/>
    <property name="AttentionIconName" type="s" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <method name="ContextMenu">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="Activate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="SecondaryActivate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="Scroll">
      <arg name="delta" type="i" direction="in"/>
      <arg name="orientation" type="s" direction="in"/>
    </method>
    <signal name="NewTitle"/>
    <signal name="NewIcon"/>
    <signal name="NewAttentionIcon"/>
    <signal name="NewOverlayIcon"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus">
      <arg name="status" type="s"/>
    </signal>
  </interface>
</node>
)XML";

const char* kDbusMenuIntrospectionXml = R"XML(
<node>
  <interface name="com.canonical.dbusmenu">
    <property name="Version" type="u" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
    <method name="GetLayout">
      <arg name="parentId" type="i" direction="in"/>
      <arg name="recursionDepth" type="i" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="revision" type="u" direction="out"/>
      <arg name="layout" type="(ia{sv}av)" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="properties" type="a(ia{sv})" direction="out"/>
    </method>
    <method name="GetProperty">
      <arg name="id" type="i" direction="in"/>
      <arg name="name" type="s" direction="in"/>
      <arg name="value" type="v" direction="out"/>
    </method>
    <method name="Event">
      <arg name="id" type="i" direction="in"/>
      <arg name="eventId" type="s" direction="in"/>
      <arg name="data" type="v" direction="in"/>
      <arg name="timestamp" type="u" direction="in"/>
    </method>
    <method name="EventGroup">
      <arg name="events" type="a(isvu)" direction="in"/>
      <arg name="idErrors" type="ai" direction="out"/>
    </method>
    <method name="AboutToShow">
      <arg name="id" type="i" direction="in"/>
      <arg name="needUpdate" type="b" direction="out"/>
    </method>
    <method name="AboutToShowGroup">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="updatesNeeded" type="ai" direction="out"/>
      <arg name="idErrors" type="ai" direction="out"/>
    </method>
    <signal name="ItemsPropertiesUpdated">
      <arg name="updatedProps" type="a(ia{sv})"/>
      <arg name="removedProps" type="a(ias)"/>
    </signal>
    <signal name="LayoutUpdated">
      <arg name="revision" type="u"/>
      <arg name="parent" type="i"/>
    </signal>
  </interface>
</node>
)XML";

xcb_atom_t internAtom(xcb_connection_t* c, const char* name) {
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(c, 0, static_cast<uint16_t>(std::strlen(name)), name);
    xcb_generic_error_t* err = nullptr;
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(c, cookie, &err);
    if (err) { free(err); return static_cast<xcb_atom_t>(XCB_ATOM_NONE); }
    xcb_atom_t atom = reply ? reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    free(reply);
    return atom;
}

std::string toUpper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::filesystem::path configFilePath() {
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".config" / "lfg-linux" / "hotkey.conf";
    }
    return "/tmp/lfg-hotkey.conf";
}

const uint8_t* glyph5x7Full(char c) {
    static const uint8_t kSpace[7] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00};
    static const uint8_t kDigits[10][7] = {
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
        {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
        {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
        {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // 3
        {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
        {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
        {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
        {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
        {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
        {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
    };
    static const uint8_t kAlpha[26][7] = {
        {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // A
        {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // B
        {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // C
        {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, // D
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // E
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // F
        {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, // G
        {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, // H
        {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, // I
        {0x07,0x02,0x02,0x02,0x12,0x12,0x0C}, // J
        {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // K
        {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // L
        {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // M
        {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, // N
        {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // O
        {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // P
        {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // Q
        {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // R
        {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E}, // S
        {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // T
        {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // U
        {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, // V
        {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, // W
        {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // X
        {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, // Y
        {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, // Z
    };
    static const uint8_t kLower[26][7] = {
        {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}, // a
        {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E}, // b
        {0x00,0x00,0x0E,0x11,0x10,0x11,0x0E}, // c
        {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F}, // d
        {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}, // e
        {0x06,0x08,0x1E,0x08,0x08,0x08,0x08}, // f
        {0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E}, // g
        {0x10,0x10,0x1E,0x11,0x11,0x11,0x11}, // h
        {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}, // i
        {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}, // j
        {0x10,0x10,0x12,0x14,0x18,0x14,0x12}, // k
        {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}, // l
        {0x00,0x00,0x1A,0x15,0x15,0x15,0x15}, // m
        {0x00,0x00,0x1E,0x11,0x11,0x11,0x11}, // n
        {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}, // o
        {0x00,0x1E,0x11,0x11,0x1E,0x10,0x10}, // p
        {0x00,0x0F,0x11,0x11,0x0F,0x01,0x01}, // q
        {0x00,0x00,0x16,0x19,0x10,0x10,0x10}, // r
        {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E}, // s
        {0x08,0x08,0x1E,0x08,0x08,0x09,0x06}, // t
        {0x00,0x00,0x11,0x11,0x11,0x13,0x0D}, // u
        {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}, // v
        {0x00,0x00,0x11,0x11,0x15,0x15,0x0A}, // w
        {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}, // x
        {0x00,0x11,0x11,0x11,0x0F,0x01,0x0E}, // y
        {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}, // z
    };
    static const uint8_t kColon[7]  = {0x00,0x04,0x04,0x00,0x04,0x04,0x00};
    static const uint8_t kDash[7]   = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
    static const uint8_t kPlus[7]   = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00};
    static const uint8_t kGt[7]     = {0x08,0x04,0x02,0x01,0x02,0x04,0x08};
    static const uint8_t kLt[7]     = {0x02,0x04,0x08,0x10,0x08,0x04,0x02};
    static const uint8_t kSlash[7]  = {0x01,0x02,0x02,0x04,0x08,0x08,0x10};
    static const uint8_t kBSlash[7] = {0x10,0x08,0x08,0x04,0x02,0x02,0x01};
    static const uint8_t kLParen[7] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02};
    static const uint8_t kRParen[7] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08};
    static const uint8_t kLBrack[7] = {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E};
    static const uint8_t kRBrack[7] = {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E};
    static const uint8_t kDot[7]    = {0x00,0x00,0x00,0x00,0x00,0x06,0x06};
    static const uint8_t kPipe[7]   = {0x04,0x04,0x04,0x04,0x04,0x04,0x04};
    static const uint8_t kUnder[7]  = {0x00,0x00,0x00,0x00,0x00,0x00,0x1F};
    static const uint8_t kComma[7]   = {0x00,0x00,0x00,0x00,0x04,0x04,0x08};
    static const uint8_t kBang[7]    = {0x04,0x04,0x04,0x04,0x04,0x00,0x04};
    static const uint8_t kPercent[7] = {0x19,0x19,0x02,0x04,0x08,0x13,0x13};
    static const uint8_t kTilde[7]   = {0x00,0x00,0x09,0x15,0x12,0x00,0x00};
    static const uint8_t kEq[7]      = {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00};
    static const uint8_t kQuest[7]   = {0x0E,0x11,0x01,0x02,0x04,0x00,0x04};
    static const uint8_t kQuote[7]   = {0x04,0x04,0x08,0x00,0x00,0x00,0x00};
    static const uint8_t kDQuote[7]  = {0x0A,0x0A,0x14,0x00,0x00,0x00,0x00};

    if (c >= '0' && c <= '9') return kDigits[c - '0'];
    if (c >= 'A' && c <= 'Z') return kAlpha[c - 'A'];
    if (c >= 'a' && c <= 'z') return kLower[c - 'a'];
    switch (c) {
        case ':': return kColon;
        case '-': return kDash;
        case '+': return kPlus;
        case '>': return kGt;
        case '<': return kLt;
        case '/': return kSlash;
        case '\\': return kBSlash;
        case '(': return kLParen;
        case ')': return kRParen;
        case '[': return kLBrack;
        case ']': return kRBrack;
        case '.': return kDot;
        case '|': return kPipe;
        case '_': return kUnder;
        case ',': return kComma;
        case '!': return kBang;
        case '%': return kPercent;
        case '~': return kTilde;
        case '=': return kEq;
        case '?': return kQuest;
        case '\'': return kQuote;
        case '"': return kDQuote;
        default:  return kSpace;
    }
}

uint32_t lightenColor(uint32_t rgb, int delta) {
    int r = std::clamp(static_cast<int>((rgb >> 16) & 0xFF) + delta, 0, 255);
    int g = std::clamp(static_cast<int>((rgb >> 8) & 0xFF) + delta, 0, 255);
    int b = std::clamp(static_cast<int>(rgb & 0xFF) + delta, 0, 255);
    return (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(b);
}

std::string fitTextToWidth(const std::string& s, int maxPx, int scale) {
    int charW = 6 * scale;
    if (charW <= 0 || maxPx <= 0) return {};
    size_t maxChars = static_cast<size_t>(maxPx / charW);
    if (s.size() <= maxChars) return s;
    if (maxChars <= 2) return s.substr(0, maxChars);
    return s.substr(0, maxChars - 2) + "..";
}

std::string trimWhitespace(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string sanitizePathInput(std::string s) {
    auto nl = s.find_first_of("\r\n");
    if (nl != std::string::npos) s.resize(nl);
    s = trimWhitespace(s);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\''))) {
        s = trimWhitespace(s.substr(1, s.size() - 2));
    }
    if (s.rfind("file://", 0) == 0) {
        s = s.substr(7);
        std::string decoded;
        decoded.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size() &&
                std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
                char hex[3] = {s[i + 1], s[i + 2], '\0'};
                decoded.push_back(static_cast<char>(std::strtol(hex, nullptr, 16)));
                i += 2;
            } else {
                decoded.push_back(s[i]);
            }
        }
        s = trimWhitespace(decoded);
    }
    return s;
}

std::string runCommandReadLine(const std::string& cmd) {
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return {};
    char buf[4096]{};
    std::string out;
    if (std::fgets(buf, sizeof(buf), pipe)) {
        out = buf;
    }
    int rc = ::pclose(pipe);
    if (rc != 0) return {};
    return sanitizePathInput(out);
}

std::string readClipboardText() {
    const char* cmds[] = {
        "wl-paste --no-newline 2>/dev/null",
        "xclip -o -selection clipboard 2>/dev/null",
        "xsel -ob 2>/dev/null",
        "xclip -o 2>/dev/null",
        "xsel -o 2>/dev/null",
    };
    for (const char* cmd : cmds) {
        std::string res = runCommandReadLine(cmd);
        if (!res.empty()) return res;
    }
    return {};
}

std::string shellQuoteSingle(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::string openNativeLosslessDialog(bool chooseFolder, const std::string& currentPath) {
    std::filesystem::path startDir;
    std::error_code ec;
    if (!currentPath.empty()) {
        std::filesystem::path cp(currentPath);
        if (std::filesystem::is_directory(cp, ec)) {
            startDir = cp;
        } else if (cp.has_parent_path() && std::filesystem::is_directory(cp.parent_path(), ec)) {
            startDir = cp.parent_path();
        }
    }
    if (startDir.empty()) {
        if (const char* home = std::getenv("HOME"); home && *home) {
            std::filesystem::path h(home);
            auto steamCommon1 = h / ".local" / "share" / "Steam" / "steamapps" / "common";
            auto steamCommon2 = h / ".steam" / "steam" / "steamapps" / "common";
            if (std::filesystem::is_directory(steamCommon1, ec)) startDir = steamCommon1;
            else if (std::filesystem::is_directory(steamCommon2, ec)) startDir = steamCommon2;
            else startDir = h;
        } else {
            startDir = "/";
        }
    }

    const std::string qStart = shellQuoteSingle(startDir.string());
    const std::string qStartSlash = shellQuoteSingle(startDir.string() + "/");

    if (chooseFolder) {
        std::string cmds[] = {
            "command -v kdialog >/dev/null 2>&1 && kdialog --title 'Select Lossless Scaling Folder' --getexistingdirectory " + qStart + " 2>/dev/null",
            "command -v zenity >/dev/null 2>&1 && zenity --file-selection --directory --title='Select Lossless Scaling Folder' --filename=" + qStartSlash + " 2>/dev/null",
            "command -v yad >/dev/null 2>&1 && yad --file --directory --title='Select Lossless Scaling Folder' --filename=" + qStartSlash + " 2>/dev/null",
        };
        for (const auto& cmd : cmds) {
            std::string picked = runCommandReadLine(cmd);
            if (!picked.empty()) return picked;
        }
    } else {
        std::string cmds[] = {
            "command -v kdialog >/dev/null 2>&1 && kdialog --title 'Select Lossless.dll' --getopenfilename " + qStart + " 'Lossless.dll (Lossless.dll *.dll *.DLL)|All Files (*)' 2>/dev/null",
            "command -v zenity >/dev/null 2>&1 && zenity --file-selection --title='Select Lossless.dll' --file-filter='Lossless.dll | Lossless.dll *.dll *.DLL' --file-filter='All files | *' --filename=" + qStartSlash + " 2>/dev/null",
            "command -v yad >/dev/null 2>&1 && yad --file --title='Select Lossless.dll' --filename=" + qStartSlash + " 2>/dev/null",
        };
        for (const auto& cmd : cmds) {
            std::string picked = runCommandReadLine(cmd);
            if (!picked.empty()) return picked;
        }
    }
    return {};
}

void sniMethodCallTrampoline(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                             const gchar* methodName, GVariant* parameters,
                             GDBusMethodInvocation* invocation, gpointer userData) {
    static_cast<ControlPanel*>(userData)->onSniMethodCall(methodName, parameters, invocation);
}

GVariant* sniGetPropertyTrampoline(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                                   const gchar* propertyName, GError**, gpointer userData) {
    return static_cast<ControlPanel*>(userData)->onSniGetProperty(propertyName);
}

void menuMethodCallTrampoline(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                              const gchar* methodName, GVariant* parameters,
                              GDBusMethodInvocation* invocation, gpointer userData) {
    static_cast<ControlPanel*>(userData)->onMenuMethodCall(methodName, parameters, invocation);
}

GVariant* menuGetPropertyTrampoline(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                                    const gchar* propertyName, GError**, gpointer userData) {
    return static_cast<ControlPanel*>(userData)->onMenuGetProperty(propertyName);
}

const GDBusInterfaceVTable kSniVTable = {
    sniMethodCallTrampoline,
    sniGetPropertyTrampoline,
    nullptr,
    {nullptr}
};

const GDBusInterfaceVTable kMenuVTable = {
    menuMethodCallTrampoline,
    menuGetPropertyTrampoline,
    nullptr,
    {nullptr}
};

}  // namespace

ControlPanel::ControlPanel(int initialDisplay, int initialMultiplier, bool initialAdaptive, int initialTargetFps,
                           bool initialShowFps, const std::string& initialHotkey,
                           const std::string& initialGpuLabel,
                           int initialWidth, int initialHeight,
                           int initialFlowScale, bool initialPerformanceMode,
                           const std::string& initialDllPath,
                           bool initialVsync)
    : winW_(static_cast<uint16_t>(std::clamp<int>(initialWidth, kMinWinW, 1920))),
      winH_(static_cast<uint16_t>(std::clamp<int>(initialHeight, kMinWinH, 1440))),
      displayIndex_(initialDisplay),
      multiplier_(std::clamp(initialMultiplier, 2, kMaxMultiplier)),
      adaptive_(initialAdaptive),
      targetFps_(initialTargetFps > 0 ? std::clamp(initialTargetFps, 30, kMaxTargetFps) : 120),
      flowScale_(std::clamp(initialFlowScale, 25, 100)),
      performanceMode_(initialPerformanceMode),
      dllPath_(initialDllPath),
      dllEditBuffer_(initialDllPath),
      gpuLabel_(initialGpuLabel),
      showFps_(initialShowFps),
      vsync_(initialVsync) {
    int screenNum = 0;
    conn_ = xcb_connect(nullptr, &screenNum);
    if (!conn_ || xcb_connection_has_error(conn_)) {
        conn_ = nullptr;
        return;
    }

    const xcb_setup_t* setup = xcb_get_setup(conn_);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screenNum; ++i) xcb_screen_next(&iter);
    screen_ = iter.data;

    window_ = xcb_generate_id(conn_);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[] = {
        0x0A1214,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
            XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
            XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_LEAVE_WINDOW
    };
    xcb_create_window(conn_, XCB_COPY_FROM_PARENT, window_, screen_->root,
                      60, 60, winW_, winH_, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen_->root_visual, mask, values);

    const char* title = "LWFG";
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, XCB_ATOM_WM_NAME,
                        XCB_ATOM_STRING, 8, static_cast<uint32_t>(std::strlen(title)), title);
    xcb_atom_t netWmName = internAtom(conn_, "_NET_WM_NAME");
    xcb_atom_t utf8Str = internAtom(conn_, "UTF8_STRING");
    if (netWmName != XCB_ATOM_NONE && utf8Str != XCB_ATOM_NONE) {
        xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, netWmName,
                            utf8Str, 8, static_cast<uint32_t>(std::strlen(title)), title);
    }

    // Set ICCCM WM_CLASS so desktop environments (KDE, GNOME, Hyprland, etc.) match lfg-linux.desktop
    const char wmClass[] = "lfg-linux\0lfg-linux\0";
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, XCB_ATOM_WM_CLASS,
                        XCB_ATOM_STRING, 8, sizeof(wmClass) - 1, wmClass);

    // Set _NET_WM_ICON (32x32 + 64x64 nearest-neighbor ARGB) so titlebar, Alt+Tab, and dock show the duck logo
    xcb_atom_t netWmIcon = internAtom(conn_, "_NET_WM_ICON");
    if (netWmIcon != XCB_ATOM_NONE) {
        std::vector<uint32_t> iconData;
        iconData.reserve(2 + 32 * 32 + 2 + 64 * 64);
        iconData.push_back(kAppIconWidth);
        iconData.push_back(kAppIconHeight);
        for (size_t i = 0; i < kAppIconWidth * kAppIconHeight; ++i) {
            iconData.push_back(kAppIconArgb[i]);
        }
        iconData.push_back(64);
        iconData.push_back(64);
        for (uint32_t y = 0; y < 64; ++y) {
            for (uint32_t x = 0; x < 64; ++x) {
                iconData.push_back(kAppIconArgb[(y / 2) * kAppIconWidth + (x / 2)]);
            }
        }
        xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, netWmIcon,
                            XCB_ATOM_CARDINAL, 32, static_cast<uint32_t>(iconData.size()), iconData.data());
    }

    // Set ICCCM WM_NORMAL_HINTS with PMinSize so the window manager allows smooth resizing.
    struct WmSizeHints {
        uint32_t flags;
        int32_t x, y, width, height;
        int32_t minWidth, minHeight;
        int32_t maxWidth, maxHeight;
        int32_t widthInc, heightInc;
        int32_t minAspectNum, minAspectDen;
        int32_t maxAspectNum, maxAspectDen;
        int32_t baseWidth, baseHeight;
        uint32_t winGravity;
    } hints{};
    hints.flags = (1u << 4) | (1u << 8); // PMinSize | PBaseSize
    hints.minWidth = kMinWinW;
    hints.minHeight = kMinWinH;
    hints.baseWidth = 460;
    hints.baseHeight = 480;
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, XCB_ATOM_WM_NORMAL_HINTS,
                        XCB_ATOM_WM_SIZE_HINTS, 32, sizeof(hints) / 4, &hints);

    xcb_atom_t wmProtocols = internAtom(conn_, "WM_PROTOCOLS");
    wmDeleteWindow_ = internAtom(conn_, "WM_DELETE_WINDOW");
    wmStateAtom_ = internAtom(conn_, "WM_STATE");
    netWmStateAtom_ = internAtom(conn_, "_NET_WM_STATE");
    netWmStateHiddenAtom_ = internAtom(conn_, "_NET_WM_STATE_HIDDEN");
    netWmStateSkipTaskbarAtom_ = internAtom(conn_, "_NET_WM_STATE_SKIP_TASKBAR");
    netWmStateSkipPagerAtom_ = internAtom(conn_, "_NET_WM_STATE_SKIP_PAGER");
    netActiveWindowAtom_ = internAtom(conn_, "_NET_ACTIVE_WINDOW");
    if (wmProtocols != XCB_ATOM_NONE && wmDeleteWindow_ != XCB_ATOM_NONE) {
        xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, wmProtocols,
                            XCB_ATOM_ATOM, 32, 1, &wmDeleteWindow_);
    }

    gc_ = xcb_generate_id(conn_);
    uint32_t gcVal = 0xFFFFFF;
    xcb_create_gc(conn_, gc_, window_, XCB_GC_FOREGROUND, &gcVal);
    recreateBackbuffer();

    toggleKeycode_ = findKeycode(0xFFC6, 75); // F9
    toggleMods_ = XCB_MOD_MASK_1;             // Alt
    hotkeyLabel_ = "ALT+F9";

    loadHotkeyConfig();
    if (!initialHotkey.empty()) {
        applyHotkeyString(initialHotkey);
        saveHotkeyConfig();
    }

    setupGlobalHotkeys(screen_->root);
    rebuildButtons();
    initSystemTray();

    xcb_map_window(conn_, window_);
    windowVisible_ = true;
    xcb_flush(conn_);
}

ControlPanel::~ControlPanel() {
    teardownSystemTray();
    if (conn_) {
        if (screen_) xcb_ungrab_key(conn_, XCB_GRAB_ANY, screen_->root, XCB_MOD_MASK_ANY);
        if (backbuf_) xcb_free_pixmap(conn_, backbuf_);
        if (gc_) xcb_free_gc(conn_, gc_);
        if (window_) xcb_destroy_window(conn_, window_);
        xcb_disconnect(conn_);
    }
}

void ControlPanel::recreateBackbuffer() {
    if (!conn_ || !screen_ || !window_) return;
    if (backbuf_ && backbufW_ == winW_ && backbufH_ == winH_) return;
    if (backbuf_) {
        xcb_free_pixmap(conn_, backbuf_);
        backbuf_ = 0;
    }
    backbuf_ = xcb_generate_id(conn_);
    xcb_create_pixmap(conn_, screen_->root_depth, backbuf_, window_, winW_, winH_);
    backbufW_ = winW_;
    backbufH_ = winH_;
}

void ControlPanel::initSystemTray() {
    GError* err = nullptr;
    dbusConn_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!dbusConn_) {
        if (err) g_error_free(err);
        return;
    }

    sniNodeInfo_ = g_dbus_node_info_new_for_xml(kSniIntrospectionXml, &err);
    if (err) { g_error_free(err); err = nullptr; }

    menuNodeInfo_ = g_dbus_node_info_new_for_xml(kDbusMenuIntrospectionXml, &err);
    if (err) { g_error_free(err); err = nullptr; }

    if (sniNodeInfo_ && sniNodeInfo_->interfaces && sniNodeInfo_->interfaces[0]) {
        sniRegId_ = g_dbus_connection_register_object(
            dbusConn_, "/StatusNotifierItem", sniNodeInfo_->interfaces[0],
            &kSniVTable, this, nullptr, nullptr);
    }

    if (menuNodeInfo_ && menuNodeInfo_->interfaces && menuNodeInfo_->interfaces[0]) {
        menuRegId_ = g_dbus_connection_register_object(
            dbusConn_, "/MenuBar", menuNodeInfo_->interfaces[0],
            &kMenuVTable, this, nullptr, nullptr);
    }

    sniBusName_ = "org.kde.StatusNotifierItem-" + std::to_string(::getpid()) + "-1";
    GVariant* reqRes = g_dbus_connection_call_sync(
        dbusConn_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", sniBusName_.c_str(), 0u),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 200, nullptr, nullptr);
    if (reqRes) g_variant_unref(reqRes);

    registerWithTrayWatcher();

    watcherSubId_ = g_dbus_connection_signal_subscribe(
        dbusConn_, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
        "/org/freedesktop/DBus", "org.kde.StatusNotifierWatcher",
        G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
           GVariant* params, gpointer userData) {
            const gchar* name = nullptr;
            const gchar* oldOwner = nullptr;
            const gchar* newOwner = nullptr;
            g_variant_get(params, "(&s&s&s)", &name, &oldOwner, &newOwner);
            if (newOwner && newOwner[0] != '\0') {
                static_cast<ControlPanel*>(userData)->registerWithTrayWatcher();
            }
        },
        this, nullptr);
}

void ControlPanel::registerWithTrayWatcher() {
    if (!dbusConn_ || sniBusName_.empty()) return;
    for (const char* watcher : {"org.kde.StatusNotifierWatcher", "org.freedesktop.StatusNotifierWatcher"}) {
        GVariant* res = g_dbus_connection_call_sync(
            dbusConn_, watcher, "/StatusNotifierWatcher",
            watcher, "RegisterStatusNotifierItem",
            g_variant_new("(s)", sniBusName_.c_str()),
            nullptr, G_DBUS_CALL_FLAGS_NONE, 200, nullptr, nullptr);
        if (res) g_variant_unref(res);
    }
}

void ControlPanel::teardownSystemTray() {
    if (dbusConn_) {
        if (watcherSubId_) g_dbus_connection_signal_unsubscribe(dbusConn_, watcherSubId_);
        if (sniRegId_) g_dbus_connection_unregister_object(dbusConn_, sniRegId_);
        if (menuRegId_) g_dbus_connection_unregister_object(dbusConn_, menuRegId_);
        if (!sniBusName_.empty()) {
            GVariant* rel = g_dbus_connection_call_sync(
                dbusConn_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "ReleaseName",
                g_variant_new("(s)", sniBusName_.c_str()),
                G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 100, nullptr, nullptr);
            if (rel) g_variant_unref(rel);
        }
        g_object_unref(dbusConn_);
        dbusConn_ = nullptr;
    }
    if (sniNodeInfo_) {
        g_dbus_node_info_unref(sniNodeInfo_);
        sniNodeInfo_ = nullptr;
    }
    if (menuNodeInfo_) {
        g_dbus_node_info_unref(menuNodeInfo_);
        menuNodeInfo_ = nullptr;
    }
}

void ControlPanel::emitTraySignals() {
    if (!dbusConn_) return;
    menuRevision_++;
    g_dbus_connection_emit_signal(dbusConn_, nullptr, "/StatusNotifierItem",
                                  "org.kde.StatusNotifierItem", "NewToolTip", nullptr, nullptr);
    g_dbus_connection_emit_signal(dbusConn_, nullptr, "/StatusNotifierItem",
                                  "org.kde.StatusNotifierItem", "NewTitle", nullptr, nullptr);
    g_dbus_connection_emit_signal(dbusConn_, nullptr, "/StatusNotifierItem",
                                  "org.kde.StatusNotifierItem", "NewIcon", nullptr, nullptr);
    g_dbus_connection_emit_signal(dbusConn_, nullptr, "/StatusNotifierItem",
                                  "org.kde.StatusNotifierItem", "NewStatus",
                                  g_variant_new("(s)", "Active"), nullptr);
    g_dbus_connection_emit_signal(dbusConn_, nullptr, "/MenuBar",
                                  "com.canonical.dbusmenu", "LayoutUpdated",
                                  g_variant_new("(ui)", menuRevision_, 0), nullptr);
}

void ControlPanel::onSniMethodCall(const char* method, GVariant*, GDBusMethodInvocation* invocation) {
    if (std::strcmp(method, "Activate") == 0) {
        toggleTrayWindow();
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else if (std::strcmp(method, "SecondaryActivate") == 0) {
        pendingTrayAction_ = ControlAction::Toggle;
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else if (std::strcmp(method, "ContextMenu") == 0) {
        toggleTrayWindow();
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else if (std::strcmp(method, "Scroll") == 0) {
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else {
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }
}

GVariant* ControlPanel::onSniGetProperty(const char* prop) {
    auto buildDuckPixmapArray = []() -> GVariant* {
        GVariantBuilder pixmaps;
        g_variant_builder_init(&pixmaps, G_VARIANT_TYPE("a(iiay)"));
        std::vector<uint8_t> bytes(kAppIconWidth * kAppIconHeight * 4);
        for (size_t i = 0; i < kAppIconWidth * kAppIconHeight; ++i) {
            uint32_t argb = kAppIconArgb[i];
            bytes[i * 4 + 0] = static_cast<uint8_t>((argb >> 24) & 0xFF); // A
            bytes[i * 4 + 1] = static_cast<uint8_t>((argb >> 16) & 0xFF); // R
            bytes[i * 4 + 2] = static_cast<uint8_t>((argb >> 8) & 0xFF);  // G
            bytes[i * 4 + 3] = static_cast<uint8_t>(argb & 0xFF);         // B
        }
        GVariant* byteArray = g_variant_new_fixed_array(
            G_VARIANT_TYPE_BYTE, bytes.data(), bytes.size(), sizeof(uint8_t));
        g_variant_builder_add(&pixmaps, "(ii@ay)",
                              static_cast<gint32>(kAppIconWidth),
                              static_cast<gint32>(kAppIconHeight),
                              byteArray);
        return g_variant_builder_end(&pixmaps);
    };

    if (std::strcmp(prop, "Category") == 0) return g_variant_new_string("ApplicationStatus");
    if (std::strcmp(prop, "Id") == 0)       return g_variant_new_string("lfg-linux");
    if (std::strcmp(prop, "Title") == 0) {
        if (generating_) {
            std::string t = "LWFG: " + std::to_string(capFps_) + " -> " + std::to_string(outFps_) + " FPS";
            return g_variant_new_string(t.c_str());
        }
        return g_variant_new_string("LWFG");
    }
    if (std::strcmp(prop, "Status") == 0)          return g_variant_new_string("Active");
    if (std::strcmp(prop, "WindowId") == 0)        return g_variant_new_int32(static_cast<int32_t>(window_));
    if (std::strcmp(prop, "IconName") == 0)        return g_variant_new_string("lfg-linux");
    if (std::strcmp(prop, "IconPixmap") == 0)      return buildDuckPixmapArray();
    if (std::strcmp(prop, "OverlayIconName") == 0) return g_variant_new_string(generating_ ? "media-playback-start" : "");
    if (std::strcmp(prop, "AttentionIconName") == 0) return g_variant_new_string("lfg-linux");
    if (std::strcmp(prop, "ItemIsMenu") == 0)      return g_variant_new_boolean(FALSE);
    if (std::strcmp(prop, "Menu") == 0)            return g_variant_new_object_path("/MenuBar");
    if (std::strcmp(prop, "ToolTip") == 0) {
        std::string modeDesc = adaptive_
            ? ("Adaptive (" + std::to_string(targetFps_) + " FPS)")
            : (std::to_string(multiplier_) + "X");
        std::string sub = generating_
            ? ("Active [" + modeDesc + "] | " +
               std::to_string(capFps_) + " -> " + std::to_string(outFps_) +
               " | Shortcut: " + hotkeyLabel_)
            : ("Stopped [" + modeDesc + "] | Shortcut: " + hotkeyLabel_ + " | Click icon to Show/Hide");
        return g_variant_new("(s@a(iiay)ss)", "lfg-linux", buildDuckPixmapArray(),
                             "LWFG", sub.c_str());
    }
    return nullptr;
}

GVariant* ControlPanel::buildMenuLayoutVariant() const {
    auto makeItem = [](int32_t id, const char* label, bool separator = false) -> GVariant* {
        GVariantBuilder props;
        g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
        if (separator) {
            g_variant_builder_add(&props, "{sv}", "type", g_variant_new_string("separator"));
        } else {
            g_variant_builder_add(&props, "{sv}", "label", g_variant_new_string(label));
            g_variant_builder_add(&props, "{sv}", "enabled", g_variant_new_boolean(TRUE));
            g_variant_builder_add(&props, "{sv}", "visible", g_variant_new_boolean(TRUE));
        }
        GVariantBuilder children;
        g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
        return g_variant_new("(ia{sv}av)", id, &props, &children);
    };

    GVariantBuilder rootProps;
    g_variant_builder_init(&rootProps, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&rootProps, "{sv}", "children-display", g_variant_new_string("submenu"));

    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));

    std::string showHideText = windowVisible_ ? "Hide Control Panel to Tray" : "Show Control Panel";
    std::string toggleText = (generating_ ? "Stop Frame Generation (" : "Start Frame Generation (") + hotkeyLabel_ + ")";
    std::string modeName = adaptive_ ? ("Adaptive " + std::to_string(targetFps_) + " FPS")
                                     : (std::to_string(multiplier_) + "X");
    std::string modeText = "Switch Mode (Current: " + modeName + ")";

    g_variant_builder_add(&children, "v", makeItem(1, showHideText.c_str()));
    g_variant_builder_add(&children, "v", makeItem(2, toggleText.c_str()));
    g_variant_builder_add(&children, "v", makeItem(3, modeText.c_str()));
    g_variant_builder_add(&children, "v", makeItem(4, "Select Target Window"));
    g_variant_builder_add(&children, "v", makeItem(5, "", true));
    g_variant_builder_add(&children, "v", makeItem(6, "Quit LWFG"));

    return g_variant_new("(ia{sv}av)", 0, &rootProps, &children);
}

void ControlPanel::onMenuMethodCall(const char* method, GVariant* parameters, GDBusMethodInvocation* invocation) {
    if (std::strcmp(method, "GetLayout") == 0) {
        GVariant* layout = buildMenuLayoutVariant();
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u@*)", menuRevision_, layout));
    } else if (std::strcmp(method, "GetGroupProperties") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(ia{sv})"));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(ia{sv}))", &b));
    } else if (std::strcmp(method, "GetProperty") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", g_variant_new_string("")));
    } else if (std::strcmp(method, "Event") == 0) {
        gint32 id = 0;
        const gchar* eventId = nullptr;
        GVariant* data = nullptr;
        guint32 timestamp = 0;
        g_variant_get(parameters, "(i&svu)", &id, &eventId, &data, &timestamp);
        if (data) g_variant_unref(data);

        if (eventId && std::strcmp(eventId, "clicked") == 0) {
            if (id == 1)      toggleTrayWindow();
            else if (id == 2) pendingTrayAction_ = ControlAction::Toggle;
            else if (id == 3) pendingTrayAction_ = ControlAction::CycleMode;
            else if (id == 4) { showFromTray(); pendingTrayAction_ = ControlAction::SelectWindow; }
            else if (id == 6) pendingTrayAction_ = ControlAction::Quit;
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else if (std::strcmp(method, "EventGroup") == 0) {
        GVariantIter* iter = nullptr;
        g_variant_get(parameters, "(a(isvu))", &iter);
        gint32 id = 0;
        const gchar* eventId = nullptr;
        GVariant* data = nullptr;
        guint32 timestamp = 0;
        while (g_variant_iter_loop(iter, "(i&svu)", &id, &eventId, &data, &timestamp)) {
            if (eventId && std::strcmp(eventId, "clicked") == 0) {
                if (id == 1)      toggleTrayWindow();
                else if (id == 2) pendingTrayAction_ = ControlAction::Toggle;
                else if (id == 3) pendingTrayAction_ = ControlAction::CycleMode;
                else if (id == 4) { showFromTray(); pendingTrayAction_ = ControlAction::SelectWindow; }
                else if (id == 6) pendingTrayAction_ = ControlAction::Quit;
            }
        }
        if (iter) g_variant_iter_free(iter);
        GVariantBuilder errIds;
        g_variant_builder_init(&errIds, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(ai)", &errIds));
    } else if (std::strcmp(method, "AboutToShow") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
    } else if (std::strcmp(method, "AboutToShowGroup") == 0) {
        GVariantBuilder b1, b2;
        g_variant_builder_init(&b1, G_VARIANT_TYPE("ai"));
        g_variant_builder_init(&b2, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(aiai)", &b1, &b2));
    } else {
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }
}

GVariant* ControlPanel::onMenuGetProperty(const char* prop) {
    if (std::strcmp(prop, "Version") == 0)       return g_variant_new_uint32(3);
    if (std::strcmp(prop, "TextDirection") == 0) return g_variant_new_string("ltr");
    if (std::strcmp(prop, "Status") == 0)        return g_variant_new_string("normal");
    if (std::strcmp(prop, "IconThemePath") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        return g_variant_new("as", &b);
    }
    return nullptr;
}

void ControlPanel::setSkipTaskbar(bool skip) {
    if (!conn_ || !window_ || netWmStateAtom_ == XCB_ATOM_NONE || netWmStateSkipTaskbarAtom_ == XCB_ATOM_NONE) return;

    if (screen_) {
        xcb_client_message_event_t ev{};
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format = 32;
        ev.window = window_;
        ev.type = netWmStateAtom_;
        ev.data.data32[0] = skip ? 1 : 0; // 1 = _NET_WM_STATE_ADD, 0 = _NET_WM_STATE_REMOVE
        ev.data.data32[1] = netWmStateSkipTaskbarAtom_;
        ev.data.data32[2] = (netWmStateSkipPagerAtom_ != XCB_ATOM_NONE) ? netWmStateSkipPagerAtom_ : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
        ev.data.data32[3] = 1; // source: regular application
        ev.data.data32[4] = 0;
        xcb_send_event(conn_, 0, screen_->root,
                       XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char*>(&ev));

        if (skip && netWmStateHiddenAtom_ != XCB_ATOM_NONE) {
            xcb_client_message_event_t remHidden{};
            remHidden.response_type = XCB_CLIENT_MESSAGE;
            remHidden.format = 32;
            remHidden.window = window_;
            remHidden.type = netWmStateAtom_;
            remHidden.data.data32[0] = 0; // 0 = _NET_WM_STATE_REMOVE
            remHidden.data.data32[1] = netWmStateHiddenAtom_;
            remHidden.data.data32[2] = XCB_ATOM_NONE;
            remHidden.data.data32[3] = 1;
            remHidden.data.data32[4] = 0;
            xcb_send_event(conn_, 0, screen_->root,
                           XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                           reinterpret_cast<const char*>(&remHidden));
        }
    }

    auto cookie = xcb_get_property(conn_, 0, window_, netWmStateAtom_, XCB_ATOM_ATOM, 0, 32);
    auto* reply = xcb_get_property_reply(conn_, cookie, nullptr);
    std::vector<xcb_atom_t> atoms;
    if (reply) {
        int len = xcb_get_property_value_length(reply) / static_cast<int>(sizeof(xcb_atom_t));
        const auto* cur = static_cast<const xcb_atom_t*>(xcb_get_property_value(reply));
        for (int i = 0; i < len; ++i) {
            if (cur[i] != netWmStateSkipTaskbarAtom_ &&
                cur[i] != netWmStateSkipPagerAtom_ &&
                cur[i] != netWmStateHiddenAtom_) {
                atoms.push_back(cur[i]);
            }
        }
        free(reply);
    }
    if (skip) {
        atoms.push_back(netWmStateSkipTaskbarAtom_);
        if (netWmStateSkipPagerAtom_ != XCB_ATOM_NONE) {
            atoms.push_back(netWmStateSkipPagerAtom_);
        }
    }
    if (atoms.empty()) {
        xcb_delete_property(conn_, window_, netWmStateAtom_);
    } else {
        xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, netWmStateAtom_,
                            XCB_ATOM_ATOM, 32, static_cast<uint32_t>(atoms.size()), atoms.data());
    }
    xcb_flush(conn_);
}

void ControlPanel::showFromTray() {
    if (!conn_ || !window_) return;
    windowVisible_ = true;
    setSkipTaskbar(false);
    xcb_map_window(conn_, window_);
    if (screen_ && netActiveWindowAtom_ != XCB_ATOM_NONE) {
        xcb_client_message_event_t ev{};
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format = 32;
        ev.window = window_;
        ev.type = netActiveWindowAtom_;
        ev.data.data32[0] = 2; // source indication: pager / user action
        ev.data.data32[1] = XCB_CURRENT_TIME;
        ev.data.data32[2] = XCB_WINDOW_NONE;
        xcb_send_event(conn_, 0, screen_->root,
                       XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char*>(&ev));
    }
    uint32_t stackMode = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn_, window_, XCB_CONFIG_WINDOW_STACK_MODE, &stackMode);
    draw();
    xcb_flush(conn_);
    emitTraySignals();
}

void ControlPanel::hideToTray() {
    if (!conn_ || !window_) return;
    hoverIndex_ = -1;
    draggingFlowSlider_ = false;
    windowVisible_ = false;
    setSkipTaskbar(true);
    xcb_unmap_window(conn_, window_);
    if (screen_) {
        // Send ICCCM synthetic UnmapNotify to root window so WM transitions window to WithdrawnState and drops taskbar icon
        xcb_unmap_notify_event_t ev{};
        ev.response_type = XCB_UNMAP_NOTIFY;
        ev.event = screen_->root;
        ev.window = window_;
        ev.from_configure = 0;
        xcb_send_event(conn_, 0, screen_->root,
                       XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char*>(&ev));
    }
    if (wmStateAtom_ != XCB_ATOM_NONE) {
        xcb_delete_property(conn_, window_, wmStateAtom_);
    }
    xcb_flush(conn_);
    emitTraySignals();
    std::fprintf(stderr, "[lfg] Control Panel minimized to System Tray (hidden from screen and taskbar)\n");
}

bool ControlPanel::isWindowMinimizedByWM() const {
    if (!conn_ || !window_) return false;
    if (netWmStateAtom_ != XCB_ATOM_NONE && netWmStateHiddenAtom_ != XCB_ATOM_NONE) {
        auto ck = xcb_get_property(conn_, 0, window_, netWmStateAtom_, XCB_ATOM_ATOM, 0, 32);
        if (auto* r = xcb_get_property_reply(conn_, ck, nullptr)) {
            const auto* atoms = static_cast<const xcb_atom_t*>(xcb_get_property_value(r));
            int n = xcb_get_property_value_length(r) / static_cast<int>(sizeof(xcb_atom_t));
            for (int i = 0; i < n; ++i) {
                if (atoms[i] == netWmStateHiddenAtom_) {
                    free(r);
                    return true;
                }
            }
            free(r);
        }
    }
    if (wmStateAtom_ != XCB_ATOM_NONE) {
        auto ck = xcb_get_property(conn_, 0, window_, wmStateAtom_, wmStateAtom_, 0, 2);
        if (auto* r = xcb_get_property_reply(conn_, ck, nullptr)) {
            if (xcb_get_property_value_length(r) >= 4) {
                const auto* data = static_cast<const uint32_t*>(xcb_get_property_value(r));
                if (data[0] == 3u) { // IconicState (3) in ICCCM
                    free(r);
                    return true;
                }
            }
            free(r);
        }
    }
    return false;
}

void ControlPanel::toggleTrayWindow() {
    if (windowVisible_) hideToTray();
    else showFromTray();
}

xcb_keycode_t ControlPanel::findKeycode(uint32_t keysym, xcb_keycode_t fallbackEvdev) {
    const xcb_setup_t* setup = xcb_get_setup(conn_);
    xcb_keycode_t minKc = setup->min_keycode;
    xcb_keycode_t maxKc = setup->max_keycode;
    int count = maxKc - minKc + 1;

    xcb_get_keyboard_mapping_cookie_t cookie = xcb_get_keyboard_mapping(conn_, minKc, static_cast<uint8_t>(count));
    xcb_get_keyboard_mapping_reply_t* reply = xcb_get_keyboard_mapping_reply(conn_, cookie, nullptr);
    if (!reply) return fallbackEvdev;

    xcb_keysym_t* syms = xcb_get_keyboard_mapping_keysyms(reply);
    int symsPerKc = reply->keysyms_per_keycode;
    xcb_keycode_t found = fallbackEvdev;

    for (int i = 0; i < count; ++i) {
        for (int k = 0; k < symsPerKc; ++k) {
            if (syms[i * symsPerKc + k] == keysym) {
                found = static_cast<xcb_keycode_t>(minKc + i);
                free(reply);
                return found;
            }
        }
    }
    free(reply);
    return found;
}

uint32_t ControlPanel::keycodeToKeysym(xcb_keycode_t kc) const {
    return keycodeToKeysymWithState(kc, 0);
}

uint32_t ControlPanel::keycodeToKeysymWithState(xcb_keycode_t kc, uint16_t state) const {
    if (!conn_) return 0;
    xcb_get_keyboard_mapping_cookie_t cookie = xcb_get_keyboard_mapping(conn_, kc, 1);
    xcb_get_keyboard_mapping_reply_t* reply = xcb_get_keyboard_mapping_reply(conn_, cookie, nullptr);
    if (!reply) return 0;
    xcb_keysym_t* syms = xcb_get_keyboard_mapping_keysyms(reply);
    uint32_t sym = 0;
    if (reply->keysyms_per_keycode > 0) {
        const bool shifted = (state & XCB_MOD_MASK_SHIFT) != 0;
        if (shifted && reply->keysyms_per_keycode > 1 && syms[1] != 0) {
            sym = syms[1];
        } else {
            sym = syms[0];
        }
    }
    free(reply);
    return sym;
}

std::string ControlPanel::keysymToName(uint32_t sym) {
    if (sym >= 0xFFBE && sym <= 0xFFC9) {
        return "F" + std::to_string(sym - 0xFFBE + 1);
    }
    if (sym >= 'a' && sym <= 'z') {
        return std::string(1, static_cast<char>(sym - 'a' + 'A'));
    }
    if (sym >= 'A' && sym <= 'Z') {
        return std::string(1, static_cast<char>(sym));
    }
    if (sym >= '0' && sym <= '9') {
        return std::string(1, static_cast<char>(sym));
    }
    switch (sym) {
        case 0xFF63: return "INS";
        case 0xFFFF: return "DEL";
        case 0xFF50: return "HOME";
        case 0xFF57: return "END";
        case 0xFF55: return "PGUP";
        case 0xFF56: return "PGDN";
        case 0xFF09: return "TAB";
        case 0x0020: return "SPACE";
        default:     return "KEY";
    }
}

uint32_t ControlPanel::nameToKeysym(const std::string& rawName) {
    std::string name = toUpper(rawName);
    if (name.size() >= 2 && name[0] == 'F') {
        int fn = std::atoi(name.c_str() + 1);
        if (fn >= 1 && fn <= 12) return 0xFFBE + static_cast<uint32_t>(fn - 1);
    }
    if (name.size() == 1) {
        char c = name[0];
        if (c >= 'A' && c <= 'Z') return static_cast<uint32_t>(c - 'A' + 'a');
        if (c >= '0' && c <= '9') return static_cast<uint32_t>(c);
    }
    if (name == "INS" || name == "INSERT") return 0xFF63;
    if (name == "DEL" || name == "DELETE") return 0xFFFF;
    if (name == "HOME") return 0xFF50;
    if (name == "END")  return 0xFF57;
    if (name == "PGUP") return 0xFF55;
    if (name == "PGDN") return 0xFF56;
    if (name == "SPACE") return 0x0020;
    return 0;
}

void ControlPanel::applyHotkeyString(const std::string& spec) {
    std::string up = toUpper(spec);
    uint16_t mods = 0;
    size_t pos = 0;
    while (true) {
        size_t plus = up.find('+', pos);
        if (plus == std::string::npos) {
            std::string keyPart = up.substr(pos);
            uint32_t sym = nameToKeysym(keyPart);
            if (sym != 0) {
                toggleKeycode_ = findKeycode(sym, toggleKeycode_);
                toggleMods_ = mods;
                std::string label;
                if (mods & XCB_MOD_MASK_CONTROL) label += "CTRL+";
                if (mods & XCB_MOD_MASK_1)       label += "ALT+";
                if (mods & XCB_MOD_MASK_SHIFT)   label += "SHIFT+";
                label += keysymToName(sym);
                hotkeyLabel_ = label;
            }
            break;
        }
        std::string modToken = up.substr(pos, plus - pos);
        if (modToken == "ALT")                    mods |= XCB_MOD_MASK_1;
        else if (modToken == "CTRL" || modToken == "CONTROL") mods |= XCB_MOD_MASK_CONTROL;
        else if (modToken == "SHIFT")             mods |= XCB_MOD_MASK_SHIFT;
        else if (modToken == "SUPER" || modToken == "META")   mods |= XCB_MOD_MASK_4;
        pos = plus + 1;
    }
}

void ControlPanel::loadHotkeyConfig() {
    std::ifstream in(configFilePath());
    if (!in.is_open()) return;
    unsigned int kc = 0;
    unsigned int mods = 0;
    std::string label;
    if (in >> kc >> mods >> label && kc > 0 && !label.empty()) {
        toggleKeycode_ = static_cast<xcb_keycode_t>(kc);
        toggleMods_ = static_cast<uint16_t>(mods);
        hotkeyLabel_ = label;
    }
}

void ControlPanel::saveHotkeyConfig() const {
    auto path = configFilePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto stagingPath = path;
    stagingPath += ".staging.tmp";
    {
        std::ofstream out(stagingPath, std::ios::trunc);
        if (!out.is_open()) return;
        out << static_cast<unsigned int>(toggleKeycode_) << " "
            << static_cast<unsigned int>(toggleMods_) << " "
            << hotkeyLabel_ << "\n";
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

void ControlPanel::setupGlobalHotkeys(xcb_window_t root) {
    if (!conn_ || !screen_) return;
    xcb_ungrab_key(conn_, XCB_GRAB_ANY, root, XCB_MOD_MASK_ANY);

    keyF12_ = findKeycode(0xFFC9, 96);

    const uint16_t extraMods[] = {
        0,
        XCB_MOD_MASK_LOCK,
        XCB_MOD_MASK_2,
        static_cast<uint16_t>(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2)
    };

    for (uint16_t extra : extraMods) {
        uint16_t modToggle = static_cast<uint16_t>(toggleMods_ | extra);
        xcb_grab_key(conn_, 0, root, modToggle, toggleKeycode_, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);

        uint16_t modQuit = static_cast<uint16_t>(XCB_MOD_MASK_1 | extra);
        xcb_grab_key(conn_, 0, root, modQuit, keyF12_, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    }
    xcb_flush(conn_);
}

void ControlPanel::rebuildButtons() {
    buttons_.clear();

    const int W = std::max<int>(kMinWinW, winW_);
    const int H = std::max<int>(kMinWinH, winH_);
    const int padX = std::clamp(W / 22, 12, 36);
    const int contentW = std::max(200, W - 2 * padX);
    const int gapX = std::clamp(W / 55, 6, 14);

    const int headerH = std::clamp(H / 11, 36, 54);
    const int topGap = std::clamp(H / 65, 6, 12);
    const int tabY = headerH + topGap;
    const int tabH = std::clamp(H / 14, 28, 38);
    const int statusY = tabY + tabH + topGap;
    const int statusH = std::clamp(H / 10, 40, 64);

    const int footerH = std::clamp(H / 16, 24, 36);
    const int rowsTop = statusY + statusH + topGap;
    const int rowsBottom = H - footerH - std::max(6, topGap / 2);
    const int availH = std::max(160, rowsBottom - rowsTop);

    const int gapY = std::clamp(availH / 32, 6, 14);
    const int rowH = std::clamp((availH - 5 * gapY) / 6, 26, 68);

    auto rowY = [&](int idx) -> int16_t {
        return static_cast<int16_t>(rowsTop + idx * (rowH + gapY));
    };

    // Top Navigation Tabs: [ MAIN CONTROLS ]  [ SETTINGS ]
    int wTabMain = (contentW - gapX) / 2;
    int wTabSet = contentW - gapX - wTabMain;
    std::string mainTabLabel = (wTabMain >= 170) ? "MAIN CONTROLS" : "MAIN";
    std::string setTabLabel = !lsfgActive_
        ? ((wTabSet >= 170) ? "SETTINGS (!)" : "SETTINGS!")
        : "SETTINGS";
    uint32_t setTabBg = settingsTab_ ? 0x0F766Eu : (!lsfgActive_ ? 0x3D1E20u : 0x101C20u);
    uint32_t setTabBorder = settingsTab_ ? 0x2DD4BFu : (!lsfgActive_ ? 0xF85149u : 0x223B42u);

    buttons_.push_back({static_cast<int16_t>(padX), static_cast<int16_t>(tabY),
                        static_cast<uint16_t>(wTabMain), static_cast<uint16_t>(tabH),
                        mainTabLabel,
                        !settingsTab_ ? 0x0F766Eu : 0x101C20u,
                        !settingsTab_ ? 0x2DD4BFu : 0x223B42u,
                        ControlAction::SwitchTabMain, !settingsTab_,
                        "MAIN TAB: FRAME GENERATION CONTROLS, MULTIPLIER, FLOW SCALE, AND GPU"});

    buttons_.push_back({static_cast<int16_t>(padX + wTabMain + gapX), static_cast<int16_t>(tabY),
                        static_cast<uint16_t>(wTabSet), static_cast<uint16_t>(tabH),
                        setTabLabel,
                        setTabBg, setTabBorder,
                        ControlAction::SwitchTabSettings, settingsTab_,
                        "SETTINGS TAB: CHANGE SHORTCUT KEY AND LOSSLESS SCALING FILES LOCATION"});

    if (settingsTab_) {
        flowSliderW_ = 0;
        flowSliderH_ = 0;

        // Settings Row 0: Change Shortcut Key Button
        int16_t y0 = rowY(0);
        std::string bindLabel = waitingForHotkey_
            ? "PRESS NEW SHORTCUT KEY... (ESC CANCEL)"
            : ("CHANGE SHORTCUT KEY: " + hotkeyLabel_);
        uint32_t bindColor = waitingForHotkey_ ? 0xC26200u : 0x15252Au;
        uint32_t bindBorder = waitingForHotkey_ ? 0xFFA133u : 0x14B8A6u;
        buttons_.push_back({static_cast<int16_t>(padX), y0,
                            static_cast<uint16_t>(contentW), static_cast<uint16_t>(rowH),
                            bindLabel, bindColor, bindBorder, ControlAction::RebindHotkey, waitingForHotkey_,
                            "CLICK THEN PRESS ANY KEY COMBINATION TO CHANGE START/STOP SHORTCUT"});

        // Settings Row 1: Right side has [ AUTO-DETECT STEAM ] button; Left side is drawn as Status Card in draw()
        int16_t y1 = rowY(1);
        int wStatusCard = (contentW - gapX) * 58 / 100;
        int wAutoBtn = contentW - gapX - wStatusCard;
        std::string autoLabel = (wAutoBtn >= 170) ? "AUTO-DETECT STEAM" : "AUTO-DETECT";
        buttons_.push_back({static_cast<int16_t>(padX + wStatusCard + gapX), y1,
                            static_cast<uint16_t>(wAutoBtn), static_cast<uint16_t>(rowH),
                            autoLabel, 0x0F766Eu, 0x2DD4BFu, ControlAction::AutoDetectDllPath, false,
                            "AUTOMATICALLY SEARCH STEAM LIBRARIES FOR LOSSLESS SCALING FILES"});

        // Settings Row 2: Interactive Lossless Scaling File Location Box (Click to Type / Edit Path)
        int16_t y2 = rowY(2);
        uint32_t pathBg = editingDllPath_ ? 0x070E10u : 0x0E191Du;
        uint32_t pathBorder = editingDllPath_ ? 0xFF8200u : (lsfgActive_ ? 0x14B8A6u : 0xF85149u);
        buttons_.push_back({static_cast<int16_t>(padX), y2,
                            static_cast<uint16_t>(contentW), static_cast<uint16_t>(rowH),
                            dllPath_, pathBg, pathBorder, ControlAction::EditDllPath, editingDllPath_,
                            "CLICK TO TYPE LOSSLESS SCALING LOCATION MANUALLY (ENTER TO APPLY, ESC CANCEL)"});

        // Settings Row 3: [ BROWSE FOLDER... ] + [ BROWSE FILE (DLL)... ]
        int16_t y3 = rowY(3);
        int wHalf1 = (contentW - gapX) / 2;
        int wHalf2 = contentW - gapX - wHalf1;
        std::string browseDirLabel = (wHalf1 >= 180) ? "BROWSE FOLDER..." : "BROWSE DIR";
        std::string browseFileLabel = (wHalf2 >= 180) ? "BROWSE FILE (DLL)..." : "BROWSE DLL";
        buttons_.push_back({static_cast<int16_t>(padX), y3,
                            static_cast<uint16_t>(wHalf1), static_cast<uint16_t>(rowH),
                            browseDirLabel, 0x15252Au, 0x14B8A6u, ControlAction::BrowseDllFolder, false,
                            "SELECT THE LOSSLESS SCALING FOLDER MANUALLY USING FILE DIALOG"});
        buttons_.push_back({static_cast<int16_t>(padX + wHalf1 + gapX), y3,
                            static_cast<uint16_t>(wHalf2), static_cast<uint16_t>(rowH),
                            browseFileLabel, 0x15252Au, 0x14B8A6u, ControlAction::BrowseDllFile, false,
                            "SELECT LOSSLESS.DLL MANUALLY USING FILE DIALOG"});

        // Settings Row 4: [ PASTE PATH (CTRL+V) ] + [ APPLY MANUAL PATH ]
        int16_t y4 = rowY(4);
        std::string pasteLabel = (wHalf1 >= 180) ? "PASTE PATH (CTRL+V)" : "PASTE PATH";
        std::string applyLabel = (wHalf2 >= 180) ? "APPLY MANUAL PATH" : "APPLY PATH";
        uint32_t applyBg = editingDllPath_ ? 0xC26200u : 0x0F766Eu;
        uint32_t applyBorder = editingDllPath_ ? 0xFFA133u : 0x2DD4BFu;
        buttons_.push_back({static_cast<int16_t>(padX), y4,
                            static_cast<uint16_t>(wHalf1), static_cast<uint16_t>(rowH),
                            pasteLabel, 0x182A30u, 0x2A4750u, ControlAction::PasteDllPath, false,
                            "PASTE LOSSLESS SCALING PATH FROM CLIPBOARD AND LOAD IT"});
        buttons_.push_back({static_cast<int16_t>(padX + wHalf1 + gapX), y4,
                            static_cast<uint16_t>(wHalf2), static_cast<uint16_t>(rowH),
                            applyLabel, applyBg, applyBorder, ControlAction::ApplyDllPath, editingDllPath_,
                            "APPLY AND VERIFY THE MANUAL LOSSLESS SCALING LOCATION ABOVE"});

        // Settings Row 5: [ < BACK TO MAIN CONTROLS ]
        int16_t y5 = rowY(5);
        buttons_.push_back({static_cast<int16_t>(padX), y5,
                            static_cast<uint16_t>(contentW), static_cast<uint16_t>(rowH),
                            "< BACK TO MAIN CONTROLS", 0x182A30u, 0x2A4750u, ControlAction::SwitchTabMain, false,
                            "RETURN TO THE MAIN FRAME GENERATION CONTROLS TAB"});
        return;
    }

    // MAIN TAB:
    // Row 0: Full-width Primary Start/Stop Button (Shortcut rebind is in SETTINGS tab)
    int16_t y0 = rowY(0);
    std::string startStopLabel = (generating_ ? "STOP (" : "START (") + hotkeyLabel_ + ")";
    uint32_t startStopColor = generating_ ? 0xC2410Cu : 0x0F766Eu;
    uint32_t startStopBorder = generating_ ? 0xFF8200u : 0x2DD4BFu;
    buttons_.push_back({static_cast<int16_t>(padX), y0,
                        static_cast<uint16_t>(contentW), static_cast<uint16_t>(rowH),
                        startStopLabel, startStopColor, startStopBorder, ControlAction::Toggle, true,
                        "START OR STOP STREAMING ON ACTIVE TOP WINDOW (" + hotkeyLabel_ + ")"});

    // Row 1: Direct Mode Selector ([2X] [3X] [4X] [ADAPTIVE]) + Target FPS Controls ([-] [TGT:N] [+])
    int16_t y1 = rowY(1);
    int segGap = std::max(3, gapX / 2);
    int modeGroupW = (contentW - gapX) * 56 / 100;
    int tgtGroupW = contentW - gapX - modeGroupW;

    int w2x = (modeGroupW - 3 * segGap) * 20 / 100;
    int w3x = w2x;
    int w4x = w2x;
    int wAdp = modeGroupW - 3 * segGap - w2x - w3x - w4x;

    bool is2x = (!adaptive_ && multiplier_ == 2);
    bool is3x = (!adaptive_ && multiplier_ == 3);
    bool is4x = (!adaptive_ && multiplier_ == 4);
    bool isAdp = adaptive_;

    buttons_.push_back({static_cast<int16_t>(padX), y1,
                        static_cast<uint16_t>(w2x), static_cast<uint16_t>(rowH),
                        "2X",
                        is2x ? 0x0F766Eu : 0x15252Au,
                        is2x ? 0x2DD4BFu : 0x223B42u,
                        ControlAction::SetMode2X, is2x,
                        "FIXED 2X MODE: GENERATE 1 INTERMEDIATE FRAME PER CAPTURED FRAME"});

    buttons_.push_back({static_cast<int16_t>(padX + w2x + segGap), y1,
                        static_cast<uint16_t>(w3x), static_cast<uint16_t>(rowH),
                        "3X",
                        is3x ? 0x0E7490u : 0x15252Au,
                        is3x ? 0x38BDF8u : 0x223B42u,
                        ControlAction::SetMode3X, is3x,
                        "FIXED 3X MODE: GENERATE 2 INTERMEDIATE FRAMES PER CAPTURED FRAME"});

    buttons_.push_back({static_cast<int16_t>(padX + w2x + w3x + 2 * segGap), y1,
                        static_cast<uint16_t>(w4x), static_cast<uint16_t>(rowH),
                        "4X",
                        is4x ? 0xC26200u : 0x15252Au,
                        is4x ? 0xFFA133u : 0x223B42u,
                        ControlAction::SetMode4X, is4x,
                        "FIXED 4X MODE: GENERATE 3 INTERMEDIATE FRAMES PER CAPTURED FRAME"});

    std::string adpLabel = (wAdp >= 96) ? "ADAPTIVE" : "ADAPT";
    buttons_.push_back({static_cast<int16_t>(padX + w2x + w3x + w4x + 3 * segGap), y1,
                        static_cast<uint16_t>(wAdp), static_cast<uint16_t>(rowH),
                        adpLabel,
                        isAdp ? 0xC26200u : 0x15252Au,
                        isAdp ? 0xFFA133u : 0x223B42u,
                        ControlAction::SetModeAdaptive, isAdp,
                        "ADAPTIVE MODE: DYNAMICALLY PACE MULTIPLIER TO REACH TARGET FPS"});

    int tgtX = padX + modeGroupW + gapX;
    int wMinus = std::clamp(tgtGroupW * 22 / 100, 26, 48);
    int wPlus = wMinus;
    int wTgt = std::max(44, tgtGroupW - 2 * segGap - wMinus - wPlus);
    uint32_t tgtBg = isAdp ? 0x134E4Au : 0x15252Au;
    uint32_t tgtBorder = isAdp ? 0x2DD4BFu : 0x223B42u;

    buttons_.push_back({static_cast<int16_t>(tgtX), y1,
                        static_cast<uint16_t>(wMinus), static_cast<uint16_t>(rowH),
                        "-", tgtBg, tgtBorder, ControlAction::TargetFpsDown, isAdp,
                        "DECREASE ADAPTIVE TARGET FPS (10 BELOW 240, 20 ABOVE; OR SCROLL)"});

    std::string tgtLabel = (wTgt >= 110)
        ? ("TARGET: " + std::to_string(targetFps_))
        : ("TGT:" + std::to_string(targetFps_));
    buttons_.push_back({static_cast<int16_t>(tgtX + wMinus + segGap), y1,
                        static_cast<uint16_t>(wTgt), static_cast<uint16_t>(rowH),
                        tgtLabel, tgtBg, tgtBorder, ControlAction::CycleTargetFps, isAdp,
                        "CLICK TO CYCLE TARGET FPS PRESETS (60 / 120 / 144 / 240 / 360 / 500 / 1000)"});

    buttons_.push_back({static_cast<int16_t>(tgtX + wMinus + segGap + wTgt + segGap), y1,
                        static_cast<uint16_t>(wPlus), static_cast<uint16_t>(rowH),
                        "+", tgtBg, tgtBorder, ControlAction::TargetFpsUp, isAdp,
                        "INCREASE ADAPTIVE TARGET FPS (10 BELOW 240, 20 ABOVE; OR SCROLL)"});

    // Row 2: Flow Scale Slider ([-] [ ==== FLOW SCALE: 75% ==== ] [+])
    int16_t y2 = rowY(2);
    int wFlowBtn = std::clamp(contentW * 11 / 100, 28, 52);
    int wFlowSlider = std::max(100, contentW - 2 * segGap - 2 * wFlowBtn);
    flowSliderX_ = static_cast<int16_t>(padX + wFlowBtn + segGap);
    flowSliderY_ = y2;
    flowSliderW_ = static_cast<uint16_t>(wFlowSlider);
    flowSliderH_ = static_cast<uint16_t>(rowH);

    buttons_.push_back({static_cast<int16_t>(padX), y2,
                        static_cast<uint16_t>(wFlowBtn), static_cast<uint16_t>(rowH),
                        "-", 0x15252Au, 0x14B8A6u, ControlAction::FlowScaleDown, false,
                        "LOWER OPTICAL FLOW RESOLUTION SCALE BY 5% FOR HIGHER PERFORMANCE"});

    std::string flowLabel = (wFlowSlider >= 190)
        ? ("FLOW SCALE: " + std::to_string(flowScale_) + "%")
        : ("FLOW: " + std::to_string(flowScale_) + "%");
    buttons_.push_back({flowSliderX_, y2,
                        flowSliderW_, flowSliderH_,
                        flowLabel, 0x0E191Du, 0x14B8A6u, ControlAction::FlowScaleChanged, true,
                        "DRAG OR SCROLL TO ADJUST OPTICAL FLOW SCALE (25% FASTEST - 100% SHARPEST)"});

    buttons_.push_back({static_cast<int16_t>(padX + wFlowBtn + segGap + wFlowSlider + segGap), y2,
                        static_cast<uint16_t>(wFlowBtn), static_cast<uint16_t>(rowH),
                        "+", 0x15252Au, 0x14B8A6u, ControlAction::FlowScaleUp, false,
                        "RAISE OPTICAL FLOW RESOLUTION SCALE BY 5% FOR HIGHER PRECISION"});

    // Row 3: Select Target Window + LSFG Performance Mode Toggle
    int16_t y3 = rowY(3);
    int wSelWin = (contentW - gapX) * 55 / 100;
    int wPerf = contentW - gapX - wSelWin;
    std::string selWinLabel = (wSelWin >= 240) ? "SELECT TARGET WINDOW" : "SELECT WINDOW";
    buttons_.push_back({static_cast<int16_t>(padX), y3,
                        static_cast<uint16_t>(wSelWin), static_cast<uint16_t>(rowH),
                        selWinLabel, 0x182A30u, 0x2A4750u, ControlAction::SelectWindow, false,
                        "OPEN WINDOW PICKER TO MANUALLY CHOOSE TARGET WINDOW"});

    std::string perfLabel = (wPerf >= 180)
        ? (performanceMode_ ? "PERF MODE: ON" : "PERF MODE: OFF")
        : (performanceMode_ ? "PERF: ON" : "PERF: OFF");
    uint32_t perfBg = performanceMode_ ? 0xC26200u : 0x182A30u;
    uint32_t perfBorder = performanceMode_ ? 0xFFA133u : 0x2A4750u;
    buttons_.push_back({static_cast<int16_t>(padX + wSelWin + gapX), y3,
                        static_cast<uint16_t>(wPerf), static_cast<uint16_t>(rowH),
                        perfLabel, perfBg, perfBorder, ControlAction::TogglePerformanceMode, performanceMode_,
                        "TOGGLE LOSSLESS SCALING LSFG PERFORMANCE MODE (LIGHTER SHADER NETWORK)"});

    // Row 4: GPU Selector
    int16_t y4 = rowY(4);
    std::string gpuBtnLabel = "GPU: " + gpuLabel_;
    bool isNvidia = (gpuLabel_.find("NVIDIA") != std::string::npos);
    uint32_t gpuColor = isNvidia ? 0x0F766Eu : 0x134E5Eu;
    uint32_t gpuBorder = isNvidia ? 0x2DD4BFu : 0x14B8A6u;
    buttons_.push_back({static_cast<int16_t>(padX), y4,
                        static_cast<uint16_t>(contentW), static_cast<uint16_t>(rowH),
                        gpuBtnLabel, gpuColor, gpuBorder, ControlAction::CycleGpu, false,
                        "CLICK TO SWITCH VULKAN COMPUTE / RENDER GPU DEVICE"});

    // Row 5: Monitor + VSync + FPS HUD
    int16_t y5 = rowY(5);
    int wMon = (contentW - 2 * gapX) * 36 / 100;
    int wVsync = (contentW - 2 * gapX) * 32 / 100;
    int wFps = contentW - 2 * gapX - wMon - wVsync;

    std::string dispLabel = (wMon >= 165) ? "MONITOR: AUTO" : "DISP: AUTO";
    if (displayIndex_ >= 0) {
        dispLabel = (wMon >= 175)
            ? ("MONITOR: DISP " + std::to_string(displayIndex_))
            : ("DISP: " + std::to_string(displayIndex_));
    }
    buttons_.push_back({static_cast<int16_t>(padX), y5,
                        static_cast<uint16_t>(wMon), static_cast<uint16_t>(rowH),
                        dispLabel, 0x182A30u, 0x2A4750u, ControlAction::CycleDisplay, false,
                        "SELECT TARGET MONITOR FOR OVERLAY PRESENTATION"});

    std::string vsyncLabel = vsync_ ? "VSYNC: ON" : "VSYNC: OFF";
    uint32_t vsyncBg = vsync_ ? 0x0F766Eu : 0x182A30u;
    uint32_t vsyncBorder = vsync_ ? 0x2DD4BFu : 0x2A4750u;
    buttons_.push_back({static_cast<int16_t>(padX + wMon + gapX), y5,
                        static_cast<uint16_t>(wVsync), static_cast<uint16_t>(rowH),
                        vsyncLabel, vsyncBg, vsyncBorder, ControlAction::ToggleVsync, vsync_,
                        vsync_ ? "VSYNC ON (FIFO): LOCKS FPS TO MONITOR HZ (CLICK TO UNCAP FPS)"
                               : "VSYNC OFF (IMMEDIATE): UNCAPS FPS ABOVE MONITOR HZ (CLICK FOR VSYNC)"});

    std::string fpsLabel = (wFps >= 145)
        ? (showFps_ ? "FPS HUD: ON" : "FPS HUD: OFF")
        : (showFps_ ? "HUD: ON" : "HUD: OFF");
    uint32_t fpsBg = showFps_ ? 0x0F766Eu : 0x182A30u;
    uint32_t fpsBorder = showFps_ ? 0x2DD4BFu : 0x2A4750u;
    buttons_.push_back({static_cast<int16_t>(padX + wMon + wVsync + 2 * gapX), y5,
                        static_cast<uint16_t>(wFps), static_cast<uint16_t>(rowH),
                        fpsLabel, fpsBg, fpsBorder, ControlAction::ToggleFps, showFps_,
                        "TOGGLE ON-SCREEN FPS COUNTER OVERLAY"});
}

void ControlPanel::setGenerating(bool active) {
    if (generating_ != active) {
        generating_ = active;
        if (!generating_) { capFps_ = 0; outFps_ = 0; }
        rebuildButtons();
        draw();
        emitTraySignals();
    }
}

void ControlPanel::setFps(int capFps, int outFps) {
    if (capFps_ != capFps || outFps_ != outFps) {
        capFps_ = capFps;
        outFps_ = outFps;
        draw();
    }
}

void ControlPanel::setDisplayMode(int displayIndex) {
    displayIndex_ = displayIndex;
    rebuildButtons();
    draw();
}

void ControlPanel::setMode(bool adaptive, int multiplier) {
    adaptive_ = adaptive;
    multiplier_ = std::clamp(multiplier, 2, kMaxMultiplier);
    rebuildButtons();
    draw();
    emitTraySignals();
}

void ControlPanel::setTargetFps(int targetFps) {
    targetFps_ = std::clamp(targetFps, 30, kMaxTargetFps);
    rebuildButtons();
    draw();
    emitTraySignals();
}

void ControlPanel::setFlowScale(int flowScale) {
    flowScale_ = std::clamp(flowScale, 25, 100);
    rebuildButtons();
    draw();
    emitTraySignals();
}

void ControlPanel::setPerformanceMode(bool perf) {
    performanceMode_ = perf;
    rebuildButtons();
    draw();
    emitTraySignals();
}

void ControlPanel::setLsfgActive(bool active) {
    lsfgActive_ = active;
    rebuildButtons();
    draw();
}

void ControlPanel::setDllPath(const std::string& dllPath) {
    dllPath_ = dllPath;
    if (!editingDllPath_) {
        dllEditBuffer_ = dllPath;
    }
    rebuildButtons();
    draw();
}

bool ControlPanel::updateFlowScaleFromMouseX(int16_t mx) {
    if (flowSliderW_ <= 12) return false;
    int left = static_cast<int>(flowSliderX_) + 6;
    int span = std::max(1, static_cast<int>(flowSliderW_) - 12);
    double ratio = std::clamp(static_cast<double>(mx - left) / static_cast<double>(span), 0.0, 1.0);
    int rawScale = static_cast<int>(std::lround(25.0 + ratio * 75.0));
    int snapped = std::clamp(((rawScale + 2) / 5) * 5, 25, 100);
    if (snapped != flowScale_) {
        flowScale_ = snapped;
        configChanged_ = true;
        rebuildButtons();
        draw();
        emitTraySignals();
        return true;
    }
    return false;
}

void ControlPanel::setGpuLabel(const std::string& gpuLabel) {
    gpuLabel_ = gpuLabel;
    rebuildButtons();
    draw();
}

void ControlPanel::setStatusNotice(const std::string& notice) {
    statusNotice_ = notice;
    draw();
}

void ControlPanel::setShowFps(bool show) {
    showFps_ = show;
    rebuildButtons();
    draw();
}

void ControlPanel::setVsync(bool enabled) {
    if (vsync_ != enabled) {
        vsync_ = enabled;
        rebuildButtons();
        draw();
    }
}

void ControlPanel::fillRect(int16_t x, int16_t y, uint16_t w, uint16_t h, uint32_t rgb) {
    if (!conn_ || !windowVisible_ || w == 0 || h == 0) return;
    xcb_drawable_t dst = backbuf_ ? backbuf_ : window_;
    xcb_change_gc(conn_, gc_, XCB_GC_FOREGROUND, &rgb);
    xcb_rectangle_t r{x, y, w, h};
    xcb_poly_fill_rectangle(conn_, dst, gc_, 1, &r);
}

void ControlPanel::drawRectBorder(int16_t x, int16_t y, uint16_t w, uint16_t h, uint32_t rgb) {
    if (!conn_ || !windowVisible_ || w < 2 || h < 2) return;
    xcb_drawable_t dst = backbuf_ ? backbuf_ : window_;
    xcb_change_gc(conn_, gc_, XCB_GC_FOREGROUND, &rgb);
    xcb_rectangle_t rects[4] = {
        {x, y, w, 1},
        {x, static_cast<int16_t>(y + h - 1), w, 1},
        {x, y, 1, h},
        {static_cast<int16_t>(x + w - 1), y, 1, h},
    };
    xcb_poly_fill_rectangle(conn_, dst, gc_, 4, rects);
}

void ControlPanel::drawText(int16_t x, int16_t y, const std::string& text, uint32_t rgb, int scale) {
    if (!conn_ || !windowVisible_ || text.empty() || scale <= 0) return;
    xcb_drawable_t dst = backbuf_ ? backbuf_ : window_;
    xcb_change_gc(conn_, gc_, XCB_GC_FOREGROUND, &rgb);
    std::vector<xcb_rectangle_t> rects;
    rects.reserve(text.size() * 20);

    int16_t cursorX = x;
    for (char c : text) {
        const uint8_t* rows = glyph5x7Full(c);
        for (int gy = 0; gy < 7; ++gy) {
            uint8_t bits = rows[gy];
            for (int gx = 0; gx < 5; ++gx) {
                if ((bits >> (4 - gx)) & 1) {
                    rects.push_back(xcb_rectangle_t{
                        static_cast<int16_t>(cursorX + gx * scale),
                        static_cast<int16_t>(y + gy * scale),
                        static_cast<uint16_t>(scale),
                        static_cast<uint16_t>(scale)
                    });
                }
            }
        }
        cursorX = static_cast<int16_t>(cursorX + 6 * scale);
    }
    if (!rects.empty()) {
        xcb_poly_fill_rectangle(conn_, dst, gc_, static_cast<uint32_t>(rects.size()), rects.data());
    }
}

void ControlPanel::draw() {
    if (!conn_ || !windowVisible_) return;
    recreateBackbuffer();

    const int W = std::max<int>(kMinWinW, winW_);
    const int H = std::max<int>(kMinWinH, winH_);
    const int padX = std::clamp(W / 22, 12, 36);
    const int contentW = std::max(200, W - 2 * padX);
    const int gapX = std::clamp(W / 55, 6, 14);
    const int headerH = std::clamp(H / 11, 36, 54);
    const int topGap = std::clamp(H / 65, 6, 12);
    const int tabY = headerH + topGap;
    const int tabH = std::clamp(H / 14, 28, 38);
    const int statusY = tabY + tabH + topGap;
    const int statusH = std::clamp(H / 10, 40, 64);
    const int footerH = std::clamp(H / 16, 24, 36);
    const int rowsTop = statusY + statusH + topGap;
    const int rowsBottom = H - footerH - std::max(6, topGap / 2);
    const int availH = std::max(160, rowsBottom - rowsTop);
    const int gapY = std::clamp(availH / 32, 6, 14);
    const int rowH = std::clamp((availH - 5 * gapY) / 6, 26, 68);

    // Background & Header (Teal & Orange Duck Logo Theme)
    fillRect(0, 0, static_cast<uint16_t>(W), static_cast<uint16_t>(H), 0x0A1214);
    fillRect(0, 0, static_cast<uint16_t>(W), static_cast<uint16_t>(headerH), 0x101C20);
    fillRect(0, static_cast<int16_t>(headerH - 2), static_cast<uint16_t>(W), 2,
             generating_ ? 0x14B8A6 : 0xFF8200);

    // Draw the 32x32 Duck Logo in the left of the header bar
    const int iconSize = std::clamp(headerH - 8, 24, 40);
    const int iconX = padX;
    const int iconY = (headerH - iconSize) / 2;
    for (int iy = 0; iy < iconSize; ++iy) {
        uint32_t srcY = (static_cast<uint32_t>(iy) * kAppIconHeight) / static_cast<uint32_t>(iconSize);
        for (int ix = 0; ix < iconSize; ++ix) {
            uint32_t srcX = (static_cast<uint32_t>(ix) * kAppIconWidth) / static_cast<uint32_t>(iconSize);
            uint32_t argb = kAppIconArgb[srcY * kAppIconWidth + srcX];
            if (((argb >> 24) & 0xFF) >= 128) {
                fillRect(static_cast<int16_t>(iconX + ix), static_cast<int16_t>(iconY + iy),
                         1, 1, argb & 0x00FFFFFFu);
            }
        }
    }

    int titleLeft = padX + iconSize + 10;
    int titleScale = (W >= 680 && headerH >= 48) ? 3 : 2;
    std::string titleStr = (W >= 360) ? "LWFG-LossLess Scaling" : "LWFG";
    int titleY = (headerH - 7 * titleScale) / 2;
    drawText(static_cast<int16_t>(titleLeft), static_cast<int16_t>(titleY), titleStr, 0x2DD4BF, titleScale);

    // Header badge on right
    std::string badgeStr = adaptive_
        ? (std::string(lsfgActive_ ? "LSFG 3.1 | ADAPT " : "STEAM LSFG REQ | ") + std::to_string(targetFps_))
        : (std::string(lsfgActive_ ? "LSFG 3.1 | " : "STEAM LSFG REQ | ") + std::to_string(multiplier_) + "X");
    int badgeTextW = static_cast<int>(badgeStr.size()) * 6;
    int badgeW = badgeTextW + 16;
    int badgeH = 19;
    int badgeX = W - padX - badgeW;
    if (badgeX > titleLeft + static_cast<int>(titleStr.size()) * 6 * titleScale + 12) {
        int badgeY = (headerH - badgeH) / 2;
        uint32_t badgeBg = adaptive_ ? 0x6B3400 : ((multiplier_ >= 4) ? 0x6B3400 : ((multiplier_ == 3) ? 0x0E4F63 : 0x0F524C));
        uint32_t badgeBorder = adaptive_ ? 0xFF8200 : ((multiplier_ >= 4) ? 0xFF8200 : ((multiplier_ == 3) ? 0x38BDF8 : 0x2DD4BF));
        fillRect(static_cast<int16_t>(badgeX), static_cast<int16_t>(badgeY),
                 static_cast<uint16_t>(badgeW), static_cast<uint16_t>(badgeH), badgeBg);
        drawRectBorder(static_cast<int16_t>(badgeX), static_cast<int16_t>(badgeY),
                       static_cast<uint16_t>(badgeW), static_cast<uint16_t>(badgeH), badgeBorder);
        drawText(static_cast<int16_t>(badgeX + 8), static_cast<int16_t>(badgeY + 6), badgeStr, 0xFFFFFF, 1);
    }

    // Status & Telemetry Card
    fillRect(static_cast<int16_t>(padX), static_cast<int16_t>(statusY),
             static_cast<uint16_t>(contentW), static_cast<uint16_t>(statusH), 0x101C20);
    uint32_t statusAccent = 0xFF8200;
    std::string statusMain;
    uint32_t statusColor = 0xFFA133;
    if (waitingForHotkey_) {
        statusAccent = 0xFF8200;
        statusColor = 0xFFB357;
        statusMain = "PRESS NEW SHORTCUT (ESC CANCEL)";
    } else if (editingDllPath_) {
        statusAccent = 0x14B8A6;
        statusColor = 0x2DD4BF;
        statusMain = "TYPE PATH + ENTER (CTRL+V PASTE)";
    } else if (!statusNotice_.empty()) {
        const bool isSuccess = (statusNotice_.find("LOADED") != std::string::npos);
        statusAccent = isSuccess ? 0x14B8A6 : 0xF85149;
        statusColor = isSuccess ? 0x2DD4BF : 0xFF7B72;
        statusMain = statusNotice_;
    } else if (!lsfgActive_) {
        statusAccent = 0xF85149;
        statusColor = 0xFF7B72;
        statusMain = settingsTab_
            ? "SELECT OR TYPE LOSSLESS.DLL LOCATION"
            : "SET LOSSLESS SCALING PATH IN SETTINGS";
    } else if (generating_) {
        statusAccent = 0x14B8A6;
        statusColor = 0x2DD4BF;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "ACTIVE  |  FPS: %3d -> %3d", capFps_, outFps_);
        statusMain = buf;
    } else {
        statusAccent = 0xFF8200;
        statusColor = 0xFFA133;
        statusMain = "READY - PRESS " + hotkeyLabel_ + " OR START";
    }
    drawRectBorder(static_cast<int16_t>(padX), static_cast<int16_t>(statusY),
                   static_cast<uint16_t>(contentW), static_cast<uint16_t>(statusH), 0x223B42);
    fillRect(static_cast<int16_t>(padX), static_cast<int16_t>(statusY),
             4, static_cast<uint16_t>(statusH), statusAccent);

    int statusScale = (W >= 660 && statusH >= 54) ? 3 : 2;
    int maxStatusPx = contentW - 24;
    if (static_cast<int>(statusMain.size()) * 6 * statusScale > maxStatusPx && statusScale > 1) {
        statusScale--;
    }
    std::string fittedStatus = fitTextToWidth(statusMain, maxStatusPx, statusScale);
    int statusTextY = statusY + (statusH - 7 * statusScale) / 2;
    drawText(static_cast<int16_t>(padX + 14), static_cast<int16_t>(statusTextY),
             fittedStatus, statusColor, statusScale);

    // If on SETTINGS tab, draw the Lossless Scaling Files Status Card on the left of Row 1
    if (settingsTab_) {
        int16_t y1 = static_cast<int16_t>(rowsTop + 1 * (rowH + gapY));
        int wStatusCard = (contentW - gapX) * 58 / 100;
        fillRect(static_cast<int16_t>(padX), y1,
                 static_cast<uint16_t>(wStatusCard), static_cast<uint16_t>(rowH), 0x101C20);
        uint32_t cardBorder = lsfgActive_ ? 0x14B8A6u : 0xF85149u;
        drawRectBorder(static_cast<int16_t>(padX), y1,
                       static_cast<uint16_t>(wStatusCard), static_cast<uint16_t>(rowH), cardBorder);
        fillRect(static_cast<int16_t>(padX), y1, 3, static_cast<uint16_t>(rowH), cardBorder);

        int maxCardTextPx = std::max(40, wStatusCard - 16);
        std::string line1 = fitTextToWidth("LOSSLESS SCALING FILES:", maxCardTextPx, 1);
        std::string line2 = fitTextToWidth(
            lsfgActive_ ? "STATUS: FOUND (LSFG 3.1)" : "STATUS: NOT FOUND",
            maxCardTextPx, 1);
        int textTop = y1 + std::max(3, (rowH - 17) / 2);
        drawText(static_cast<int16_t>(padX + 10), static_cast<int16_t>(textTop), line1, 0x94B8C0, 1);
        drawText(static_cast<int16_t>(padX + 10), static_cast<int16_t>(textTop + 10),
                 line2, lsfgActive_ ? 0x2DD4BF : 0xFF7B72, 1);
    }

    // Buttons, Flow Scale Slider, and Settings Location Input Box
    for (size_t i = 0; i < buttons_.size(); ++i) {
        const auto& b = buttons_[i];
        bool hovered = (static_cast<int>(i) == hoverIndex_);
        uint32_t fill = hovered ? lightenColor(b.bgColor, 20) : b.bgColor;
        uint32_t border = hovered ? lightenColor(b.borderColor, 45) : b.borderColor;

        if (b.action == ControlAction::EditDllPath) {
            fillRect(b.x, b.y, b.w, b.h, editingDllPath_ ? 0x070E10u : fill);
            drawRectBorder(b.x, b.y, b.w, b.h, editingDllPath_ ? 0xFF8200u : border);
            if (editingDllPath_ && b.w > 4 && b.h > 4) {
                fillRect(static_cast<int16_t>(b.x + 1), static_cast<int16_t>(b.y + b.h - 3),
                         static_cast<uint16_t>(b.w - 2), 2, 0xFF8200u);
            }

            const int maxBoxPx = std::max(40, static_cast<int>(b.w) - 16);
            const size_t charsPerLine = std::max<size_t>(10, static_cast<size_t>(maxBoxPx / 6));
            std::string displayPath;
            uint32_t pathTextColor = 0xE6F4F1u;

            if (editingDllPath_) {
                displayPath = "EDIT: " + dllEditBuffer_ + "_";
                pathTextColor = 0xFFB357u;
            } else if (!dllPath_.empty()) {
                displayPath = "LOCATION: " + dllPath_;
                pathTextColor = lsfgActive_ ? 0xE6F4F1u : 0xFF7B72u;
            } else {
                displayPath = "LOCATION: NOT SET - CLICK TO TYPE OR CLICK BROWSE BELOW";
                pathTextColor = 0x8BAEB5u;
            }

            if (displayPath.size() <= charsPerLine || b.h < 24) {
                std::string singleLine = displayPath;
                if (singleLine.size() > charsPerLine) {
                    singleLine = ".." + singleLine.substr(singleLine.size() - (charsPerLine - 2));
                }
                int16_t ty = static_cast<int16_t>(b.y + (static_cast<int>(b.h) - 7) / 2);
                drawText(static_cast<int16_t>(b.x + 8), ty, singleLine, pathTextColor, 1);
            } else {
                // Split across 2 lines so the entire Lossless.dll path is legible
                std::string textToShow = displayPath;
                const size_t maxTwoLines = charsPerLine * 2;
                if (textToShow.size() > maxTwoLines) {
                    textToShow = ".." + textToShow.substr(textToShow.size() - (maxTwoLines - 2));
                }
                std::string line1 = textToShow.substr(0, charsPerLine);
                std::string line2 = textToShow.substr(charsPerLine);
                int16_t topY = static_cast<int16_t>(b.y + std::max(3, (static_cast<int>(b.h) - 17) / 2));
                drawText(static_cast<int16_t>(b.x + 8), topY, line1, pathTextColor, 1);
                drawText(static_cast<int16_t>(b.x + 8), static_cast<int16_t>(topY + 10), line2, pathTextColor, 1);
            }
            continue;
        }

        if (b.action == ControlAction::FlowScaleChanged) {
            // Render interactive Flow Scale slider track, progress fill, and duck-orange thumb knob
            fillRect(b.x, b.y, b.w, b.h, 0x0B1518);
            const int trackPad = 3;
            const int innerW = std::max(12, static_cast<int>(b.w) - 2 * trackPad);
            const int innerH = std::max(6, static_cast<int>(b.h) - 2 * trackPad);
            const double norm = std::clamp((flowScale_ - 25.0) / 75.0, 0.0, 1.0);
            const int fillW = std::clamp(static_cast<int>(std::lround(innerW * (0.08 + 0.92 * norm))), 8, innerW);
            const uint32_t barColor = (hovered || draggingFlowSlider_) ? 0x14B8A6 : 0x0F766E;
            fillRect(static_cast<int16_t>(b.x + trackPad), static_cast<int16_t>(b.y + trackPad),
                     static_cast<uint16_t>(fillW), static_cast<uint16_t>(innerH), barColor);
            fillRect(static_cast<int16_t>(b.x + trackPad), static_cast<int16_t>(b.y + trackPad),
                     static_cast<uint16_t>(fillW), 1, 0x2DD4BF);
            const int knobW = std::min(8, fillW);
            const int knobX = static_cast<int>(b.x) + trackPad + fillW - knobW;
            fillRect(static_cast<int16_t>(knobX), static_cast<int16_t>(b.y + trackPad),
                     static_cast<uint16_t>(knobW), static_cast<uint16_t>(innerH),
                     (hovered || draggingFlowSlider_) ? 0xFFA133 : 0xFF8200);
            drawRectBorder(b.x, b.y, b.w, b.h, (hovered || draggingFlowSlider_) ? 0x2DD4BF : border);
        } else {
            fillRect(b.x, b.y, b.w, b.h, fill);
            if (b.w > 2 && b.h > 4) {
                // Subtle top highlight for depth
                fillRect(static_cast<int16_t>(b.x + 1), static_cast<int16_t>(b.y + 1),
                         static_cast<uint16_t>(b.w - 2), 1, lightenColor(fill, 18));
            }
            drawRectBorder(b.x, b.y, b.w, b.h, border);
            if (b.activeHighlight && b.w > 4 && b.h > 4) {
                fillRect(static_cast<int16_t>(b.x + 1), static_cast<int16_t>(b.y + b.h - 3),
                         static_cast<uint16_t>(b.w - 2), 2, border);
            }
        }

        int btnScale = (b.h >= 54 && W >= 660) ? 3 : ((b.h >= 24) ? 2 : 1);
        int maxLabelPx = std::max(8, static_cast<int>(b.w) - 10);
        while (btnScale > 1 && static_cast<int>(b.label.size()) * 6 * btnScale > maxLabelPx) {
            btnScale--;
        }
        std::string fittedLabel = fitTextToWidth(b.label, maxLabelPx, btnScale);
        int textWidth = static_cast<int>(fittedLabel.size()) * 6 * btnScale - btnScale;
        int16_t tx = static_cast<int16_t>(b.x + std::max(4, (static_cast<int>(b.w) - textWidth) / 2));
        int16_t ty = static_cast<int16_t>(b.y + (static_cast<int>(b.h) - 7 * btnScale) / 2);
        uint32_t textColor = (b.activeHighlight || hovered) ? 0xFFFFFF : 0xE6F4F1;
        drawText(tx, ty, fittedLabel, textColor, btnScale);
    }

    // Clean Footer Bar with version 0.1.6 and dynamic hover tooltips
    int footerY = H - footerH;
    fillRect(0, static_cast<int16_t>(footerY), static_cast<uint16_t>(W), static_cast<uint16_t>(footerH), 0x0C1619);
    fillRect(0, static_cast<int16_t>(footerY), static_cast<uint16_t>(W), 1, 0x1F363D);
    int footerTextY = footerY + (footerH - 7) / 2;

    const std::string versionStr = "version 0.1.6";
    int versionW = static_cast<int>(versionStr.size()) * 6;

    if (hoverIndex_ >= 0 && hoverIndex_ < static_cast<int>(buttons_.size()) &&
        !buttons_[hoverIndex_].tooltip.empty()) {
        int availTooltipW = std::max(50, contentW - versionW - 16);
        std::string fittedTip = fitTextToWidth(buttons_[hoverIndex_].tooltip, availTooltipW, 1);
        drawText(static_cast<int16_t>(padX), static_cast<int16_t>(footerTextY), fittedTip, 0x5EEAD4, 1);
    }

    // Right-aligned version 0.1.6
    int versionX = W - padX - versionW;
    drawText(static_cast<int16_t>(versionX), static_cast<int16_t>(footerTextY), versionStr, 0xFFA133, 1);

    if (backbuf_) {
        xcb_copy_area(conn_, backbuf_, window_, gc_, 0, 0, 0, 0, winW_, winH_);
    }
    xcb_flush(conn_);
}

ControlAction ControlPanel::pollEvents() {
    while (g_main_context_iteration(nullptr, FALSE)) {}

    if (pendingTrayAction_ != ControlAction::None) {
        ControlAction act = pendingTrayAction_;
        pendingTrayAction_ = ControlAction::None;
        return act;
    }

    if (!conn_) return ControlAction::None;

    ControlAction result = ControlAction::None;
    while (xcb_generic_event_t* ev = xcb_poll_for_event(conn_)) {
        uint8_t type = ev->response_type & ~0x80;
        switch (type) {
            case XCB_EXPOSE: {
                auto* ex = reinterpret_cast<xcb_expose_event_t*>(ev);
                if (ex->count == 0 && windowVisible_) {
                    draw();
                }
                break;
            }
            case XCB_MAP_NOTIFY: {
                auto* mn = reinterpret_cast<xcb_map_notify_event_t*>(ev);
                if (mn->window == window_) {
                    if (windowVisible_) {
                        draw();
                    } else {
                        hideToTray();
                    }
                }
                break;
            }
            case XCB_CONFIGURE_NOTIFY: {
                auto* cfg = reinterpret_cast<xcb_configure_notify_event_t*>(ev);
                uint16_t newW = std::max<uint16_t>(kMinWinW, cfg->width);
                uint16_t newH = std::max<uint16_t>(kMinWinH, cfg->height);
                if (newW != winW_ || newH != winH_) {
                    winW_ = newW;
                    winH_ = newH;
                    configChanged_ = true;
                    recreateBackbuffer();
                    rebuildButtons();
                    draw();
                }
                break;
            }
            case XCB_MOTION_NOTIFY: {
                auto* mn = reinterpret_cast<xcb_motion_notify_event_t*>(ev);
                if (draggingFlowSlider_) {
                    if ((mn->state & XCB_BUTTON_MASK_1) == 0) {
                        draggingFlowSlider_ = false;
                        draw();
                    } else if (updateFlowScaleFromMouseX(mn->event_x)) {
                        result = ControlAction::FlowScaleChanged;
                    }
                }
                int newHover = -1;
                for (size_t i = 0; i < buttons_.size(); ++i) {
                    const auto& b = buttons_[i];
                    if (mn->event_x >= b.x && mn->event_x < b.x + b.w &&
                        mn->event_y >= b.y && mn->event_y < b.y + b.h) {
                        newHover = static_cast<int>(i);
                        break;
                    }
                }
                if (newHover != hoverIndex_) {
                    hoverIndex_ = newHover;
                    draw();
                }
                break;
            }
            case XCB_LEAVE_NOTIFY: {
                if (hoverIndex_ != -1) {
                    hoverIndex_ = -1;
                    draw();
                }
                break;
            }
            case XCB_BUTTON_RELEASE: {
                auto* br = reinterpret_cast<xcb_button_release_event_t*>(ev);
                if (br->detail == 1 && draggingFlowSlider_) {
                    draggingFlowSlider_ = false;
                    draw();
                }
                break;
            }
            case XCB_BUTTON_PRESS: {
                auto* bp = reinterpret_cast<xcb_button_press_event_t*>(ev);
                if (bp->detail == 1) {
                    statusNotice_.clear();
                    bool clickedPathBox = false;
                    for (const auto& b : buttons_) {
                        if (bp->event_x >= b.x && bp->event_x < b.x + b.w &&
                            bp->event_y >= b.y && bp->event_y < b.y + b.h) {
                            if (b.action == ControlAction::SwitchTabMain) {
                                settingsTab_ = false;
                                waitingForHotkey_ = false;
                                editingDllPath_ = false;
                                rebuildButtons();
                                draw();
                            } else if (b.action == ControlAction::SwitchTabSettings) {
                                settingsTab_ = true;
                                waitingForHotkey_ = false;
                                editingDllPath_ = false;
                                dllEditBuffer_ = dllPath_;
                                rebuildButtons();
                                draw();
                            } else if (b.action == ControlAction::RebindHotkey) {
                                editingDllPath_ = false;
                                waitingForHotkey_ = !waitingForHotkey_;
                                rebuildButtons();
                                draw();
                            } else if (b.action == ControlAction::EditDllPath) {
                                clickedPathBox = true;
                                waitingForHotkey_ = false;
                                if (!editingDllPath_) {
                                    editingDllPath_ = true;
                                    dllEditBuffer_ = dllPath_;
                                }
                                rebuildButtons();
                                draw();
                            } else if (b.action == ControlAction::BrowseDllFolder ||
                                       b.action == ControlAction::BrowseDllFile) {
                                waitingForHotkey_ = false;
                                editingDllPath_ = false;
                                statusNotice_ = "OPENING FILE DIALOG...";
                                draw();
                                std::string picked = openNativeLosslessDialog(
                                    b.action == ControlAction::BrowseDllFolder,
                                    dllPath_);
                                statusNotice_.clear();
                                if (!picked.empty()) {
                                    dllPath_ = picked;
                                    dllEditBuffer_ = picked;
                                    configChanged_ = true;
                                    result = ControlAction::DllPathChanged;
                                }
                                rebuildButtons();
                                draw();
                            } else if (b.action == ControlAction::PasteDllPath) {
                                waitingForHotkey_ = false;
                                editingDllPath_ = false;
                                std::string pasted = readClipboardText();
                                if (!pasted.empty()) {
                                    dllPath_ = pasted;
                                    dllEditBuffer_ = pasted;
                                    configChanged_ = true;
                                    result = ControlAction::DllPathChanged;
                                } else {
                                    statusNotice_ = "CLIPBOARD EMPTY - INSTALL XCLIP/WL-CLIPBOARD OR TYPE PATH";
                                }
                                rebuildButtons();
                                draw();
                            } else if (b.action == ControlAction::ApplyDllPath) {
                                waitingForHotkey_ = false;
                                if (editingDllPath_) {
                                    dllPath_ = sanitizePathInput(dllEditBuffer_);
                                    dllEditBuffer_ = dllPath_;
                                    editingDllPath_ = false;
                                }
                                configChanged_ = true;
                                result = ControlAction::DllPathChanged;
                                rebuildButtons();
                                draw();
                            } else if (b.action == ControlAction::AutoDetectDllPath) {
                                waitingForHotkey_ = false;
                                editingDllPath_ = false;
                                result = ControlAction::AutoDetectDllPath;
                                rebuildButtons();
                                draw();
                            } else if (b.action == ControlAction::MinimizeToTray) {
                                waitingForHotkey_ = false;
                                editingDllPath_ = false;
                                hideToTray();
                            } else if (b.action == ControlAction::FlowScaleChanged) {
                                waitingForHotkey_ = false;
                                editingDllPath_ = false;
                                draggingFlowSlider_ = true;
                                updateFlowScaleFromMouseX(bp->event_x);
                                result = ControlAction::FlowScaleChanged;
                                draw();
                            } else {
                                waitingForHotkey_ = false;
                                editingDllPath_ = false;
                                result = b.action;
                            }
                            break;
                        }
                    }
                    if (!clickedPathBox && editingDllPath_ && result != ControlAction::DllPathChanged) {
                        editingDllPath_ = false;
                        rebuildButtons();
                        draw();
                    }
                } else if (bp->detail == 4 || bp->detail == 5) {
                    if (!settingsTab_) {
                        // Mouse wheel up (4) / down (5) adjusts Flow Scale when hovering the slider row,
                        // or Adaptive Target FPS anywhere else on the Main tab.
                        if (bp->event_y >= flowSliderY_ && bp->event_y < flowSliderY_ + flowSliderH_) {
                            result = (bp->detail == 4) ? ControlAction::FlowScaleUp : ControlAction::FlowScaleDown;
                        } else {
                            result = (bp->detail == 4) ? ControlAction::TargetFpsUp : ControlAction::TargetFpsDown;
                        }
                    }
                }
                break;
            }
            case XCB_KEY_PRESS: {
                auto* kp = reinterpret_cast<xcb_key_press_event_t*>(ev);
                uint32_t sym = keycodeToKeysym(kp->detail);

                if (editingDllPath_) {
                    // Modifier keys alone do nothing
                    if (sym >= 0xFFE1 && sym <= 0xFFEE) break;
                    if (sym == 0xFF1B) { // Escape cancels editing
                        editingDllPath_ = false;
                        dllEditBuffer_ = dllPath_;
                        rebuildButtons();
                        draw();
                        break;
                    }
                    if (sym == 0xFF0D || sym == 0xFF8D) { // Return / KP_Enter applies path
                        dllPath_ = sanitizePathInput(dllEditBuffer_);
                        dllEditBuffer_ = dllPath_;
                        editingDllPath_ = false;
                        configChanged_ = true;
                        result = ControlAction::DllPathChanged;
                        rebuildButtons();
                        draw();
                        break;
                    }
                    uint16_t cleanMods = kp->state &
                        (XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_4);
                    if (sym == 0xFF08 || sym == 0xFFFF) { // BackSpace or Delete
                        if (cleanMods & XCB_MOD_MASK_CONTROL) {
                            dllEditBuffer_.clear();
                        } else if (!dllEditBuffer_.empty()) {
                            dllEditBuffer_.pop_back();
                        }
                        draw();
                        break;
                    }
                    if ((cleanMods & XCB_MOD_MASK_CONTROL) && (sym == 'v' || sym == 'V')) {
                        std::string pasted = readClipboardText();
                        if (!pasted.empty()) {
                            dllEditBuffer_ = pasted;
                            draw();
                        }
                        break;
                    }
                    if ((cleanMods & XCB_MOD_MASK_CONTROL) && (sym == 'u' || sym == 'U')) {
                        dllEditBuffer_.clear();
                        draw();
                        break;
                    }
                    uint32_t typedSym = keycodeToKeysymWithState(kp->detail, kp->state);
                    if (typedSym >= 0x20 && typedSym <= 0x7E &&
                        !(cleanMods & (XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_4))) {
                        char ch = static_cast<char>(typedSym);
                        if ((kp->state & XCB_MOD_MASK_LOCK) && ch >= 'a' && ch <= 'z') {
                            ch = static_cast<char>(ch - 'a' + 'A');
                        }
                        dllEditBuffer_.push_back(ch);
                        draw();
                    }
                    break;
                }

                if (waitingForHotkey_) {
                    if (sym >= 0xFFE1 && sym <= 0xFFEE) break;
                    if (sym == 0xFF1B) {
                        waitingForHotkey_ = false;
                        rebuildButtons();
                        draw();
                        break;
                    }

                    uint16_t cleanMods = kp->state &
                        (XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_4);
                    toggleKeycode_ = kp->detail;
                    toggleMods_ = cleanMods;

                    std::string newLabel;
                    if (cleanMods & XCB_MOD_MASK_CONTROL) newLabel += "CTRL+";
                    if (cleanMods & XCB_MOD_MASK_1)       newLabel += "ALT+";
                    if (cleanMods & XCB_MOD_MASK_SHIFT)   newLabel += "SHIFT+";
                    if (cleanMods & XCB_MOD_MASK_4)       newLabel += "SUPER+";
                    std::string keyName = keysymToName(sym);
                    if (keyName == "KEY") keyName = "KC" + std::to_string(kp->detail);
                    newLabel += keyName;

                    hotkeyLabel_ = newLabel;
                    waitingForHotkey_ = false;
                    configChanged_ = true;
                    setupGlobalHotkeys(screen_->root);
                    saveHotkeyConfig();
                    rebuildButtons();
                    draw();
                    emitTraySignals();
                    std::fprintf(stderr, "[lfg] Start/Stop shortcut changed to: %s\n", hotkeyLabel_.c_str());
                    break;
                }

                uint16_t cleanMods = kp->state &
                    (XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_4);
                if (kp->detail == toggleKeycode_ && cleanMods == toggleMods_) {
                    const uint32_t dtRelease = kp->time - lastHotkeyReleaseTime_;
                    const uint32_t dtPress   = kp->time - lastHotkeyPressTime_;
                    if (isHotkeyHeld_ && dtPress > 400u) {
                        isHotkeyHeld_ = false;
                    }
                    if (!isHotkeyHeld_ &&
                        (lastHotkeyReleaseTime_ == 0 || dtRelease > 25u) &&
                        (lastHotkeyPressTime_ == 0 || dtPress > 220u)) {
                        isHotkeyHeld_ = true;
                        lastHotkeyPressTime_ = kp->time;
                        statusNotice_.clear();
                        result = ControlAction::Toggle;
                    }
                } else if (kp->detail == keyF12_ && (cleanMods & XCB_MOD_MASK_1)) {
                    result = ControlAction::Quit;
                }
                break;
            }
            case XCB_KEY_RELEASE: {
                auto* kr = reinterpret_cast<xcb_key_release_event_t*>(ev);
                if (kr->detail == toggleKeycode_) {
                    isHotkeyHeld_ = false;
                    lastHotkeyReleaseTime_ = kr->time;
                }
                break;
            }
            case XCB_UNMAP_NOTIFY: {
                auto* un = reinterpret_cast<xcb_unmap_notify_event_t*>(ev);
                if (un->window == window_ && windowVisible_) {
                    // Window manager unmapped the window (e.g. titlebar Minimize button was clicked)
                    hideToTray();
                }
                break;
            }
            case XCB_PROPERTY_NOTIFY: {
                auto* pn = reinterpret_cast<xcb_property_notify_event_t*>(ev);
                if (pn->atom == netWmStateAtom_ || pn->atom == wmStateAtom_) {
                    if (windowVisible_ && isWindowMinimizedByWM()) {
                        hideToTray();
                    }
                }
                break;
            }
            case XCB_CLIENT_MESSAGE: {
                auto* cm = reinterpret_cast<xcb_client_message_event_t*>(ev);
                if (cm->data.data32[0] == wmDeleteWindow_) {
                    // Clicking the window Close (X) button exits the program entirely
                    result = ControlAction::Quit;
                }
                break;
            }
            default:
                break;
        }
        free(ev);
        if (result != ControlAction::None) break;
    }
    return result;
}

}  // namespace lfg::present
