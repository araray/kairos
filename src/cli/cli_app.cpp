// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  cli_app.cpp — CLI application with CLI11                                 ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/cli/cli_app.hpp"
#include "kairos/config/config_store.hpp"
#include "kairos/core/exit_codes.hpp"
#include "kairos/core/version.hpp"
#include "kairos/daemon/daemon.hpp"
#include "kairos/observability/logging.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/platform/platform.hpp"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

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

    // ── Future subcommand stubs ───────────────────────────────────────
    app.add_subcommand("workflows", "Manage workflows")->disabled();
    app.add_subcommand("jobs", "Manage jobs")->disabled();
    app.add_subcommand("runs", "Query run history")->disabled();
    app.add_subcommand("logs", "View/follow logs")->disabled();
    app.add_subcommand("events", "View watch events")->disabled();
    app.add_subcommand("explain", "Explain execution plan")->disabled();
    app.add_subcommand("mcp", "Start MCP stdio server")->disabled();
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

    // Should not reach here (require_subcommand is set).
    std::cerr << "No subcommand specified. Use --help for usage.\n";
    return 1;
}

}  // namespace kairos::cli
