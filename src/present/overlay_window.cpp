#include "lfg/present/overlay_window.hpp"

#include <gio/gio.h>
#include <vulkan/vulkan_xcb.h>
#include <xcb/randr.h>
#include <xcb/shape.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace lfg::present {

namespace {

xcb_atom_t internAtom(xcb_connection_t* c, const char* name) {
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(c, 0, static_cast<uint16_t>(std::strlen(name)), name);
    xcb_generic_error_t* err = nullptr;
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(c, cookie, &err);
    if (err) { free(err); return static_cast<xcb_atom_t>(XCB_ATOM_NONE); }
    xcb_atom_t atom = reply ? reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    free(reply);
    return atom;
}

const uint8_t* glyph5x7(char c) {
    static const uint8_t kSpace[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t kF[7]     = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    static const uint8_t kP[7]     = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    static const uint8_t kS[7]     = {0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E};
    static const uint8_t kX[7]     = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
    static const uint8_t kColon[7] = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
    static const uint8_t kDash[7]  = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    static const uint8_t kGt[7]    = {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08};
    static const uint8_t kDigits[10][7] = {
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, // 0
        {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, // 1
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, // 2
        {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, // 3
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, // 4
        {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, // 5
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, // 6
        {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // 7
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, // 8
        {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, // 9
    };
    if (c >= '0' && c <= '9') return kDigits[c - '0'];
    switch (c) {
        case 'F': return kF;
        case 'P': return kP;
        case 'S': return kS;
        case 'X': return kX;
        case ':': return kColon;
        case '-': return kDash;
        case '>': return kGt;
        default:  return kSpace;
    }
}

uint32_t packColor(VkFormat format, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB) {
        return (static_cast<uint32_t>(a) << 24) |
               (static_cast<uint32_t>(b) << 16) |
               (static_cast<uint32_t>(g) << 8)  |
               static_cast<uint32_t>(r);
    }
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8)  |
           static_cast<uint32_t>(b);
}

struct KWinTargetPlacement {
    bool received = false;
    bool isMaximized = false;
    int32_t x = 0;
    int32_t y = 0;
};

const char* kTargetInfoIntrospectionXml = R"XML(
<node>
  <interface name="org.lfg.TargetInfo">
    <method name="ReportRect">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
      <arg name="w" type="i" direction="in"/>
      <arg name="h" type="i" direction="in"/>
      <arg name="isMax" type="i" direction="in"/>
    </method>
  </interface>
</node>
)XML";

void targetInfoMethodCall(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                          const gchar* methodName, GVariant* parameters,
                          GDBusMethodInvocation* invocation, gpointer userData) {
    auto* out = static_cast<KWinTargetPlacement*>(userData);
    if (out && std::strcmp(methodName, "ReportRect") == 0) {
        gint32 rx = 0, ry = 0, rw = 0, rh = 0, isMax = 0;
        g_variant_get(parameters, "(iiiii)", &rx, &ry, &rw, &rh, &isMax);
        out->received = true;
        out->isMaximized = (isMax != 0);
        out->x = rx;
        out->y = ry;
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

const GDBusInterfaceVTable kTargetInfoVTable = {
    targetInfoMethodCall,
    nullptr,
    nullptr,
    {nullptr}
};

// Queries (and for non-maximized floating windows, centers) the captured window via KWin 6 Scripting.
// IMPORTANT:
//  1. Never modifies `w.frameGeometry` if the window is maximized or fullscreen — modifying
//     `frameGeometry` in KWin un-maximizes the window!
//  2. Reports the exact on-screen coordinates of the captured client region back over D-Bus so
//     the overlay blits at the exact same (x, y) as the real window underneath (eliminating vertical
//     mouse offset caused by panels/taskbars or window titlebars).
KWinTargetPlacement queryOrCenterViaKWinScript(const std::vector<TargetRect>& monitors,
                                               const TargetRect& mon,
                                               int preferredDisplay,
                                               uint32_t capW, uint32_t capH) {
    KWinTargetPlacement placement{};

    GError* err = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!bus) {
        if (err) g_error_free(err);
        return placement;
    }

    const char* uniqueBusName = g_dbus_connection_get_unique_name(bus);
    GDBusNodeInfo* nodeInfo = g_dbus_node_info_new_for_xml(kTargetInfoIntrospectionXml, &err);
    if (err) { g_error_free(err); err = nullptr; }

    guint regId = 0;
    if (nodeInfo && nodeInfo->interfaces && nodeInfo->interfaces[0] && uniqueBusName) {
        regId = g_dbus_connection_register_object(
            bus, "/LfgTargetInfo", nodeInfo->interfaces[0], &kTargetInfoVTable, &placement, nullptr, nullptr);
    }

    const char* jsPath = "/tmp/lfg-center-win.js";
    {
        std::ofstream out(jsPath, std::ios::trunc);
        if (!out.is_open()) {
            if (regId) g_dbus_connection_unregister_object(bus, regId);
            if (nodeInfo) g_dbus_node_info_unref(nodeInfo);
            g_object_unref(bus);
            return placement;
        }
        out << "const mons = [\n";
        for (const auto& m : monitors) {
            out << "    { x: " << m.x << ", y: " << m.y
                << ", width: " << m.width << ", height: " << m.height << " },\n";
        }
        out << "];\n"
            << "const wins = workspace.stackingOrder;\n"
            << "for (let i = wins.length - 1; i >= 0; --i) {\n"
            << "    const w = wins[i];\n"
            << "    if (!w || w.specialWindow || w.minimized) continue;\n"
            << "    const cap = String(w.caption || '');\n"
            << "    const cls = String(w.resourceClass || '');\n"
            << "    const df  = String(w.desktopFileName || '');\n"
            << "    if (cap === 'LWFG' || cap.indexOf('LWFG') !== -1 || cap.indexOf('LFG') !== -1 || cap.indexOf('Overlay') !== -1) continue;\n"
            << "    if (cls === 'lfg' || cls === 'LFG' || cls === 'lfg-linux' || df === 'lfg' || df === 'lfg-linux') continue;\n"
            << "    const fg = w.frameGeometry;\n"
            << "    const cg = w.clientGeometry ? w.clientGeometry : fg;\n"
            << "    const fw = Math.round(fg.width);\n"
            << "    const fh = Math.round(fg.height);\n"
            << "    const cw = Math.round(cg.width);\n"
            << "    const ch = Math.round(cg.height);\n"
            << "    const errClient = Math.abs(cw - " << capW << ") + Math.abs(ch - " << capH << ");\n"
            << "    const errFrame  = Math.abs(fw - " << capW << ") + Math.abs(fh - " << capH << ");\n"
            << "    const matchClient = (Math.abs(cw - " << capW << ") <= 24 && Math.abs(ch - " << capH << ") <= 24);\n"
            << "    const matchFrame  = (Math.abs(fw - " << capW << ") <= 48 && Math.abs(fh - " << capH << ") <= 96);\n"
            << "    if (!matchClient && !matchFrame) continue;\n"
            << "    const useClient = (errClient <= errFrame);\n"
            << "    let targetMon = { x: " << mon.x << ", y: " << mon.y
            << ", width: " << mon.width << ", height: " << mon.height << " };\n"
            << "    if (" << preferredDisplay << " < 0 && mons.length > 0) {\n"
            << "        const wcx = Math.round(cg.x + cw / 2);\n"
            << "        const wcy = Math.round(cg.y + ch / 2);\n"
            << "        for (let m = 0; m < mons.length; ++m) {\n"
            << "            if (wcx >= mons[m].x && wcx < mons[m].x + mons[m].width &&\n"
            << "                wcy >= mons[m].y && wcy < mons[m].y + mons[m].height) {\n"
            << "                targetMon = mons[m];\n"
            << "                break;\n"
            << "            }\n"
            << "        }\n"
            << "    }\n"
            << "    const isMax = Boolean(w.fullScreen) ||\n"
            << "                  (w.maximizeMode !== undefined && w.maximizeMode !== 0) ||\n"
            << "                  (fw >= targetMon.width - 64 && fh >= targetMon.height - 128);\n"
            << "    if (!isMax) {\n"
            << "        const offX = useClient ? Math.round(cg.x - fg.x) : 0;\n"
            << "        const offY = useClient ? Math.round(cg.y - fg.y) : 0;\n"
            << "        const targetX = targetMon.x + Math.round((targetMon.width - " << capW << ") / 2) - offX;\n"
            << "        const targetY = targetMon.y + Math.round((targetMon.height - " << capH << ") / 2) - offY;\n"
            << "        w.frameGeometry = {\n"
            << "            x: targetX,\n"
            << "            y: targetY,\n"
            << "            width: fw,\n"
            << "            height: fh\n"
            << "        };\n"
            << "    }\n"
            << "    const fg2 = w.frameGeometry;\n"
            << "    const cg2 = w.clientGeometry ? w.clientGeometry : fg2;\n"
            << "    const rx = Math.round(useClient ? cg2.x : fg2.x);\n"
            << "    const ry = Math.round(useClient ? cg2.y : fg2.y);\n";
        if (uniqueBusName && regId != 0) {
            out << "    try {\n"
                << "        callDBus('" << uniqueBusName << "', '/LfgTargetInfo', 'org.lfg.TargetInfo',\n"
                << "                 'ReportRect', rx, ry, " << capW << ", " << capH << ", isMax ? 1 : 0);\n"
                << "    } catch (e) {}\n";
        }
        out << "    break;\n"
            << "}\n";
    }

    const char* pluginName = "lfg_center_target";
    GVariant* unloadRes = g_dbus_connection_call_sync(
        bus, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "unloadScript",
        g_variant_new("(s)", pluginName), nullptr, G_DBUS_CALL_FLAGS_NONE, 200, nullptr, nullptr);
    if (unloadRes) g_variant_unref(unloadRes);

    GVariant* loadRes = g_dbus_connection_call_sync(
        bus, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "loadScript",
        g_variant_new("(ss)", jsPath, pluginName), G_VARIANT_TYPE("(i)"),
        G_DBUS_CALL_FLAGS_NONE, 300, nullptr, nullptr);
    if (loadRes) {
        gint32 scriptId = -1;
        g_variant_get(loadRes, "(i)", &scriptId);
        g_variant_unref(loadRes);

        if (scriptId >= 0) {
            std::string scriptObj = "/Scripting/Script" + std::to_string(scriptId);
            GVariant* runRes = g_dbus_connection_call_sync(
                bus, "org.kde.KWin", scriptObj.c_str(), "org.kde.kwin.Script", "run",
                nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 300, nullptr, nullptr);
            if (runRes) g_variant_unref(runRes);

            // Pump the GLib main context briefly so KWin's callDBus(ReportRect) arrives before unload.
            for (int waitMs = 0; waitMs < 35 && !placement.received; ++waitMs) {
                while (g_main_context_iteration(nullptr, FALSE)) {}
                if (!placement.received) ::usleep(1000);
            }

            GVariant* stopRes = g_dbus_connection_call_sync(
                bus, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "unloadScript",
                g_variant_new("(s)", pluginName), nullptr, G_DBUS_CALL_FLAGS_NONE, 200, nullptr, nullptr);
            if (stopRes) g_variant_unref(stopRes);
        }
    }

    if (regId) g_dbus_connection_unregister_object(bus, regId);
    if (nodeInfo) g_dbus_node_info_unref(nodeInfo);
    g_object_unref(bus);
    ::unlink(jsPath);
    return placement;
}

}  // namespace

OverlayWindow::OverlayWindow(VulkanContext& vk, const TargetRect& rect, int preferredDisplay,
                             bool showFps, bool vsync)
    : vk_(vk), preferredDisplay_(preferredDisplay), vsync_(vsync), showFps_(showFps) {
    connection_ = xcb_connect(nullptr, &screenNumber_);
    if (!connection_ || xcb_connection_has_error(connection_))
        throw std::runtime_error("xcb_connect failed — no X11/XWayland display available");

    createXcbWindow(rect, preferredDisplay);
    createSurfaceAndSwapchain();
    createHudBuffer();
    renderHudBitmap(0, 0);

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(vkCreateFence(vk_.device(), &fenceInfo, nullptr, &inFlightFence_));

    VkCommandBufferAllocateInfo cmdAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdAlloc.commandPool = vk_.graphicsCommandPool();
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(vk_.device(), &cmdAlloc, &blitCmd_));
}

OverlayWindow::~OverlayWindow() {
    if (vk_.device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vk_.device());
        destroyHudBuffer();
        if (inFlightFence_) vkDestroyFence(vk_.device(), inFlightFence_, nullptr);

        destroySwapchain();
        if (surface_) vkDestroySurfaceKHR(vk_.instance(), surface_, nullptr);
    }
    if (connection_) xcb_disconnect(connection_);
}

std::vector<const char*> OverlayWindow::requiredInstanceExtensions() {
    return {VK_KHR_XCB_SURFACE_EXTENSION_NAME};
}

void OverlayWindow::show() {
    if (!connection_ || !window_ || mapped_) return;
    makeClickThroughAndBorderless();
    xcb_map_window(connection_, window_);
    uint32_t stackMode = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(connection_, window_, XCB_CONFIG_WINDOW_STACK_MODE, &stackMode);
    xcb_flush(connection_);
    mapped_ = true;
}

void OverlayWindow::hide() {
    if (!connection_ || !window_ || !mapped_) return;
    xcb_unmap_window(connection_, window_);
    xcb_flush(connection_);
    mapped_ = false;
}

void OverlayWindow::setPreferredDisplay(int displayIndex, const TargetRect& fallbackRect) {
    preferredDisplay_ = displayIndex;
    TargetRect rect = resolveTargetMonitor(fallbackRect, preferredDisplay_);
    monitorRect_ = rect;
    updateTargetRect(rect);
    if (swapchainExtent_.width != rect.width || swapchainExtent_.height != rect.height) {
        swapchainExtent_ = {rect.width, rect.height};
        recreateSwapchain();
    }
}

void OverlayWindow::setVsync(bool enabled) {
    if (vsync_ == enabled) return;
    vsync_ = enabled;
    if (surface_ != VK_NULL_HANDLE) {
        recreateSwapchain();
    }
}

void OverlayWindow::alignWindowedTarget(uint32_t capWidth, uint32_t capHeight) {
    hasTargetPos_ = false;
    if (capWidth == 0 || capHeight == 0) return;

    // 1. Ask KWin for the target window's exact client geometry (and in AUTO display mode, verify
    //    which monitor the captured window is actually on).
    if (capWidth < monitorRect_.width || capHeight < monitorRect_.height ||
        (preferredDisplay_ < 0 && monitors_.size() > 1)) {
        KWinTargetPlacement kwin =
            queryOrCenterViaKWinScript(monitors_, monitorRect_, preferredDisplay_, capWidth, capHeight);
        if (kwin.received) {
            if (preferredDisplay_ < 0 && monitors_.size() > 1) {
                const int32_t wcx = kwin.x + static_cast<int32_t>(capWidth) / 2;
                const int32_t wcy = kwin.y + static_cast<int32_t>(capHeight) / 2;
                for (size_t i = 0; i < monitors_.size(); ++i) {
                    const auto& m = monitors_[i];
                    if (wcx >= m.x && wcx < m.x + static_cast<int32_t>(m.width) &&
                        wcy >= m.y && wcy < m.y + static_cast<int32_t>(m.height)) {
                        if (monitorRect_.x != m.x || monitorRect_.y != m.y ||
                            monitorRect_.width != m.width || monitorRect_.height != m.height) {
                            monitorRect_ = m;
                            if (i < monitorHz_.size() && monitorHz_[i] > 0.0) {
                                refreshHz_ = monitorHz_[i];
                            }
                            updateTargetRect(m);
                            swapchainExtent_ = {m.width, m.height};
                            recreateSwapchain();
                            std::fprintf(stderr,
                                         "[overlay] auto-moved overlay to target window's display [%zu] (%ux%u+%d+%d @ %.2f Hz)\n",
                                         i, m.width, m.height, m.x, m.y, refreshHz_);
                        }
                        break;
                    }
                }
            }
            if (capWidth >= monitorRect_.width && capHeight >= monitorRect_.height) {
                return;
            }
            targetDstX_ = kwin.x - monitorRect_.x;
            targetDstY_ = kwin.y - monitorRect_.y;
            hasTargetPos_ = true;
            std::fprintf(stderr, "[overlay] aligned to target window at +%d+%d (%s)\n",
                         targetDstX_, targetDstY_, kwin.isMaximized ? "maximized - preserved" : "centered");
            return;
        }
    }

    if (capWidth >= monitorRect_.width && capHeight >= monitorRect_.height) return;

    const bool likelyMaximized =
        (capWidth >= monitorRect_.width - 64 && capHeight >= monitorRect_.height - 128);

    if (!connection_) return;
    const xcb_setup_t* setup = xcb_get_setup(connection_);
    xcb_screen_iterator_t screenIter = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screenNumber_; ++i) xcb_screen_next(&screenIter);
    xcb_screen_t* screen = screenIter.data;

    // 2. Fallback for maximized windows when KWin scripting did not report:
    //    Never move/unmaximize a maximized window! Align it to the monitor's work area (typically
    //    top-left at y = 0 when the taskbar is at the bottom, or y = workY when the panel is at the top).
    if (likelyMaximized) {
        int32_t workX = monitorRect_.x;
        int32_t workY = monitorRect_.y;
        int32_t workW = static_cast<int32_t>(monitorRect_.width);
        xcb_atom_t netWorkArea = internAtom(connection_, "_NET_WORKAREA");
        if (netWorkArea != XCB_ATOM_NONE) {
            xcb_get_property_cookie_t waCookie =
                xcb_get_property(connection_, 0, screen->root, netWorkArea, XCB_ATOM_CARDINAL, 0, 4);
            xcb_get_property_reply_t* waReply = xcb_get_property_reply(connection_, waCookie, nullptr);
            if (waReply) {
                if (xcb_get_property_value_length(waReply) >= static_cast<int>(4 * sizeof(uint32_t))) {
                    const auto* wa = static_cast<const uint32_t*>(xcb_get_property_value(waReply));
                    int32_t wx = static_cast<int32_t>(wa[0]);
                    int32_t wy = static_cast<int32_t>(wa[1]);
                    int32_t ww = static_cast<int32_t>(wa[2]);
                    if (wx >= monitorRect_.x && wx < monitorRect_.x + static_cast<int32_t>(monitorRect_.width) &&
                        wy >= monitorRect_.y && wy < monitorRect_.y + static_cast<int32_t>(monitorRect_.height)) {
                        workX = wx;
                        workY = wy;
                        workW = ww;
                    }
                }
                free(waReply);
            }
        }
        targetDstX_ = (workX - monitorRect_.x) + std::max(0, (workW - static_cast<int32_t>(capWidth)) / 2);
        targetDstY_ = (workY - monitorRect_.y);
        hasTargetPos_ = true;
        return;
    }

    // 3. Fallback for non-maximized X11/XWayland floating windows:
    xcb_query_tree_cookie_t treeCookie = xcb_query_tree(connection_, screen->root);
    xcb_query_tree_reply_t* tree = xcb_query_tree_reply(connection_, treeCookie, nullptr);
    if (!tree) return;

    xcb_window_t* children = xcb_query_tree_children(tree);
    int len = xcb_query_tree_children_length(tree);

    for (int i = len - 1; i >= 0; --i) {
        xcb_window_t w = children[i];
        if (w == window_) continue;

        xcb_get_window_attributes_cookie_t attrCookie = xcb_get_window_attributes(connection_, w);
        xcb_get_geometry_cookie_t geomCookie = xcb_get_geometry(connection_, w);
        xcb_get_window_attributes_reply_t* attr = xcb_get_window_attributes_reply(connection_, attrCookie, nullptr);
        xcb_get_geometry_reply_t* geom = xcb_get_geometry_reply(connection_, geomCookie, nullptr);

        if (attr && geom && attr->map_state == XCB_MAP_STATE_VIEWABLE && !attr->override_redirect) {
            int dw = std::abs(static_cast<int>(geom->width) - static_cast<int>(capWidth));
            int dh = std::abs(static_cast<int>(geom->height) - static_cast<int>(capHeight));
            if (dw <= 48 && dh <= 96 && !(geom->width == 440 && (geom->height == 388 || geom->height == 438))) {
                int32_t newX = monitorRect_.x + (static_cast<int32_t>(monitorRect_.width) - geom->width) / 2;
                int32_t newY = monitorRect_.y + (static_cast<int32_t>(monitorRect_.height) - geom->height) / 2;
                uint32_t vals[2] = {static_cast<uint32_t>(newX), static_cast<uint32_t>(newY)};
                xcb_configure_window(connection_, w, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
                targetDstX_ = newX - monitorRect_.x;
                targetDstY_ = newY - monitorRect_.y;
                hasTargetPos_ = true;
                free(attr);
                free(geom);
                break;
            }
        }
        if (attr) free(attr);
        if (geom) free(geom);
    }

    free(tree);
    xcb_flush(connection_);
}

TargetRect OverlayWindow::resolveTargetMonitor(const TargetRect& requested, int preferredDisplay) {
    const xcb_setup_t* setup = xcb_get_setup(connection_);
    xcb_screen_iterator_t screenIter = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screenNumber_; ++i) xcb_screen_next(&screenIter);
    xcb_screen_t* screen = screenIter.data;

    monitors_.clear();
    monitorHz_.clear();
    xcb_randr_get_screen_resources_current_cookie_t resCookie =
        xcb_randr_get_screen_resources_current(connection_, screen->root);
    xcb_randr_get_screen_resources_current_reply_t* resReply =
        xcb_randr_get_screen_resources_current_reply(connection_, resCookie, nullptr);

    if (resReply) {
        int numCrtcs = xcb_randr_get_screen_resources_current_crtcs_length(resReply);
        xcb_randr_crtc_t* crtcs = xcb_randr_get_screen_resources_current_crtcs(resReply);
        const int numModes = xcb_randr_get_screen_resources_current_modes_length(resReply);
        const xcb_randr_mode_info_t* modes = xcb_randr_get_screen_resources_current_modes(resReply);
        auto modeRefreshHz = [&](xcb_randr_mode_t id) -> double {
            for (int m = 0; m < numModes; ++m) {
                if (modes[m].id != id) continue;
                double vtotal = modes[m].vtotal;
                if (modes[m].mode_flags & XCB_RANDR_MODE_FLAG_DOUBLE_SCAN) vtotal *= 2.0;
                if (modes[m].mode_flags & XCB_RANDR_MODE_FLAG_INTERLACE) vtotal /= 2.0;
                if (modes[m].htotal == 0 || vtotal <= 0.0) return 0.0;
                return static_cast<double>(modes[m].dot_clock) / (static_cast<double>(modes[m].htotal) * vtotal);
            }
            return 0.0;
        };
        for (int i = 0; i < numCrtcs; ++i) {
            xcb_randr_get_crtc_info_cookie_t crtcCookie =
                xcb_randr_get_crtc_info(connection_, crtcs[i], resReply->config_timestamp);
            xcb_randr_get_crtc_info_reply_t* crtc =
                xcb_randr_get_crtc_info_reply(connection_, crtcCookie, nullptr);
            if (crtc) {
                if (crtc->width > 0 && crtc->height > 0 &&
                    xcb_randr_get_crtc_info_outputs_length(crtc) > 0) {
                    monitors_.push_back(TargetRect{crtc->x, crtc->y, crtc->width, crtc->height});
                    monitorHz_.push_back(modeRefreshHz(crtc->mode));
                }
                free(crtc);
            }
        }
        free(resReply);
    }

    const char* forcedHz = std::getenv("LFG_REFRESH_HZ");
    auto pick = [&](size_t i) -> TargetRect {
        refreshHz_ = (i < monitorHz_.size()) ? monitorHz_[i] : 0.0;
        if (forcedHz) refreshHz_ = std::max(0.0, std::atof(forcedHz));
        std::fprintf(stderr, "[overlay] display refresh rate: %.2f Hz%s\n", refreshHz_,
                     forcedHz ? " (LFG_REFRESH_HZ override)" : (refreshHz_ > 0.0 ? "" : " (unknown)"));
        return monitors_[i];
    };

    if (monitors_.empty()) {
        monitors_.push_back(TargetRect{0, 0, screen->width_in_pixels, screen->height_in_pixels});
        monitorHz_.push_back(0.0);
        return pick(0);
    }

    std::fprintf(stderr, "[overlay] detected %zu display(s):\n", monitors_.size());
    for (size_t i = 0; i < monitors_.size(); ++i) {
        std::fprintf(stderr, "  [%zu] %ux%u at +%d+%d @ %.2f Hz\n", i, monitors_[i].width, monitors_[i].height,
                     monitors_[i].x, monitors_[i].y, monitorHz_[i]);
    }

    if (preferredDisplay >= 0 && static_cast<size_t>(preferredDisplay) < monitors_.size()) {
        std::fprintf(stderr, "[overlay] using display [%d]\n", preferredDisplay);
        return pick(static_cast<size_t>(preferredDisplay));
    }

    if (requested.width > 0 && requested.height > 0) {
        int32_t cx = requested.x + static_cast<int32_t>(requested.width) / 2;
        int32_t cy = requested.y + static_cast<int32_t>(requested.height) / 2;
        for (size_t i = 0; i < monitors_.size(); ++i) {
            const auto& m = monitors_[i];
            if (cx >= m.x && cx < m.x + static_cast<int32_t>(m.width) &&
                cy >= m.y && cy < m.y + static_cast<int32_t>(m.height)) {
                std::fprintf(stderr, "[overlay] matched target window position to display [%zu] (%ux%u+%d+%d)\n",
                             i, m.width, m.height, m.x, m.y);
                return pick(i);
            }
        }
    }

    xcb_query_pointer_cookie_t ptrCookie = xcb_query_pointer(connection_, screen->root);
    xcb_query_pointer_reply_t* ptrReply = xcb_query_pointer_reply(connection_, ptrCookie, nullptr);
    if (ptrReply) {
        int32_t px = ptrReply->root_x;
        int32_t py = ptrReply->root_y;
        free(ptrReply);
        for (size_t i = 0; i < monitors_.size(); ++i) {
            const auto& m = monitors_[i];
            if (px >= m.x && px < m.x + static_cast<int32_t>(m.width) &&
                py >= m.y && py < m.y + static_cast<int32_t>(m.height)) {
                std::fprintf(stderr, "[overlay] placing overlay on active display [%zu] (%ux%u+%d+%d)\n",
                             i, m.width, m.height, m.x, m.y);
                return pick(i);
            }
        }
    }

    return pick(0);
}

void OverlayWindow::createXcbWindow(const TargetRect& requested, int preferredDisplay) {
    const xcb_setup_t* setup = xcb_get_setup(connection_);
    xcb_screen_iterator_t screenIter = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screenNumber_; ++i) xcb_screen_next(&screenIter);
    xcb_screen_t* screen = screenIter.data;

    TargetRect rect = resolveTargetMonitor(requested, preferredDisplay);
    monitorRect_ = rect;

    window_ = xcb_generate_id(connection_);
    uint32_t valueMask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
                          XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t values[] = {
        screen->black_pixel, 0, 1, XCB_EVENT_MASK_STRUCTURE_NOTIFY, screen->default_colormap,
    };
    xcb_create_window(connection_, screen->root_depth, window_, screen->root, static_cast<int16_t>(rect.x),
                       static_cast<int16_t>(rect.y), static_cast<uint16_t>(rect.width),
                       static_cast<uint16_t>(rect.height), 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                       screen->root_visual, valueMask, values);

    swapchainExtent_ = {rect.width, rect.height};
    makeClickThroughAndBorderless();
    xcb_flush(connection_);
    mapped_ = false;
}

void OverlayWindow::makeClickThroughAndBorderless() {
    xcb_atom_t netWmState = internAtom(connection_, "_NET_WM_STATE");
    xcb_atom_t stateAbove = internAtom(connection_, "_NET_WM_STATE_ABOVE");
    xcb_atom_t stateSkipTaskbar = internAtom(connection_, "_NET_WM_STATE_SKIP_TASKBAR");
    xcb_atom_t stateSkipPager = internAtom(connection_, "_NET_WM_STATE_SKIP_PAGER");
    xcb_atom_t states[] = {stateAbove, stateSkipTaskbar, stateSkipPager};
    xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, window_, netWmState, XCB_ATOM_ATOM, 32,
                         3, states);

    xcb_atom_t netWmWindowType = internAtom(connection_, "_NET_WM_WINDOW_TYPE");
    xcb_atom_t typeUtility = internAtom(connection_, "_NET_WM_WINDOW_TYPE_UTILITY");
    xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, window_, netWmWindowType, XCB_ATOM_ATOM,
                         32, 1, &typeUtility);

    const char* overlayTitle = "LFG Overlay";
    xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, window_, XCB_ATOM_WM_NAME,
                        XCB_ATOM_STRING, 8, static_cast<uint32_t>(std::strlen(overlayTitle)), overlayTitle);
    xcb_atom_t netWmName = internAtom(connection_, "_NET_WM_NAME");
    xcb_atom_t utf8String = internAtom(connection_, "UTF8_STRING");
    if (netWmName != XCB_ATOM_NONE && utf8String != XCB_ATOM_NONE) {
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, window_, netWmName,
                            utf8String, 8, static_cast<uint32_t>(std::strlen(overlayTitle)), overlayTitle);
    }
    const char wmClass[] = "lfg\0LFG\0";
    xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, window_, XCB_ATOM_WM_CLASS,
                        XCB_ATOM_STRING, 8, sizeof(wmClass) - 1, wmClass);

    xcb_atom_t netWmWindowOpacity = internAtom(connection_, "_NET_WM_WINDOW_OPACITY");
    if (netWmWindowOpacity != XCB_ATOM_NONE) {
        uint32_t opacity = 0xFEFFFFFFu;
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, window_, netWmWindowOpacity,
                             XCB_ATOM_CARDINAL, 32, 1, &opacity);
    }

    const xcb_query_extension_reply_t* shapeExt = xcb_get_extension_data(connection_, &xcb_shape_id);
    if (shapeExt && shapeExt->present) {
        xcb_shape_rectangles(connection_, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED,
                              window_, 0, 0, 0, nullptr);
    } else {
        std::fprintf(stderr, "[overlay] X SHAPE extension unavailable — overlay will NOT be click-through\n");
    }
}

void OverlayWindow::createHudBuffer() {
    VkDeviceSize size = static_cast<VkDeviceSize>(kHudWidth) * kHudHeight * sizeof(uint32_t);
    VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size = size;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(vk_.device(), &bufInfo, nullptr, &hudBuffer_));

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(vk_.device(), hudBuffer_, &memReqs);
    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = vk_.findMemoryType(
        memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(vk_.device(), &allocInfo, nullptr, &hudMemory_));
    VK_CHECK(vkBindBufferMemory(vk_.device(), hudBuffer_, hudMemory_, 0));

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(vk_.device(), hudMemory_, 0, size, 0, &mapped));
    hudMapped_ = static_cast<uint32_t*>(mapped);
}

void OverlayWindow::destroyHudBuffer() {
    if (hudMemory_) {
        if (hudMapped_) {
            vkUnmapMemory(vk_.device(), hudMemory_);
            hudMapped_ = nullptr;
        }
        vkFreeMemory(vk_.device(), hudMemory_, nullptr);
        hudMemory_ = VK_NULL_HANDLE;
    }
    if (hudBuffer_) {
        vkDestroyBuffer(vk_.device(), hudBuffer_, nullptr);
        hudBuffer_ = VK_NULL_HANDLE;
    }
}

void OverlayWindow::renderHudBitmap(int capFps, int outFps) {
    if (!hudMapped_) return;
    if (capFps == lastCapFps_ && outFps == lastOutFps_ && !hudCopies_.empty()) return;
    lastCapFps_ = capFps;
    lastOutFps_ = outFps;

    // Transparent background: clear all pixels to 0 (no black bar)
    std::memset(hudMapped_, 0, kHudWidth * kHudHeight * sizeof(uint32_t));
    hudCopies_.clear();

    const uint32_t shadowColor = packColor(swapchainFormat_, 12, 14, 18, 255);
    const uint32_t textColor   = packColor(swapchainFormat_, 255, 255, 255, 255);

    // Show only before -> after framerate (no 2X/3X prefix)
    char label[32];
    std::snprintf(label, sizeof(label), "%3d -> %-3d", capFps, outFps);

    constexpr int kScale = 2;
    constexpr int kCharW = 5 * kScale;
    constexpr int kSpacing = 3;
    int cursorX = 8;
    int startY = (static_cast<int>(kHudHeight) - 7 * kScale) / 2;

    std::vector<std::pair<int, int>> textPixels;
    textPixels.reserve(64 * kScale * kScale);

    for (size_t i = 0; label[i] != '\0'; ++i) {
        const uint8_t* rows = glyph5x7(label[i]);
        for (int gy = 0; gy < 7; ++gy) {
            uint8_t rowBits = rows[gy];
            for (int gx = 0; gx < 5; ++gx) {
                if ((rowBits >> (4 - gx)) & 1) {
                    for (int sy = 0; sy < kScale; ++sy) {
                        for (int sx = 0; sx < kScale; ++sx) {
                            int px = cursorX + gx * kScale + sx;
                            int py = startY + gy * kScale + sy;
                            if (px >= 0 && px < static_cast<int>(kHudWidth) &&
                                py >= 0 && py < static_cast<int>(kHudHeight)) {
                                textPixels.emplace_back(px, py);
                            }
                        }
                    }
                }
            }
        }
        cursorX += kCharW + kSpacing;
    }

    // 1. Draw 1-pixel shadow outline around glyphs so numbers remain crisp over any game background
    for (const auto& [px, py] : textPixels) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                int nx = px + dx;
                int ny = py + dy;
                if (nx >= 0 && nx < static_cast<int>(kHudWidth) &&
                    ny >= 0 && ny < static_cast<int>(kHudHeight)) {
                    if (hudMapped_[ny * kHudWidth + nx] == 0) {
                        hudMapped_[ny * kHudWidth + nx] = shadowColor;
                    }
                }
            }
        }
    }

    // 2. Draw glyph text pixels
    for (const auto& [px, py] : textPixels) {
        hudMapped_[py * kHudWidth + px] = textColor;
    }

    // 3. Scan lines to generate contiguous copy runs (leaving transparent background uncopied)
    for (uint32_t y = 0; y < kHudHeight; ++y) {
        uint32_t x = 0;
        while (x < kHudWidth) {
            if (hudMapped_[y * kHudWidth + x] != 0) {
                uint32_t startX = x;
                while (x < kHudWidth && hudMapped_[y * kHudWidth + x] != 0) {
                    ++x;
                }
                uint32_t runLen = x - startX;
                VkBufferImageCopy copy{};
                copy.bufferOffset = (y * kHudWidth + startX) * sizeof(uint32_t);
                copy.bufferRowLength = kHudWidth;
                copy.bufferImageHeight = kHudHeight;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageOffset = {static_cast<int32_t>(12 + startX), static_cast<int32_t>(12 + y), 0};
                copy.imageExtent = {runLen, 1, 1};
                hudCopies_.push_back(copy);
            } else {
                ++x;
            }
        }
    }
}

void OverlayWindow::setFps(float captureFps, float outputFps) {
    int capRounded = static_cast<int>(std::lround(captureFps));
    int outRounded = static_cast<int>(std::lround(outputFps));
    if (capRounded == lastCapFps_ && outRounded == lastOutFps_) return;
    // The HUD buffer may still be read by an in-flight blit (presentImage no longer blocks after
    // submitting), so wait for it before rewriting the host-visible pixels. Happens at most 4x/second.
    if (inFlightFence_ != VK_NULL_HANDLE) {
        VK_CHECK(vkWaitForFences(vk_.device(), 1, &inFlightFence_, VK_TRUE, UINT64_MAX));
    }
    renderHudBitmap(capRounded, outRounded);
}

void OverlayWindow::createSurfaceAndSwapchain() {
    if (surface_ == VK_NULL_HANDLE) {
        VkXcbSurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR};
        surfaceInfo.connection = connection_;
        surfaceInfo.window = window_;
        VK_CHECK(vkCreateXcbSurfaceKHR(vk_.instance(), &surfaceInfo, nullptr, &surface_));

        VkBool32 canPresent = VK_FALSE;
        VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(vk_.physicalDevice(), vk_.queueFamilies().graphicsPresent,
                                                        surface_, &canPresent));
        if (!canPresent)
            throw std::runtime_error("The selected GPU cannot present to this X11 window "
                                     "(on a hybrid laptop, try running without --nvidia)");
    }

    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_.physicalDevice(), surface_, &caps));

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk_.physicalDevice(), surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk_.physicalDevice(), surface_, &formatCount, formats.data());
    VkSurfaceFormatKHR chosenFormat = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { chosenFormat = f; break; }
    }
    swapchainFormat_ = chosenFormat.format;

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk_.physicalDevice(), surface_, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk_.physicalDevice(), surface_, &presentModeCount,
                                               presentModes.data());
    auto hasMode = [&](VkPresentModeKHR m) {
        return std::find(presentModes.begin(), presentModes.end(), m) != presentModes.end();
    };
    VkPresentModeKHR chosenPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (vsync_) {
        chosenPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    } else if (hasMode(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
        chosenPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    } else if (hasMode(VK_PRESENT_MODE_MAILBOX_KHR)) {
        chosenPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    } else if (hasMode(VK_PRESENT_MODE_FIFO_RELAXED_KHR)) {
        chosenPresentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }
    if (const char* pm = std::getenv("LFG_PRESENT_MODE")) {
        const std::string want = pm;
        if (want == "fifo") chosenPresentMode = VK_PRESENT_MODE_FIFO_KHR;
        else if (want == "relaxed" && hasMode(VK_PRESENT_MODE_FIFO_RELAXED_KHR)) chosenPresentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
        else if (want == "mailbox" && hasMode(VK_PRESENT_MODE_MAILBOX_KHR)) chosenPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (want == "immediate" && hasMode(VK_PRESENT_MODE_IMMEDIATE_KHR)) chosenPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    presentMode_ = chosenPresentMode;
    std::fprintf(stderr, "[overlay] present mode (vsync=%s): %s\n",
                 vsync_ ? "ON" : "OFF",
                 chosenPresentMode == VK_PRESENT_MODE_FIFO_KHR         ? "FIFO (vsync queue)" :
                 chosenPresentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "FIFO_RELAXED" :
                 chosenPresentMode == VK_PRESENT_MODE_MAILBOX_KHR      ? "MAILBOX (tear-free low-latency PLL paced)" :
                                                                         "IMMEDIATE (uncapped low-latency PLL paced)");

    swapchainExtent_ = caps.currentExtent.width != UINT32_MAX
                           ? caps.currentExtent
                           : VkExtent2D{std::clamp(swapchainExtent_.width, caps.minImageExtent.width,
                                                    caps.maxImageExtent.width),
                                        std::clamp(swapchainExtent_.height, caps.minImageExtent.height,
                                                    caps.maxImageExtent.height)};

    uint32_t desiredImages = (chosenPresentMode == VK_PRESENT_MODE_MAILBOX_KHR ||
                              chosenPresentMode == VK_PRESENT_MODE_IMMEDIATE_KHR)
                                 ? 4u
                                 : 3u;
    uint32_t imageCount = std::max(caps.minImageCount + 1, desiredImages);
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR swapInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapInfo.surface = surface_;
    swapInfo.minImageCount = imageCount;
    swapInfo.imageFormat = chosenFormat.format;
    swapInfo.imageColorSpace = chosenFormat.colorSpace;
    swapInfo.imageExtent = swapchainExtent_;
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapInfo.preTransform = caps.currentTransform;
    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (VkCompositeAlphaFlagBitsKHR candidate :
         {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
        if (caps.supportedCompositeAlpha & candidate) { compositeAlpha = candidate; break; }
    }
    swapInfo.compositeAlpha = compositeAlpha;
    swapInfo.presentMode = chosenPresentMode;
    swapInfo.clipped = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(vk_.device(), &swapInfo, nullptr, &swapchain_));

    uint32_t swapImageCount = 0;
    vkGetSwapchainImagesKHR(vk_.device(), swapchain_, &swapImageCount, nullptr);
    swapchainImages_.resize(swapImageCount);
    vkGetSwapchainImagesKHR(vk_.device(), swapchain_, &swapImageCount, swapchainImages_.data());

    imageAvailable_.resize(swapImageCount);
    renderFinished_.resize(swapImageCount);
    syncIndex_ = 0;
    imageBlitRect_.assign(swapImageCount, TargetRect{0, 0, swapchainExtent_.width, swapchainExtent_.height});
    VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < swapImageCount; ++i) {
        VK_CHECK(vkCreateSemaphore(vk_.device(), &semInfo, nullptr, &imageAvailable_[i]));
        VK_CHECK(vkCreateSemaphore(vk_.device(), &semInfo, nullptr, &renderFinished_[i]));
    }

    VkCommandBuffer initCmd = vk_.beginOneShotCommands(vk_.graphicsCommandPool());
    VkClearColorValue blackClear{};
    blackClear.float32[3] = 1.0f;
    VkImageSubresourceRange clearRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (uint32_t i = 0; i < swapImageCount; ++i) {
        VkImageMemoryBarrier2 b1{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b1.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        b1.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        b1.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b1.image = swapchainImages_[i];
        b1.subresourceRange = clearRange;
        VkDependencyInfo d1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d1.imageMemoryBarrierCount = 1;
        d1.pImageMemoryBarriers = &b1;
        vkCmdPipelineBarrier2(initCmd, &d1);

        vkCmdClearColorImage(initCmd, swapchainImages_[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &blackClear, 1, &clearRange);

        VkImageMemoryBarrier2 b2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b2.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        b2.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b2.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b2.image = swapchainImages_[i];
        b2.subresourceRange = clearRange;
        VkDependencyInfo d2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d2.imageMemoryBarrierCount = 1;
        d2.pImageMemoryBarriers = &b2;
        vkCmdPipelineBarrier2(initCmd, &d2);
    }
    vk_.endOneShotCommands(vk_.graphicsCommandPool(), vk_.graphicsPresentQueue(), initCmd);
}

void OverlayWindow::recreateSwapchain() {
    vkDeviceWaitIdle(vk_.device());
    destroySwapchain();
    createSurfaceAndSwapchain();
}

void OverlayWindow::destroySwapchain() {
    for (VkSemaphore sem : imageAvailable_) {
        if (sem) vkDestroySemaphore(vk_.device(), sem, nullptr);
    }
    imageAvailable_.clear();
    for (VkSemaphore sem : renderFinished_) {
        if (sem) vkDestroySemaphore(vk_.device(), sem, nullptr);
    }
    renderFinished_.clear();
    if (swapchain_) vkDestroySwapchainKHR(vk_.device(), swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    swapchainImages_.clear();
}

void OverlayWindow::updateTargetRect(const TargetRect& rect) {
    uint32_t values[] = {static_cast<uint32_t>(rect.x), static_cast<uint32_t>(rect.y), rect.width, rect.height};
    xcb_configure_window(connection_, window_,
                          XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
                              XCB_CONFIG_WINDOW_HEIGHT,
                          values);
    xcb_flush(connection_);
}

bool OverlayWindow::presentImage(VkImage image, VkImageLayout srcLayout, uint32_t width, uint32_t height) {
    if (imageAvailable_.empty()) return false;

    VK_CHECK(vkWaitForFences(vk_.device(), 1, &inFlightFence_, VK_TRUE, UINT64_MAX));

    VkSemaphore acquireSem = imageAvailable_[syncIndex_];
    syncIndex_ = (syncIndex_ + 1) % static_cast<uint32_t>(imageAvailable_.size());

    uint32_t imageIndex = 0;
    VkResult acquireResult = vkAcquireNextImageKHR(vk_.device(), swapchain_, UINT64_MAX, acquireSem,
                                                    VK_NULL_HANDLE, &imageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        if (imageAvailable_.empty()) return false;
        acquireSem = imageAvailable_[syncIndex_];
        syncIndex_ = (syncIndex_ + 1) % static_cast<uint32_t>(imageAvailable_.size());
        acquireResult = vkAcquireNextImageKHR(vk_.device(), swapchain_, UINT64_MAX, acquireSem,
                                              VK_NULL_HANDLE, &imageIndex);
        if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) return false;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) VK_CHECK(acquireResult);

    int32_t dstW = static_cast<int32_t>(width);
    int32_t dstH = static_cast<int32_t>(height);
    const int32_t monW = static_cast<int32_t>(swapchainExtent_.width);
    const int32_t monH = static_cast<int32_t>(swapchainExtent_.height);

    if (dstW > monW || dstH > monH) {
        double scale = std::min(static_cast<double>(monW) / std::max(1, dstW),
                                static_cast<double>(monH) / std::max(1, dstH));
        dstW = std::max(1, static_cast<int32_t>( std::lround(dstW * scale) ));
        dstH = std::max(1, static_cast<int32_t>( std::lround(dstH * scale) ));
    }

    int32_t dstX0 = (monW - dstW) / 2;
    int32_t dstY0 = (monH - dstH) / 2;
    if (hasTargetPos_ && dstW == static_cast<int32_t>(width) && dstH == static_cast<int32_t>(height)) {
        dstX0 = std::clamp(targetDstX_, 0, std::max(0, monW - dstW));
        dstY0 = std::clamp(targetDstY_, 0, std::max(0, monH - dstH));
    }
    const int32_t dstX1 = dstX0 + dstW;
    const int32_t dstY1 = dstY0 + dstH;

    bool rectChanged = true;
    if (imageIndex < imageBlitRect_.size()) {
        const auto& prev = imageBlitRect_[imageIndex];
        rectChanged = (prev.x != dstX0 || prev.y != dstY0 ||
                       prev.width != static_cast<uint32_t>(dstW) || prev.height != static_cast<uint32_t>(dstH));
        imageBlitRect_[imageIndex] = TargetRect{dstX0, dstY0, static_cast<uint32_t>(dstW), static_cast<uint32_t>(dstH)};
    }

    VK_CHECK(vkResetFences(vk_.device(), 1, &inFlightFence_));
    VK_CHECK(vkResetCommandBuffer(blitCmd_, 0));

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(blitCmd_, &beginInfo));

    VkImage dstImage = swapchainImages_[imageIndex];
    const bool needsBlackClear = rectChanged && (dstW < monW || dstH < monH);

    VkImageMemoryBarrier2 toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toDst.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    toDst.dstStageMask = needsBlackClear ? VK_PIPELINE_STAGE_2_CLEAR_BIT : VK_PIPELINE_STAGE_2_BLIT_BIT;
    toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toDst.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = dstImage;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep1.imageMemoryBarrierCount = 1;
    dep1.pImageMemoryBarriers = &toDst;
    vkCmdPipelineBarrier2(blitCmd_, &dep1);

    if (needsBlackClear) {
        VkClearColorValue blackClear{};
        blackClear.float32[3] = 1.0f;
        VkImageSubresourceRange clearRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(blitCmd_, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &blackClear, 1, &clearRange);

        VkImageMemoryBarrier2 afterClear{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        afterClear.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        afterClear.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        afterClear.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        afterClear.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        afterClear.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        afterClear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        afterClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        afterClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        afterClear.image = dstImage;
        afterClear.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo depClear{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depClear.imageMemoryBarrierCount = 1;
        depClear.pImageMemoryBarriers = &afterClear;
        vkCmdPipelineBarrier2(blitCmd_, &depClear);
    }

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {dstX0, dstY0, 0};
    blit.dstOffsets[1] = {dstX1, dstY1, 1};

    VkFilter filter = (dstW == static_cast<int32_t>(width) && dstH == static_cast<int32_t>(height))
                          ? VK_FILTER_NEAREST
                          : VK_FILTER_LINEAR;
    vkCmdBlitImage(blitCmd_, image, srcLayout, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, filter);

    if (showFps_ && !hudCopies_.empty() && hudBuffer_ != VK_NULL_HANDLE &&
        swapchainExtent_.width >= kHudWidth + 24 && swapchainExtent_.height >= kHudHeight + 24) {
        VkImageMemoryBarrier2 hudBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        hudBarrier.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        hudBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        hudBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        hudBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        hudBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        hudBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        hudBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hudBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hudBarrier.image = dstImage;
        hudBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo depHud{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depHud.imageMemoryBarrierCount = 1;
        depHud.pImageMemoryBarriers = &hudBarrier;
        vkCmdPipelineBarrier2(blitCmd_, &depHud);

        vkCmdCopyBufferToImage(blitCmd_, hudBuffer_, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(hudCopies_.size()), hudCopies_.data());
    }

    VkImageMemoryBarrier2 toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toPresent.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
    toPresent.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toPresent.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.image = dstImage;
    toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep2.imageMemoryBarrierCount = 1;
    dep2.pImageMemoryBarriers = &toPresent;
    vkCmdPipelineBarrier2(blitCmd_, &dep2);

    VK_CHECK(vkEndCommandBuffer(blitCmd_));

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSemaphore signalSem = renderFinished_[imageIndex];
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquireSem;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &blitCmd_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &signalSem;
    VK_CHECK(vkQueueSubmit(vk_.graphicsPresentQueue(), 1, &submit, inFlightFence_));

    // v15: no CPU wait here. vkQueuePresentKHR already waits on `signalSem` on the GPU timeline, and the
    // next presentImage()/waitForIdle() waits on inFlightFence_ before blitCmd_ is reused. Blocking here
    // (v14) serialised CPU and GPU for every single generated frame and was one of the reasons high
    // multipliers could not keep up with the display.

    VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &signalSem;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &imageIndex;
    VkResult presentResult = vkQueuePresentKHR(vk_.graphicsPresentQueue(), &presentInfo);
    xcb_flush(connection_);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR) {
        VK_CHECK(vkWaitForFences(vk_.device(), 1, &inFlightFence_, VK_TRUE, UINT64_MAX));
        recreateSwapchain();
        return false;
    }
    if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR)
        VK_CHECK(presentResult);
    if (!mapped_) {
        show();
    }
    return true;
}

void OverlayWindow::waitForIdle() {
    if (inFlightFence_ != VK_NULL_HANDLE) {
        VK_CHECK(vkWaitForFences(vk_.device(), 1, &inFlightFence_, VK_TRUE, UINT64_MAX));
    }
}

}  // namespace lfg::present
