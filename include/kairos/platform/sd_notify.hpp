/// include/kairos/platform/sd_notify.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/platform/sd_notify.hpp — systemd readiness & watchdog protocol   ║
// ║                                                                          ║
// ║  Implements the sd_notify protocol directly via AF_UNIX datagram socket  ║
// ║  to $NOTIFY_SOCKET — no libsystemd dependency required.                 ║
// ║                                                                          ║
// ║  On non-Linux platforms or when $NOTIFY_SOCKET is unset, all calls are  ║
// ║  silent no-ops.                                                          ║
// ║                                                                          ║
// ║  Protocol reference:                                                     ║
// ║    https://www.freedesktop.org/software/systemd/man/sd_notify.html      ║
// ║                                                                          ║
// ║  Spec reference: §27.4                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <string>
#include <string_view>

namespace kairos::platform {

/// Low-level: send an arbitrary notification string to systemd.
/// Returns true if sent (or if no socket → vacuously succeeds),
/// false only on socket error.
bool sd_notify_send(std::string_view state);

/// Convenience: tell systemd the daemon is ready.
///   Sends "READY=1"
inline bool sd_notify_ready() {
    return sd_notify_send("READY=1");
}

/// Convenience: pet the watchdog.
///   Sends "WATCHDOG=1"
inline bool sd_notify_watchdog() {
    return sd_notify_send("WATCHDOG=1");
}

/// Convenience: tell systemd we are stopping.
///   Sends "STOPPING=1"
inline bool sd_notify_stopping() {
    return sd_notify_send("STOPPING=1");
}

/// Convenience: update systemd status line.
///   Sends "STATUS=<text>"
inline bool sd_notify_status(std::string_view text) {
    std::string msg = "STATUS=";
    msg += text;
    return sd_notify_send(msg);
}

/// Returns the watchdog interval in seconds from $WATCHDOG_USEC,
/// or 0 if not set / not applicable.  Per the protocol, the daemon
/// should ping at half this interval.
int sd_watchdog_interval_s();

}  // namespace kairos::platform
