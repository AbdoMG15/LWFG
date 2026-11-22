#include "lfg/capture/portal_session.hpp"

#include <gio/gio.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace lfg::capture {

namespace {

std::filesystem::path restoreTokenFilePath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "lfg-linux" / "restore_token";
    }
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        return std::filesystem::path(home) / ".config" / "lfg-linux" / "restore_token";
    }
    return std::filesystem::path("/tmp/lfg-linux-restore_token");
}

std::string loadSavedRestoreToken() {
    std::ifstream in(restoreTokenFilePath());
    if (!in.is_open()) return {};
    std::string tok;
    std::getline(in, tok);
    while (!tok.empty() && (tok.back() == '\r' || tok.back() == '\n' || tok.back() == ' ')) {
        tok.pop_back();
    }
    return tok;
}

void saveRestoreToken(const char* token) {
    if (!token || token[0] == '\0') return;
    auto path = restoreTokenFilePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (out.is_open()) {
        out << token << "\n";
    }
}

struct NegotiationState {
    GMainLoop* loop = nullptr;
    XdpPortal* portal = nullptr;
    XdpSession* session = nullptr;
    GError* error = nullptr;
};

void onSessionStarted(GObject* source, GAsyncResult* result, gpointer userData) {
    auto* state = static_cast<NegotiationState*>(userData);
    xdp_session_start_finish(XDP_SESSION(source), result, &state->error);
    g_main_loop_quit(state->loop);
}

void onSessionCreated(GObject* source, GAsyncResult* result, gpointer userData) {
    auto* state = static_cast<NegotiationState*>(userData);
    state->session = xdp_portal_create_screencast_session_finish(XDP_PORTAL(source), result, &state->error);
    if (!state->session) {
        g_main_loop_quit(state->loop);
        return;
    }
    xdp_session_start(state->session, nullptr, nullptr, onSessionStarted, state);
}

void parseFirstStream(GVariant* streams, CaptureTarget& out) {
    GVariantIter iter;
    g_variant_iter_init(&iter, streams);

    guint32 nodeId = 0;
    GVariant* props = nullptr;
    if (!g_variant_iter_next(&iter, "(u@a{sv})", &nodeId, &props))
        throw std::runtime_error("Portal returned zero streams for the ScreenCast session");

    out.nodeId = nodeId;

    gint32 x = 0, y = 0, w = 0, h = 0;
    if (g_variant_lookup(props, "position", "(ii)", &x, &y)) { out.x = x; out.y = y; }
    if (g_variant_lookup(props, "size", "(ii)", &w, &h)) { out.width = w; out.height = h; }

    g_variant_unref(props);
}

// Helpers to serialize Qt6 Big-Endian QDataStream payload for xdg-desktop-portal-kde RestoreData
void appendBE32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
}

void appendBE16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
}

void appendQString(std::vector<uint8_t>& buf, const std::string& utf8) {
    if (utf8.empty()) {
        appendBE32(buf, 0u);
        return;
    }
    glong itemsWritten = 0;
    gunichar2* utf16 = g_utf8_to_utf16(utf8.c_str(), static_cast<glong>(utf8.size()),
                                       nullptr, &itemsWritten, nullptr);
    if (!utf16 || itemsWritten <= 0) {
        if (utf16) g_free(utf16);
        appendBE32(buf, 0u);
        return;
    }
    appendBE32(buf, static_cast<uint32_t>(itemsWritten * 2));
    for (glong i = 0; i < itemsWritten; ++i) {
        appendBE16(buf, static_cast<uint16_t>(utf16[i]));
    }
    g_free(utf16);
}

// Builds Qt6 QDataStream-serialized QVariantMap{{"windows", QList<WindowRestoreInfo>{{appId, title}}}}
std::vector<uint8_t> buildKdeWindowRestorePayload(const std::string& appId, const std::string& title) {
    std::vector<uint8_t> buf;
    buf.reserve(128 + appId.size() * 2 + title.size() * 2);

    // QMap<QString, QVariant> entry count = 1
    appendBE32(buf, 1u);

    // Key: QString("windows")
    appendQString(buf, "windows");

    // Value: QVariant of user type "QList<WindowRestoreInfo>"
    // In Qt6 QDataStream: uint32_t typeId = QMetaType::User (65536 = 0x00010000), int8_t is_null = 0,
    // followed by const char* typeName ("QList<WindowRestoreInfo>\0", length 25).
    appendBE32(buf, 65536u);
    buf.push_back(0u);
    const char kTypeName[] = "QList<WindowRestoreInfo>";
    const uint32_t kTypeNameLenWithNull = static_cast<uint32_t>(sizeof(kTypeName)); // 25 bytes including '\0'
    appendBE32(buf, kTypeNameLenWithNull);
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(kTypeName),
               reinterpret_cast<const uint8_t*>(kTypeName) + kTypeNameLenWithNull);

    // QList<WindowRestoreInfo> size = 1
    appendBE32(buf, 1u);
    // WindowRestoreInfo: QString appId, QString title
    appendQString(buf, appId);
    appendQString(buf, title);

    return buf;
}

struct ActiveWindowInfo {
    bool received = false;
    std::string appId;
    std::string resourceClass;
    std::string title;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
};

const char* kActiveWinIntrospectionXml =
    "<node>"
    "  <interface name='org.lfg.ActiveWindow'>"
    "    <method name='Report'>"
    "      <arg type='s' name='appId' direction='in'/>"
    "      <arg type='s' name='resClass' direction='in'/>"
    "      <arg type='s' name='title' direction='in'/>"
    "      <arg type='i' name='x' direction='in'/>"
    "      <arg type='i' name='y' direction='in'/>"
    "      <arg type='i' name='w' direction='in'/>"
    "      <arg type='i' name='h' direction='in'/>"
    "    </method>"
    "  </interface>"
    "</node>";

void onActiveWinMethodCall(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                           const gchar* methodName, GVariant* parameters,
                           GDBusMethodInvocation* invocation, gpointer userData) {
    auto* info = static_cast<ActiveWindowInfo*>(userData);
    if (std::strcmp(methodName, "Report") == 0) {
        const gchar* appId = nullptr;
        const gchar* resClass = nullptr;
        const gchar* title = nullptr;
        gint32 x = 0, y = 0, w = 0, h = 0;
        g_variant_get(parameters, "(&s&s&siiii)", &appId, &resClass, &title, &x, &y, &w, &h);
        info->appId = appId ? appId : "";
        info->resourceClass = resClass ? resClass : "";
        info->title = title ? title : "";
        info->x = x;
        info->y = y;
        info->width = w;
        info->height = h;
        info->received = true;
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

const GDBusInterfaceVTable kActiveWinVTable = {
    onActiveWinMethodCall,
    nullptr,
    nullptr,
    {nullptr}
};

// Queries KWin for the currently active / top normal window (excluding LFG's own windows).
bool queryActiveKWinWindow(GDBusConnection* bus, ActiveWindowInfo& outInfo) {
    if (!bus) return false;
    const gchar* uniqueName = g_dbus_connection_get_unique_name(bus);
    if (!uniqueName || uniqueName[0] == '\0') return false;

    GError* err = nullptr;
    GDBusNodeInfo* nodeInfo = g_dbus_node_info_new_for_xml(kActiveWinIntrospectionXml, &err);
    if (!nodeInfo) {
        if (err) g_error_free(err);
        return false;
    }

    guint regId = g_dbus_connection_register_object(
        bus, "/org/lfg/ActiveWindow", nodeInfo->interfaces[0],
        &kActiveWinVTable, &outInfo, nullptr, &err);
    if (regId == 0) {
        if (err) g_error_free(err);
        g_dbus_node_info_unref(nodeInfo);
        return false;
    }

    std::string jsPath = "/tmp/lfg_active_win_" + std::to_string(::getpid()) + ".js";
    std::string pluginName = "lfg_active_win_" + std::to_string(::getpid());
    {
        std::ofstream js(jsPath, std::ios::trunc);
        if (!js.is_open()) {
            g_dbus_connection_unregister_object(bus, regId);
            g_dbus_node_info_unref(nodeInfo);
            return false;
        }
        js << "(function() {\n"
           << "    function isCandidate(w) {\n"
           << "        if (!w) return false;\n"
           << "        if (w.deleted || w.minimized || w.specialWindow || w.desktopWindow || w.dock || w.toolbar || w.menu || w.popupWindow || w.tooltip || w.notification || w.onScreenDisplay) return false;\n"
           << "        var cap = String(w.caption || '');\n"
           << "        var cls = String(w.resourceClass || '');\n"
           << "        var df  = String(w.desktopFileName || '');\n"
           << "        if (cap.length === 0 && cls.length === 0 && df.length === 0) return false;\n"
           << "        if (cap === 'LWFG' || cap.indexOf('LWFG') !== -1 || cap.indexOf('LFG Frame Generation') !== -1 || cap.indexOf('LFG Overlay') !== -1) return false;\n"
           << "        if (cls === 'lfg' || cls === 'LFG' || cls === 'lfg-linux' || df === 'lfg' || df === 'lfg-linux') return false;\n"
           << "        return true;\n"
           << "    }\n"
           << "    var target = workspace.activeWindow;\n"
           << "    if (!isCandidate(target)) {\n"
           << "        target = null;\n"
           << "        var order = workspace.stackingOrder;\n"
           << "        if (order && order.length) {\n"
           << "            for (var i = order.length - 1; i >= 0; --i) {\n"
           << "                if (isCandidate(order[i])) {\n"
           << "                    target = order[i];\n"
           << "                    break;\n"
           << "                }\n"
           << "            }\n"
           << "        }\n"
           << "    }\n"
           << "    if (target) {\n"
           << "        var df = String(target.desktopFileName || '');\n"
           << "        var rc = String(target.resourceClass || '');\n"
           << "        var appId = (df.length > 0) ? df : rc;\n"
           << "        var title = String(target.caption || '');\n"
           << "        var g = target.clientGeometry || target.frameGeometry;\n"
           << "        var x = g ? Math.round(g.x) : 0;\n"
           << "        var y = g ? Math.round(g.y) : 0;\n"
           << "        var w = g ? Math.round(g.width) : 0;\n"
           << "        var h = g ? Math.round(g.height) : 0;\n"
           << "        callDBus('" << uniqueName << "', '/org/lfg/ActiveWindow', 'org.lfg.ActiveWindow', 'Report', appId, rc, title, x, y, w, h);\n"
           << "    } else {\n"
           << "        callDBus('" << uniqueName << "', '/org/lfg/ActiveWindow', 'org.lfg.ActiveWindow', 'Report', '', '', '', 0, 0, 0, 0);\n"
           << "    }\n"
           << "})();\n";
    }

    // Unload any stale script with the same plugin name first
    GVariant* unloadReply = g_dbus_connection_call_sync(
        bus, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "unloadScript",
        g_variant_new("(s)", pluginName.c_str()), nullptr, G_DBUS_CALL_FLAGS_NONE, 500, nullptr, nullptr);
    if (unloadReply) g_variant_unref(unloadReply);

    GVariant* loadReply = g_dbus_connection_call_sync(
        bus, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "loadScript",
        g_variant_new("(ss)", jsPath.c_str(), pluginName.c_str()),
        G_VARIANT_TYPE("(i)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &err);

    if (!loadReply) {
        if (err) g_error_free(err);
        std::remove(jsPath.c_str());
        g_dbus_connection_unregister_object(bus, regId);
        g_dbus_node_info_unref(nodeInfo);
        return false;
    }

    gint32 scriptId = -1;
    g_variant_get(loadReply, "(i)", &scriptId);
    g_variant_unref(loadReply);

    std::string paths[2] = {
        "/Scripting/Script" + std::to_string(scriptId),
        "/" + std::to_string(scriptId)
    };
    for (const auto& sp : paths) {
        GVariant* r = g_dbus_connection_call_sync(
            bus, "org.kde.KWin", sp.c_str(), "org.kde.kwin.Script", "run",
            nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 500, nullptr, nullptr);
        if (r) {
            g_variant_unref(r);
            break;
        }
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(180);
    while (!outInfo.received && std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        if (!outInfo.received) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    GVariant* unloadAfter = g_dbus_connection_call_sync(
        bus, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "unloadScript",
        g_variant_new("(s)", pluginName.c_str()), nullptr, G_DBUS_CALL_FLAGS_NONE, 500, nullptr, nullptr);
    if (unloadAfter) g_variant_unref(unloadAfter);

    std::remove(jsPath.c_str());
    g_dbus_connection_unregister_object(bus, regId);
    g_dbus_node_info_unref(nodeInfo);

    return outInfo.received && (!outInfo.appId.empty() || !outInfo.title.empty());
}

}  // namespace

PortalSession::PortalSession() {
    portal_ = xdp_portal_new();
    if (!portal_) throw std::runtime_error("Failed to connect to xdg-desktop-portal");
    dbusConn_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
}

PortalSession::~PortalSession() {
    close();
    if (dbusConn_) {
        g_object_unref(dbusConn_);
        dbusConn_ = nullptr;
    }
    if (portal_) {
        g_object_unref(portal_);
        portal_ = nullptr;
    }
}

bool PortalSession::tryAutoSelectActiveKdeWindow(CaptureTarget& outTarget) {
    if (!dbusConn_) return false;

    ActiveWindowInfo win{};
    if (!queryActiveKWinWindow(dbusConn_, win)) {
        return false;
    }

    static uint32_t sSeq = 1;
    uint32_t seq = sSeq++;
    std::string pidStr = std::to_string(::getpid()) + "_" + std::to_string(seq);
    std::string reqCreate  = "/org/freedesktop/portal/desktop/request/lfg/create_" + pidStr;
    std::string reqSelect  = "/org/freedesktop/portal/desktop/request/lfg/select_" + pidStr;
    std::string reqStart   = "/org/freedesktop/portal/desktop/request/lfg/start_" + pidStr;
    std::string sessHandle = "/org/freedesktop/portal/desktop/session/lfg/sess_" + pidStr;

    // 1. CreateSession on org.freedesktop.impl.portal.desktop.kde
    GVariantBuilder emptyOpts;
    g_variant_builder_init(&emptyOpts, G_VARIANT_TYPE("a{sv}"));
    GError* err = nullptr;
    GVariant* createReply = g_dbus_connection_call_sync(
        dbusConn_,
        "org.freedesktop.impl.portal.desktop.kde",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.impl.portal.ScreenCast",
        "CreateSession",
        g_variant_new("(oosa{sv})", reqCreate.c_str(), sessHandle.c_str(), "lfg-linux", &emptyOpts),
        G_VARIANT_TYPE("(ua{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        1500,
        nullptr,
        &err);
    if (!createReply) {
        if (err) g_error_free(err);
        return false;
    }
    guint32 createResp = 1;
    g_variant_get(createReply, "(u@a{sv})", &createResp, nullptr);
    g_variant_unref(createReply);
    if (createResp != 0) {
        return false;
    }
    kdeSessionPath_ = sessHandle;

    // 2. SelectSources with RestoreData matching the active window's (appId, title)
    std::vector<uint8_t> payloadBytes = buildKdeWindowRestorePayload(win.appId, win.title);
    GVariant* byteArrayVar = g_variant_new_fixed_array(
        G_VARIANT_TYPE_BYTE, payloadBytes.data(), payloadBytes.size(), sizeof(uint8_t));
    GVariant* restoreDataStruct = g_variant_new(
        "(suv)", "KDE", static_cast<guint32>(1), byteArrayVar);

    GVariantBuilder selectOpts;
    g_variant_builder_init(&selectOpts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&selectOpts, "{sv}", "types", g_variant_new_uint32(2u));        // Window
    g_variant_builder_add(&selectOpts, "{sv}", "multiple", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&selectOpts, "{sv}", "cursor_mode", g_variant_new_uint32(1u)); // Hidden
    g_variant_builder_add(&selectOpts, "{sv}", "persist_mode", g_variant_new_uint32(2u));
    g_variant_builder_add(&selectOpts, "{sv}", "restore_data", restoreDataStruct);

    GVariant* selectReply = g_dbus_connection_call_sync(
        dbusConn_,
        "org.freedesktop.impl.portal.desktop.kde",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.impl.portal.ScreenCast",
        "SelectSources",
        g_variant_new("(oosa{sv})", reqSelect.c_str(), sessHandle.c_str(), "lfg-linux", &selectOpts),
        G_VARIANT_TYPE("(ua{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        1500,
        nullptr,
        &err);
    if (!selectReply) {
        if (err) g_error_free(err);
        close();
        return false;
    }
    guint32 selectResp = 1;
    g_variant_get(selectReply, "(u@a{sv})", &selectResp, nullptr);
    g_variant_unref(selectReply);
    if (selectResp != 0) {
        close();
        return false;
    }

    // 3. Start session — since restore_data matches the active window, xdg-desktop-portal-kde
    //    calls continueStartAfterDialog() directly without showing ScreenChooserDialog.
    GVariantBuilder startOpts;
    g_variant_builder_init(&startOpts, G_VARIANT_TYPE("a{sv}"));
    GVariant* startReply = g_dbus_connection_call_sync(
        dbusConn_,
        "org.freedesktop.impl.portal.desktop.kde",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.impl.portal.ScreenCast",
        "Start",
        g_variant_new("(oossa{sv})", reqStart.c_str(), sessHandle.c_str(), "lfg-linux", "", &startOpts),
        G_VARIANT_TYPE("(ua{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        4000,
        nullptr,
        &err);
    if (!startReply) {
        if (err) g_error_free(err);
        close();
        return false;
    }

    guint32 startResp = 1;
    GVariant* resultsDict = nullptr;
    g_variant_get(startReply, "(u@a{sv})", &startResp, &resultsDict);
    g_variant_unref(startReply);

    if (startResp != 0 || !resultsDict) {
        if (resultsDict) g_variant_unref(resultsDict);
        close();
        return false;
    }

    GVariant* streams = g_variant_lookup_value(resultsDict, "streams", G_VARIANT_TYPE("a(ua{sv})"));
    if (!streams) {
        g_variant_unref(resultsDict);
        close();
        return false;
    }

    try {
        parseFirstStream(streams, outTarget);
    } catch (...) {
        g_variant_unref(streams);
        g_variant_unref(resultsDict);
        close();
        return false;
    }
    g_variant_unref(streams);
    g_variant_unref(resultsDict);

    if (outTarget.width <= 0 && win.width > 0) outTarget.width = win.width;
    if (outTarget.height <= 0 && win.height > 0) outTarget.height = win.height;
    outTarget.x = win.x;
    outTarget.y = win.y;
    outTarget.pipewireFd = -1; // Connect directly to local PipeWire daemon for impl portal nodeId
    kdeSessionPath_ = sessHandle;

    std::fprintf(stderr,
                 "[lfg] Auto-selected active top window '%s' (%s) [%dx%d at %d,%d] -> PipeWire node %u\n",
                 win.title.c_str(), win.appId.c_str(),
                 outTarget.width, outTarget.height, outTarget.x, outTarget.y, outTarget.nodeId);
    return true;
}

CaptureTarget PortalSession::requestCapture(CaptureSource source, bool autoSelectActiveWindow) {
    close();

    // When starting via shortcut or Start button in Window mode, automatically capture the
    // currently active top window without showing the Choose Window dialog.
    if (autoSelectActiveWindow && source == CaptureSource::Window) {
        CaptureTarget autoTarget{};
        if (tryAutoSelectActiveKdeWindow(autoTarget)) {
            return autoTarget;
        }
    }

    NegotiationState state;
    state.portal = portal_;
    state.loop = g_main_loop_new(nullptr, FALSE);

    std::string savedToken = autoSelectActiveWindow ? loadSavedRestoreToken() : std::string{};
    const char* tokenPtr = savedToken.empty() ? nullptr : savedToken.c_str();

    // Window capture is the intended mode: it records just the chosen window, so our own
    // full-screen overlay on top is not part of what we capture. Monitor capture records the
    // whole screen, overlay included, which feeds the overlay back into itself — it exists only
    // as a fallback for testing capture on setups where per-window capture is unavailable.
    xdp_portal_create_screencast_session(
        portal_, source == CaptureSource::Window ? XDP_OUTPUT_WINDOW : XDP_OUTPUT_MONITOR,
        XDP_SCREENCAST_FLAG_NONE, XDP_CURSOR_MODE_HIDDEN, XDP_PERSIST_MODE_PERSISTENT, tokenPtr,
        nullptr, onSessionCreated, &state);

    g_main_loop_run(state.loop);
    g_main_loop_unref(state.loop);

    if (state.error) {
        std::string message = state.error->message ? state.error->message : "unknown portal error";
        g_error_free(state.error);
        throw std::runtime_error("ScreenCast portal negotiation failed: " + message);
    }
    if (!state.session) throw std::runtime_error("ScreenCast portal negotiation returned no session");

    session_ = state.session;

    if (char* newToken = xdp_session_get_restore_token(session_)) {
        saveRestoreToken(newToken);
        g_free(newToken);
    }

    CaptureTarget target;
    GVariant* streams = xdp_session_get_streams(session_);
    if (!streams) throw std::runtime_error("Portal session has no streams after starting");
    parseFirstStream(streams, target);
    g_variant_unref(streams);

    target.pipewireFd = xdp_session_open_pipewire_remote(session_);
    if (target.pipewireFd < 0) throw std::runtime_error("Failed to open the portal's PipeWire remote");

    return target;
}

void PortalSession::close() {
    if (!kdeSessionPath_.empty() && dbusConn_) {
        GVariant* r = g_dbus_connection_call_sync(
            dbusConn_,
            "org.freedesktop.impl.portal.desktop.kde",
            kdeSessionPath_.c_str(),
            "org.freedesktop.impl.portal.Session",
            "Close",
            nullptr,
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            1000,
            nullptr,
            nullptr);
        if (r) g_variant_unref(r);
        kdeSessionPath_.clear();
    }
    if (session_) {
        xdp_session_close(session_);
        g_object_unref(session_);
        session_ = nullptr;
    }
}

}  // namespace lfg::capture
