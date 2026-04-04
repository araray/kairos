/// src/platform/sd_notify_win32.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  sd_notify_win32.cpp — No-op stubs for Windows (no systemd)              ║
// ║  Spec reference: §27.4                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/platform/sd_notify.hpp"

namespace kairos::platform {

bool sd_notify_send(std::string_view /*state*/) {
    return true;  // No systemd on Windows.
}

int sd_watchdog_interval_s() {
    return 0;
}

}  // namespace kairos::platform
