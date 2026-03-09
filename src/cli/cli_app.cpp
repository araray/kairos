/// src/cli/cli_app.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  cli_app.cpp — CLI application with CLI11                                 ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/cli/cli_app.hpp"
#include "kairos/config/config_store.hpp"
#include "kairos/config/yaml_loader.hpp"
#include "kairos/core/exit_codes.hpp"
#include "kairos/core/version.hpp"
#include "kairos/daemon/daemon.hpp"
#include "kairos/engine/workflow_registry.hpp"
#include "kairos/mcp/handler.hpp"
#include "kairos/mcp/transport.hpp"
#include "kairos/observability/logging.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/persist/query_reader.hpp"
#include "kairos/platform/platform.hpp"
#include "kairos/testing/fake_clock.hpp"
#include "kairos/watch/real_scanner.hpp"
#include "kairos/watch/watch_engine.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/types.h>
#include <signal.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace kairos::cli {

namespace {

/// Resolve a YAML directory path relative to the config file directory.
static fs::path resolve_yaml_dir(
    const fs::path& config_file,
    const std::string& dir_value)
{
    fs::path p(dir_value);
    if (p.is_absolute()) return p;
    auto config_dir = config_file.parent_path();
    if (config_dir.empty()) config_dir = ".";
    return config_dir / p;
}

/// Load workflow/watch-group definitions from YAML directories.
/// Shared by CLI commands that need standalone access to definitions.
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
            if (fs::is_directory(watch_groups_path, ec)) {
                yaml_result = config::load_watch_groups_dir(watch_groups_path);
            } else {
                yaml_result = config::load_watch_groups_file(watch_groups_path);
            }
        }
    }

    for (const auto& err : yaml_result.errors) {
        if (log) {
            log->warn("YAML error in {}: {} — {}",
                      err.file, err.path, err.message);
        }
    }

    return std::make_shared<engine::WorkflowRegistry>(
        std::move(yaml_result.workflows),
        std::move(yaml_result.triggers),
        std::move(yaml_result.standalone_jobs),
        std::move(yaml_result.watch_groups));
}

/// Initialize logging based on config or CLI flags.
void setup_logging(const std::string& log_level, bool json_output, bool is_daemon) {
    observability::LogConfig log_cfg;
    log_cfg.level = observability::parse_log_level(log_level);

    // Use JSON if daemon mode or explicitly requested.
    // Use text if interactive terminal (unless --json is set).
    if (json_output || is_daemon) {
        log_cfg.json_format = true;
    } else {
        log_cfg.json_format = !platform::is_tty();
    }

    observability::initialize_logging(log_cfg);
}

/// Load config with error reporting.  Returns nullptr on failure.
std::shared_ptr<const config::ConfigState> load_config_or_die(
    const std::string& config_path_str,
    const std::unordered_map<std::string, confy::Value>& overrides)
{
    auto config_path = config::resolve_config_path(config_path_str);

    // For init-db and start, we need data_dir and db_path.
    // If no config file exists, apply defaults + env.
    auto result = config::load_config(config_path, overrides);
    if (!result.ok()) {
        for (const auto& err : result.errors) {
            std::cerr << "Config error [" << err.key_path << "]: "
                      << err.message << "\n";
        }
        return nullptr;
    }
    return result.state;
}

}  // anonymous namespace

int run(int argc, char** argv) {
    // ── Root CLI app ──────────────────────────────────────────────────
    CLI::App app{"Kairos — Unified Orchestration Daemon"};
    app.require_subcommand(1);
    app.fallthrough();  // Allow global options after subcommand name

    // ── Global options ────────────────────────────────────────────────
    std::string config_path;
    std::string log_level = "info";
    bool json_output = false;

    app.add_option("-c,--config", config_path,
                   "Path to kairos.toml config file");
    app.add_option("--log-level", log_level,
                   "Log level (trace|debug|info|warn|error|critical)")
        ->default_val("info");
    app.add_flag("--json", json_output,
                 "Force JSON output (for scripting)");

    // ── version ───────────────────────────────────────────────────────
    auto* cmd_version = app.add_subcommand("version", "Print version information");

    // ── start ─────────────────────────────────────────────────────────
    auto* cmd_start = app.add_subcommand("start", "Start the Kairos daemon");
    bool foreground = false;
    cmd_start->add_flag("-f,--foreground", foreground,
                        "Run in foreground (default behavior)");

    // ── init-db ───────────────────────────────────────────────────────
    auto* cmd_initdb = app.add_subcommand("init-db",
        "Initialize the SQLite database (create tables, run migrations)");
    std::string db_path_override;
    cmd_initdb->add_option("--db-path", db_path_override,
                           "Override database path");

    // ── watches ─────────────────────────────────────────────────────
    auto* cmd_watches = app.add_subcommand("watches",
        "Watch group management");
    cmd_watches->require_subcommand(1);

    auto* watches_list = cmd_watches->add_subcommand("list",
        "List watch groups and their status");

    auto* watches_show = cmd_watches->add_subcommand("show",
        "Show watch group detail");
    std::string watch_show_name;
    watches_show->add_option("name", watch_show_name,
        "Watch group name")->required();

    auto* watches_scan = cmd_watches->add_subcommand("scan-once",
        "Run a single scan cycle");
    std::string watch_scan_group;
    watches_scan->add_option("group", watch_scan_group,
        "Watch group to scan (all if omitted)");

    // ── events ───────────────────────────────────────────────────
    auto* cmd_events = app.add_subcommand("events",
        "Watch events");
    cmd_events->require_subcommand(1);

    auto* events_list = cmd_events->add_subcommand("list",
        "List recent events");
    std::string events_group;
    int events_limit = 50;
    events_list->add_option("--watch-group", events_group,
        "Filter by watch group");
    events_list->add_option("-n,--limit", events_limit,
        "Max events to show (default 50)");

    // ── mcp ──────────────────────────────────────────────────────
    auto* cmd_mcp = app.add_subcommand("mcp",
        "Start MCP stdio server for agent integration");

    // ── status ─────────────────────────────────────────────────────
    auto* cmd_status = app.add_subcommand("status",
        "Show daemon and engine status");
    bool show_history = false;
    cmd_status->add_flag("--history", show_history,
        "Include metric trends");

    // ── config ───────────────────────────────────────────────────
    auto* cmd_config = app.add_subcommand("config",
        "Configuration management");
    cmd_config->require_subcommand(1);

    auto* config_reload = cmd_config->add_subcommand("reload",
        "Reload configuration (send SIGHUP to running daemon)");

    auto* config_show = cmd_config->add_subcommand("show",
        "Show effective configuration");

    auto* config_validate = cmd_config->add_subcommand("validate",
        "Validate configuration files");

    // ── Future subcommand stubs ──────────────────────────────────
    app.add_subcommand("workflows", "Manage workflows")->disabled();
    app.add_subcommand("jobs", "Manage jobs")->disabled();
    app.add_subcommand("runs", "Query run history")->disabled();
    app.add_subcommand("logs", "View/follow logs")->disabled();
    app.add_subcommand("explain", "Explain execution plan")->disabled();
    app.add_subcommand("stop", "Stop the daemon")->disabled();
    app.add_subcommand("prune", "Prune old records")->disabled();

    // ── Parse ─────────────────────────────────────────────────────────
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    // ── Dispatch ──────────────────────────────────────────────────────

    if (cmd_version->parsed()) {
        if (json_output) {
            std::cout << "{\"version\":\"" << kairos::kVersion << "\"}\n";
        } else {
            std::cout << "Kairos v" << kairos::kVersion << "\n";
        }
        return 0;
    }

    if (cmd_initdb->parsed()) {
        setup_logging(log_level, json_output, false);

        // Build overrides.
        std::unordered_map<std::string, confy::Value> overrides;
        if (!db_path_override.empty()) {
            overrides["kairos.db_path"] = db_path_override;
        }

        auto cfg = load_config_or_die(config_path, overrides);
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        try {
            fs::path db_path = cfg->db_path;
            if (!db_path_override.empty()) {
                db_path = db_path_override;
            }
            int version = persist::init_database(db_path);
            spdlog::info("Database initialized at {} (schema v{})",
                         db_path.string(), version);
            std::cout << "Database initialized: " << db_path.string()
                      << " (schema v" << version << ")\n";
            return 0;
        } catch (const std::exception& e) {
            spdlog::error("Failed to initialize database: {}", e.what());
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
    }

    if (cmd_start->parsed()) {
        // Build overrides from env/CLI.
        std::unordered_map<std::string, confy::Value> overrides;

        auto cfg = load_config_or_die(config_path, overrides);
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        // Initialize logging for daemon mode.
        observability::LogConfig log_cfg;
        log_cfg.level = observability::parse_log_level(
            cfg->global.get<std::string>("kairos.logging.level", log_level));

        std::string fmt = cfg->global.get<std::string>("kairos.logging.format", "auto");
        if (fmt == "json") {
            log_cfg.json_format = true;
        } else if (fmt == "text") {
            log_cfg.json_format = false;
        } else {
            // "auto" — JSON if not a TTY.
            log_cfg.json_format = !platform::is_tty();
        }

        log_cfg.file_path = cfg->global.get<std::string>("kairos.logging.file", "");
        log_cfg.max_file_size_mb = static_cast<size_t>(
            cfg->global.get<int>("kairos.logging.max_file_size_mb", 100));
        log_cfg.max_files = static_cast<size_t>(
            cfg->global.get<int>("kairos.logging.max_files", 5));
        log_cfg.async_queue_size = static_cast<size_t>(
            cfg->global.get<int>("kairos.logging.async_queue_size", 8192));

        observability::initialize_logging(log_cfg);

        return daemon::run_daemon(cfg);
    }

    // ── watches list ──────────────────────────────────────────────
    if (watches_list->parsed()) {
        setup_logging(log_level, json_output, false);

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        // Load YAML watch-group definitions to populate the engine
        // with actual group metadata (standalone mode per §23.10).
        auto registry = load_registry_from_yaml(cfg, spdlog::default_logger());

        // Create a standalone WatchEngine with the loaded groups.
        watch::WatchEngineConfig watch_cfg;
        watch::RealFilesystemScanner scanner;
        SystemClockSource clock;
        watch::WatchEngine engine(
            watch_cfg,
            watch::WatchEngine::Dependencies{
                .clock = &clock,
                .scanner = &scanner,
            },
            registry->watch_groups());

        auto statuses = engine.get_status();

        if (json_output) {
            json arr = json::array();
            for (const auto& s : statuses) {
                arr.push_back({
                    {"name", s.group_name},
                    {"mode", s.mode},
                    {"watched_paths", s.watched_paths},
                    {"files_in_last_sample", s.files_in_last_sample},
                    {"last_scan_time", s.last_scan_time},
                    {"status", s.status}
                });
            }
            std::cout << json{{"watch_groups", arr}}.dump(2) << "\n";
        } else {
            if (statuses.empty()) {
                std::cout << "No watch groups configured.\n";
            } else {
                // Table output.
                std::cout << fmt::format("{:<20} {:<8} {:<6} {:<6} {:<22} {}\n",
                    "GROUP", "MODE", "PATHS", "FILES",
                    "LAST SCAN", "STATUS");
                std::cout << std::string(80, '-') << "\n";
                for (const auto& s : statuses) {
                    std::cout << fmt::format(
                        "{:<20} {:<8} {:<6} {:<6} {:<22} {}\n",
                        s.group_name, s.mode, s.watched_paths,
                        s.files_in_last_sample,
                        s.last_scan_time.empty() ? "(none)" : s.last_scan_time,
                        s.status);
                }
            }
        }
        return 0;
    }

    // ── watches show ──────────────────────────────────────────────
    if (watches_show->parsed()) {
        setup_logging(log_level, json_output, false);

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        auto registry = load_registry_from_yaml(cfg, spdlog::default_logger());
        const auto& groups = registry->watch_groups();

        // Find the requested group.
        const watch::WatchGroupDef* found = nullptr;
        for (const auto& g : groups) {
            if (g.group_name == watch_show_name) {
                found = &g;
                break;
            }
        }

        if (!found) {
            if (json_output) {
                std::cout << json{
                    {"error", "watch group not found"},
                    {"name", watch_show_name}
                }.dump(2) << "\n";
            } else {
                std::cerr << "Watch group '" << watch_show_name
                          << "' not found.\n";
            }
            return 1;
        }

        // Convert WatchMode enum to string.
        auto mode_str = [](watch::WatchMode m) -> std::string {
            switch (m) {
                case watch::WatchMode::Native: return "native";
                case watch::WatchMode::Sample: return "sample";
                case watch::WatchMode::Hybrid: return "hybrid";
            }
            return "unknown";
        };

        if (json_output) {
            json paths = json::array();
            for (const auto& wi : found->watch_items) {
                paths.push_back(wi);
            }
            json rules = json::array();
            for (const auto& r : found->rules) {
                rules.push_back({
                    {"name", r.rule_name},
                    {"condition", r.condition},
                    {"severity", r.severity},
                    {"description", r.description}
                });
            }
            std::cout << json{
                {"name", found->group_name},
                {"id", found->group_id},
                {"mode", mode_str(found->mode)},
                {"sample_rate_s", found->sample_rate.count()},
                {"max_depth", found->max_depth},
                {"max_files", found->max_files},
                {"watch_items", paths},
                {"rules", rules},
                {"exclude_globs", found->exclude_globs},
                {"enabled", found->enabled}
            }.dump(2) << "\n";
        } else {
            std::cout << "Watch Group: " << found->group_name << "\n";
            std::cout << "ID: " << found->group_id << "\n";
            std::cout << "Mode: " << mode_str(found->mode) << "\n";
            std::cout << "Sample Rate: " << found->sample_rate.count()
                      << "s\n";
            std::cout << "Max Depth: " << found->max_depth << "\n";
            std::cout << "Max Files: " << found->max_files << "\n";
            std::cout << "\nWatch Items:\n";
            for (const auto& wi : found->watch_items) {
                std::cout << "  " << wi << "\n";
            }
            if (!found->exclude_globs.empty()) {
                std::cout << "\nExclude Globs:\n";
                for (const auto& g : found->exclude_globs) {
                    std::cout << "  " << g << "\n";
                }
            }
            std::cout << "\nRules (" << found->rules.size() << "):\n";
            for (const auto& r : found->rules) {
                std::cout << "  " << r.rule_name << ": "
                          << r.condition
                          << " [" << r.severity << "]\n";
            }
        }
        return 0;
    }

    // ── watches scan-once ─────────────────────────────────────────
    if (watches_scan->parsed()) {
        setup_logging(log_level, json_output, false);

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        auto registry = load_registry_from_yaml(cfg, spdlog::default_logger());

        // Create a standalone WatchEngine for scanning.
        watch::WatchEngineConfig watch_cfg;
        watch::RealFilesystemScanner scanner;
        SystemClockSource clock;
        watch::WatchEngine engine(
            watch_cfg,
            watch::WatchEngine::Dependencies{
                .clock = &clock,
                .scanner = &scanner,
            },
            registry->watch_groups());

        // Null sink — standalone scan doesn't push to trigger bus.
        engine::TriggerSink null_sink = [](engine::TriggerEvent) {
            return true;
        };

        if (watch_scan_group.empty()) {
            // Scan all groups.
            auto results = engine.scan_once(null_sink);

            if (json_output) {
                json arr = json::array();
                for (size_t i = 0; i < results.size(); ++i) {
                    const auto& r = results[i];
                    json triggered = json::array();
                    for (const auto& t : r.triggered) {
                        triggered.push_back({
                            {"rule_name", t.rule_name},
                            {"event_type", t.event_type},
                            {"affected_count",
                             static_cast<int>(t.affected_paths.size())}
                        });
                    }
                    arr.push_back({
                        {"files_scanned",
                         static_cast<int>(r.sample.entries.size())},
                        {"changes", {
                            {"created",
                             static_cast<int>(r.diff.created.size())},
                            {"modified",
                             static_cast<int>(r.diff.modified.size())},
                            {"deleted",
                             static_cast<int>(r.diff.deleted.size())},
                        }},
                        {"triggered", triggered},
                        {"scan_duration_ms", r.scan_duration.count()},
                        {"incomplete", r.incomplete}
                    });
                }
                std::cout << json{
                    {"scanned_groups", static_cast<int>(results.size())},
                    {"results", arr}
                }.dump(2) << "\n";
            } else {
                for (size_t i = 0; i < results.size(); ++i) {
                    const auto& r = results[i];
                    auto groups = registry->watch_groups();
                    std::string gname = i < groups.size()
                        ? groups[i].group_name : "(unknown)";
                    std::cout << fmt::format(
                        "Group: {} — {} files scanned "
                        "(+{} -{} ~{}) in {}ms",
                        gname,
                        r.sample.entries.size(),
                        r.diff.created.size(),
                        r.diff.deleted.size(),
                        r.diff.modified.size(),
                        r.scan_duration.count()) << "\n";
                    for (const auto& t : r.triggered) {
                        std::cout << "  Triggered: " << t.rule_name
                                  << " (" << t.event_type << ", "
                                  << t.affected_paths.size()
                                  << " files)\n";
                    }
                }
                if (results.empty()) {
                    std::cout << "No watch groups configured.\n";
                }
            }
        } else {
            // Scan a specific group.
            try {
                auto result = engine.scan_group(watch_scan_group, null_sink);
                if (json_output) {
                    json triggered = json::array();
                    for (const auto& t : result.triggered) {
                        json paths = json::array();
                        for (const auto& p : t.affected_paths) {
                            paths.push_back(p);
                        }
                        triggered.push_back({
                            {"rule_name", t.rule_name},
                            {"event_type", t.event_type},
                            {"affected_paths", paths}
                        });
                    }
                    std::cout << json{
                        {"watch_group", watch_scan_group},
                        {"files_scanned",
                         static_cast<int>(result.sample.entries.size())},
                        {"changes", {
                            {"created",
                             static_cast<int>(result.diff.created.size())},
                            {"modified",
                             static_cast<int>(result.diff.modified.size())},
                            {"deleted",
                             static_cast<int>(result.diff.deleted.size())},
                        }},
                        {"triggered", triggered},
                        {"scan_duration_ms", result.scan_duration.count()},
                        {"incomplete", result.incomplete}
                    }.dump(2) << "\n";
                } else {
                    std::cout << fmt::format(
                        "Group: {} — {} files scanned "
                        "(+{} -{} ~{}) in {}ms\n",
                        watch_scan_group,
                        result.sample.entries.size(),
                        result.diff.created.size(),
                        result.diff.deleted.size(),
                        result.diff.modified.size(),
                        result.scan_duration.count());
                    for (const auto& t : result.triggered) {
                        std::cout << "  Triggered: " << t.rule_name
                                  << " (" << t.event_type << ", "
                                  << t.affected_paths.size()
                                  << " files)\n";
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "Error scanning group '"
                          << watch_scan_group << "': "
                          << e.what() << "\n";
                return 1;
            }
        }
        return 0;
    }

    // ── events list ───────────────────────────────────────────────
    if (events_list->parsed()) {
        setup_logging(log_level, json_output, false);

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        // Open the SQLite database in read-only mode and query
        // watch_events directly (no running daemon needed).
        try {
            auto db = persist::open_database(cfg->db_path);
            persist::QueryReader reader(*db);

            auto events = reader.query_watch_events(
                events_limit, events_group);

            if (json_output) {
                json arr = json::array();
                for (const auto& e : events) {
                    arr.push_back({
                        {"event_uid", e.event_uid},
                        {"watch_group", e.watch_group},
                        {"rule_name", e.rule_name},
                        {"event_type", e.event_type},
                        {"severity", e.severity},
                        {"affected_files", e.affected_files_json},
                        {"sample_epoch", e.sample_epoch},
                        {"created_at", e.created_at}
                    });
                }
                std::cout << json{
                    {"events", arr},
                    {"count", static_cast<int>(events.size())}
                }.dump(2) << "\n";
            } else {
                if (events.empty()) {
                    std::cout << "No events found.\n";
                    if (!events_group.empty()) {
                        std::cout << "  (filtered by group: "
                                  << events_group << ")\n";
                    }
                } else {
                    std::cout << fmt::format(
                        "{:<20} {:<15} {:<15} {:<10} {}\n",
                        "GROUP", "RULE", "TYPE", "SEVERITY",
                        "CREATED");
                    std::cout << std::string(80, '-') << "\n";
                    for (const auto& e : events) {
                        std::cout << fmt::format(
                            "{:<20} {:<15} {:<15} {:<10} {}\n",
                            e.watch_group, e.rule_name, e.event_type,
                            e.severity, e.created_at);
                    }
                    std::cout << "\n" << events.size() << " event(s)\n";
                }
            }
        } catch (const std::exception& e) {
            if (json_output) {
                std::cout << json{
                    {"events", json::array()},
                    {"error", e.what()}
                }.dump(2) << "\n";
            } else {
                std::cerr << "Failed to query events: " << e.what()
                          << "\n"
                          << "Run 'kairos init-db' first if the "
                          << "database does not exist.\n";
            }
            return 1;
        }
        return 0;
    }

    // ── status ───────────────────────────────────────────────────
    if (cmd_status->parsed()) {
        setup_logging(log_level, json_output, false);

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        // Determine if daemon is running (check PID file).
        auto lock_path = cfg->data_dir / "kairos.lock";
        std::string daemon_pid;
        bool daemon_running = false;
        {
            std::ifstream pf(lock_path);
            if (pf.is_open()) {
                std::getline(pf, daemon_pid);
                // Check if the PID is actually alive.
                if (!daemon_pid.empty()) {
#ifndef _WIN32
                    pid_t pid = std::stoi(daemon_pid);
                    daemon_running = (::kill(pid, 0) == 0);
#else
                    daemon_running = true;  // Best effort on Windows.
#endif
                }
            }
        }

        // Open DB for run stats.
        persist::QueryReader::RunStats stats;
        int64_t db_size = 0;
        try {
            auto db = persist::open_database(cfg->db_path);
            persist::QueryReader reader(*db);
            stats = reader.query_run_stats();
            db_size = persist::QueryReader::query_db_size(cfg->db_path);
        } catch (...) {
            // DB may not exist — show zeros.
        }

        // Load registry for watch group count.
        auto registry = load_registry_from_yaml(
            cfg, spdlog::default_logger());

        if (json_output) {
            std::cout << json{
                {"version", std::string(kairos::kVersion)},
                {"daemon_running", daemon_running},
                {"daemon_pid", daemon_pid},
                {"config_path", cfg->config_file_path.string()},
                {"data_dir", cfg->data_dir.string()},
                {"db_size_bytes", db_size},
                {"workflows", static_cast<int>(registry->workflow_count())},
                {"watch_groups", static_cast<int>(registry->watch_group_count())},
                {"total_runs", stats.total_runs},
                {"runs_today", stats.runs_today},
                {"failures_today", stats.failures_today},
                {"active_runs", stats.active_runs},
            }.dump(2) << "\n";
        } else {
            // Human-friendly status display (§23.4).
            auto format_bytes = [](int64_t bytes) -> std::string {
                if (bytes < 1024) return fmt::format("{} B", bytes);
                if (bytes < 1024 * 1024)
                    return fmt::format("{:.1f} KB", bytes / 1024.0);
                return fmt::format("{:.1f} MB", bytes / (1024.0 * 1024.0));
            };

            std::string status_str = daemon_running
                ? "RUNNING" : "STOPPED";
            std::string status_icon = daemon_running ? "[ok]" : "[--]";

            std::cout << "\n";
            std::cout << "  KAIROS v" << kairos::kVersion << "\n";
            std::cout << "  Status: " << status_str;
            if (daemon_running && !daemon_pid.empty()) {
                std::cout << "  (PID " << daemon_pid << ")";
            }
            std::cout << "\n";
            std::cout << "  Config: " << cfg->config_file_path.string()
                      << "\n";
            std::cout << "  Data:   " << cfg->data_dir.string() << "\n";
            std::cout << "\n";

            std::cout << fmt::format(
                "  Workflows:    {}    Watch Groups: {}\n",
                registry->workflow_count(),
                registry->watch_group_count());
            std::cout << fmt::format(
                "  DB Size:      {}    Total Runs:   {}\n",
                format_bytes(db_size), stats.total_runs);
            std::cout << fmt::format(
                "  Runs Today:   {}    Failures:     {}\n",
                stats.runs_today, stats.failures_today);
            if (stats.active_runs > 0) {
                std::cout << fmt::format(
                    "  Active Runs:  {}\n", stats.active_runs);
            }
            std::cout << "\n";
        }
        return 0;
    }

    // ── config reload ────────────────────────────────────────────
    if (config_reload->parsed()) {
        setup_logging(log_level, json_output, false);

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        auto lock_path = cfg->data_dir / "kairos.lock";

        // Read PID from lock file.
        std::string pid_str;
        {
            std::ifstream pf(lock_path);
            if (!pf.is_open()) {
                if (json_output) {
                    std::cout << json{
                        {"success", false},
                        {"error", "Daemon not running (no PID file)"}
                    }.dump(2) << "\n";
                } else {
                    std::cerr << "Daemon not running "
                              << "(no PID file at "
                              << lock_path.string() << ")\n";
                }
                return static_cast<int>(ExitCode::kNotRunning);
            }
            std::getline(pf, pid_str);
        }

        if (pid_str.empty()) {
            std::cerr << "PID file is empty.\n";
            return static_cast<int>(ExitCode::kNotRunning);
        }

#ifndef _WIN32
        pid_t pid = std::stoi(pid_str);

        // Check if process is alive.
        if (::kill(pid, 0) != 0) {
            if (json_output) {
                std::cout << json{
                    {"success", false},
                    {"error", "Daemon process " + pid_str +
                              " is not running"}
                }.dump(2) << "\n";
            } else {
                std::cerr << "Daemon process " << pid_str
                          << " is not running.\n";
            }
            return static_cast<int>(ExitCode::kNotRunning);
        }

        // Send SIGHUP to trigger reload.
        if (::kill(pid, SIGHUP) != 0) {
            int err = errno;
            if (json_output) {
                std::cout << json{
                    {"success", false},
                    {"error", "Failed to send SIGHUP: " +
                              std::string(std::strerror(err))}
                }.dump(2) << "\n";
            } else {
                std::cerr << "Failed to send SIGHUP to PID "
                          << pid_str << ": "
                          << std::strerror(err) << "\n";
            }
            return 1;
        }

        if (json_output) {
            std::cout << json{
                {"success", true},
                {"pid", pid},
                {"signal", "SIGHUP"}
            }.dump(2) << "\n";
        } else {
            std::cout << "Reload signal sent to daemon (PID "
                      << pid_str << ")\n";
        }
        return 0;
#else
        // Windows: SIGHUP not available.
        // Future: use a named event or control pipe.
        if (json_output) {
            std::cout << json{
                {"success", false},
                {"error", "Reload via signal not supported on Windows"}
            }.dump(2) << "\n";
        } else {
            std::cerr << "Reload via signal is not supported on Windows.\n"
                      << "Use the MCP reloadConfig tool instead.\n";
        }
        return 1;
#endif
    }

    // ── config show ──────────────────────────────────────────────
    if (config_show->parsed()) {
        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        if (json_output) {
            std::cout << json{
                {"config_file", cfg->config_file_path.string()},
                {"data_dir", cfg->data_dir.string()},
                {"db_path", cfg->db_path.string()},
            }.dump(2) << "\n";
        } else {
            std::cout << "Config file: " << cfg->config_file_path.string()
                      << "\n";
            std::cout << "Data dir:    " << cfg->data_dir.string() << "\n";
            std::cout << "DB path:     " << cfg->db_path.string() << "\n";
        }
        return 0;
    }

    // ── config validate ──────────────────────────────────────────
    if (config_validate->parsed()) {
        auto config_file = config::resolve_config_path(config_path);
        auto result = config::load_config(config_file, {});
        if (result.ok()) {
            if (json_output) {
                std::cout << json{{"valid", true}}.dump(2) << "\n";
            } else {
                std::cout << "Configuration is valid.\n";
            }
            return 0;
        } else {
            if (json_output) {
                json errs = json::array();
                for (const auto& e : result.errors) {
                    errs.push_back({
                        {"key", e.key_path},
                        {"message", e.message}
                    });
                }
                std::cout << json{
                    {"valid", false},
                    {"errors", errs}
                }.dump(2) << "\n";
            } else {
                std::cerr << "Configuration has errors:\n";
                for (const auto& e : result.errors) {
                    std::cerr << "  [" << e.key_path << "] "
                              << e.message << "\n";
                }
            }
            return static_cast<int>(ExitCode::kConfigError);
        }
    }

    // ── mcp ───────────────────────────────────────────────────────
    if (cmd_mcp->parsed()) {
        // MCP mode: stdout is reserved for JSON-RPC.
        // Redirect all logging to stderr.
        auto stderr_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        stderr_sink->set_level(spdlog::level::info);
        auto logger = std::make_shared<spdlog::logger>("kairos", stderr_sink);
        logger->set_level(spdlog::level::info);
        spdlog::set_default_logger(logger);

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        spdlog::info("Starting MCP stdio server");

        // Create a standalone MCP handler with minimal deps.
        // In a full daemon, this would share state with the daemon.
        // For v1, MCP runs standalone and creates its own watch engine.
        mcp::McpHandler::Dependencies mcp_deps;
        mcp_deps.server_info.name = "kairos";
        mcp_deps.server_info.version = std::string(kairos::kVersion);

        mcp::McpHandler handler(mcp_deps);

        mcp::StdioTransport transport(
            [&handler](const std::string& method,
                       const json& params,
                       const json& id) -> json {
                return handler.dispatch(method, params, id);
            });

        // Run the MCP transport loop (blocks until EOF).
        transport.run();

        spdlog::info("MCP server stopped ({} requests processed)",
                     transport.requests_processed());
        return 0;
    }

    // Should not reach here (require_subcommand is set).
    std::cerr << "No subcommand specified. Use --help for usage.\n";
    return 1;
}

}  // namespace kairos::cli
