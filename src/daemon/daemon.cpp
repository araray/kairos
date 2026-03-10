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
#include "kairos/daemon/command_reader.hpp"
#include "kairos/config/yaml_loader.hpp"
#include "kairos/core/id_generator.hpp"
#include "kairos/core/version.hpp"
#include "kairos/engine/cancel_registry.hpp"
#include "kairos/engine/pipeline.hpp"
#include "kairos/engine/scheduler.hpp"
#include "kairos/engine/trigger_event.hpp"
#include "kairos/engine/workflow_registry.hpp"
#include "kairos/exec/output_sink.hpp"
#include "kairos/exec/runner_pool.hpp"
#include "kairos/mcp/handler.hpp"
#include "kairos/mcp/transport.hpp"
#include "kairos/observability/logging.hpp"
#include "kairos/observability/metrics.hpp"
#include "kairos/observability/tracer.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/persist/query_reader.hpp"
#include "kairos/platform/platform.hpp"
#include "kairos/security/secret_store.hpp"
#include "kairos/testing/fake_clock.hpp"
#include "kairos/watch/real_scanner.hpp"
#include "kairos/watch/watch_engine.hpp"
#include "kairos/watch/file_watcher.hpp"

#ifdef KAIROS_HTTP_ENABLED
#include "kairos/http/http_server.hpp"
#endif

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <stop_token>
#include <thread>

namespace kairos::daemon {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

// ── YAML loading helper ─────────────────────────────────────────────────

/// Resolve a YAML directory path relative to the config file directory.
/// If the path is absolute, use it as-is. Otherwise, resolve relative to
/// the config file's parent directory.
static fs::path resolve_yaml_dir(
    const fs::path& config_file,
    const std::string& dir_value)
{
    fs::path p(dir_value);
    if (p.is_absolute()) return p;

    // Resolve relative to config file's parent directory.
    auto config_dir = config_file.parent_path();
    if (config_dir.empty()) config_dir = ".";
    return config_dir / p;
}

/// Load workflows and watch groups from YAML directories using the
/// YAML loader. Returns a WorkflowRegistry built from the loaded defs.
///
/// On YAML errors, logs warnings but continues with whatever loaded
/// successfully (fail-open on individual files, not the daemon).
static std::shared_ptr<engine::WorkflowRegistry> load_registry_from_yaml(
    const std::shared_ptr<const config::ConfigState>& cfg,
    std::shared_ptr<spdlog::logger> log)
{
    auto workflows_dir_str =
        cfg->global.get<std::string>("kairos.workflows_dir", "workflows");
    auto watch_groups_dir_str =
        cfg->global.get<std::string>("kairos.watch_groups_dir", "watch_groups");

    auto workflows_dir =
        resolve_yaml_dir(cfg->config_file_path, workflows_dir_str);
    auto watch_groups_path =
        resolve_yaml_dir(cfg->config_file_path, watch_groups_dir_str);

    log->info("Loading YAML: workflows_dir={}, watch_groups_path={}",
              workflows_dir.string(), watch_groups_path.string());

    // Attempt to load. Missing directories are not fatal — the daemon
    // can run with an empty registry and load files later via reload.
    config::YamlLoadResult yaml_result;

    std::error_code ec;
    bool wf_exists = fs::exists(workflows_dir, ec) && !ec;
    bool wg_exists = fs::exists(watch_groups_path, ec) && !ec;

    if (wf_exists || wg_exists) {
        if (wf_exists && wg_exists) {
            yaml_result = config::load_all(workflows_dir, watch_groups_path);
        } else if (wf_exists) {
            yaml_result = config::load_workflows_dir(workflows_dir);
        } else {
            // watch_groups_path may be a file or directory.
            if (fs::is_directory(watch_groups_path, ec)) {
                yaml_result = config::load_watch_groups_dir(watch_groups_path);
            } else {
                yaml_result = config::load_watch_groups_file(watch_groups_path);
            }
        }
    } else {
        log->info("No workflow/watch-group directories found — "
                  "starting with empty registry");
    }

    // Log any YAML errors (non-fatal).
    for (const auto& err : yaml_result.errors) {
        log->warn("YAML error in {}: {} — {}", err.file, err.path, err.message);
    }

    auto registry = std::make_shared<engine::WorkflowRegistry>(
        std::move(yaml_result.workflows),
        std::move(yaml_result.triggers),
        std::move(yaml_result.standalone_jobs),
        std::move(yaml_result.watch_groups));

    log->info("Registry loaded: {} workflows, {} triggers, {} watch groups",
              registry->workflow_count(),
              registry->trigger_count(),
              registry->watch_group_count());

    return registry;
}

// ── Config reload helper ────────────────────────────────────────────────

/// Perform a full config reload: re-parse TOML, re-load YAML, rebuild
/// registry, and propagate to all engines.
///
/// Returns true on success, false if the reload failed (old config kept).
static bool perform_config_reload(
    const std::filesystem::path& config_path,
    engine::Scheduler& scheduler,
    engine::Pipeline& pipeline,
    watch::WatchEngine& watch_engine,
    std::shared_ptr<spdlog::logger> log,
    mcp::McpHandler* mcp_handler = nullptr)
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

    // Reload YAML workflows and watch groups.
    auto new_registry = load_registry_from_yaml(result.state, log);

    // Propagate to engines.
    scheduler.request_reload(new_registry);
    pipeline.request_reload(new_registry);
    watch_engine.request_reload(new_registry->watch_groups());

    // Propagate to MCP handler if available (Phase 4 Batch 3).
    if (mcp_handler) {
        mcp_handler->update_registry(new_registry);
    }

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

    // Register daemon health metrics (§20.3.6).
    auto* gauge_uptime = metrics_registry.register_gauge(
        "kairos_uptime_seconds", "Seconds since daemon start");
    auto* gauge_active_runs = metrics_registry.register_gauge(
        "kairos_runs_active", "Currently executing runs");
    auto* gauge_pipeline_depth = metrics_registry.register_gauge(
        "kairos_pipeline_queue_depth",
        "Trigger events waiting in the pipeline queue");
    auto* counter_reloads_ok = metrics_registry.register_counter(
        "kairos_config_reloads_total",
        "Configuration reload attempts",
        {{"result", "success"}});
    auto* counter_reloads_fail = metrics_registry.register_counter(
        "kairos_config_reloads_total",
        "Configuration reload attempts",
        {{"result", "failure"}});
    auto* gauge_watch_groups = metrics_registry.register_gauge(
        "kairos_watch_groups_active",
        "Number of watch groups currently monitored");

    log->debug("Metrics registry initialized");

    // ── Step 5: Create clock source ────────────────────────────────
    SystemClockSource clock;

    // ── Step 6: Load workflow/watch-group definitions ──────────────
    auto registry = load_registry_from_yaml(config, log);

    // ── Step 6.5: Load SecretStore (§17.1) ────────────────────────
    security::SecretStore secret_store;
    bool vault_enabled = config->global.get<bool>(
        "kairos.vault.enabled", false);
    if (vault_enabled) {
        std::string vault_file = config->global.get<std::string>(
            "kairos.vault.file", "");
        std::string password_env = config->global.get<std::string>(
            "kairos.vault.password_env", "KAIROS_VAULT_PASSWORD");
        std::string password_file = config->global.get<std::string>(
            "kairos.vault.password_file", "");

        // Resolve password: password_file takes precedence over env var.
        std::string vault_password;
        if (!password_file.empty()) {
            std::ifstream pw_stream(password_file);
            if (!pw_stream.is_open()) {
                log->error("Cannot open vault password file: {}",
                           password_file);
                return 1;
            }
            std::getline(pw_stream, vault_password);
            // Trim trailing whitespace/newline from password.
            while (!vault_password.empty() &&
                   (vault_password.back() == '\n' ||
                    vault_password.back() == '\r' ||
                    vault_password.back() == ' ')) {
                vault_password.pop_back();
            }
        } else {
            const char* pw = std::getenv(password_env.c_str());
            if (!pw || pw[0] == '\0') {
                log->error("Vault enabled but {} env var is empty/unset",
                           password_env);
                return 1;
            }
            vault_password = pw;
        }

        try {
            secret_store.load_vault(vault_file, vault_password);
            // Secure-clear password from our local variable.
            volatile char* p = vault_password.data();
            for (std::size_t i = 0; i < vault_password.size(); ++i) {
                p[i] = '\0';
            }
            vault_password.clear();
            log->info("Vault loaded: {} secrets", secret_store.size());
        } catch (const std::exception& e) {
            log->error("Failed to load vault: {}", e.what());
            return 1;
        }
    }

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

    // ── Step 12.5: Create RunStream (per-run pub-sub, §14.7) ──────
    exec::RunStream run_stream;
    log->debug("RunStream pub-sub created");

    // ── Step 12.6: Create CancelRegistry (per-run cancel, §23.10) ─
    engine::CancelRegistry cancel_registry;
    log->debug("CancelRegistry created");

    // ── Step 13: Start Pipeline thread ─────────────────────────────
    // Prepare secret resolver and masking values (§17.1, §17.3).
    auto secret_resolver = secret_store.is_loaded()
        ? secret_store.make_resolver()
        : exec::EnvBuilder::SecretResolver{};

    std::vector<std::string> secret_values;
    if (secret_store.is_loaded()) {
        secret_values = secret_store.values();
        // Sort longest-first to prevent partial matches (§17.3).
        std::sort(secret_values.begin(), secret_values.end(),
            [](const std::string& a, const std::string& b) {
                return a.size() > b.size();
            });
    }

    engine::PipelineConfig pipeline_cfg;
    engine::Pipeline pipeline(pipeline_cfg, engine::Pipeline::Dependencies{
        .clock = &clock,
        .trigger_bus = &trigger_bus,
        .runner_pool = &runner_pool,
        .registry = registry,
        .active_runs = &active_runs,
        .db_writer = &db_writer,
        .query_reader = &query_reader,
        .run_stream = &run_stream,
        .cancel_registry = &cancel_registry,
        .secret_resolver = secret_resolver,
        .secret_values = secret_values,
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

    // ── Step 15: Create tracer ───────────────────────────────────
    // NullTracer by default; OTelTracer when KAIROS_OTEL=ON and
    // kairos.otel.enabled=true in config.
    // Must be created before WatchEngine which uses it.
    bool otel_enabled = config->global.get<bool>(
        "kairos.otel.enabled", false);
    std::string otel_endpoint = config->global.get<std::string>(
        "kairos.otel.endpoint", "localhost:4317");
    auto tracer = observability::create_tracer(
        otel_enabled, otel_endpoint, "kairos");
    log->debug("Tracer initialized (otel={})", otel_enabled);

    // ── Step 15.2: Start Watch Engine thread ──────────────────────
    watch::WatchEngineConfig watch_cfg;
    watch::RealFilesystemScanner real_scanner;  // Production scanner.

    // Create native watcher backend (inotify/FSEvents/RDCW).
    // Returns nullptr on unsupported platforms — WatchEngine handles
    // the null case by using sample-only mode (§12.2, §27.2).
    auto native_watcher = watch::create_native_watcher();
    if (native_watcher) {
        log->info("Native watcher backend: {}",
                  native_watcher->platform_name());
    } else {
        log->info("No native watcher backend — using sample-only mode");
    }

    watch::WatchEngine watch_engine(
        watch_cfg,
        watch::WatchEngine::Dependencies{
            .clock = &clock,
            .scanner = &real_scanner,
            .db_writer = &db_writer,
            .native_watcher = native_watcher.get(),
            .tracer = tracer.get(),
        },
        registry->watch_groups());

    engine::TriggerSink watch_sink = [&trigger_bus](engine::TriggerEvent evt) {
        return trigger_bus.push(std::move(evt), std::chrono::milliseconds(5000));
    };
    watch_engine.start(stop_token, watch_sink);
    log->info("Watch engine started ({} groups)", watch_engine.group_count());

    // ── Step 15.7: Start MCP server thread (if enabled) ───────────
    // The MCP server runs on a dedicated thread (Thread N+3 per §3.3).
    // It reads JSON-RPC from stdin and writes to stdout.
    // Dependencies are wired to live engine state.
    bool mcp_enabled = config->global.get<bool>(
        "kairos.mcp.enabled", false);

    // MCP handler and transport (kept alive for the daemon's lifetime).
    std::unique_ptr<mcp::McpHandler> mcp_handler;
    std::unique_ptr<mcp::StdioTransport> mcp_transport;
    std::jthread mcp_thread;

    if (mcp_enabled) {
        // Wire all live dependencies into the MCP handler.
        mcp::McpHandler::Dependencies mcp_deps;
        mcp_deps.watch_engine = &watch_engine;
        mcp_deps.metrics = &metrics_registry;
        mcp_deps.run_stream = &run_stream;
        mcp_deps.server_info.name = "kairos";
        mcp_deps.server_info.version = std::string(kairos::kVersion);

        // Phase 4 Batch 3: wire workflow/run dependencies.
        mcp_deps.registry = registry;
        mcp_deps.query_reader = &query_reader;
        mcp_deps.submit_run =
            [&trigger_bus](const std::string& target_id,
                           engine::TriggerEvent::TargetKind kind)
            -> std::string {
                auto run_id = core::generate_run_id();
                engine::TriggerEvent evt;
                evt.type = engine::TriggerType::ManualRun;
                evt.target_id = target_id;
                evt.target_kind = kind;
                evt.trigger_id = "mcp";
                evt.correlation_id = run_id;
                evt.fire_time = std::chrono::system_clock::now();
                evt.mono_time = std::chrono::steady_clock::now();
                evt.payload = engine::ManualRunPayload{
                    .invoked_by = "mcp"};
                bool ok = trigger_bus.push(
                    std::move(evt),
                    std::chrono::milliseconds(5000));
                return ok ? run_id : std::string{};
            };

        // Config reload callback: delegates to the daemon's reload logic.
        mcp_deps.reload_config =
            [&config, &scheduler, &pipeline, &watch_engine, &log]
            (std::vector<std::string>& errors) -> bool {
                bool ok = perform_config_reload(
                    config->config_file_path,
                    scheduler, pipeline, watch_engine, log);
                if (!ok) {
                    errors.push_back("Config reload failed — see logs");
                }
                return ok;
            };

        mcp_handler = std::make_unique<mcp::McpHandler>(std::move(mcp_deps));

        mcp_transport = std::make_unique<mcp::StdioTransport>(
            [&mcp_handler](const std::string& method,
                           const nlohmann::json& params,
                           const nlohmann::json& id) -> nlohmann::json {
                return mcp_handler->dispatch(method, params, id);
            });

        // Wire the transport back into the handler (resolves circular dep).
        mcp_handler->set_transport(mcp_transport.get());

        // Start MCP on a dedicated thread.
        mcp_thread = std::jthread([&mcp_transport, &log](std::stop_token st) {
            platform::set_thread_name("kairos-mcp");
            log->info("MCP server thread started (stdio transport)");
            mcp_transport->run();
            log->info("MCP server thread stopped");
        });

        log->info("MCP server enabled (stdio transport)");
    } else {
        log->debug("MCP server disabled (kairos.mcp.enabled=false)");
    }

    // ── Step 15.8: Create Command Reader (§23.10) ─────────────────
    auto commands_dir = config->data_dir / "commands";
    daemon::CommandReader command_reader(commands_dir);
    log->debug("Command reader initialized: {}", commands_dir.string());

    // ── Step 15.9: Start HTTP server (optional, §26) ─────────────
#ifdef KAIROS_HTTP_ENABLED
    std::unique_ptr<http::HttpServer> http_server;
    bool http_enabled = config->global.get<bool>(
        "kairos.http.enabled", false);

    if (http_enabled) {
        http::HttpConfig http_cfg;
        http_cfg.listen_addr = config->global.get<std::string>(
            "kairos.http.host", "127.0.0.1");
        http_cfg.listen_port = static_cast<uint16_t>(
            config->global.get<int>("kairos.http.port", 8420));
        http_cfg.api_token = config->global.get<std::string>(
            "kairos.http.auth_token", "");
        http_cfg.enable_cors = config->global.get<bool>(
            "kairos.http.cors_enabled", false);

        auto uptime_start_http = std::chrono::steady_clock::now();

        http::HttpDependencies http_deps;
        http_deps.metrics = &metrics_registry;
        http_deps.reader = &query_reader;
        http_deps.registry = registry;
        http_deps.run_stream = &run_stream;
        http_deps.get_uptime = [uptime_start_http]() -> double {
            return std::chrono::duration<double>(
                std::chrono::steady_clock::now() - uptime_start_http).count();
        };
        http_deps.submit_run = [&trigger_bus](
            const std::string& name) -> std::string {
            auto run_id = core::generate_run_id();
            engine::TriggerEvent evt;
            evt.type = engine::TriggerType::ManualRun;
            evt.target_id = name;
            evt.trigger_id = "http-api";
            evt.correlation_id = run_id;
            bool ok = trigger_bus.push(std::move(evt),
                std::chrono::milliseconds(5000));
            return ok ? run_id : std::string{};
        };
        http_deps.reload_config = [&config, &scheduler, &pipeline,
                                    &watch_engine, &log]() -> bool {
            return perform_config_reload(
                config->config_file_path,
                scheduler, pipeline, watch_engine, log);
        };

        http_server = std::make_unique<http::HttpServer>(
            std::move(http_cfg), std::move(http_deps));
        http_server->start(stop_token);
        log->info("HTTP server started: {}",
                  http_server->listen_address());
    } else {
        log->debug("HTTP server disabled (kairos.http.enabled=false)");
    }
#endif

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

    // Metrics snapshot interval (§20.5).
    int snapshot_interval_s = config->global.get<int>(
        "kairos.telemetry.metrics_snapshot_interval_seconds", 60);
    auto last_snapshot = std::chrono::steady_clock::now();

    // Metrics snapshot pruning interval — prune old snapshots
    // periodically. Default: 7 days retention (§16.8).
    int metrics_retention_days = config->global.get<int>(
        "kairos.telemetry.metrics_retention_days", 7);
    auto last_metrics_prune = std::chrono::steady_clock::now();

    // ── Retention auto-prune config (§16.8) ──────────────────────
    int retention_days = config->global.get<int>(
        "kairos.persistence.retention_days", 90);
    int prune_interval_hours = config->global.get<int>(
        "kairos.persistence.prune_interval_hours", 24);
    int max_samples_per_group = config->global.get<int>(
        "kairos.persistence.max_samples_per_group", 1000);
    auto last_retention_prune = std::chrono::steady_clock::now();
    double prune_interval_s = static_cast<double>(prune_interval_hours) * 3600.0;
    log->info("Retention auto-prune: every {}h, keep {}d of runs, "
              "{}  samples/group",
              prune_interval_hours, retention_days, max_samples_per_group);

    // ── Step 18: Main loop ─────────────────────────────────────────
    while (!stop_token.stop_requested()) {
        auto now = std::chrono::steady_clock::now();
        double uptime_s = std::chrono::duration<double>(
            now - uptime_start).count();

        // Update daemon health gauges (§20.3.6).
        gauge_uptime->set(uptime_s);
        gauge_active_runs->set(
            static_cast<double>(active_runs.total_active()));
        gauge_pipeline_depth->set(
            static_cast<double>(trigger_bus.size()));
        gauge_watch_groups->set(
            static_cast<double>(watch_engine.group_count()));

        // Periodic metrics snapshot to SQLite (§20.5).
        if (snapshot_interval_s > 0) {
            auto elapsed = std::chrono::duration<double>(
                now - last_snapshot).count();
            if (elapsed >= static_cast<double>(snapshot_interval_s)) {
                auto entries = metrics_registry.snapshot_entries();
                persist::BatchInsertMetricsSnapshots batch;
                batch.entries.reserve(entries.size());
                for (auto& e : entries) {
                    batch.entries.push_back({
                        .metric_name = std::move(e.metric_name),
                        .metric_type = std::move(e.metric_type),
                        .value = e.value,
                        .labels_json = std::move(e.labels_json),
                    });
                }
                db_writer.enqueue(std::move(batch));
                last_snapshot = now;
                log->trace("Metrics snapshot persisted ({} entries)",
                           entries.size());
            }
        }

        // Periodic metrics snapshot pruning (§16.8).
        // Run once per hour — prune snapshots older than retention_days.
        {
            auto prune_elapsed = std::chrono::duration<double>(
                now - last_metrics_prune).count();
            if (prune_elapsed >= 3600.0 && metrics_retention_days > 0) {
                db_writer.enqueue(persist::PruneMetricsSnapshots{
                    .retention_days = metrics_retention_days,
                });
                last_metrics_prune = now;
                log->debug("Enqueued metrics snapshot prune "
                           "(retention: {} days)", metrics_retention_days);
            }
        }

        // Retention auto-prune: runs, watch samples, watch events (§16.8).
        // Triggered every prune_interval_hours.
        {
            auto prune_elapsed = std::chrono::duration<double>(
                now - last_retention_prune).count();
            if (prune_elapsed >= prune_interval_s && retention_days > 0) {
                // 1. Prune old runs (CASCADE deletes run_jobs, run_steps,
                //    log_chunks via ON DELETE CASCADE).
                // Compute actual ISO 8601 cutoff timestamp — SQLite
                // parameters are literal values, not evaluated SQL.
                auto wall_now = std::chrono::system_clock::now();
                auto cutoff_tp = wall_now -
                    std::chrono::hours(retention_days * 24);
                auto cutoff_tt = std::chrono::system_clock::to_time_t(
                    cutoff_tp);
                std::tm cutoff_tm{};
#ifdef _WIN32
                gmtime_s(&cutoff_tm, &cutoff_tt);
#else
                gmtime_r(&cutoff_tt, &cutoff_tm);
#endif
                char cutoff_buf[32];
                std::strftime(cutoff_buf, sizeof(cutoff_buf),
                              "%Y-%m-%dT%H:%M:%SZ", &cutoff_tm);
                std::string cutoff_date(cutoff_buf);

                db_writer.enqueue(persist::PruneOlderThan{
                    .cutoff_date = cutoff_date,
                });

                // 2. Prune old watch events.
                // Uses the same cutoff as runs.
                // (PruneOlderThan handles both runs and watch_events.)

                // 3. Prune old watch samples (keep max N per group).
                auto watch_groups = registry->watch_groups();
                for (const auto& wg : watch_groups) {
                    db_writer.enqueue(persist::PruneWatchSamples{
                        .watch_group = wg.group_name,
                        .max_epochs = max_samples_per_group,
                    });
                }

                last_retention_prune = now;
                log->info("Retention auto-prune enqueued: "
                          "runs older than {}d, "
                          "{} watch groups (max {} samples each)",
                          retention_days,
                          watch_groups.size(),
                          max_samples_per_group);
            }
        }

        // Check for reload.
        if (platform::g_reload_requested.exchange(false)) {
            bool ok = perform_config_reload(
                config->config_file_path,
                scheduler, pipeline, watch_engine, log,
                mcp_handler.get());
            if (ok) {
                counter_reloads_ok->increment();
            } else {
                counter_reloads_fail->increment();
            }
        }

        // Poll for CLI commands (cancel, reload, etc.) — §23.10.
        auto commands = command_reader.poll();
        for (const auto& cmd : commands) {
            switch (cmd.type) {
                case daemon::Command::Type::Cancel:
                    log->info("Command: cancel run '{}'", cmd.run_id);
                    // Request cancellation of the in-flight run via
                    // CancelRegistry. This fires the per-run stop_token,
                    // which propagates to ProcessHandle::wait() in the
                    // runner pool → soft-then-hard kill of child processes.
                    if (cancel_registry.cancel(cmd.run_id)) {
                        log->info("Run '{}' cancel signal sent — "
                                  "processes will be terminated",
                                  cmd.run_id);
                    } else {
                        log->info("Run '{}' not active (already "
                                  "completed or unknown)", cmd.run_id);
                    }
                    break;
                case daemon::Command::Type::Reload:
                    log->info("Command: config reload (via CLI)");
                    platform::g_reload_requested.store(true);
                    break;
                default:
                    log->warn("Unknown command type from file: {}",
                              cmd.source_file);
                    break;
            }
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

    // 2.3. Stop HTTP server (close listener).
#ifdef KAIROS_HTTP_ENABLED
    if (http_server) {
        log->debug("Stopping HTTP server");
        http_server->stop();
        http_server.reset();
    }
#endif

    // 2.5. Stop MCP server (close stdin reader).
    if (mcp_transport) {
        log->debug("Stopping MCP server");
        mcp_transport->stop();
        // The MCP thread blocks on stdin — stopping the transport
        // sets the stopped_ flag, but the thread may still be blocked
        // on std::getline. It will exit on next stdin input or EOF.
        // We join below after pipeline stops to give it time.
    }

    // 3. Close trigger bus → pipeline drains remaining events.
    log->debug("Stopping pipeline");
    trigger_bus.close();
    pipeline.stop();

    // 4. Stop runner pool (wait for in-flight).
    log->debug("Stopping runner pool");
    runner_pool.shutdown();

    // 4.5. Join MCP thread if running.
    if (mcp_thread.joinable()) {
        log->debug("Joining MCP server thread");
        mcp_thread.request_stop();
        // Give the MCP thread a brief timeout — if it's blocked on stdin,
        // it won't exit until EOF. We can proceed regardless.
        mcp_thread.join();
    }
    mcp_handler.reset();
    mcp_transport.reset();

    // 4.7. Flush tracer spans.
    if (tracer) {
        tracer->flush();
    }

    // 5. Stop and flush DB writer — joins the writer thread, drains
    //    remaining items, and releases all prepared statements so that
    //    ~Database (during stack unwinding) will not hit SQLITE_BUSY.
    log->debug("Stopping DB writer");
    db_writer.stop();

    // 6. Stop shutdown watchdog.
    shutdown_watchdog.request_stop();
    if (shutdown_watchdog.joinable()) {
        shutdown_watchdog.join();
    }

    // 7. Database and instance lock are released by RAII.
    //    Stack destruction order guarantees correctness:
    //      ~watch_engine → ~scheduler → ~pipeline → ~query_reader
    //      → ~db_writer (calls stop(), releases statements)
    //      → ... → ~db (sqlite3_close, now safe)
    //      → ~instance_lock (releases file lock)
    //
    //    DO NOT call db.reset() or instance_lock.reset() here!
    //    Doing so destroys the database while stack-allocated objects
    //    (Pipeline, Scheduler, etc.) still hold references to it.
    //    Their destructors would then access freed memory → segfault.

    log->info("Kairos stopped");

    // NOTE: Do NOT call observability::shutdown_logging() here!
    // Stack-allocated objects (InotifyWatcher, WatchEngine, etc.) have
    // destructors that call spdlog::debug(). If we shut down spdlog
    // before those destructors run, the default logger is null → segfault.
    // shutdown_logging() is called by the CLI layer after run_daemon()
    // returns and all locals have been destroyed.

    return 0;
}

}  // namespace kairos::daemon
