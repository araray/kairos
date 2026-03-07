/// src/daemon/daemon.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  daemon.cpp — Phase 1 daemon skeleton                                     ║
// ║                                                                           ║
// ║  Phase 1 only: initialize subsystems, log "started", wait for signal,     ║
// ║  shut down cleanly.  Engines (scheduler, pipeline, watch) are Phase 3.    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/daemon/daemon.hpp"
#include "kairos/core/version.hpp"
#include "kairos/observability/logging.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/platform/platform.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace kairos::daemon {

int run_daemon(std::shared_ptr<const kairos::config::ConfigState> config) {
    auto log = spdlog::default_logger();

    // ── Step 4: Acquire instance lock ─────────────────────────────────
    auto lock_path = config->data_dir / "kairos.lock";
    auto instance_lock = platform::InstanceLock::try_acquire(lock_path);
    if (!instance_lock) {
        log->error("Another Kairos instance is already running (lock: {})",
                   lock_path.string());
        return 1;
    }
    log->info("Instance lock acquired: {}", lock_path.string());

    // ── Step 5: Open SQLite database + migrations ─────────────────────
    std::unique_ptr<SQLite::Database> db;
    try {
        db = persist::open_database(config->db_path);
        int schema_ver = persist::get_schema_version(*db);
        log->info("Database opened: {} (schema v{})",
                  config->db_path.string(), schema_ver);
    } catch (const std::exception& e) {
        log->error("Failed to open database: {}", e.what());
        return 1;
    }

    // ── Step 8: Log startup banner ────────────────────────────────────
    log->info("Kairos v{} started", std::string(kairos::kVersion));
    log->info("Config: {}", config->config_file_path.string());
    log->info("Data dir: {}", config->data_dir.string());

    // ── Step 14–15: Install signal handlers ───────────────────────────
    std::mutex shutdown_mtx;
    std::condition_variable shutdown_cv;

    platform::install_signal_handlers(
        [&](platform::SignalType sig) {
            if (sig == platform::SignalType::kShutdown) {
                log->info("Shutdown signal received");
                std::lock_guard lk(shutdown_mtx);
                shutdown_cv.notify_all();
            } else if (sig == platform::SignalType::kReload) {
                log->info("Reload signal received (not yet implemented)");
            }
        });

    // ── Step 16: Main loop — wait for shutdown ────────────────────────
    // Phase 1: just wait.  Phase 3 will start engine threads here.
    {
        std::unique_lock lk(shutdown_mtx);
        shutdown_cv.wait(lk, [] {
            return platform::g_shutdown_requested.load();
        });
    }

    // ── Graceful shutdown ─────────────────────────────────────────────
    log->info("Kairos shutting down...");

    // Phase 3+ will shut down engines here.
    // For now, just close the database and release the lock.

    db.reset();
    instance_lock.reset();

    log->info("Kairos stopped");
    observability::shutdown_logging();

    return 0;
}

}  // namespace kairos::daemon
