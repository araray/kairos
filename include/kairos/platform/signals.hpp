/// include/kairos/platform/signals.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/platform/signals.hpp — Cross-platform signal/console handling     ║
// ║  Spec reference: §25.6                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <atomic>
#include <functional>

namespace kairos::platform {

/// Signal types that Kairos handles.
enum class SignalType {
    kShutdown,     ///< SIGTERM / SIGINT / Ctrl+C
    kReload,       ///< SIGHUP
};

/// Callback for signal delivery.
using SignalCallback = std::function<void(SignalType)>;

/// Install signal handlers.  On POSIX, uses the self-pipe trick to
/// convert signals into a file descriptor readable by the main loop.
/// On Windows, uses SetConsoleCtrlHandler.
///
/// The callback is invoked from a safe context (not the signal handler
/// itself on POSIX — the handler writes to the self-pipe, and the
/// callback is called when the pipe is read).
void install_signal_handlers(SignalCallback callback);

/// Global shutdown flag.  Set to true when a shutdown signal is received.
/// Safe to read from any thread.
extern std::atomic<bool> g_shutdown_requested;

/// Global reload flag.  Set to true when a reload signal is received.
extern std::atomic<bool> g_reload_requested;

}  // namespace kairos::platform
