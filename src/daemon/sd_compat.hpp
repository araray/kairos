/// src/daemon/sd_compat.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  sd_compat.hpp — Systemd notification compatibility layer                ║
// ║                                                                         ║
// ║  When KAIROS_SYSTEMD is defined (libsystemd found on Linux), this       ║
// ║  includes the real systemd headers.  Otherwise, it provides stub        ║
// ║  functions that compile to no-ops, allowing the same daemon code to     ║
// ║  work on macOS, Windows, and Linux-without-systemd.                    ║
// ║                                                                         ║
// ║  Spec reference: §27.4                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#ifdef KAIROS_SYSTEMD
// ── Real systemd integration (Linux with libsystemd) ─────────────────────
#include <systemd/sd-daemon.h>

#else
// ── Stub implementation ──────────────────────────────────────────────────
// Provides the same function signatures as libsystemd so the daemon code
// compiles without #ifdef guards around every sd_notify() call.

#include <cstdint>

/// Stub: sd_notify is a no-op when systemd is not available.
///
/// On real systemd, this sends a state notification to the service manager.
/// The `unset_environment` parameter controls whether NOTIFY_SOCKET is
/// unset after sending — irrelevant for the stub.
///
/// @return 0 (stub always "succeeds" silently)
inline int sd_notify(int /*unset_environment*/,
                     const char* /*state*/) {
    return 0;
}

/// Stub: sd_watchdog_enabled always returns 0 (watchdog not enabled).
///
/// On real systemd, this checks if the service manager expects periodic
/// watchdog keepalive pings and returns the configured interval.
///
/// @return 0 (watchdog not enabled)
inline int sd_watchdog_enabled(int /*unset_environment*/,
                                uint64_t* /*usec*/) {
    return 0;
}

#endif // KAIROS_SYSTEMD
