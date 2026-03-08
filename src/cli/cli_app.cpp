/// src/cli/cli_app.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  cli_app.cpp — CLI application with CLI11                                 ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/cli/cli_app.hpp"
#include "kairos/config/config_store.hpp"
#include "kairos/core/exit_codes.hpp"
#include "kairos/core/version.hpp"
#include "kairos/daemon/daemon.hpp"
#include "kairos/mcp/handler.hpp"
#include "kairos/mcp/transport.hpp"
#include "kairos/observability/logging.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/platform/platform.hpp"
#include "kairos/watch/real_scanner.hpp"
#include "kairos/watch/watch_engine.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace kairos::cli {

namespace {

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

    // ── Future subcommand stubs ──────────────────────────────────
    app.add_subcommand("workflows", "Manage workflows")->disabled();
    app.add_subcommand("jobs", "Manage jobs")->disabled();
    app.add_subcommand("runs", "Query run history")->disabled();
    app.add_subcommand("logs", "View/follow logs")->disabled();
    app.add_subcommand("explain", "Explain execution plan")->disabled();
    app.add_subcommand("status", "Show daemon status")->disabled();
    app.add_subcommand("reload", "Reload configuration")->disabled();
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

        // Build a minimal watch engine to query status.
        // For a running daemon, this would connect via IPC.
        // For v1, we load config and create a standalone WatchEngine.
        watch::WatchEngineConfig watch_cfg;
        watch::RealFilesystemScanner scanner;
        watch::WatchEngine engine(
            watch_cfg,
            watch::WatchEngine::Dependencies{
                .clock = nullptr,
                .scanner = &scanner,
            },
            {}  // Empty groups — status from config.
        );

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
                // Simple table output.
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
        // Stub: show details for a specific watch group.
        if (json_output) {
            std::cout << json{
                {"watch_group", watch_show_name},
                {"status", "not_implemented"},
                {"message", "Watch group detail requires a running daemon"}
            }.dump(2) << "\n";
        } else {
            std::cerr << "Watch group detail requires a running daemon.\n"
                      << "Use 'kairos start' first, then query via MCP.\n";
        }
        return 0;
    }

    // ── watches scan-once ─────────────────────────────────────────
    if (watches_scan->parsed()) {
        setup_logging(log_level, json_output, false);
        // Stub: scan-once requires daemon access.
        if (json_output) {
            std::cout << json{
                {"status", "not_implemented"},
                {"message", "scan-once requires a running daemon"}
            }.dump(2) << "\n";
        } else {
            std::cerr << "scan-once requires a running daemon.\n"
                      << "Use 'kairos start' first, then query via MCP.\n";
        }
        return 0;
    }

    // ── events list ───────────────────────────────────────────────
    if (events_list->parsed()) {
        setup_logging(log_level, json_output, false);
        // Stub: events list requires daemon access.
        if (json_output) {
            std::cout << json{
                {"events", json::array()},
                {"message", "Event listing requires a running daemon"}
            }.dump(2) << "\n";
        } else {
            std::cerr << "Event listing requires a running daemon.\n"
                      << "Use 'kairos start' first, then query via MCP.\n";
        }
        return 0;
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
