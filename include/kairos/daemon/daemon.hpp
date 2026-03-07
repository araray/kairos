// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/daemon/daemon.hpp — Daemon lifecycle management                   ║
// ║  Spec reference: §27.2–27.3                                               ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/config/config_store.hpp"

#include <memory>

namespace kairos::daemon {

/// Run the Kairos daemon.  This is the main entry point called by
/// `kairos start`.
///
/// Lifecycle:
///   1. Acquire instance lock
///   2. Open SQLite database + run migrations
///   3. Initialize logging
///   4. Install signal handlers
///   5. Main loop (wait for shutdown signal)
///   6. Graceful shutdown
///
/// @param config  Loaded and validated configuration.
/// @return        Exit code (0 on clean shutdown).
int run_daemon(std::shared_ptr<const kairos::config::ConfigState> config);

}  // namespace kairos::daemon
