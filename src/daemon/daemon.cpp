/// src/daemon/daemon.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  daemon.cpp — Phase 3 daemon lifecycle (Batch 3 update)                   ║
// ║                                                                           ║
// ║  Creates all subsystems (DB, DBWriter, RunnerPool, TriggerBus,           ║
// ║  Scheduler, Pipeline, WatchEngine), starts them as jthreads with a       ║
// ║  shared stop_source, and shuts them down in reverse-dependency order.    ║
// ║                                                                           ║
// ║  New in Batch 3:                                                         ║
// ║    - WatchEngine wired into daemon lifecycle                              ║
// ║    - SIGHUP triggers full config reload: re-parse config, rebuild        ║
// ║      registry, propagate to Scheduler + Pipeline + WatchEngine           ║
// ║                                                                           ║
// ║  Spec reference: §27.2–§27.3, §27.7, §25.6                              ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/daemon/daemon.hpp"
#include "kairos/core/version.hpp"
#include "kairos/engine/pipeline.hpp"
#include "kairos/engine/scheduler.hpp"
#include "kairos/engine/trigger_event.hpp"
#include "kairos/engine/workflow_registry.hpp"
#include "kairos/exec/runner_pool.hpp"
#include "kairos/observability/logging.hpp"
#include "kairos/observability/metrics.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/persist/query_reader.hpp"
#include "kairos/platform/platform.hpp"
#include "kairos/testing/fake_clock.hpp"
#include "kairos/watch/watch_engine.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>

namespace kairos::daemon {

using namespace std::chrono_literals;

// ── Config reload helper ────────────────────────────────────────────────

/// Perform a full config reload: re-parse, validate, rebuild registry,
/// and propagate to all engines.
///
/// Returns true on success, false if the reload failed (old config kept).
static bool perform_config_reload(
    const std::filesystem::path& config_path,
    engine::Scheduler& scheduler,
    engine::Pipeline& pipeline,
    watch::WatchEngine& watch_engine,
    std::shared_ptr<spdlog::logger> log)
{
    log->info("Config reload: re-parsing {}", config_path.string());

    auto result = config::load_config(config_path);
    if (!result.ok()) {
        log->error("Config reload failed: {} validation error(s)",
                   result.errors.size());
        for (const auto& err : result.errors) {
            log->error("  {}: {}", err.key_path, err.message);
        }
        return false;
    }

    // Build new registry from reloaded config.
    // In Phase 4, this will parse YAML workflow files.
    // For now, rebuild with the same empty-ish structure.
    auto new_registry = std::make_shared<engine::WorkflowRegistry>(
        std::vector<engine::WorkflowDef>{},
        std::vector<engine::TimerEntry>{},
        std::vector<engine::JobDef>{},
        std::vector<watch::WatchGroupDef>{});

    log->info("Config reload: rebuilt registry ({} workflows, {} triggers, "
              "{} watch groups)",
              new_registry->workflow_count(),
              new_registry->trigger_count(),
              new_registry->watch_group_count());

    // Propagate to engines.
    scheduler.request_reload(new_registry);
    pipeline.request_reload(new_registry);
    watch_engine.request_reload(new_registry->watch_groups());

    log->info("Config reload complete");
    return true;
}

// ── Daemon entry point ──────────────────────────────────────────────────

int run_daemon(std::shared_ptr<const kairos::config::ConfigState> config) {
    auto log = spdlog::default_logger();

    // ── Step 1: Acquire instance lock ──────────────────────────────
    auto lock_path = config->data_dir / "kairos.lock";
    auto instance_lock = platform::InstanceLock::try_acquire(lock_path);
    if (!instance_lock) {
        log->error("Another Kairos instance is already running (lock: {})",
                   lock_path.string());
        return 1;
    }
    log->info("Instance lock acquired: {}", lock_path.string());

    // ── Step 2: Open SQLite database + migrations ──────────────────
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

    // ── Step 3: Log startup banner ─────────────────────────────────
    log->info("Kairos v{} starting", std::string(kairos::kVersion));
    log->info("Config: {}", config->config_file_path.string());
    log->info("Data dir: {}", config->data_dir.string());

    // ── Step 4: Initialize metrics ─────────────────────────────────
    metrics::MetricsRegistry metrics_registry;
    log->debug("Metrics registry initialized");

    // ── Step 5: Create clock source ────────────────────────────────
    SystemClockSource clock;

    // ── Step 6: Load workflow/watch-group definitions ──────────────
    auto registry = std::make_shared<engine::WorkflowRegistry>(
        std::vector<engine::WorkflowDef>{},
        std::vector<engine::TimerEntry>{},
        std::vector<engine::JobDef>{},
        std::vector<watch::WatchGroupDef>{});
    log->info("Workflow registry loaded: {} workflows, {} triggers, "
              "{} watch groups",
              registry->workflow_count(), registry->trigger_count(),
              registry->watch_group_count());

    // ── Step 7: Create shared stop source ──────────────────────────
    std::stop_source stop_source;
    auto stop_token = stop_source.get_token();

    // ── Step 8: Active run tracker ─────────────────────────────────
    engine::ActiveRunTracker active_runs;

    // ── Step 9: Start DB Writer ────────────────────────────────────
    persist::DBWriterConfig db_writer_cfg;
    persist::DBWriter db_writer(*db, db_writer_cfg);
    db_writer.start(stop_token);
    log->info("DB Writer started");

    // ── Step 10: Start Runner Pool ─────────────────────────────────
    int worker_count = 4;
    exec::RunnerPoolConfig pool_cfg{
        .worker_count = static_cast<size_t>(worker_count),
        .queue_capacity = 256,
    };
    exec::RunnerPool runner_pool(pool_cfg);

    runner_pool.set_process_handle_factory([]() {
        return exec::create_process_handle();
    });

    runner_pool.start(stop_token);
    log->info("Runner pool started with {} workers", worker_count);

    // ── Step 11: Create Trigger Bus ────────────────────────────────
    engine::TriggerBus trigger_bus(1024);
    log->debug("Trigger bus created (capacity: 1024)");

    // ── Step 12: Create Query Reader ───────────────────────────────
    persist::QueryReader query_reader(*db);

    // ── Step 13: Start Pipeline thread ─────────────────────────────
    engine::PipelineConfig pipeline_cfg;
    engine::Pipeline pipeline(pipeline_cfg, engine::Pipeline::Dependencies{
        .clock = &clock,
        .trigger_bus = &trigger_bus,
        .runner_pool = &runner_pool,
        .registry = registry,
        .active_runs = &active_runs,
        .db_writer = &db_writer,
        .query_reader = &query_reader,
    });
    pipeline.start(stop_token);
    log->info("Pipeline thread started");

    // ── Step 14: Start Scheduler thread ────────────────────────────
    engine::SchedulerConfig sched_cfg;
    engine::Scheduler scheduler(sched_cfg, engine::Scheduler::Dependencies{
        .clock = &clock,
        .registry = registry,
        .active_runs = &active_runs,
        .db_writer = &db_writer,
    });

    engine::TriggerSink sched_sink = [&trigger_bus](engine::TriggerEvent evt) {
        return trigger_bus.push(std::move(evt), std::chrono::milliseconds(5000));
    };
    scheduler.start(stop_token, sched_sink);
    log->info("Scheduler thread started");

    // ── Step 15: Start Watch Engine thread ─────────────────────────
    watch::WatchEngineConfig watch_cfg;
    watch::WatchEngine watch_engine(
        watch_cfg,
        watch::WatchEngine::Dependencies{
            .clock = &clock,
            .scanner = nullptr,  // Real scanner created in Phase 4.
            .db_writer = &db_writer,
        },
        registry->watch_groups());

    engine::TriggerSink watch_sink = [&trigger_bus](engine::TriggerEvent evt) {
        return trigger_bus.push(std::move(evt), std::chrono::milliseconds(5000));
    };
    watch_engine.start(stop_token, watch_sink);
    log->info("Watch engine started ({} groups)", watch_engine.group_count());

    // ── Step 16: Install signal handlers ───────────────────────────
    platform::install_signal_handlers(
        [&](platform::SignalType sig) {
            if (sig == platform::SignalType::kShutdown) {
                log->info("Shutdown signal received");
                stop_source.request_stop();
                clock.wake();
            } else if (sig == platform::SignalType::kReload) {
                log->info("Reload signal received");
                platform::g_reload_requested.store(true);
                clock.wake();  // Unblock main loop.
            }
        });

    // ── Step 17: Startup complete ──────────────────────────────────
    log->info("Kairos v{} started — daemon ready", std::string(kairos::kVersion));

    auto uptime_start = std::chrono::steady_clock::now();

    // ── Step 18: Main loop ─────────────────────────────────────────
    while (!stop_token.stop_requested()) {
        auto now = std::chrono::steady_clock::now();
        double uptime_s = std::chrono::duration<double>(
            now - uptime_start).count();
        (void)uptime_s;  // Metrics wiring deferred to Phase 4.

        // Check for reload.
        if (platform::g_reload_requested.exchange(false)) {
            perform_config_reload(
                config->config_file_path,
                scheduler, pipeline, watch_engine, log);
        }

        // Sleep for 5 seconds (or until stop).
        clock.sleep_for(std::chrono::milliseconds(5000));
    }

    // ── Graceful shutdown ──────────────────────────────────────────
    // Shutdown in reverse-dependency order per §27.7:
    //   Scheduler → WatchEngine → TriggerBus → Pipeline →
    //   RunnerPool → DBWriter → DB → Lock
    log->info("Kairos shutting down...");

    // Start shutdown watchdog.
    int shutdown_timeout_s = 90;
    std::jthread shutdown_watchdog([shutdown_timeout_s](std::stop_token st) {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(shutdown_timeout_s);
        while (!st.stop_requested()) {
            if (std::chrono::steady_clock::now() > deadline) {
                spdlog::critical(
                    "Shutdown timeout ({}s) exceeded — forcing exit",
                    shutdown_timeout_s);
                spdlog::shutdown();
                std::_Exit(1);
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

    // 1. Stop scheduler (no new ticks).
    log->debug("Stopping scheduler");
    scheduler.stop();

    // 2. Stop watch engine (no new watch events).
    log->debug("Stopping watch engine");
    watch_engine.stop();

    // 3. Close trigger bus → pipeline drains remaining events.
    log->debug("Stopping pipeline");
    trigger_bus.close();
    pipeline.stop();

    // 4. Stop runner pool (wait for in-flight).
    log->debug("Stopping runner pool");
    runner_pool.shutdown();

    // 5. Flush DB writer.
    log->debug("Flushing DB writer");
    db_writer.flush();

    // 6. Stop shutdown watchdog.
    shutdown_watchdog.request_stop();
    if (shutdown_watchdog.joinable()) {
        shutdown_watchdog.join();
    }

    // 7. Close database and release lock.
    db.reset();
    instance_lock.reset();

    log->info("Kairos stopped");
    observability::shutdown_logging();

    return 0;
}

}  // namespace kairos::daemon
