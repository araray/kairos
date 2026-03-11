/// src/platform/signals_win32.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  signals_win32.cpp — Console control handler for Windows                  ║
// ║                                                                           ║
// ║  Windows has no direct equivalent to POSIX signals. We use                ║
// ║  SetConsoleCtrlHandler to intercept Ctrl+C, Ctrl+Break, and console       ║
// ║  close events. There is no SIGHUP equivalent; reload is triggered         ║
// ║  via the CommandReader (file-based IPC) or MCP interface instead.         ║
// ║                                                                           ║
// ║  Spec reference: §25.6                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef _WIN32

#include "kairos/platform/signals.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace kairos::platform {

// ── Global atomic flags ──────────────────────────────────────────────────

std::atomic<bool> g_shutdown_requested{false};
std::atomic<bool> g_reload_requested{false};

namespace {

/// User-provided callback. Set once during install_signal_handlers()
/// and never modified afterward (the console handler thread reads it).
SignalCallback g_user_callback;

/// Console control handler invoked by the OS on a dedicated thread.
/// Must return TRUE if the event was handled, FALSE to pass it on.
BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            // All map to kShutdown — Windows lacks a reload signal.
            g_shutdown_requested.store(true, std::memory_order_release);
            if (g_user_callback) {
                try {
                    g_user_callback(SignalType::kShutdown);
                } catch (...) {
                    // Callback must not throw, but be defensive.
                }
            }
            // Return TRUE to prevent the default handler (which would
            // terminate the process for CTRL_CLOSE_EVENT).
            return TRUE;

        default:
            return FALSE;
    }
}

}  // anonymous namespace

void install_signal_handlers(SignalCallback callback) {
    g_user_callback = std::move(callback);
    ::SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
}

}  // namespace kairos::platform

#endif  // _WIN32
