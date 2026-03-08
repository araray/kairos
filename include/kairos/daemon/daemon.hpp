/// include/kairos/daemon/daemon.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/daemon/daemon.hpp — Daemon lifecycle management                   ║
// ║                                                                           ║
// ║  Phase 3: Full daemon with all engines wired together.                    ║
// ║  Scheduler, Pipeline, RunnerPool, DBWriter, and their shared             ║
// ║  dependencies are created, started, and shut down here.                  ║
// ║                                                                           ║
// ║  Spec reference: §27.2–27.3, §27.7                                      ║
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
///   4. Initialize metrics
///   5. Load workflow/watch-group definitions
///   6. Start DB Writer thread
///   7. Start Runner Pool threads
///   8. Start Pipeline thread (consumes trigger bus)
///   9. Start Scheduler thread (produces trigger ticks)
///  10. Install signal handlers
///  11. Main loop (watchdog + uptime gauge)
///  12. Graceful shutdown (reverse order)
///
/// @param config  Loaded and validated configuration.
/// @return        Exit code (0 on clean shutdown).
int run_daemon(std::shared_ptr<const kairos::config::ConfigState> config);

}  // namespace kairos::daemon
