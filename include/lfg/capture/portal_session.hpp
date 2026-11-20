#pragma once

#include <gio/gio.h>
#include <libportal/portal.h>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace lfg::capture {

enum class CaptureSource {
    Window,   // captures one window's own content, even when covered by our overlay
    Monitor,  // captures everything on that monitor, overlay included
};

struct CaptureTarget {
    int pipewireFd = -1;
    uint32_t nodeId = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t x = 0;
    int32_t y = 0;
};

class PortalSession {
public:
    PortalSession();
    ~PortalSession();

    PortalSession(const PortalSession&) = delete;
    PortalSession& operator=(const PortalSession&) = delete;

    // When autoSelectActiveWindow is true and source is Window, automatically selects
    // the top/active window currently in use without showing the Choose Window dialog.
    CaptureTarget requestCapture(CaptureSource source, bool autoSelectActiveWindow = true);

    void close();

private:
    bool tryAutoSelectActiveKdeWindow(CaptureTarget& outTarget);

    XdpPortal* portal_ = nullptr;
    XdpSession* session_ = nullptr;
    GDBusConnection* dbusConn_ = nullptr;
    std::string kdeSessionPath_;
};

}  // namespace lfg::capture
