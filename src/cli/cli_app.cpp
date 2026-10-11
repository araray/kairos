/// src/cli/cli_app.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  cli_app.cpp — CLI application with CLI11                                 ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/cli/cli_app.hpp"
#include "kairos/cli/cli_migrate.hpp"
#include "kairos/cli/filter.hpp"
#include "kairos/cli/interactive_selector.hpp"
#include "kairos/cli/table.hpp"
#include "kairos/config/config_store.hpp"
#include "kairos/config/yaml_loader.hpp"
#include "kairos/core/exit_codes.hpp"
#include "kairos/core/id_generator.hpp"
#include "kairos/core/version.hpp"
#include "kairos/daemon/daemon.hpp"
#include "kairos/engine/explain.hpp"
#include "kairos/engine/pipeline.hpp"
#include "kairos/engine/trigger_event.hpp"
#include "kairos/engine/trigger_types.hpp"
#include "kairos/engine/workflow_registry.hpp"
#include "kairos/exec/process_handle.hpp"
#include "kairos/exec/runner_pool.hpp"
#include "kairos/kel/errors.hpp"
#include "kairos/kel/evaluator.hpp"
#include "kairos/kel/value.hpp"
#include "kairos/mcp/handler.hpp"
#include "kairos/mcp/transport.hpp"
#include "kairos/observability/health.hpp"
#include "kairos/observability/logging.hpp"
#include "kairos/security/native_secret_file.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/persist/query_reader.hpp"
#include "kairos/persist/tag_store.hpp"
#include "kairos/platform/platform.hpp"
#include "kairos/platform/time_compat.hpp"
#include "kairos/platform/timezone.hpp"
#include "kairos/testing/fake_clock.hpp"
#include "kairos/watch/real_scanner.hpp"
#include "kairos/watch/watch_engine.hpp"
#include "kairos/tui/tui_dashboard.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>


#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/types.h>
#include <signal.h>
#endif

#ifdef _WIN32
#include "kairos/daemon/win32_service.hpp"
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

/// Global timezone config for display — set once per command from the
/// loaded ConfigState. Defaults to "local" if no config is loaded.
static platform::TimezoneConfig g_tz_config =
    platform::TimezoneConfig::parse("local");

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

    // Patch CronTriggers with configured timezone delta so that
    // next_fire_after (used by `kairos jobs list` etc.) computes
    // correct fire times in the user's timezone.
    int cron_delta = platform::cron_tz_delta_seconds(g_tz_config);
    for (auto& entry : yaml_result.triggers) {
        if (auto* cron = std::get_if<engine::CronTrigger>(&entry.spec)) {
            cron->cron_tz_delta_s = cron_delta;
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

// ── Helpers for enriched CLI output ─────────────────────────────────────

/// Initialize the display timezone from a loaded config.
static void init_timezone(
    const std::shared_ptr<const config::ConfigState>& cfg)
{
    if (cfg) {
        auto tz_str = cfg->global.get<std::string>(
            "kairos.timezone", "local");
        g_tz_config = platform::TimezoneConfig::parse(tz_str);
    }
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
    // Set display timezone for all CLI output.
    init_timezone(result.state);
    return result.state;
}

/// Parse a UTC ISO-8601 timestamp string.
/// Delegates to platform::parse_utc_timestamp.
static std::chrono::system_clock::time_point
parse_iso8601(const std::string& ts)
{
    return platform::parse_utc_timestamp(ts);
}

/// Format a time_point as relative string ("2h ago", "just now", etc.)
/// Delegates to platform::format_relative.
static std::string format_relative(
    std::chrono::system_clock::time_point tp)
{
    return platform::format_relative(tp);
}

/// Format a UTC timestamp string as relative.
static std::string format_relative_ts(const std::string& ts)
{
    return platform::format_relative(ts);
}

/// Format a UTC timestamp for display in the configured timezone.
static std::string format_display_ts(const std::string& ts)
{
    return platform::format_display_time(ts, g_tz_config);
}

/// Compute next fire time for a trigger and return as ISO-8601 + relative.
struct NextFireInfo {
    std::string iso;           ///< ISO-8601 timestamp
    std::string relative;      ///< "in 15m", "in 2h 30m"
    std::string schedule_expr; ///< Cron expression, interval, or date
};

static NextFireInfo compute_next_fire(const engine::TimerEntry& t)
{
    NextFireInfo info;
    auto now = std::chrono::system_clock::now();

    info.schedule_expr = std::visit([](const auto& s) -> std::string {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, engine::CronTrigger>)
            return s.expression;
        if constexpr (std::is_same_v<T, engine::IntervalTrigger>) {
            auto secs = s.interval.count() / 1000;
            if (secs < 60) return fmt::format("every {}s", secs);
            if (secs < 3600) return fmt::format("every {}m", secs / 60);
            return fmt::format("every {}h", secs / 3600);
        }
        if constexpr (std::is_same_v<T, engine::DateTrigger>) {
            if (s.fired) return "(exhausted)";
            // Format fire_at as ISO.
            auto tt = std::chrono::system_clock::to_time_t(s.fire_at);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &tt);
#else
            localtime_r(&tt, &tm);
#endif
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
            return std::string(buf);
        }
    }, t.spec);

    // Compute next fire.
    auto next = std::visit([&](const auto& s) ->
        std::chrono::system_clock::time_point {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, engine::CronTrigger>) {
            try { return s.next_fire_after(now); }
            catch (...) { return {}; }
        }
        if constexpr (std::is_same_v<T, engine::IntervalTrigger>) {
            // For standalone CLI: next fire is now + interval.
            return now + s.interval;
        }
        if constexpr (std::is_same_v<T, engine::DateTrigger>) {
            return s.fired ? std::chrono::system_clock::time_point{}
                           : s.fire_at;
        }
    }, t.spec);

    if (next.time_since_epoch().count() == 0) {
        info.iso = "--";
        info.relative = "--";
        return info;
    }

    // Format as ISO.
    auto tt = std::chrono::system_clock::to_time_t(next);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    info.iso = buf;
    info.relative = format_relative(next);
    return info;
}

/// Try to open a read-only DB reader. Returns nullptr if DB doesn't exist.
static std::unique_ptr<SQLite::Database> try_open_db(
    const std::shared_ptr<const config::ConfigState>& cfg)
{
    std::error_code ec;
    if (!fs::exists(cfg->db_path, ec)) return nullptr;
    try {
        return persist::open_database(cfg->db_path);
    } catch (...) {
        return nullptr;
    }
}

/// Get last run status for a target by name.
struct LastRunInfo {
    std::string status;     ///< "SUCCESS", "FAILED", etc. or "--"
    std::string run_id;
    std::string when;       ///< Relative time string
    int64_t duration_ms = 0;
};

static LastRunInfo last_run_for_target(
    persist::QueryReader* reader, const std::string& target_name)
{
    if (!reader) return {"--", "", "--", 0};
    auto runs = reader->query_recent_runs(1, "", target_name);
    if (runs.empty()) return {"--", "", "never", 0};
    const auto& r = runs[0];
    return {r.status, r.run_id, format_relative_ts(r.start_ts),
            r.duration_ms};
}

/// Find triggers targeting a specific target (by name or id).
static std::vector<const engine::TimerEntry*> triggers_for_target(
    const engine::WorkflowRegistry& reg,
    const std::string& target_id,
    const std::string& target_name)
{
    std::vector<const engine::TimerEntry*> result;
    for (const auto& t : reg.triggers()) {
        if (t.target_id == target_id || t.target_name == target_name)
            result.push_back(&t);
    }
    return result;
}

/// Resolve a (possibly truncated) run ID prefix to a full run_id.
/// On success, returns the full ID. On failure, prints an appropriate
/// error message and returns an empty string.
/// Roadmap §1.1 — Run ID prefix matching.
static std::string resolve_prefix_or_error(
    const persist::QueryReader& reader,
    const std::string& input,
    bool json_mode)
{
    auto pr = reader.resolve_run_id_prefix(input);
    switch (pr.status) {
        case persist::QueryReader::PrefixResult::kExact:
        case persist::QueryReader::PrefixResult::kUnique:
            return pr.resolved_id;

        case persist::QueryReader::PrefixResult::kAmbiguous:
            if (json_mode) {
                nlohmann::json j;
                j["error"] = "Ambiguous run ID prefix";
                j["prefix"] = input;
                j["candidates"] = pr.candidates;
                std::cout << j.dump(2) << "\n";
            } else {
                std::cerr << "Ambiguous run ID prefix '" << input
                          << "' — matches " << pr.candidates.size()
                          << " runs:\n";
                for (const auto& c : pr.candidates) {
                    std::cerr << "  " << c << "\n";
                }
                std::cerr << "Provide a longer prefix to disambiguate.\n";
            }
            return {};

        case persist::QueryReader::PrefixResult::kNotFound:
            if (json_mode) {
                std::cout << nlohmann::json{
                    {"error", "Run not found"},
                    {"run_id", input}
                }.dump(2) << "\n";
            } else {
                std::cerr << "Run '" << input << "' not found.\n";
            }
            return {};

        case persist::QueryReader::PrefixResult::kEmpty:
            if (json_mode) {
                std::cout << nlohmann::json{
                    {"error", "Empty run ID"}
                }.dump(2) << "\n";
            } else {
                std::cerr << "Error: run ID cannot be empty.\n";
            }
            return {};
    }
    return {};  // unreachable
}

/// Summarize affected files JSON for CLI display.
/// Parses the JSON array and returns a comma-separated summary,
/// truncated to max_len unless verbose is true.
static std::string summarize_affected_files(
    const std::string& affected_files_json,
    bool verbose, size_t max_len = 40)
{
    if (affected_files_json.empty() ||
        affected_files_json == "[]" ||
        affected_files_json == "null") {
        return "--";
    }

    try {
        auto arr = nlohmann::json::parse(affected_files_json);
        if (!arr.is_array() || arr.empty()) return "--";

        std::string result;
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i > 0) result += ", ";
            std::string path = arr[i].get<std::string>();
            // Show just the filename in non-verbose mode.
            if (!verbose) {
                auto pos = path.find_last_of("/\\");
                if (pos != std::string::npos) {
                    path = path.substr(pos + 1);
                }
            }
            result += path;
        }

        if (!verbose && result.size() > max_len) {
            result = result.substr(0, max_len - 3) + "...";
        }
        return result;
    } catch (...) {
        // If JSON parsing fails, return the raw string truncated.
        if (verbose) return affected_files_json;
        if (affected_files_json.size() > max_len) {
            return affected_files_json.substr(0, max_len - 3) + "...";
        }
        return affected_files_json;
    }
}

// ── Sorted help formatter ────────────────────────────────────────────────
// CLI11 displays subcommands in registration order. This formatter sorts
// them alphabetically within each group so `--help` is easy to scan.
// Inherits all other formatting from CLI::Formatter.

class SortedHelpFormatter : public CLI::Formatter {
public:
    using CLI::Formatter::Formatter;

    std::string make_subcommands(const CLI::App *app,
                                 CLI::AppFormatMode mode) const override {
        std::string out;
        auto subcommands = app->get_subcommands({});

        // Collect unique group names in first-seen order.
        std::vector<std::string> groups_seen;
        for (const auto *com : subcommands) {
            if (com->get_name().empty()) {
                if (!com->get_group().empty()) {
                    out += make_expanded(com);
                }
                continue;
            }
            const auto &gk = com->get_group();
            if (!gk.empty() &&
                std::find(groups_seen.begin(), groups_seen.end(), gk)
                    == groups_seen.end()) {
                groups_seen.push_back(gk);
            }
        }

        // For each group: collect, sort, format.
        for (const auto &group : groups_seen) {
            std::vector<const CLI::App *> group_cmds;
            for (const auto *sub : subcommands) {
                if (sub->get_group() == group)
                    group_cmds.push_back(sub);
            }

            std::sort(group_cmds.begin(), group_cmds.end(),
                [](const CLI::App *a, const CLI::App *b) {
                    return a->get_name() < b->get_name();
                });

            out += "\n" + group + ":\n";
            for (const auto *cmd : group_cmds) {
                if (mode != CLI::AppFormatMode::All) {
                    out += make_subcommand(cmd);
                } else {
                    out += make_expanded(cmd);
                    out += "\n";
                }
            }
        }
        return out;
    }
};

}  // anonymous namespace

int run(int argc, char** argv) {
    // ── Root CLI app ──────────────────────────────────────────────────
    CLI::App app{"Kairos — Unified Orchestration Daemon"};
    app.formatter(std::make_shared<SortedHelpFormatter>());
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

#ifdef _WIN32
    bool as_service = false;
    cmd_start->add_flag("--service", as_service,
        "Run as Windows Service (internal — do not use directly)");
#endif

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
    std::string watches_filter;
    bool watches_interactive = false;
    watches_list->add_option("--filter,-F", watches_filter,
        "Regex filter on displayed rows (§5.1)");
    watches_list->add_flag("-i,--interactive", watches_interactive,
        "Interactive selection mode (§2.1)");

    auto* watches_show = cmd_watches->add_subcommand("show",
        "Show watch group detail");
    std::string watch_show_name;
    watches_show->add_option("name", watch_show_name,
        "Watch group name")->required();

    auto* watches_scan = cmd_watches->add_subcommand("scan-once",
        "Run a single scan cycle");
    std::string watch_scan_group;
    bool watch_list_files = false;
    watches_scan->add_option("group", watch_scan_group,
        "Watch group to scan (all if omitted)");
    watches_scan->add_flag("--list-files,-l", watch_list_files,
        "List all monitored files");

    // ── events ───────────────────────────────────────────────────
    auto* cmd_events = app.add_subcommand("events",
        "Watch events");
    cmd_events->require_subcommand(1);

    auto* events_list = cmd_events->add_subcommand("list",
        "List recent events");
    std::string events_group;
    int events_limit = 50;
    bool events_verbose = false;
    bool events_no_header = false;
    events_list->add_option("--watch-group", events_group,
        "Filter by watch group");
    events_list->add_option("-n,--limit", events_limit,
        "Max events to show (default 50)");
    events_list->add_flag("-v,--verbose", events_verbose,
        "Show full affected file paths");
    events_list->add_flag("--no-header", events_no_header,
        "Suppress table header");
    std::string events_filter;
    bool events_interactive = false;
    events_list->add_option("--filter,-F", events_filter,
        "Regex filter on displayed rows (§5.1)");
    events_list->add_flag("-i,--interactive", events_interactive,
        "Interactive selection mode (§2.1)");

    // ── mcp ──────────────────────────────────────────────────────
    auto* cmd_mcp = app.add_subcommand("mcp",
        "Start MCP stdio server for agent integration");

    // ── status ─────────────────────────────────────────────────────
    auto* cmd_status = app.add_subcommand("status",
        "Show daemon and engine status");
    bool show_history = false;
    cmd_status->add_flag("--history", show_history,
        "Include metric trends");
    bool show_health = false;
    cmd_status->add_flag("--health", show_health,
        "Run self-monitoring checks (Roadmap \u00a715.5)");

    // ── config ───────────────────────────────────────────────────
    // ── secrets (§12.2.1) ────────────────────────────────────────
    auto* cmd_secrets = app.add_subcommand(
        "secrets", "Manage the native encrypted secrets file");
    cmd_secrets->require_subcommand(1);

    std::string secrets_file_override;
    cmd_secrets->add_option("--file", secrets_file_override,
        "Secrets file path (default: <data_dir>/secrets.enc)");
    std::string secrets_password;
    cmd_secrets->add_option("--password", secrets_password,
        "Master password (default: KAIROS_SECRETS_PASSWORD env, "
        "or prompt)");

    auto* secrets_init = cmd_secrets->add_subcommand(
        "init", "Create an empty secrets file");
    std::string secrets_set_key;
    auto* secrets_set = cmd_secrets->add_subcommand(
        "set", "Add or update a secret (value read from stdin)");
    secrets_set->add_option("key", secrets_set_key, "Secret key")
        ->required();
    std::string secrets_set_value;
    secrets_set->add_option("--value", secrets_set_value,
        "Value (if omitted, read one line from stdin)");
    std::string secrets_rm_key;
    bool secrets_rm_force = false;
    auto* secrets_remove = cmd_secrets->add_subcommand(
        "remove", "Remove a secret");
    secrets_remove->add_option("key", secrets_rm_key, "Secret key")
        ->required();
    secrets_remove->add_flag("-f,--force", secrets_rm_force,
        "Do not error when the key is absent");
    auto* secrets_list = cmd_secrets->add_subcommand(
        "list", "List secret keys (values are never shown)");
    auto* secrets_rotate = cmd_secrets->add_subcommand(
        "rotate-password", "Re-encrypt under a new master password");

    // ── export/import (§15.9) ────────────────────────────────────
    auto* cmd_export = app.add_subcommand("export",
        "Export run history, events, and audit to a JSON bundle");
    std::string export_since;
    cmd_export->add_option("--since", export_since,
        "Only records after this ISO-8601 timestamp");
    std::string export_output;
    cmd_export->add_option("--output,-o", export_output,
        "Output file")->required();
    auto* cmd_import = app.add_subcommand("import",
        "Import a JSON bundle produced by kairos export");
    std::string import_file;
    cmd_import->add_option("file", import_file,
        "Bundle file path")->required();
    bool import_merge = false;
    cmd_import->add_flag("--merge", import_merge,
        "Keep existing rows; insert only missing run_ids");



    auto* cmd_config = app.add_subcommand("config",
        "Configuration management");
    cmd_config->require_subcommand(1);

    auto* config_reload = cmd_config->add_subcommand("reload",
        "Reload configuration (send SIGHUP to running daemon)");

    auto* config_show = cmd_config->add_subcommand("show",
        "Show effective configuration");

    auto* config_validate = cmd_config->add_subcommand("validate",
        "Validate configuration files");

    // ── runs ───────────────────────────────────────────────────────
    auto* cmd_runs = app.add_subcommand("runs",
        "Query run history");
    cmd_runs->require_subcommand(1);

    auto* runs_list = cmd_runs->add_subcommand("list",
        "List recent runs");
    int runs_limit = 20;
    std::string runs_status;
    std::string runs_workflow;
    std::string runs_since;
    bool runs_full_id = false;
    bool runs_no_header = false;
    runs_list->add_option("-n,--limit", runs_limit,
        "Max runs to show (default 20)");
    runs_list->add_option("--status", runs_status,
        "Filter by status (SUCCESS|FAILURE|RUNNING|CANCELLED|INTERRUPTED)");
    runs_list->add_option("--workflow", runs_workflow,
        "Filter by workflow name (substring match)");
    runs_list->add_option("--since", runs_since,
        "Only runs after this ISO-8601 timestamp");
    runs_list->add_flag("--full-id", runs_full_id,
        "Show full run IDs (no truncation)");
    runs_list->add_flag("--no-header", runs_no_header,
        "Suppress table header (for scripting pipelines)");
    std::string runs_filter;
    bool runs_interactive = false;
    runs_list->add_option("--filter,-F", runs_filter,
        "Regex filter on displayed rows (§5.1)");
    runs_list->add_flag("-i,--interactive", runs_interactive,
        "Interactive selection mode (§2.1)");
    bool runs_only_pinned = false;
    runs_list->add_flag("--pinned", runs_only_pinned,
        "Only pinned runs (§15.6)");

    auto* runs_show = cmd_runs->add_subcommand("show",
        "Show run detail with jobs and steps");
    std::string runs_show_id;
    runs_show->add_option("run_id", runs_show_id,
        "Run ID")->required();

    auto* runs_cancel = cmd_runs->add_subcommand("cancel",
        "Cancel a running run");
    std::string run_cancel_id;
    runs_cancel->add_option("run_id", run_cancel_id,
        "Run ID to cancel")->required();

    std::string runs_pin_id;
    auto* runs_pin = cmd_runs->add_subcommand("pin",
        "Pin a run — survives retention pruning (§15.6)");
    runs_pin->add_option("run_id", runs_pin_id,
        "Run ID (unique prefix ok)")->required();
    std::string runs_unpin_id;
    auto* runs_unpin = cmd_runs->add_subcommand("unpin",
        "Unpin a run");
    runs_unpin->add_option("run_id", runs_unpin_id,
        "Run ID (unique prefix ok)")->required();

    // ── logs ──────────────────────────────────────────────────────
    auto* cmd_logs = app.add_subcommand("logs",
        "View logs for a run");
    std::string logs_run_id;
    bool logs_follow = false;
    cmd_logs->add_option("run_id", logs_run_id,
        "Run ID")->required();
    cmd_logs->add_flag("-f,--follow", logs_follow,
        "Follow log output (poll until run completes)");

    // ── triggers ──────────────────────────────────────────────────
    auto* cmd_triggers = app.add_subcommand("triggers",
        "Manage schedule triggers");
    cmd_triggers->require_subcommand(1);

    auto* triggers_list = cmd_triggers->add_subcommand("list",
        "List all configured triggers with next fire times");
    int triggers_limit = 50;
    triggers_list->add_option("-n,--limit", triggers_limit,
        "Max number of triggers to show (default: 50)");
    std::string triggers_filter;
    bool triggers_interactive = false;
    triggers_list->add_option("--filter,-F", triggers_filter,
        "Regex filter on displayed rows (§5.1)");
    triggers_list->add_flag("-i,--interactive", triggers_interactive,
        "Interactive selection mode (§2.1)");

    auto* triggers_next = cmd_triggers->add_subcommand("next",
        "Show the next N triggers to fire");
    int triggers_next_n = 10;
    triggers_next->add_option("-n,--limit", triggers_next_n,
        "Number of upcoming triggers (default: 10)");

    auto* triggers_preview = cmd_triggers->add_subcommand("preview",
        "Preview fire times for a cron expression");
    std::string cron_expr;
    int cron_count = 10;
    triggers_preview->add_option("expression", cron_expr,
        "Cron expression (5 or 6 field)")->required();
    triggers_preview->add_option("-n,--count", cron_count,
        "Number of fire times to show (default: 10)");

    // ── workflows ────────────────────────────────────────────────
    auto* cmd_workflows = app.add_subcommand("workflows",
        "Manage workflows");
    cmd_workflows->require_subcommand(1);

    auto* workflows_list = cmd_workflows->add_subcommand("list",
        "List all workflows");
    std::string workflows_filter;
    bool workflows_interactive = false;
    workflows_list->add_option("--filter,-F", workflows_filter,
        "Regex filter on displayed rows (§5.1)");
    workflows_list->add_flag("-i,--interactive", workflows_interactive,
        "Interactive selection mode (§2.1)");

    auto* workflows_show = cmd_workflows->add_subcommand("show",
        "Show workflow detail");
    std::string workflows_show_id;
    workflows_show->add_option("id", workflows_show_id,
        "Workflow ID or name")->required();

    auto* workflows_run = cmd_workflows->add_subcommand("run",
        "Trigger a workflow run");
    std::string workflows_run_id;
    bool workflows_run_follow = false;
    workflows_run->add_option("id", workflows_run_id,
        "Workflow ID or name")->required();
    workflows_run->add_flag("-f,--follow", workflows_run_follow,
        "Follow log output until completion");
    bool workflows_run_dry_run = false;
    workflows_run->add_flag("--dry-run", workflows_run_dry_run,
        "Execute the workflow with all steps as no-ops (\u00a715.1)");

    auto* workflows_explain = cmd_workflows->add_subcommand("explain",
        "Explain execution plan (dry-run)");
    std::string workflows_explain_id;
    workflows_explain->add_option("id", workflows_explain_id,
        "Workflow ID or name")->required();

    // ── Future subcommand stubs ──────────────────────────────────
    // ── jobs ──────────────────────────────────────────────────────
    auto* cmd_jobs = app.add_subcommand("jobs", "Manage standalone jobs");
    cmd_jobs->require_subcommand(1);

    auto* jobs_list = cmd_jobs->add_subcommand("list",
        "List standalone jobs");
    std::string jobs_filter;
    bool jobs_interactive = false;
    jobs_list->add_option("--filter,-F", jobs_filter,
        "Regex filter on displayed rows (§5.1)");
    jobs_list->add_flag("-i,--interactive", jobs_interactive,
        "Interactive selection mode (§2.1)");

    auto* jobs_show = cmd_jobs->add_subcommand("show",
        "Show job detail");
    std::string jobs_show_id;
    jobs_show->add_option("id", jobs_show_id,
        "Job ID or name")->required();

    auto* jobs_run = cmd_jobs->add_subcommand("run",
        "Run a standalone job");
    std::string jobs_run_id;
    bool jobs_run_follow = false;
    jobs_run->add_option("id", jobs_run_id,
        "Job ID or name")->required();
    jobs_run->add_flag("-f,--follow", jobs_run_follow,
        "Follow log output until completion");

    // ── stop ──────────────────────────────────────────────────────
    auto* cmd_stop = app.add_subcommand("stop",
        "Stop the running daemon (sends SIGTERM)");

    // ── prune ─────────────────────────────────────────────────────
    auto* cmd_prune = app.add_subcommand("prune",
        "Prune old records from the database");
    int prune_days = 30;
    bool prune_dry_run = false;
    bool prune_force = false;
    cmd_prune->add_option("--older-than", prune_days,
        "Prune records older than N days (default: 30)");
    cmd_prune->add_flag("--dry-run", prune_dry_run,
        "Show what would be pruned without deleting");
    cmd_prune->add_flag("--force", prune_force,
        "Skip confirmation prompt");

    // ── events tail ───────────────────────────────────────────────
    auto* events_tail = cmd_events->add_subcommand("tail",
        "Stream watch events in real-time (poll-based)");
    std::string events_tail_group;
    events_tail->add_option("--watch-group", events_tail_group,
        "Filter by watch group");

    // ── completions ──────────────────────────────────────────────
    auto* cmd_completions = app.add_subcommand("completions",
        "Generate shell completion scripts");
    cmd_completions->group("");  // Hidden from --help.
    std::string completions_shell;
    cmd_completions->add_option("shell", completions_shell,
        "Shell type: bash, zsh, or fish")->required();

    // ── Dynamic completion helpers (Phase 8.5, Roadmap §2.3) ────
    // Hidden subcommands that output one entity name/ID per line.
    // Called by shell completion scripts for <TAB> completion of
    // workflow names, job names, run IDs, watch group names, etc.
    auto* cpl_workflows = app.add_subcommand("__complete_workflows",
        "List workflow names for shell completion");
    cpl_workflows->group("");  // Hidden.

    auto* cpl_jobs = app.add_subcommand("__complete_jobs",
        "List standalone job names for shell completion");
    cpl_jobs->group("");

    auto* cpl_runs = app.add_subcommand("__complete_runs",
        "List recent run IDs for shell completion");
    cpl_runs->group("");

    auto* cpl_watches = app.add_subcommand("__complete_watches",
        "List watch group names for shell completion");
    cpl_watches->group("");

    auto* cpl_triggers = app.add_subcommand("__complete_triggers",
        "List trigger IDs for shell completion");
    cpl_triggers->group("");

    // ── migrate-config ───────────────────────────────────────────────
    auto* cmd_migrate_config = app.add_subcommand("migrate-config",
        "Migrate config from a legacy tool to Kairos format");
    std::string mc_source;
    std::string mc_source_config;
    std::string mc_source_watches;
    std::string mc_source_workflows;
    std::string mc_output_dir = ".";
    bool mc_dry_run = false;
    cmd_migrate_config->add_option("--source", mc_source,
        "Source tool: avscheduler, eventwatcher, or localflow")->required();
    cmd_migrate_config->add_option("--source-config", mc_source_config,
        "Path to source config file (TOML)");
    cmd_migrate_config->add_option("--source-watches", mc_source_watches,
        "Path to source watch-groups file (EventWatcher only)");
    cmd_migrate_config->add_option("--source-workflows", mc_source_workflows,
        "Path to source workflows directory (LocalFlow only)");
    cmd_migrate_config->add_option("--output-dir", mc_output_dir,
        "Output directory for generated Kairos config");
    cmd_migrate_config->add_flag("--dry-run", mc_dry_run,
        "Show what would be generated without writing files");

    // ── migrate-db ───────────────────────────────────────────────────
    auto* cmd_migrate_db = app.add_subcommand("migrate-db",
        "Import run history from a legacy tool database");
    std::string md_source;
    std::string md_source_db;
    std::string md_target_db;
    bool md_dry_run = false;
    cmd_migrate_db->add_option("--source", md_source,
        "Source tool: avscheduler or eventwatcher")->required();
    cmd_migrate_db->add_option("--source-db", md_source_db,
        "Path to source SQLite database")->required();
    cmd_migrate_db->add_option("--target-db", md_target_db,
        "Path to target Kairos database")->required();
    cmd_migrate_db->add_flag("--dry-run", md_dry_run,
        "Show what would be imported without writing to target");

    // ── service (Windows only, hidden on POSIX) ──────────────────────
    auto* cmd_service = app.add_subcommand("service",
        "Windows Service management (install/uninstall)");
    cmd_service->require_subcommand(1);
#ifndef _WIN32
    cmd_service->group("");  // Hidden on non-Windows.
#endif

    auto* svc_install = cmd_service->add_subcommand("install",
        "Install Kairos as a Windows Service");
    std::string svc_install_config;
    svc_install->add_option("--config", svc_install_config,
        "Config file path for the service");

    auto* svc_uninstall = cmd_service->add_subcommand("uninstall",
        "Uninstall the Kairos Windows Service");

    // ── dashboard (TUI) ──────────────────────────────────────────────
    auto* cmd_dashboard = app.add_subcommand("dashboard",
        "Launch real-time TUI dashboard (requires KAIROS_TUI=ON)");
    std::string dashboard_db;
    int dashboard_refresh = 1000;
    cmd_dashboard->add_option("--db", dashboard_db,
        "Path to Kairos SQLite database");
    cmd_dashboard->add_option("--refresh", dashboard_refresh,
        "Refresh interval in milliseconds (default: 1000)");

    // ── kel (expression evaluator) ──────────────────────────────────
    // Roadmap §7: KEL Interactive REPL
    auto* cmd_kel = app.add_subcommand("kel",
        "KEL expression evaluator (eval one-shot or interactive REPL)");
    cmd_kel->require_subcommand(1);

    auto* kel_eval = cmd_kel->add_subcommand("eval",
        "Evaluate a single KEL expression");
    std::string kel_eval_expr;
    kel_eval->add_option("expression", kel_eval_expr,
        "KEL expression to evaluate")->required();

    auto* kel_repl = cmd_kel->add_subcommand("repl",
        "Start interactive KEL REPL (read-eval-print loop)");

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

        // Add this block inside cmd_start->parsed(), before run_daemon():
    #ifdef _WIN32
            if (as_service) {
                // Windows Service mode — delegate to SCM dispatcher.
                int rc = daemon::run_as_windows_service(cfg);
                observability::shutdown_logging();
                return rc;
            }
    #endif

        int rc = daemon::run_daemon(cfg);
        // Shutdown logging AFTER run_daemon() returns — all its stack
        // locals (including InotifyWatcher whose destructor logs) are
        // now destroyed, so it's safe to tear down spdlog.
        observability::shutdown_logging();
        return rc;
    }

    // ── watches list ──────────────────────────────────────────────
    if (watches_list->parsed()) {
        setup_logging(log_level, json_output, false);

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        auto registry = load_registry_from_yaml(cfg, spdlog::default_logger());
        const auto& groups = registry->watch_groups();

        // Query the database for actual scan state (file counts, last scan).
        // The CLI doesn't connect to the running daemon — it reads from
        // the shared SQLite database (read-only).
        auto db = try_open_db(cfg);

        struct GroupDisplayInfo {
            std::string name;
            std::string mode;
            int watched_paths = 0;
            int files_in_last_sample = 0;
            std::string last_scan_time;
            int event_count = 0;
        };

        std::vector<GroupDisplayInfo> display_groups;
        for (const auto& g : groups) {
            GroupDisplayInfo info;
            info.name = g.group_name;
            switch (g.mode) {
                case watch::WatchMode::Native: info.mode = "native"; break;
                case watch::WatchMode::Sample: info.mode = "sample"; break;
                case watch::WatchMode::Hybrid: info.mode = "hybrid"; break;
            }
            info.watched_paths = static_cast<int>(g.watch_items.size());

            // Query actual file count and last scan from DB.
            if (db) {
                try {
                    // Get file count from the latest sample epoch.
                    persist::QueryReader reader(*db);
                    auto epoch = reader.last_sample_epoch(g.group_name);
                    if (epoch > 0) {
                        auto sample = reader.query_sample(g.group_name, epoch);
                        info.files_in_last_sample = static_cast<int>(sample.size());
                    }
                } catch (...) {}

                try {
                    // Get last scan time from most recent watch event
                    // or watch sample timestamp.
                    SQLite::Statement q(*db,
                        "SELECT MAX(created_at) FROM watch_samples "
                        "WHERE watch_group = ?");
                    q.bind(1, g.group_name);
                    if (q.executeStep() && !q.isColumnNull(0)) {
                        info.last_scan_time = q.getColumn(0).getString();
                    }
                } catch (...) {}

                try {
                    // Event count.
                    SQLite::Statement q(*db,
                        "SELECT COUNT(*) FROM watch_events "
                        "WHERE watch_group = ?");
                    q.bind(1, g.group_name);
                    if (q.executeStep()) {
                        info.event_count = q.getColumn(0).getInt();
                    }
                } catch (...) {}
            }

            display_groups.push_back(std::move(info));
        }

        if (json_output) {
            // §5.1: Compile row filter.
            auto filter = cli::make_filter(watches_filter);
            json arr = json::array();
            for (const auto& s : display_groups) {
                if (filter.active()) {
                    std::vector<std::string> rf = {s.name, s.mode};
                    if (!filter.matches(rf)) continue;
                }
                arr.push_back({
                    {"name", s.name},
                    {"mode", s.mode},
                    {"watched_paths", s.watched_paths},
                    {"files_in_last_sample", s.files_in_last_sample},
                    {"last_scan_time", s.last_scan_time},
                    {"event_count", s.event_count}
                });
            }
            std::cout << json{{"watch_groups", arr}}.dump(2) << "\n";
        } else {
            // §5.1: Compile row filter.
            auto filter = cli::make_filter(watches_filter);
            if (display_groups.empty()) {
                std::cout << "No watch groups configured.\n";
            } else {
                bool use_color = cli::supports_color();
                cli::Table table({"GROUP", "MODE", "PATHS", "FILES",
                                  "EVENTS", "LAST SCAN"});
                std::vector<cli::SelectorItem> select_items;
                int shown = 0;
                for (const auto& s : display_groups) {
                    std::string last_scan = s.last_scan_time.empty()
                        ? "(no scans yet)" : s.last_scan_time;
                    if (last_scan.size() > 19) {
                        last_scan = last_scan.substr(0, 19);
                    }
                    std::vector<std::string> row = {
                        s.name, s.mode,
                        std::to_string(s.watched_paths),
                        std::to_string(s.files_in_last_sample),
                        std::to_string(s.event_count),
                        last_scan
                    };

                    // §5.1: Apply filter.
                    if (!filter.matches(row)) continue;

                    table.add_row(row);
                    shown++;

                    if (watches_interactive) {
                        select_items.push_back({
                            s.name,
                            s.name + "  " + s.mode + "  "
                                + std::to_string(s.event_count) + " events",
                            s.name + " " + s.mode
                        });
                    }
                }

                // §2.1: Interactive selection → watches show.
                if (watches_interactive && !select_items.empty()) {
                    table.render(std::cout, use_color);
                    std::cout << "\n";
                    auto selected = cli::interactive_select(
                        select_items,
                        {.prompt = "Select watch group"});
                    if (selected) {
                        std::cout << "\nSelected: " << *selected
                                  << "\n(use 'kairos watches show "
                                  << *selected << "' for detail)\n";
                    }
                    return 0;
                }

                table.render(std::cout, use_color);
                std::cout << "\n" << shown << " watch group(s)";
                if (filter.active())
                    std::cout << " (filtered from "
                              << display_groups.size() << ")";
                std::cout << "\n";
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
            json events_arr = json::array();
            auto db = try_open_db(cfg);
            if (db) {
                persist::QueryReader reader(*db);
                auto events = reader.query_watch_events(
                    20, found->group_name);
                for (const auto& e : events) {
                    events_arr.push_back({
                        {"rule_name", e.rule_name},
                        {"event_type", e.event_type},
                        {"severity", e.severity},
                        {"created_at", format_display_ts(e.created_at)},
                        {"affected_files", e.affected_files_json}
                    });
                }
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
                {"enabled", found->enabled},
                {"recent_events", events_arr}
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

            // Event history from DB.
            auto db = try_open_db(cfg);
            if (db) {
                persist::QueryReader reader(*db);
                auto events = reader.query_watch_events(
                    10, found->group_name);
                if (!events.empty()) {
                    bool use_color = cli::supports_color();
                    std::cout << "\nRecent Events (" << events.size()
                              << "):\n\n";
                    cli::Table et({"RULE", "TYPE", "SEVERITY",
                                    "WHEN"});
                    for (const auto& e : events) {
                        std::string sev = e.severity;
                        if (use_color) {
                            if (sev == "critical" || sev == "error")
                                sev = cli::colorize(sev, cli::ansi::red,
                                                     true);
                            else if (sev == "warning")
                                sev = cli::colorize(sev,
                                    cli::ansi::yellow, true);
                        }
                        et.add_row({
                            e.rule_name,
                            e.event_type,
                            sev,
                            format_relative_ts(e.created_at)
                        });
                    }
                    et.render(std::cout, use_color);
                } else {
                    std::cout << "\nRecent Events: (none)\n";
                }
            }
            std::cout << "\n";
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

        // Two-pass scan:
        //   Pass 1 → baseline (populates last_sample in the engine)
        //   Pass 2 → diff against baseline
        // Without two passes, last_sample is empty and diff is always
        // skipped (EventWatcher parity: first scan = baseline only).

        // Helper lambda for printing one group result.
        auto print_group_result = [&](
            const std::string& gname,
            const watch::ScanResult& baseline,
            const watch::ScanResult& result)
        {
            if (json_output) {
                json triggered = json::array();
                for (const auto& t : result.triggered) {
                    json paths = json::array();
                    for (const auto& p : t.affected_paths) paths.push_back(p);
                    triggered.push_back({
                        {"rule_name", t.rule_name},
                        {"event_type", t.event_type},
                        {"affected_paths", paths}
                    });
                }
                json j = {
                    {"watch_group", gname},
                    {"files_monitored",
                     static_cast<int>(baseline.sample.entries.size())},
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
                };
                if (watch_list_files) {
                    json files = json::array();
                    for (const auto& [path, entry] : baseline.sample.entries) {
                        files.push_back({
                            {"path", path},
                            {"size", entry.size},
                            {"type", entry.entry_type},
                        });
                    }
                    j["files"] = std::move(files);
                }
                std::cout << j.dump(2) << "\n";
            } else {
                auto file_count = baseline.sample.entries.size();
                std::cout << fmt::format(
                    "Group: {} — {} files monitored "
                    "(+{} -{} ~{}) in {}ms\n",
                    gname,
                    file_count,
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

                if (watch_list_files) {
                    // Show file inventory grouped by extension.
                    std::map<std::string, int> ext_counts;
                    for (const auto& [path, entry] : baseline.sample.entries) {
                        auto dot = path.rfind('.');
                        std::string ext = (dot != std::string::npos)
                            ? path.substr(dot) : "(no ext)";
                        ext_counts[ext]++;
                    }
                    std::cout << "  Files by type:\n";
                    for (const auto& [ext, count] : ext_counts) {
                        std::cout << "    " << ext << ": " << count << "\n";
                    }
                    std::cout << "  File list:\n";
                    for (const auto& [path, entry] : baseline.sample.entries) {
                        std::cout << "    " << path;
                        if (entry.entry_type == "directory") {
                            std::cout << "  (dir)";
                        } else {
                            std::cout << "  (" << entry.size << " bytes)";
                        }
                        std::cout << "\n";
                    }
                }
            }
        };

        if (watch_scan_group.empty()) {
            // Scan all groups (two-pass).
            auto baseline_results = engine.scan_once(null_sink);
            auto diff_results = engine.scan_once(null_sink);

            auto groups = registry->watch_groups();
            for (size_t i = 0; i < diff_results.size(); ++i) {
                std::string gname = i < groups.size()
                    ? groups[i].group_name : "(unknown)";
                print_group_result(gname,
                    i < baseline_results.size()
                        ? baseline_results[i] : diff_results[i],
                    diff_results[i]);
            }
            if (diff_results.empty()) {
                std::cout << "No watch groups configured.\n";
            }
        } else {
            // Scan a specific group (two-pass).
            try {
                auto baseline = engine.scan_group(watch_scan_group, null_sink);
                auto result = engine.scan_group(watch_scan_group, null_sink);
                print_group_result(watch_scan_group, baseline, result);
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

            // §5.1: Compile row filter.
            auto filter = cli::make_filter(events_filter);

            if (json_output) {
                json arr = json::array();
                for (const auto& e : events) {
                    if (filter.active()) {
                        std::vector<std::string> rf = {
                            e.watch_group, e.rule_name,
                            e.event_type, e.severity};
                        if (!filter.matches(rf)) continue;
                    }
                    arr.push_back({
                        {"event_uid", e.event_uid},
                        {"watch_group", e.watch_group},
                        {"rule_name", e.rule_name},
                        {"event_type", e.event_type},
                        {"severity", e.severity},
                        {"affected_files", e.affected_files_json},
                        {"sample_epoch", e.sample_epoch},
                        {"created_at", format_display_ts(e.created_at)}
                    });
                }
                std::cout << json{
                    {"events", arr},
                    {"count", static_cast<int>(arr.size())}
                }.dump(2) << "\n";
            } else {
                if (events.empty()) {
                    std::cout << "No events found.\n";
                    if (!events_group.empty()) {
                        std::cout << "  (filtered by group: "
                                  << events_group << ")\n";
                    }
                } else {
                    bool use_color = cli::supports_color();
                    cli::Table table({"GROUP", "RULE", "TYPE",
                                      "SEVERITY", "FILES", "CREATED"});
                    if (events_no_header) table.set_show_header(false);
                    std::vector<cli::SelectorItem> select_items;
                    int shown = 0;
                    for (const auto& e : events) {
                        std::string sev = e.severity;
                        if (use_color) {
                            if (sev == "critical") {
                                sev = cli::colorize(
                                    sev, cli::ansi::red, true);
                            } else if (sev == "warning") {
                                sev = cli::colorize(
                                    sev, cli::ansi::yellow, true);
                            } else {
                                sev = cli::colorize(
                                    sev, cli::ansi::green, true);
                            }
                        }
                        std::string ts = e.created_at.size() > 19
                            ? e.created_at.substr(0, 19) : e.created_at;
                        std::string files = summarize_affected_files(
                            e.affected_files_json, events_verbose);

                        std::vector<std::string> row = {
                            e.watch_group, e.rule_name,
                            e.event_type, sev, files, ts
                        };

                        // §5.1: Apply filter.
                        if (!filter.matches(row)) continue;

                        table.add_row(row);
                        shown++;

                        if (events_interactive) {
                            select_items.push_back({
                                e.event_uid,
                                e.watch_group + "  " + e.rule_name
                                    + "  " + e.event_type,
                                e.watch_group + " " + e.rule_name
                                    + " " + e.event_type + " "
                                    + e.severity
                            });
                        }
                    }

                    // §2.1: Interactive selection — show detail.
                    if (events_interactive && !select_items.empty()) {
                        table.render(std::cout, use_color);
                        std::cout << "\n";
                        auto selected = cli::interactive_select(
                            select_items,
                            {.prompt = "Select event"});
                        if (selected) {
                            // Show full event detail.
                            for (const auto& e : events) {
                                if (e.event_uid == *selected) {
                                    std::cout << "\nEvent: "
                                              << e.event_uid << "\n"
                                              << "Group: "
                                              << e.watch_group << "\n"
                                              << "Rule: "
                                              << e.rule_name << "\n"
                                              << "Type: "
                                              << e.event_type << "\n"
                                              << "Severity: "
                                              << e.severity << "\n"
                                              << "Created: "
                                              << e.created_at << "\n"
                                              << "Affected files: "
                                              << e.affected_files_json
                                              << "\n";
                                    break;
                                }
                            }
                        }
                        return 0;
                    }

                    table.render(std::cout, use_color);
                    std::cout << "\n" << shown << " event(s)";
                    if (filter.active())
                        std::cout << " (filtered from "
                                  << events.size() << ")";
                    std::cout << "\n";
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
        nlohmann::json health_json;
        std::unique_ptr<SQLite::Database> health_db;
        std::unique_ptr<persist::QueryReader> health_reader;
        try {
            auto db = persist::open_database(cfg->db_path);
            persist::QueryReader reader(*db);
            stats = reader.query_run_stats();
            db_size = persist::QueryReader::query_db_size(cfg->db_path);
            if (show_health) {
                // Keep a reader alive for the health computation.
                health_db = std::move(db);
                health_reader = std::make_unique<persist::QueryReader>(
                    *health_db);
                auto hcfg = observability::parse_health_config(
                    cfg->global);
                health_json = observability::compute_health(
                    hcfg, cfg->db_path, health_reader.get(), -1);
            }
        } catch (...) {
            // DB may not exist — show zeros.
        }

        // Load registry for watch group count.
        auto registry = load_registry_from_yaml(
            cfg, spdlog::default_logger());

        if (json_output) {
            json result = {
                {"version", std::string(kairos::kVersion)},
                {"daemon_running", daemon_running},
                {"daemon_pid", daemon_pid},
                {"config_path", cfg->config_file_path.string()},
                {"data_dir", cfg->data_dir.string()},
                {"db_size_bytes", db_size},
                {"workflows", static_cast<int>(registry->workflow_count())},
                {"standalone_jobs", static_cast<int>(registry->standalone_job_count())},
                {"triggers", static_cast<int>(registry->trigger_count())},
                {"watch_groups", static_cast<int>(registry->watch_group_count())},
                {"total_runs", stats.total_runs},
                {"runs_today", stats.runs_today},
                {"failures_today", stats.failures_today},
                {"active_runs", stats.active_runs},
            };

            // Include metrics history if --history (§23.4).
            if (show_history) {
                json metrics = json::array();
                try {
                    auto db = persist::open_database(cfg->db_path);
                    persist::QueryReader reader(*db);
                    auto snapshots = reader.query_metrics_snapshots(20);
                    for (const auto& s : snapshots) {
                        metrics.push_back({
                            {"metric_name", s.metric_name},
                            {"metric_type", s.metric_type},
                            {"value", s.value},
                            {"labels", s.labels_json},
                            {"recorded_at", s.created_at}
                        });
                    }
                } catch (...) {}
                result["metrics_history"] = metrics;
            }

            // §15.5 health snapshot with --health.
            if (show_health && !health_json.is_null()) {
                result["health"] = health_json;
            }

            std::cout << result.dump(2) << "\n";
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
                "  Jobs:         {}    Triggers:     {}\n",
                registry->standalone_job_count(),
                registry->trigger_count());
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

            // §15.5 health checks with --health.
            if (show_health) {
                if (health_json.is_null()) {
                    std::cout << "  Health: unavailable (no database)"
                              << "\n\n";
                } else {
                    bool degraded =
                        health_json["status"] == "degraded";
                    std::cout << "  Health: "
                              << (degraded ? "DEGRADED" : "OK") << "\n";
                    for (const auto& check :
                         health_json["checks"]) {
                        std::cout << "    "
                                  << (check["ok"].get<bool>()
                                          ? "[ok]" : "[!!]")
                                  << " " << check["name"].get<std::string>()
                                  << " — "
                                  << check["detail"].get<std::string>()
                                  << "\n";
                    }
                    std::cout << "\n";
                }
            }

            // Display metric trends if --history is set (§23.4).
            if (show_history) {
                try {
                    auto db = persist::open_database(cfg->db_path);
                    persist::QueryReader reader(*db);
                    auto snapshots = reader.query_metrics_snapshots(20);

                    if (snapshots.empty()) {
                        std::cout << "  No metrics snapshots available.\n"
                                  << "  (Metrics are snapshoted by the "
                                  << "running daemon every 60s.)\n\n";
                    } else {
                        bool use_color = cli::supports_color();
                        std::cout << "  Metrics History (latest snapshots):\n\n";

                        // Group by metric name and show latest value.
                        std::unordered_map<std::string,
                            std::vector<const persist::QueryReader::MetricsSnapshotRow*>
                        > by_name;
                        for (const auto& s : snapshots) {
                            by_name[s.metric_name].push_back(&s);
                        }

                        cli::Table table({"METRIC", "TYPE", "VALUE", "RECORDED AT"});
                        // Show only the most recent value per metric.
                        for (const auto& [name, rows] : by_name) {
                            if (!rows.empty()) {
                                const auto* latest = rows.front();
                                std::string val_str;
                                if (latest->metric_type == "counter") {
                                    val_str = fmt::format("{:.0f}",
                                                          latest->value);
                                } else {
                                    val_str = fmt::format("{:.2f}",
                                                          latest->value);
                                }
                                std::string ts = latest->created_at.size() > 19
                                    ? latest->created_at.substr(0, 19)
                                    : latest->created_at;
                                table.add_row({
                                    name, latest->metric_type,
                                    val_str, ts
                                });
                            }
                        }
                        table.render(std::cout, use_color);
                        std::cout << "\n";
                    }
                } catch (const std::exception& e) {
                    std::cout << "  (Could not query metrics history: "
                              << e.what() << ")\n\n";
                }
            }
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

    // ── secrets (§12.2.1) ────────────────────────────────────────
    if (cmd_secrets->parsed()) {
        setup_logging(log_level, json_output, false);
        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        std::filesystem::path file = !secrets_file_override.empty()
            ? std::filesystem::path(secrets_file_override)
            : cfg->data_dir / security::NativeSecretFile::kDefaultName;

        // Password resolution: --password > env > prompt.
        auto resolve_password = [&](bool confirm) -> std::string {
            if (!secrets_password.empty()) return secrets_password;
            auto env_pw = platform::get_env("KAIROS_SECRETS_PASSWORD");
            if (env_pw && !env_pw->empty()) return *env_pw;
            std::cout << (confirm ? "New master password: "
                                  : "Master password: ");
            std::string pw;
            std::getline(std::cin, pw);
            if (confirm) {
                std::cout << "Confirm password: ";
                std::string pw2;
                std::getline(std::cin, pw2);
                if (pw != pw2) {
                    throw std::runtime_error("passwords do not match");
                }
            }
            return pw;
        };

        try {
            if (secrets_init->parsed()) {
                if (security::NativeSecretFile::exists(file)) {
                    std::cerr << "Secrets file already exists: "
                              << file.string() << "\n";
                    return 1;
                }
                auto pw = resolve_password(true);
                security::NativeSecretFile::save(file, pw, {});
                std::cout << "Initialized " << file.string() << "\n";
            } else if (secrets_set->parsed()) {
                auto pw = resolve_password(false);
                auto secrets = security::NativeSecretFile::exists(file)
                    ? security::NativeSecretFile::load(file, pw)
                    : std::unordered_map<std::string, std::string>{};
                std::string value = secrets_set_value;
                if (value.empty()) {
                    std::cout << "Value for '" << secrets_set_key
                              << "': ";
                    std::getline(std::cin, value);
                }
                secrets[secrets_set_key] = value;
                security::NativeSecretFile::save(file, pw, secrets);
                std::cout << "Set '" << secrets_set_key << "' ("
                          << secrets.size() << " secret(s))\n";
            } else if (secrets_remove->parsed()) {
                auto pw = resolve_password(false);
                auto secrets = security::NativeSecretFile::load(file, pw);
                auto it = secrets.find(secrets_rm_key);
                if (it == secrets.end()) {
                    if (!secrets_rm_force) {
                        std::cerr << "Key not found: "
                                  << secrets_rm_key << "\n";
                        return static_cast<int>(ExitCode::kNotFound);
                    }
                } else {
                    secrets.erase(it);
                    security::NativeSecretFile::save(file, pw, secrets);
                }
                std::cout << "Removed '" << secrets_rm_key << "' ("
                          << secrets.size() << " secret(s))\n";
            } else if (secrets_list->parsed()) {
                auto pw = resolve_password(false);
                auto secrets = security::NativeSecretFile::load(file, pw);
                if (json_output) {
                    json arr = json::array();
                    for (const auto& [k, _] : secrets) arr.push_back(k);
                    std::cout << arr.dump(2) << "\n";
                } else {
                    if (secrets.empty()) {
                        std::cout << "(no secrets)\n";
                    }
                    for (const auto& [k, _] : secrets) {
                        std::cout << k << "\n";
                    }
                }
            } else if (secrets_rotate->parsed()) {
                if (!security::NativeSecretFile::exists(file)) {
                    std::cerr << "Secrets file not found: "
                              << file.string() << "\n";
                    return static_cast<int>(ExitCode::kNotFound);
                }
                std::string old_pw;
                std::cout << "Current master password: ";
                std::getline(std::cin, old_pw);
                auto secrets =
                    security::NativeSecretFile::load(file, old_pw);
                auto new_pw = resolve_password(true);
                security::NativeSecretFile::save(file, new_pw, secrets);
                std::cout << "Password rotated ("
                          << secrets.size() << " secret(s))\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    // ── export (§15.9) ───────────────────────────────────────────
    if (cmd_export->parsed()) {
        setup_logging(log_level, json_output, false);
        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);
        try {
            auto db = persist::open_database(cfg->db_path);
            persist::QueryReader reader(*db);

            json bundle;
            bundle["format"] = "kairos-export";
            bundle["version"] = 1;

            json runs_arr = json::array();
            int64_t log_count = 0;
            for (auto& er : reader.export_runs(export_since)) {
                json rj;
                rj["run_id"] = er.run.run_id;
                rj["target_type"] = er.run.target_type;
                rj["target_id"] = er.run.target_id;
                rj["target_name"] = er.run.target_name;
                rj["trigger_type"] = er.run.trigger_type;
                rj["status"] = er.run.status;
                rj["exit_code"] = er.run.exit_code;
                rj["start_ts"] = er.run.start_ts;
                rj["end_ts"] = er.run.end_ts;
                rj["duration_ms"] = er.run.duration_ms;
                rj["tags"] = er.run.tags_json;
                json jobs_arr = json::array();
                for (const auto& jd : er.jobs) {
                    json jj;
                    jj["job_id"] = jd.job_id;
                    jj["job_name"] = jd.job_name;
                    jj["status"] = jd.status;
                    jj["exit_code"] = jd.exit_code;
                    jj["start_ts"] = jd.start_ts;
                    jj["end_ts"] = jd.end_ts;
                    jj["duration_ms"] = jd.duration_ms;
                    json steps_arr = json::array();
                    for (const auto& st : jd.steps) {
                        steps_arr.push_back({
                            {"step_id", st.step_id},
                            {"step_name", st.step_name},
                            {"status", st.status},
                            {"exit_code", st.exit_code},
                            {"command", st.command},
                            {"duration_ms", st.duration_ms},
                        });
                    }
                    jj["steps"] = std::move(steps_arr);
                    jobs_arr.push_back(std::move(jj));
                }
                rj["jobs"] = std::move(jobs_arr);
                json log_arr = json::array();
                for (const auto& lc : er.logs) {
                    log_arr.push_back({
                        {"job_id", lc.job_id},
                        {"step_id", lc.step_id},
                        {"stream", lc.stream},
                        {"content", lc.content},
                    });
                    ++log_count;
                }
                rj["logs"] = std::move(log_arr);
                runs_arr.push_back(std::move(rj));
            }
            bundle["runs"] = std::move(runs_arr);

            json ev_arr = json::array();
            for (const auto& e : reader.export_events(export_since)) {
                ev_arr.push_back({
                    {"watch_group", e.watch_group},
                    {"rule_name", e.rule_name},
                    {"event_type", e.event_type},
                    {"severity", e.severity},
                    {"affected_files", e.affected_files_json},
                    {"created_at", e.created_at},
                });
            }
            bundle["events"] = std::move(ev_arr);

            json audit_arr = json::array();
            for (const auto& a : reader.export_audit()) {
                audit_arr.push_back({
                    {"timestamp", a.timestamp},
                    {"actor", a.actor},
                    {"action", a.action},
                    {"target", a.target},
                    {"detail", a.detail_json},
                });
            }
            bundle["audit"] = std::move(audit_arr);

            bundle["counts"] = {
                {"runs", bundle["runs"].size()},
                {"events", bundle["events"].size()},
                {"audit", bundle["audit"].size()},
                {"log_chunks", log_count},
            };

            std::ofstream out(export_output, std::ios::trunc);
            if (!out) {
                std::cerr << "Cannot write " << export_output << "\n";
                return 1;
            }
            out << bundle.dump(2) << "\n";
            std::cout << "Exported to " << export_output << ": "
                      << bundle["counts"].dump() << "\n";
        } catch (const std::exception& e) {
            std::cerr << "Export failed: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    // ── import (§15.9) ───────────────────────────────────────────
    if (cmd_import->parsed()) {
        setup_logging(log_level, json_output, false);
        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);
        try {
            std::ifstream in(import_file);
            if (!in) {
                std::cerr << "Cannot read " << import_file << "\n";
                return 1;
            }
            auto bundle = json::parse(in);
            if (bundle.value("format", "") != "kairos-export") {
                std::cerr << "Not a kairos export bundle\n";
                return 1;
            }

            auto db = persist::open_database(cfg->db_path);
            int imported = 0, skipped = 0, log_count = 0;
            int events = 0;

            SQLite::Statement ins_run(*db,
                "INSERT OR IGNORE INTO runs (run_id, target_type, "
                "target_id, target_name, trigger_type, trigger_id, "
                "correlation_id, status, exit_code, start_ts, end_ts, "
                "duration_ms, tags_json) VALUES "
                "(?,?,?,?,?,'imported','',?,?,?,?,?,?)");
            SQLite::Statement ins_job(*db,
                "INSERT OR IGNORE INTO job_runs (run_id, job_id, "
                "job_name, status, exit_code, start_ts, end_ts, "
                "duration_ms) VALUES (?,?,?,?,?,?,?,?)");
            SQLite::Statement ins_step(*db,
                "INSERT OR IGNORE INTO step_runs (run_id, job_id, "
                "step_id, step_name, status, exit_code, command, "
                "duration_ms) VALUES (?,?,?,?,?,?,?,?)");
            SQLite::Statement ins_log(*db,
                "INSERT INTO log_chunks (run_id, job_id, step_id, "
                "stream, chunk_index, content) VALUES "
                "(?,?,?,?, (SELECT COALESCE(MAX(chunk_index),-1)+1 "
                "FROM log_chunks WHERE run_id = ?), ?)");
            SQLite::Statement ins_event(*db,
                "INSERT INTO watch_events (watch_group, event_type, "
                "file_path, rule_name, details_json, action_taken) "
                "VALUES (?,?,?,?,?,?)");

            db->exec("BEGIN");
            for (const auto& r :
                 bundle.value("runs", json::array())) {
                ins_run.reset();
                ins_run.bind(1, r.value("run_id", ""));
                ins_run.bind(2, r.value("target_type", "workflow"));
                ins_run.bind(3, r.value("target_id", ""));
                ins_run.bind(4, r.value("target_name", ""));
                ins_run.bind(5, r.value("trigger_type", "import"));
                ins_run.bind(6, r.value("status", ""));
                ins_run.bind(7, r.value("exit_code", 0));
                ins_run.bind(8, r.value("start_ts", ""));
                ins_run.bind(9, r.value("end_ts", ""));
                ins_run.bind(10, r.value("duration_ms", 0));
                ins_run.bind(11, r.value("tags", "[]"));
                if (ins_run.exec() > 0) ++imported; else ++skipped;

                for (const auto& j :
                     r.value("jobs", json::array())) {
                    ins_job.reset();
                    ins_job.bind(1, r.value("run_id", ""));
                    ins_job.bind(2, j.value("job_id", ""));
                    ins_job.bind(3, j.value("job_name", ""));
                    ins_job.bind(4, j.value("status", ""));
                    ins_job.bind(5, j.value("exit_code", 0));
                    ins_job.bind(6, j.value("start_ts", ""));
                    ins_job.bind(7, j.value("end_ts", ""));
                    ins_job.bind(8, j.value("duration_ms", 0));
                    ins_job.exec();

                    for (const auto& st :
                         j.value("steps", json::array())) {
                        ins_step.reset();
                        ins_step.bind(1, r.value("run_id", ""));
                        ins_step.bind(2, j.value("job_id", ""));
                        ins_step.bind(3, st.value("step_id", ""));
                        ins_step.bind(4, st.value("step_name", ""));
                        ins_step.bind(5, st.value("status", ""));
                        ins_step.bind(6, st.value("exit_code", 0));
                        ins_step.bind(7, st.value("command", ""));
                        ins_step.bind(8,
                            st.value("duration_ms", 0));
                        ins_step.exec();
                    }
                }

                for (const auto& lc :
                     r.value("logs", json::array())) {
                    ins_log.reset();
                    ins_log.bind(1, r.value("run_id", ""));
                    ins_log.bind(2, lc.value("job_id", ""));
                    ins_log.bind(3, lc.value("step_id", ""));
                    ins_log.bind(4, lc.value("stream", "stdout"));
                    ins_log.bind(5, r.value("run_id", ""));
                    ins_log.bind(6, lc.value("content", ""));
                    ins_log.exec();
                    ++log_count;
                }
            }

            for (const auto& e :
                 bundle.value("events", json::array())) {
                ins_event.reset();
                ins_event.bind(1, e.value("watch_group", ""));
                ins_event.bind(2, e.value("event_type", ""));
                ins_event.bind(3, e.value("affected_files", "[]"));
                ins_event.bind(4, e.value("rule_name", ""));
                ins_event.bind(5, e.value("details", "{}"));
                ins_event.bind(6, e.value("severity", "info"));
                ins_event.exec();
                ++events;
            }
            db->exec("COMMIT");

            std::cout << "Imported runs=" << imported
                      << " skipped=" << skipped
                      << " log_chunks=" << log_count
                      << " events=" << events << "\n";
        } catch (const std::exception& e) {
            std::cerr << "Import failed: " << e.what() << "\n";
            return 1;
        }
        return 0;
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

    // ── runs list ────────────────────────────────────────────────
    if (runs_list->parsed()) {
        setup_logging(log_level, json_output, false);

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        try {
            auto db = persist::open_database(cfg->db_path);
            persist::QueryReader reader(*db);

            auto runs = reader.query_recent_runs(
                runs_limit, runs_status, runs_workflow, runs_since);

            // §5.1: Compile row filter.
            auto filter = cli::make_filter(runs_filter);

            if (json_output) {
                json arr = json::array();
                for (const auto& r : runs) {
                    if (runs_only_pinned && !r.pinned) continue;
                    // Apply filter to JSON rows too.
                    if (filter.active()) {
                        std::vector<std::string> row_fields = {
                            r.run_id, r.target_name, r.status,
                            r.trigger_type};
                        if (!filter.matches(row_fields)) continue;
                    }
                    arr.push_back({
                        {"run_id", r.run_id},
                        {"target_type", r.target_type},
                        {"target_name", r.target_name},
                        {"trigger_type", r.trigger_type},
                        {"status", r.status},
                        {"exit_code", r.exit_code},
                        {"started_at", format_display_ts(r.start_ts)},
                        {"finished_at", format_display_ts(r.end_ts)},
                        {"duration_ms", r.duration_ms},
                        {"pinned", r.pinned}
                    });
                }
                std::cout << json(arr).dump(2) << "\n";
            } else {
                if (runs.empty()) {
                    std::cout << "No runs found.\n";
                } else {
                    // Human-friendly table output (§23.7).
                    bool use_color = cli::supports_color();

                    cli::Table table({
                        "RUN ID", "TARGET", "STATUS",
                        "TRIGGER", "STARTED", "DURATION"});
                    if (runs_no_header) table.set_show_header(false);

                    // §2.1: Collect items for interactive selection.
                    std::vector<cli::SelectorItem> select_items;
                    int shown = 0;

                    for (const auto& r : runs) {
                        if (runs_only_pinned && !r.pinned) continue;
                        std::string status_str =
                            cli::status_icon(r.status, use_color) + " " +
                            cli::colorize_status(r.status, use_color);
                        std::string display_id = (runs_full_id
                            ? r.run_id
                            : cli::truncate(r.run_id, 12))
                            + (r.pinned ? " \xF0\x9F\x93\x8C" : "");

                        std::vector<std::string> row = {
                            display_id,
                            cli::truncate(r.target_name, 20),
                            status_str,
                            r.trigger_type,
                            format_display_ts(r.start_ts),
                            cli::format_duration(r.duration_ms)
                        };

                        // §5.1: Apply filter.
                        if (!filter.matches(row)) continue;

                        table.add_row(row);
                        shown++;

                        if (runs_interactive) {
                            select_items.push_back({
                                r.run_id,
                                display_id + "  " + r.target_name
                                    + "  " + r.status,
                                r.run_id + " " + r.target_name
                                    + " " + r.status + " "
                                    + r.trigger_type
                            });
                        }
                    }

                    // §2.1: Interactive selection mode.
                    if (runs_interactive && !select_items.empty()) {
                        table.render(std::cout, use_color);
                        std::cout << "\n";
                        auto selected = cli::interactive_select(
                            select_items,
                            {.prompt = "Select run"});
                        if (selected) {
                            // Re-invoke as "runs show <id>".
                            std::cout << "\n";
                            auto detail = reader.get_run_detail(*selected);
                            if (detail) {
                                std::cout << "Run: " << detail->run.run_id
                                          << "\n";
                                std::cout << "Target: "
                                          << detail->run.target_name << "\n";
                                std::cout << "Status: "
                                          << detail->run.status << "\n";
                                std::cout << "Started: "
                                          << format_display_ts(
                                                 detail->run.start_ts) << "\n";
                                std::cout << "Duration: "
                                          << cli::format_duration(
                                                 detail->run.duration_ms)
                                          << "\n";
                                if (!detail->jobs.empty()) {
                                    std::cout << "\nJobs:\n";
                                    for (const auto& j : detail->jobs) {
                                        std::cout << "  "
                                            << cli::status_icon(
                                                   j.status, use_color)
                                            << " " << j.job_name
                                            << " (" << j.status << ")\n";
                                    }
                                }
                            }
                        }
                        return 0;
                    }

                    table.render(std::cout, use_color);
                    std::cout << "\n" << shown << " run(s)";
                    if (filter.active())
                        std::cout << " (filtered from "
                                  << runs.size() << ")";
                    std::cout << "\n";
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Failed to query runs: " << e.what()
                      << "\nRun 'kairos init-db' first if the "
                      << "database does not exist.\n";
            return 1;
        }
        return 0;
    }

    // ── runs show ────────────────────────────────────────────────
    if (runs_show->parsed()) {
        setup_logging(log_level, json_output, false);

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        try {
            auto db = persist::open_database(cfg->db_path);
            persist::QueryReader reader(*db);

            // Resolve prefix (§1.1).
            auto resolved = resolve_prefix_or_error(
                reader, runs_show_id, json_output);
            if (resolved.empty())
                return static_cast<int>(ExitCode::kNotFound);

            auto detail = reader.get_run_detail(resolved);
            if (!detail) {
                if (json_output) {
                    std::cout << json{
                        {"error", "Run not found"},
                        {"run_id", resolved}
                    }.dump(2) << "\n";
                } else {
                    std::cerr << "Run '" << resolved
                              << "' not found.\n";
                }
                return static_cast<int>(ExitCode::kNotFound);
            }

            if (json_output) {
                json jobs_arr = json::array();
                for (const auto& j : detail->jobs) {
                    json steps_arr = json::array();
                    for (const auto& s : j.steps) {
                        steps_arr.push_back({
                            {"step_id", s.step_id},
                            {"step_name", s.step_name},
                            {"status", s.status},
                            {"exit_code", s.exit_code},
                            {"started_at", format_display_ts(s.start_ts)},
                            {"finished_at", format_display_ts(s.end_ts)},
                            {"duration_ms", s.duration_ms},
                            {"command", s.command}
                        });
                    }
                    jobs_arr.push_back({
                        {"job_id", j.job_id},
                        {"job_name", j.job_name},
                        {"status", j.status},
                        {"exit_code", j.exit_code},
                        {"started_at", format_display_ts(j.start_ts)},
                        {"finished_at", format_display_ts(j.end_ts)},
                        {"duration_ms", j.duration_ms},
                        {"condition_result", j.condition_result},
                        {"steps", steps_arr}
                    });
                }
                std::cout << json{
                    {"run_id", detail->run.run_id},
                    {"target_type", detail->run.target_type},
                    {"target_name", detail->run.target_name},
                    {"trigger_type", detail->run.trigger_type},
                    {"status", detail->run.status},
                    {"exit_code", detail->run.exit_code},
                    {"started_at", format_display_ts(detail->run.start_ts)},
                    {"finished_at", format_display_ts(detail->run.end_ts)},
                    {"duration_ms", detail->run.duration_ms},
                    {"jobs", jobs_arr}
                }.dump(2) << "\n";
            } else {
                bool use_color = cli::supports_color();

                const auto& r = detail->run;
                std::cout << "\n  Run: " << r.run_id << "\n";
                std::cout << "  Workflow: " << r.target_name
                          << " (" << r.target_id << ")\n";
                std::cout << "  Status: "
                          << cli::status_icon(r.status, use_color) << " "
                          << cli::colorize_status(r.status, use_color)
                          << "  Exit: " << r.exit_code << "\n";
                std::cout << "  Trigger: " << r.trigger_type << "\n";
                std::cout << "  Started: " << format_display_ts(r.start_ts) << "\n";
                if (!r.end_ts.empty()) {
                    std::cout << "  Finished: " << format_display_ts(r.end_ts)
                              << "  Duration: "
                              << cli::format_duration(r.duration_ms) << "\n";
                }

                if (!detail->jobs.empty()) {
                    std::cout << "\n  Jobs (" << detail->jobs.size()
                              << "):\n";
                    for (const auto& j : detail->jobs) {
                        std::string indicator =
                            cli::status_icon(j.status, use_color);
                        std::cout << "    " << indicator << " "
                                  << j.job_name << " \xe2\x80\x94 "
                                  << cli::colorize_status(j.status, use_color)
                                  << " ("
                                  << cli::format_duration(j.duration_ms)
                                  << ")\n";

                        for (const auto& s : j.steps) {
                            std::string s_ind =
                                cli::status_icon(s.status, use_color);
                            std::cout << "        " << s_ind << " "
                                      << s.step_name << " \xe2\x80\x94 "
                                      << cli::colorize_status(s.status, use_color)
                                      << " (exit " << s.exit_code << ", "
                                      << cli::format_duration(s.duration_ms)
                                      << ")\n";
                        }
                    }
                }
                std::cout << "\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "Failed to get run detail: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    // ── runs cancel ──────────────────────────────────────────────
    // ── runs pin/unpin (§15.6) ───────────────────────────────────
    if (runs_pin->parsed() || runs_unpin->parsed()) {
        setup_logging(log_level, json_output, false);
        bool pin = runs_pin->parsed();
        auto& id_ref = pin ? runs_pin_id : runs_unpin_id;

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        try {
            auto db = persist::open_database(cfg->db_path);
            persist::QueryReader reader(*db);

            auto resolved = resolve_prefix_or_error(
                reader, id_ref, json_output);
            if (resolved.empty())
                return static_cast<int>(ExitCode::kNotFound);
            id_ref = resolved;

            // Direct UPDATE — pinning is DB metadata, safe alongside
            // the daemon under WAL.
            SQLite::Statement up(*db,
                "UPDATE runs SET pinned = ? WHERE run_id = ?");
            up.bind(1, pin ? 1 : 0);
            up.bind(2, id_ref);
            int changed = up.exec();
            if (changed == 0) {
                if (json_output) {
                    std::cout << json{{"error", "Run not found"},
                                      {"run_id", id_ref}}.dump(2)
                              << "\n";
                } else {
                    std::cerr << "Run '" << id_ref
                              << "' not found.\n";
                }
                return static_cast<int>(ExitCode::kNotFound);
            }

            if (json_output) {
                std::cout << json{{"run_id", id_ref},
                                  {"pinned", pin}}.dump(2)
                          << "\n";
            } else {
                std::cout << (pin ? "Pinned " : "Unpinned ")
                          << id_ref << "\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    if (runs_cancel->parsed()) {
        setup_logging(log_level, json_output, false);

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        try {
            auto db = persist::open_database(cfg->db_path);
            persist::QueryReader reader(*db);

            // Resolve prefix (§1.1).
            auto resolved = resolve_prefix_or_error(
                reader, run_cancel_id, json_output);
            if (resolved.empty())
                return static_cast<int>(ExitCode::kNotFound);
            run_cancel_id = resolved;

            // Verify the run exists and is actually running.
            auto run_opt = reader.get_run_summary(run_cancel_id);
            if (!run_opt) {
                if (json_output) {
                    std::cout << json{
                        {"success", false},
                        {"error", "Run not found"},
                        {"run_id", run_cancel_id}
                    }.dump(2) << "\n";
                } else {
                    std::cerr << "Run '" << run_cancel_id
                              << "' not found.\n";
                }
                return static_cast<int>(ExitCode::kNotFound);
            }

            if (run_opt->status != "RUNNING" &&
                run_opt->status != "PENDING") {
                if (json_output) {
                    std::cout << json{
                        {"success", false},
                        {"error", "Run is not active"},
                        {"run_id", run_cancel_id},
                        {"status", run_opt->status}
                    }.dump(2) << "\n";
                } else {
                    std::cerr << "Run '" << run_cancel_id
                              << "' is not active (status: "
                              << run_opt->status << ").\n";
                }
                return 1;
            }

            // Update run status to CANCELLED directly in SQLite.
            // This provides immediate feedback in `runs list/show`.
            // The daemon pipeline will detect the cancellation on
            // next status check and terminate running processes.
            {
                SQLite::Statement stmt(*db,
                    "UPDATE runs SET status = 'CANCELLED', "
                    "end_ts = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
                    "WHERE run_id = ? AND status IN ('RUNNING', 'PENDING')");
                stmt.bind(1, run_cancel_id);
                int updated = stmt.exec();

                if (updated == 0) {
                    if (json_output) {
                        std::cout << json{
                            {"success", false},
                            {"error", "Run was already completed"},
                            {"run_id", run_cancel_id}
                        }.dump(2) << "\n";
                    } else {
                        std::cerr << "Run already completed.\n";
                    }
                    return 1;
                }
            }

            // Also mark any RUNNING/PENDING jobs as CANCELLED.
            {
                SQLite::Statement stmt(*db,
                    "UPDATE job_runs SET status = 'CANCELLED', "
                    "end_ts = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
                    "WHERE run_id = ? AND status IN ('RUNNING', 'PENDING')");
                stmt.bind(1, run_cancel_id);
                stmt.exec();
            }

            // Write a cancel command file for the daemon to pick up.
            // The daemon can poll data_dir/commands/ for cancel requests
            // and terminate running processes (Phase 4 integration).
            auto cmd_dir = cfg->data_dir / "commands";
            std::error_code ec;
            fs::create_directories(cmd_dir, ec);
            if (!ec) {
                auto cmd_file = cmd_dir / ("cancel_" + run_cancel_id);
                std::ofstream ofs(cmd_file);
                if (ofs.is_open()) {
                    ofs << run_cancel_id << "\n";
                }
            }

            if (json_output) {
                std::cout << json{
                    {"success", true},
                    {"run_id", run_cancel_id},
                    {"status", "CANCELLED"},
                    {"note", "Run marked as cancelled. Running processes "
                             "may take a moment to terminate."}
                }.dump(2) << "\n";
            } else {
                std::cout << "Run '" << run_cancel_id
                          << "' cancelled.\n";
                std::cout << "  Note: Running processes may take a moment "
                          << "to terminate.\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "Failed to cancel run: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    // ── logs ─────────────────────────────────────────────────────
    if (cmd_logs->parsed()) {
        setup_logging(log_level, json_output, false);

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        try {
            auto db = persist::open_database(cfg->db_path);
            persist::QueryReader reader(*db);

            // Resolve prefix (§1.1).
            auto resolved = resolve_prefix_or_error(
                reader, logs_run_id, json_output);
            if (resolved.empty())
                return static_cast<int>(ExitCode::kNotFound);
            logs_run_id = resolved;

            // Verify run exists.
            auto run_opt = reader.get_run_summary(logs_run_id);
            if (!run_opt) {
                if (json_output) {
                    std::cout << json{
                        {"error", "Run not found"},
                        {"run_id", logs_run_id}
                    }.dump(2) << "\n";
                } else {
                    std::cerr << "Run '" << logs_run_id
                              << "' not found.\n";
                }
                return static_cast<int>(ExitCode::kNotFound);
            }

            int64_t cursor = 0;

            // Fetch and display log chunks.
            auto render_chunk = [&json_output](
                const persist::QueryReader::LogChunk& chunk)
            {
                if (json_output) {
                    std::cout << json{
                        {"id", chunk.id},
                        {"job_id", chunk.job_id},
                        {"step_id", chunk.step_id},
                        {"stream", chunk.stream},
                        {"content", chunk.content},
                        {"timestamp", chunk.created_at}
                    }.dump() << "\n";
                } else {
                    // Human output: prefix with stream indicator.
                    std::string prefix =
                        chunk.stream == "stderr" ? "ERR| " : "   | ";
                    std::cout << prefix << chunk.content;
                    // Add newline if content doesn't end with one.
                    if (!chunk.content.empty() &&
                        chunk.content.back() != '\n') {
                        std::cout << "\n";
                    }
                }
            };

            // Initial fetch.
            auto chunks = reader.get_log_chunks(logs_run_id, cursor);
            for (const auto& c : chunks) {
                render_chunk(c);
                cursor = std::max(cursor, c.id);
            }

            if (!logs_follow) {
                if (chunks.empty() && !json_output) {
                    std::cout << "(no log output for run "
                              << logs_run_id << ")\n";
                }
                return 0;
            }

            // Follow mode: poll at 200ms until run completes (§23.8).
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));

                auto new_chunks = reader.get_log_chunks(
                    logs_run_id, cursor, 100);
                for (const auto& c : new_chunks) {
                    render_chunk(c);
                    cursor = std::max(cursor, c.id);
                }

                // Check if run is still active.
                auto current = reader.get_run_summary(logs_run_id);
                if (current &&
                    current->status != "RUNNING" &&
                    current->status != "PENDING") {
                    // Final drain.
                    auto final_chunks = reader.get_log_chunks(
                        logs_run_id, cursor, 1000);
                    for (const auto& c : final_chunks) {
                        render_chunk(c);
                    }
                    if (!json_output) {
                        std::cout << "\n--- Run " << current->status
                                  << " (exit " << current->exit_code
                                  << ") ---\n";
                    }
                    break;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Failed to get logs: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    // ── triggers list ────────────────────────────────────────────
    if (triggers_list->parsed()) {
        setup_logging(log_level, json_output, false);
        bool use_color = !json_output && cli::supports_color();

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        auto registry = load_registry_from_yaml(
            cfg, spdlog::default_logger());

        const auto& triggers = registry->triggers();

        // Open DB for last fire info.
        auto db = try_open_db(cfg);
        std::unique_ptr<persist::QueryReader> reader;
        if (db) reader = std::make_unique<persist::QueryReader>(*db);

        if (json_output) {
            auto filter = cli::make_filter(triggers_filter);
            json arr = json::array();
            for (const auto& t : triggers) {
                if (filter.active()) {
                    std::vector<std::string> rf = {
                        t.trigger_id, t.target_name,
                        engine::trigger_spec_type_name(t.spec)};
                    if (!filter.matches(rf)) continue;
                }
                auto nf = compute_next_fire(t);
                auto lr = last_run_for_target(reader.get(), t.target_name);
                arr.push_back({
                    {"trigger_id", t.trigger_id},
                    {"type", engine::trigger_spec_type_name(t.spec)},
                    {"target_name", t.target_name},
                    {"target_id", t.target_id},
                    {"schedule", nf.schedule_expr},
                    {"next_fire", format_display_ts(nf.iso)},
                    {"enabled", t.enabled},
                    {"max_instances", t.max_instances},
                    {"last_run_status", lr.status}
                });
            }
            std::cout << json(arr).dump(2) << "\n";
        } else {
            auto filter = cli::make_filter(triggers_filter);
            if (triggers.empty()) {
                std::cout << "No triggers configured.\n";
            } else {
                cli::Table table({"TRIGGER", "TYPE", "TARGET",
                                  "SCHEDULE", "NEXT FIRE", "LAST RUN"});
                std::vector<cli::SelectorItem> select_items;
                int shown = 0;
                for (const auto& t : triggers) {
                    auto nf = compute_next_fire(t);
                    auto lr = last_run_for_target(reader.get(), t.target_name);
                    std::string status_str = lr.status;
                    if (use_color && lr.status != "--") {
                        status_str = cli::colorize_status(lr.status, true);
                    }
                    std::vector<std::string> row = {
                        cli::truncate(t.trigger_id, 16),
                        engine::trigger_spec_type_name(t.spec),
                        t.target_name,
                        nf.schedule_expr,
                        nf.relative,
                        status_str
                    };

                    if (!filter.matches(row)) continue;
                    table.add_row(row);
                    shown++;

                    if (triggers_interactive) {
                        select_items.push_back({
                            t.trigger_id,
                            t.target_name + "  "
                                + engine::trigger_spec_type_name(t.spec)
                                + "  " + nf.schedule_expr,
                            t.trigger_id + " " + t.target_name
                                + " " + nf.schedule_expr
                        });
                    }
                }

                if (triggers_interactive && !select_items.empty()) {
                    table.render(std::cout, use_color);
                    std::cout << "\n";
                    auto selected = cli::interactive_select(
                        select_items,
                        {.prompt = "Select trigger"});
                    if (selected) {
                        std::cout << "\nSelected trigger: " << *selected
                                  << "\n";
                    }
                    return 0;
                }

                table.render(std::cout, use_color);
                std::cout << "\n" << shown << " trigger(s)";
                if (filter.active())
                    std::cout << " (filtered from "
                              << triggers.size() << ")";
                std::cout << "\n";
            }
        }
        return 0;
    }

    // ── triggers next ────────────────────────────────────────────
    if (triggers_next->parsed()) {
        setup_logging(log_level, json_output, false);
        bool use_color = !json_output && cli::supports_color();

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        auto registry = load_registry_from_yaml(
            cfg, spdlog::default_logger());

        // Compute next fire for each trigger and sort by soonest.
        struct UpcomingTrigger {
            const engine::TimerEntry* entry;
            NextFireInfo nf;
            std::chrono::system_clock::time_point fire_tp;
        };
        std::vector<UpcomingTrigger> upcoming;
        auto now = std::chrono::system_clock::now();

        for (const auto& t : registry->triggers()) {
            if (!t.enabled) continue;
            auto nf = compute_next_fire(t);
            if (nf.iso == "--") continue;
            auto tp = parse_iso8601(nf.iso);
            upcoming.push_back({&t, std::move(nf), tp});
        }

        std::sort(upcoming.begin(), upcoming.end(),
            [](const auto& a, const auto& b) {
                return a.fire_tp < b.fire_tp;
            });

        int limit = std::min(triggers_next_n,
                             static_cast<int>(upcoming.size()));

        if (json_output) {
            json arr = json::array();
            for (int i = 0; i < limit; ++i) {
                const auto& u = upcoming[i];
                arr.push_back({
                    {"trigger_id", u.entry->trigger_id},
                    {"type", engine::trigger_spec_type_name(u.entry->spec)},
                    {"target_name", u.entry->target_name},
                    {"schedule", u.nf.schedule_expr},
                    {"next_fire", format_display_ts(u.nf.iso)},
                    {"fires_in", u.nf.relative}
                });
            }
            std::cout << json(arr).dump(2) << "\n";
        } else {
            if (upcoming.empty()) {
                std::cout << "No upcoming triggers.\n";
            } else {
                cli::Table table({"#", "FIRES IN", "TARGET",
                                  "TRIGGER", "SCHEDULE"});
                for (int i = 0; i < limit; ++i) {
                    const auto& u = upcoming[i];
                    table.add_row({
                        std::to_string(i + 1),
                        u.nf.relative,
                        u.entry->target_name,
                        cli::truncate(u.entry->trigger_id, 16),
                        u.nf.schedule_expr
                    });
                }
                table.render(std::cout, use_color);
                if (static_cast<int>(upcoming.size()) > limit) {
                    std::cout << "\n  (" << (upcoming.size() - limit)
                              << " more triggers not shown)\n";
                }
            }
        }
        return 0;
    }

    // ── triggers preview (§15.8) ─────────────────────────────────
    if (triggers_preview->parsed()) {
        setup_logging(log_level, json_output, false);

        try {
            // Build a CronTrigger from the user-provided expression.
            engine::CronTrigger cron;
            cron.expression = cron_expr;

            // Try to load timezone delta from config (best-effort).
            auto cfg = load_config_or_die(config_path, {});
            if (cfg) {
                cron.cron_tz_delta_s = platform::cron_tz_delta_seconds(
                    g_tz_config);
            }

            auto now = std::chrono::system_clock::now();
            auto cursor = now;

            int count = std::clamp(cron_count, 1, 100);

            if (json_output) {
                json arr = json::array();
                for (int i = 0; i < count; ++i) {
                    auto next = cron.next_fire_after(cursor);
                    // Safety: if next_fire_after returns the same or
                    // earlier time, we're stuck. Break to avoid infinite loop.
                    if (next <= cursor) break;

                    auto tt = std::chrono::system_clock::to_time_t(next);
                    char buf[64];
                    struct tm tm_buf{};
#ifdef _WIN32
                    localtime_s(&tm_buf, &tt);
#else
                    localtime_r(&tt, &tm_buf);
#endif
                    std::strftime(buf, sizeof(buf),
                                  "%Y-%m-%dT%H:%M:%S", &tm_buf);

                    auto delta = std::chrono::duration_cast<
                        std::chrono::seconds>(next - now);

                    arr.push_back({
                        {"index", i + 1},
                        {"fire_at", std::string(buf)},
                        {"seconds_from_now", delta.count()}
                    });
                    cursor = next;
                }
                std::cout << json{
                    {"expression", cron_expr},
                    {"count", static_cast<int>(arr.size())},
                    {"fire_times", arr}
                }.dump(2) << "\n";
            } else {
                std::cout << "Cron expression: " << cron_expr
                          << "\nNext " << count << " fire times:\n\n";

                cli::Table table({"#", "FIRE AT", "FROM NOW"});

                for (int i = 0; i < count; ++i) {
                    auto next = cron.next_fire_after(cursor);
                    if (next <= cursor) {
                        std::cerr << "  (no more fire times)\n";
                        break;
                    }

                    auto tt = std::chrono::system_clock::to_time_t(next);
                    char buf[64];
                    struct tm tm_buf{};
#ifdef _WIN32
                    localtime_s(&tm_buf, &tt);
#else
                    localtime_r(&tt, &tm_buf);
#endif
                    std::strftime(buf, sizeof(buf),
                                  "%Y-%m-%d %H:%M:%S", &tm_buf);

                    std::string relative = format_relative(next);

                    table.add_row({
                        std::to_string(i + 1),
                        std::string(buf),
                        relative
                    });
                    cursor = next;
                }
                table.render(std::cout, cli::supports_color());
            }
        } catch (const std::exception& e) {
            if (json_output) {
                std::cout << json{
                    {"error", "Invalid cron expression"},
                    {"expression", cron_expr},
                    {"detail", e.what()}
                }.dump(2) << "\n";
            } else {
                std::cerr << "Error: invalid cron expression '"
                          << cron_expr << "'\n  " << e.what() << "\n";
            }
            return 1;
        }
        return 0;
    }

    // ── workflows list ───────────────────────────────────────────
    if (workflows_list->parsed()) {
        setup_logging(log_level, json_output, false);

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        auto registry = load_registry_from_yaml(
            cfg, spdlog::default_logger());

        // Open DB for run history (best-effort).
        auto db = try_open_db(cfg);
        std::unique_ptr<persist::QueryReader> reader;
        if (db) reader = std::make_unique<persist::QueryReader>(*db);

        const auto& workflows = registry->workflows();

        // §5.1: Compile row filter.
        auto filter = cli::make_filter(workflows_filter);

        if (json_output) {
            json arr = json::array();
            for (const auto* wf : workflows) {
                if (filter.active()) {
                    std::vector<std::string> rf = {
                        wf->workflow_name, wf->workflow_id};
                    if (!filter.matches(rf)) continue;
                }
                json jobs_arr = json::array();
                for (const auto& j : wf->jobs) {
                    jobs_arr.push_back(j.job_name);
                }
                auto lr = last_run_for_target(
                    reader.get(), wf->workflow_name);
                auto trigs = triggers_for_target(
                    *registry, wf->workflow_id, wf->workflow_name);
                std::string next_fire = "--";
                if (!trigs.empty()) {
                    next_fire = format_display_ts(compute_next_fire(*trigs[0]).iso);
                }
                arr.push_back({
                    {"id", wf->workflow_id},
                    {"name", wf->workflow_name},
                    {"job_count", static_cast<int>(wf->jobs.size())},
                    {"jobs", jobs_arr},
                    {"last_run_status", lr.status},
                    {"last_run_when", lr.when},
                    {"next_fire", next_fire},
                    {"trigger_count", static_cast<int>(trigs.size())}
                });
            }
            std::cout << json(arr).dump(2) << "\n";
        } else {
            if (workflows.empty()) {
                std::cout << "No workflows configured.\n";
            } else {
                bool use_color = cli::supports_color();
                cli::Table t({"WORKFLOW", "JOBS", "LAST RUN",
                               "WHEN", "NEXT FIRE"});
                std::vector<cli::SelectorItem> select_items;
                int shown = 0;
                for (const auto* wf : workflows) {
                    auto lr = last_run_for_target(
                        reader.get(), wf->workflow_name);
                    auto trigs = triggers_for_target(
                        *registry, wf->workflow_id, wf->workflow_name);
                    std::string next_fire = "--";
                    if (!trigs.empty()) {
                        next_fire = compute_next_fire(*trigs[0]).relative;
                    }
                    std::string status_str = lr.status;
                    if (use_color && lr.status != "--") {
                        status_str = cli::status_icon(lr.status, true)
                            + " " + cli::colorize_status(lr.status, true);
                    }

                    std::vector<std::string> row = {
                        use_color ? cli::colorize(wf->workflow_name,
                                                  cli::ansi::bold, true)
                                  : wf->workflow_name,
                        std::to_string(wf->jobs.size()),
                        status_str,
                        lr.when,
                        next_fire
                    };

                    // §5.1: Apply filter.
                    if (!filter.matches(row)) continue;

                    t.add_row(row);
                    shown++;

                    if (workflows_interactive) {
                        select_items.push_back({
                            wf->workflow_name,
                            wf->workflow_name + "  ("
                                + std::to_string(wf->jobs.size())
                                + " jobs)  " + lr.status,
                            wf->workflow_name + " "
                                + wf->workflow_id + " " + lr.status
                        });
                    }
                }

                // §2.1: Interactive selection → workflows show.
                if (workflows_interactive && !select_items.empty()) {
                    t.render(std::cout, use_color);
                    std::cout << "\n";
                    auto selected = cli::interactive_select(
                        select_items,
                        {.prompt = "Select workflow"});
                    if (selected) {
                        std::cout << "\nSelected: " << *selected
                                  << "\n(use 'kairos workflows show "
                                  << *selected << "' for detail)\n";
                    }
                    return 0;
                }

                t.render(std::cout, use_color);
                std::cout << "\n" << shown << " workflow(s)";
                if (filter.active())
                    std::cout << " (filtered from "
                              << workflows.size() << ")";
                std::cout << "\n";
            }
        }
        return 0;
    }

    // ── workflows show ──────────────────────────────────────────
    if (workflows_show->parsed()) {
        setup_logging(log_level, json_output, false);
        bool use_color = !json_output && cli::supports_color();

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        auto registry = load_registry_from_yaml(
            cfg, spdlog::default_logger());

        // Find by ID or name.
        const engine::WorkflowDef* found_wf = nullptr;
        for (const auto* wf : registry->workflows()) {
            if (wf->workflow_id == workflows_show_id ||
                wf->workflow_name == workflows_show_id) {
                found_wf = wf;
                break;
            }
        }

        if (!found_wf) {
            if (json_output) {
                std::cout << json{
                    {"error", "Workflow not found"},
                    {"id", workflows_show_id}
                }.dump(2) << "\n";
            } else {
                std::cerr << "Workflow '" << workflows_show_id
                          << "' not found.\n";
            }
            return static_cast<int>(ExitCode::kNotFound);
        }

        // Enrich with DB + trigger data.
        auto db = try_open_db(cfg);
        std::unique_ptr<persist::QueryReader> reader;
        if (db) reader = std::make_unique<persist::QueryReader>(*db);

        auto trigs = triggers_for_target(
            *registry, found_wf->workflow_id, found_wf->workflow_name);
        std::vector<persist::QueryReader::RunSummary> recent_runs;
        if (reader) {
            recent_runs = reader->query_recent_runs(
                10, "", found_wf->workflow_name);
        }

        if (json_output) {
            json jobs_arr = json::array();
            for (const auto& j : found_wf->jobs) {
                json steps_arr = json::array();
                for (const auto& s : j.steps) {
                    steps_arr.push_back({
                        {"step_id", s.step_id},
                        {"step_name", s.step_name},
                        {"command", s.command},
                        {"working_dir", s.working_dir.string()},
                        {"use_shell", s.use_shell}
                    });
                }
                json needs_arr = json::array();
                for (const auto& n : j.needs) {
                    needs_arr.push_back(n);
                }
                jobs_arr.push_back({
                    {"job_id", j.job_id},
                    {"job_name", j.job_name},
                    {"needs", needs_arr},
                    {"condition", j.condition_expr.value_or("")},
                    {"continue_on_error", j.continue_on_error},
                    {"steps", steps_arr}
                });
            }
            json dag_levels = json::array();
            for (int lvl = 0; lvl < found_wf->dag.level_count(); ++lvl) {
                json level_arr = json::array();
                for (const auto& job_id : found_wf->dag.jobs_at_level(lvl)) {
                    level_arr.push_back(job_id);
                }
                dag_levels.push_back(level_arr);
            }
            json trigs_arr = json::array();
            for (const auto* t : trigs) {
                auto nf = compute_next_fire(*t);
                trigs_arr.push_back({
                    {"trigger_id", t->trigger_id},
                    {"type", engine::trigger_spec_type_name(t->spec)},
                    {"schedule", nf.schedule_expr},
                    {"next_fire", format_display_ts(nf.iso)}
                });
            }
            json runs_arr = json::array();
            for (const auto& r : recent_runs) {
                runs_arr.push_back({
                    {"run_id", r.run_id},
                    {"status", r.status},
                    {"started_at", format_display_ts(r.start_ts)},
                    {"duration_ms", r.duration_ms}
                });
            }
            std::cout << json{
                {"workflow_id", found_wf->workflow_id},
                {"workflow_name", found_wf->workflow_name},
                {"job_count", static_cast<int>(found_wf->jobs.size())},
                {"dag_levels", dag_levels},
                {"jobs", jobs_arr},
                {"triggers", trigs_arr},
                {"recent_runs", runs_arr}
            }.dump(2) << "\n";
        } else {
            // Human-friendly display.
            std::cout << "\n  Workflow: " << found_wf->workflow_name << "\n";
            std::cout << "  ID:       " << found_wf->workflow_id << "\n";
            std::cout << "  Jobs:     " << found_wf->jobs.size() << "\n";
            std::cout << "  DAG:      " << found_wf->dag.level_count()
                      << " level(s)\n";

            // Triggers section.
            if (!trigs.empty()) {
                std::cout << "\n  Triggers:\n";
                for (const auto* t : trigs) {
                    auto nf = compute_next_fire(*t);
                    std::cout << "    " << engine::trigger_spec_type_name(
                                     t->spec)
                              << ": " << nf.schedule_expr
                              << "  (next: " << nf.relative << ")\n";
                }
            } else {
                std::cout << "  Triggers: (none — manual run only)\n";
            }
            std::cout << "\n";

            // DAG visualization
            std::cout << "  Execution Order (DAG):\n";
            for (int lvl = 0; lvl < found_wf->dag.level_count(); ++lvl) {
                std::string arrow = (lvl > 0) ? "  \xe2\x86\x93\n" : "";
                std::cout << arrow;
                std::cout << "  Level " << lvl << ": ";
                const auto& job_ids = found_wf->dag.jobs_at_level(lvl);
                for (size_t i = 0; i < job_ids.size(); ++i) {
                    if (i > 0) std::cout << ", ";
                    try {
                        const auto& dn = found_wf->dag.node(job_ids[i]);
                        std::cout << dn.job_name;
                    } catch (...) {
                        std::cout << job_ids[i];
                    }
                }
                std::cout << "\n";
            }

            // Job detail table
            std::cout << "\n  Jobs:\n\n";
            for (const auto& j : found_wf->jobs) {
                std::string name = use_color
                    ? cli::colorize(j.job_name, cli::ansi::bold, true)
                    : j.job_name;
                std::cout << "    " << name
                          << "  (" << j.job_id << ")\n";

                if (!j.needs.empty()) {
                    std::cout << "      needs: ";
                    for (size_t i = 0; i < j.needs.size(); ++i) {
                        if (i > 0) std::cout << ", ";
                        std::cout << j.needs[i];
                    }
                    std::cout << "\n";
                }
                if (j.condition_expr) {
                    std::cout << "      condition: " << *j.condition_expr
                              << "\n";
                }
                if (j.continue_on_error) {
                    std::cout << "      continue_on_error: true\n";
                }
                std::cout << "      steps (" << j.steps.size() << "):\n";
                for (size_t si = 0; si < j.steps.size(); ++si) {
                    const auto& s = j.steps[si];
                    std::string cmd_display = cli::truncate(s.command, 60);
                    std::cout << "        " << (si + 1) << ". "
                              << s.step_name << ": " << cmd_display
                              << "\n";
                }
                std::cout << "\n";
            }

            // Recent runs section.
            if (!recent_runs.empty()) {
                std::cout << "  Recent Runs:\n\n";
                cli::Table rt({"RUN ID", "STATUS", "WHEN",
                                "DURATION"});
                for (const auto& r : recent_runs) {
                    std::string status_str = r.status;
                    if (use_color) {
                        status_str = cli::status_icon(r.status, true)
                            + " " + cli::colorize_status(r.status, true);
                    }
                    rt.add_row({
                        cli::truncate(r.run_id, 16),
                        status_str,
                        format_relative_ts(r.start_ts),
                        cli::format_duration(r.duration_ms)
                    });
                }
                std::cout << "  ";
                rt.render(std::cout, use_color);
            } else {
                std::cout << "  Recent Runs: (no runs recorded)\n";
            }
            std::cout << "\n";
        }
        return 0;
    }

    // ── workflows run ────────────────────────────────────────────
    if (workflows_run->parsed()) {
        setup_logging(log_level, json_output, false);

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        auto registry = load_registry_from_yaml(
            cfg, spdlog::default_logger());

        // Find the workflow by ID or name.
        const engine::WorkflowDef* found_wf = nullptr;
        for (const auto* wf : registry->workflows()) {
            if (wf->workflow_id == workflows_run_id ||
                wf->workflow_name == workflows_run_id) {
                found_wf = wf;
                break;
            }
        }

        if (!found_wf) {
            if (json_output) {
                std::cout << json{
                    {"error", "Workflow not found"},
                    {"id", workflows_run_id}
                }.dump(2) << "\n";
            } else {
                std::cerr << "Workflow '" << workflows_run_id
                          << "' not found.\n";
            }
            return static_cast<int>(ExitCode::kNotFound);
        }

        // Open the database.
        std::unique_ptr<SQLite::Database> db;
        try {
            db = persist::open_database(cfg->db_path);
        } catch (const std::exception& e) {
            std::cerr << "Failed to open database: " << e.what()
                      << "\nRun 'kairos init-db' first.\n";
            return 1;
        }

        // Create standalone pipeline dependencies (§23.10).
        SystemClockSource clock;
        persist::DBWriterConfig db_writer_cfg;
        persist::DBWriter db_writer(*db, db_writer_cfg);

        std::stop_source stop_source;
        auto stop_token = stop_source.get_token();

        db_writer.start(stop_token);

        exec::RunnerPoolConfig pool_cfg{
            .worker_count = 4,
            .queue_capacity = 256,
        };
        exec::RunnerPool runner_pool(pool_cfg);
        runner_pool.set_process_handle_factory([](const exec::ProcessSpec&) {
            return exec::create_process_handle();
        });
        runner_pool.start(stop_token);

        engine::TriggerBus trigger_bus(64);
        persist::QueryReader query_reader(*db);
        engine::ActiveRunTracker active_runs;

        engine::PipelineConfig pipeline_cfg;
        engine::Pipeline pipeline(pipeline_cfg, engine::Pipeline::Dependencies{
            .clock = &clock,
            .trigger_bus = &trigger_bus,
            .runner_pool = &runner_pool,
            .registry = registry,
            .active_runs = &active_runs,
            .db_writer = &db_writer,
            .query_reader = &query_reader,
            .data_dir = cfg->data_dir,
            .notification_process_factory = []() {
                return exec::create_process_handle();
            },
        });

        // Create a manual trigger event for this workflow.
        auto correlation_id = core::generate_correlation_id();
        auto event = engine::TriggerEvent::make_manual_run(
            found_wf->workflow_id,
            engine::TriggerEvent::TargetKind::Workflow,
            correlation_id, "cli");
        db_writer.enqueue(persist::InsertAuditEntry{
            .actor = "cli",
            .action = "run_workflow",
            .target = found_wf->workflow_id,
            .detail_json = "{}",
        });
        if (workflows_run_dry_run) {
            auto* payload = std::get_if<engine::ManualRunPayload>(
                &event.payload);
            if (payload) payload->dry_run = true;
            std::cout << "Dry-run: no commands will be executed\n";
        }

        // Execute synchronously via the pipeline.
        auto status = pipeline.process_event(event, stop_token);

        // Get the run ID from the database (most recent run
        // with this correlation ID).
        std::string run_id;
        try {
            SQLite::Statement q(*db,
                "SELECT run_id FROM runs WHERE correlation_id = ? "
                "ORDER BY start_ts DESC LIMIT 1");
            q.bind(1, correlation_id);
            if (q.executeStep()) {
                run_id = q.getColumn(0).getString();
            }
        } catch (...) {}

        // Flush DB writer to persist results.
        stop_source.request_stop();
        db_writer.flush();
        runner_pool.shutdown();

        // If --follow was requested, display log output (§23.8).
        // In standalone mode the pipeline ran synchronously, so
        // all log chunks are already persisted. Display them now.
        if (workflows_run_follow && !run_id.empty()) {
            auto chunks = query_reader.get_log_chunks(run_id, 0, 10000);
            for (const auto& c : chunks) {
                if (json_output) {
                    std::cout << json{
                        {"id", c.id},
                        {"job_id", c.job_id},
                        {"step_id", c.step_id},
                        {"stream", c.stream},
                        {"content", c.content},
                        {"timestamp", c.created_at}
                    }.dump() << "\n";
                } else {
                    std::string prefix =
                        c.stream == "stderr" ? "ERR| " : "   | ";
                    std::cout << prefix << c.content;
                    if (!c.content.empty() && c.content.back() != '\n') {
                        std::cout << "\n";
                    }
                }
            }
            if (!json_output && chunks.empty()) {
                std::cout << "(no log output)\n";
            }
        }

        if (json_output) {
            std::cout << json{
                {"run_id", run_id},
                {"status", std::string(engine::run_status_to_string(status))},
                {"workflow", found_wf->workflow_name}
            }.dump(2) << "\n";
        } else {
            std::string status_str(engine::run_status_to_string(status));
            std::string indicator =
                status == engine::RunStatus::Success ? "[ok]" :
                status == engine::RunStatus::Failure ? "[!!]" : "[--]";

            std::cout << indicator << " Workflow '" << found_wf->workflow_name
                      << "' completed: " << status_str << "\n";
            if (!run_id.empty()) {
                std::cout << "  Run ID: " << run_id << "\n";
                std::cout << "  View details: kairos runs show "
                          << run_id << "\n";
                std::cout << "  View logs: kairos logs "
                          << run_id << "\n";
            }
        }

        return status == engine::RunStatus::Success ? 0 : 1;
    }

    // ── workflows explain ────────────────────────────────────────
    if (workflows_explain->parsed()) {
        setup_logging(log_level, json_output, false);
        bool use_color = !json_output && cli::supports_color();

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        auto registry = load_registry_from_yaml(
            cfg, spdlog::default_logger());

        // Find the workflow by ID or name.
        const engine::WorkflowDef* found_wf = nullptr;
        for (const auto* wf : registry->workflows()) {
            if (wf->workflow_id == workflows_explain_id ||
                wf->workflow_name == workflows_explain_id) {
                found_wf = wf;
                break;
            }
        }

        if (!found_wf) {
            if (json_output) {
                std::cout << json{
                    {"error", "Workflow not found"},
                    {"id", workflows_explain_id}
                }.dump(2) << "\n";
            } else {
                std::cerr << "Workflow '" << workflows_explain_id
                          << "' not found.\n";
            }
            return static_cast<int>(ExitCode::kNotFound);
        }

        // Try to open DB for condition evaluation — if it fails,
        // conditions that query history will be marked PENDING.
        std::unique_ptr<SQLite::Database> db;
        std::unique_ptr<persist::QueryReader> reader;
        try {
            db = persist::open_database(cfg->db_path);
            reader = std::make_unique<persist::QueryReader>(*db);
        } catch (...) {
            // DB may not exist — proceed without history queries.
        }

        // Build execution plan from the DAG (§11.7).
        // For explain, we evaluate conditions against SQLite history
        // but do not execute anything. Shared with HTTP /api/v1/explain
        // and MCP kairos.explainPlan (Roadmap Phase 9).
        engine::ExecutionPlan plan = engine::build_explain_plan(
            *found_wf, reader.get());

        // Output the plan.
        if (json_output) {
            std::cout << plan.render_json() << "\n";
        } else {
            // Rich human-friendly display (§23.4 explain example).
            std::cout << "\n  Execution Plan for: "
                      << found_wf->workflow_name
                      << " (" << found_wf->workflow_id << ")\n";
            std::cout << "  ";
            for (int i = 0; i < 52; ++i) std::cout << "\xe2\x95\x90";
            std::cout << "\n\n";

            std::cout << "  DAG Levels: "
                      << found_wf->dag.level_count() << "\n";
            std::cout << "  Total Jobs: "
                      << found_wf->jobs.size() << "\n\n";

            // Display by level.
            int current_level = -1;
            for (const auto& e : plan.entries) {
                if (e.level != current_level) {
                    current_level = e.level;
                    int jobs_at_level = static_cast<int>(
                        found_wf->dag.jobs_at_level(current_level).size());
                    std::string par = (jobs_at_level > 1)
                        ? " (parallel)" : "";
                    std::cout << "  Level " << current_level
                              << par << ":\n";
                }

                // Action icon.
                std::string icon;
                std::string action_str;
                switch (e.action) {
                    case engine::PlanAction::Run:
                        icon = use_color
                            ? cli::colorize("\xe2\x97\x8f", cli::ansi::green, true)
                            : "[+]";
                        action_str = use_color
                            ? cli::colorize("WOULD RUN", cli::ansi::green, true)
                            : "WOULD RUN";
                        break;
                    case engine::PlanAction::Skip:
                        icon = use_color
                            ? cli::colorize("\xe2\x97\x8b", cli::ansi::yellow, true)
                            : "[-]";
                        action_str = use_color
                            ? cli::colorize("WOULD SKIP", cli::ansi::yellow, true)
                            : "WOULD SKIP";
                        break;
                    case engine::PlanAction::ConditionPending:
                        icon = use_color
                            ? cli::colorize("?", cli::ansi::cyan, true)
                            : "[?]";
                        action_str = use_color
                            ? cli::colorize("PENDING", cli::ansi::cyan, true)
                            : "PENDING";
                        break;
                    case engine::PlanAction::DependencyFailed:
                        icon = use_color
                            ? cli::colorize("\xe2\x9c\x97", cli::ansi::red, true)
                            : "[x]";
                        action_str = use_color
                            ? cli::colorize("DEP FAIL", cli::ansi::red, true)
                            : "DEP FAIL";
                        break;
                    case engine::PlanAction::Disabled:
                        icon = use_color
                            ? cli::colorize("\xe2\x8a\x98", cli::ansi::gray, true)
                            : "[.]";
                        action_str = "DISABLED";
                        break;
                }

                std::cout << "    " << icon << " "
                          << (use_color
                              ? cli::colorize(e.job_name, cli::ansi::bold, true)
                              : e.job_name)
                          << " \xe2\x80\x94 " << action_str;

                if (!e.condition_expr.empty()) {
                    std::cout << "\n      \xe2\x94\x94\xe2\x94\x80 condition: "
                              << e.condition_expr;
                    if (!e.condition_result.empty()) {
                        std::cout << " \xe2\x86\x92 " << e.condition_result;
                    }
                }
                if (!e.needs.empty()) {
                    std::string needs_str;
                    for (size_t i = 0; i < e.needs.size(); ++i) {
                        if (i > 0) needs_str += ", ";
                        try {
                            needs_str += found_wf->dag.node(
                                e.needs[i]).job_name;
                        } catch (...) {
                            needs_str += e.needs[i];
                        }
                    }
                    std::cout << "\n      \xe2\x94\x94\xe2\x94\x80 needs: ["
                              << needs_str << "]";
                }
                std::cout << "\n";
            }

            std::cout << "\n  Summary: "
                      << plan.jobs_to_run() << " to run, "
                      << plan.jobs_to_skip() << " to skip, "
                      << plan.jobs_pending() << " pending\n";
            std::cout << "  Max parallelism: "
                      << plan.max_parallelism() << "\n\n";
        }
        return 0;
    }

    // ── jobs list ────────────────────────────────────────────────
    if (jobs_list->parsed()) {
        setup_logging(log_level, json_output, false);
        bool use_color = !json_output && cli::supports_color();

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        auto registry = load_registry_from_yaml(
            cfg, spdlog::default_logger());

        auto db = try_open_db(cfg);
        std::unique_ptr<persist::QueryReader> reader;
        if (db) reader = std::make_unique<persist::QueryReader>(*db);

        const auto& jobs = registry->standalone_jobs();

        // §5.1: Compile row filter.
        auto filter = cli::make_filter(jobs_filter);

        if (json_output) {
            json arr = json::array();
            for (const auto* j : jobs) {
                if (filter.active()) {
                    std::vector<std::string> rf = {j->job_name, j->job_id};
                    if (!filter.matches(rf)) continue;
                }
                auto lr = last_run_for_target(reader.get(), j->job_name);
                auto trigs = triggers_for_target(
                    *registry, j->job_id, j->job_name);
                std::string next_fire = "--";
                if (!trigs.empty())
                    next_fire = format_display_ts(compute_next_fire(*trigs[0]).iso);
                arr.push_back({
                    {"id", j->job_id},
                    {"name", j->job_name},
                    {"step_count", static_cast<int>(j->steps.size())},
                    {"condition", j->condition_expr.value_or("")},
                    {"last_run_status", lr.status},
                    {"last_run_when", lr.when},
                    {"next_fire", next_fire},
                    {"trigger_count", static_cast<int>(trigs.size())}
                });
            }
            std::cout << json(arr).dump(2) << "\n";
        } else {
            if (jobs.empty()) {
                std::cout << "No standalone jobs configured.\n";
            } else {
                cli::Table table({"JOB", "STEPS", "LAST RUN",
                                   "WHEN", "NEXT FIRE"});
                std::vector<cli::SelectorItem> select_items;
                int shown = 0;
                for (const auto* j : jobs) {
                    auto lr = last_run_for_target(
                        reader.get(), j->job_name);
                    auto trigs = triggers_for_target(
                        *registry, j->job_id, j->job_name);
                    std::string next_fire = "--";
                    if (!trigs.empty())
                        next_fire = compute_next_fire(*trigs[0]).relative;
                    std::string status_str = lr.status;
                    if (use_color && lr.status != "--") {
                        status_str = cli::status_icon(lr.status, true)
                            + " " + cli::colorize_status(lr.status, true);
                    }
                    std::vector<std::string> row = {
                        j->job_name,
                        std::to_string(j->steps.size()),
                        status_str,
                        lr.when,
                        next_fire
                    };

                    if (!filter.matches(row)) continue;
                    table.add_row(row);
                    shown++;

                    if (jobs_interactive) {
                        select_items.push_back({
                            j->job_name,
                            j->job_name + "  ("
                                + std::to_string(j->steps.size())
                                + " steps)  " + lr.status,
                            j->job_name + " " + j->job_id
                                + " " + lr.status
                        });
                    }
                }

                if (jobs_interactive && !select_items.empty()) {
                    table.render(std::cout, use_color);
                    std::cout << "\n";
                    auto selected = cli::interactive_select(
                        select_items,
                        {.prompt = "Select job"});
                    if (selected) {
                        std::cout << "\nSelected: " << *selected
                                  << "\n(use 'kairos jobs show "
                                  << *selected << "' for detail)\n";
                    }
                    return 0;
                }

                table.render(std::cout, use_color);
                std::cout << "\n" << shown << " standalone job(s)";
                if (filter.active())
                    std::cout << " (filtered from "
                              << jobs.size() << ")";
                std::cout << "\n";
            }
        }
        return 0;
    }

    // ── jobs show ────────────────────────────────────────────────
    if (jobs_show->parsed()) {
        setup_logging(log_level, json_output, false);
        bool use_color = !json_output && cli::supports_color();

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        auto registry = load_registry_from_yaml(
            cfg, spdlog::default_logger());

        // Find by ID or name.
        const engine::JobDef* found_job = nullptr;
        for (const auto* j : registry->standalone_jobs()) {
            if (j->job_id == jobs_show_id ||
                j->job_name == jobs_show_id) {
                found_job = j;
                break;
            }
        }

        if (!found_job) {
            if (json_output) {
                std::cout << json{
                    {"error", "Job not found"},
                    {"id", jobs_show_id}
                }.dump(2) << "\n";
            } else {
                std::cerr << "Standalone job '" << jobs_show_id
                          << "' not found.\n";
            }
            return static_cast<int>(ExitCode::kNotFound);
        }

        // Enrich with DB + trigger data.
        auto db = try_open_db(cfg);
        std::unique_ptr<persist::QueryReader> reader;
        if (db) reader = std::make_unique<persist::QueryReader>(*db);

        auto trigs = triggers_for_target(
            *registry, found_job->job_id, found_job->job_name);
        std::vector<persist::QueryReader::RunSummary> recent_runs;
        if (reader) {
            recent_runs = reader->query_recent_runs(
                10, "", found_job->job_name);
        }

        if (json_output) {
            json steps_arr = json::array();
            for (const auto& s : found_job->steps) {
                json env = json::object();
                for (const auto& [k, v] : s.env) {
                    env[k] = v;
                }
                steps_arr.push_back({
                    {"step_id", s.step_id},
                    {"step_name", s.step_name},
                    {"command", s.command},
                    {"working_dir", s.working_dir.string()},
                    {"use_shell", s.use_shell},
                    {"env", env}
                });
            }
            json trigs_arr = json::array();
            for (const auto* t : trigs) {
                auto nf = compute_next_fire(*t);
                trigs_arr.push_back({
                    {"trigger_id", t->trigger_id},
                    {"type", engine::trigger_spec_type_name(t->spec)},
                    {"schedule", nf.schedule_expr},
                    {"next_fire", format_display_ts(nf.iso)}
                });
            }
            json runs_arr = json::array();
            for (const auto& r : recent_runs) {
                runs_arr.push_back({
                    {"run_id", r.run_id},
                    {"status", r.status},
                    {"started_at", format_display_ts(r.start_ts)},
                    {"duration_ms", r.duration_ms}
                });
            }
            std::cout << json{
                {"job_id", found_job->job_id},
                {"job_name", found_job->job_name},
                {"condition", found_job->condition_expr.value_or("")},
                {"continue_on_error", found_job->continue_on_error},
                {"steps", steps_arr},
                {"triggers", trigs_arr},
                {"recent_runs", runs_arr}
            }.dump(2) << "\n";
        } else {
            std::cout << "\n  Job: " << found_job->job_name << "\n";
            std::cout << "  ID:  " << found_job->job_id << "\n";
            if (found_job->condition_expr) {
                std::cout << "  Condition: "
                          << *found_job->condition_expr << "\n";
            }
            if (found_job->continue_on_error) {
                std::cout << "  Continue on error: true\n";
            }

            // Triggers section.
            if (!trigs.empty()) {
                std::cout << "\n  Triggers:\n";
                for (const auto* t : trigs) {
                    auto nf = compute_next_fire(*t);
                    std::cout << "    " << engine::trigger_spec_type_name(
                                     t->spec)
                              << ": " << nf.schedule_expr
                              << "  (next: " << nf.relative << ")\n";
                }
            } else {
                std::cout << "  Triggers: (none — manual run only)\n";
            }

            std::cout << "\n  Steps (" << found_job->steps.size() << "):\n";
            for (size_t si = 0; si < found_job->steps.size(); ++si) {
                const auto& s = found_job->steps[si];
                std::string name = use_color
                    ? cli::colorize(s.step_name, cli::ansi::bold, true)
                    : s.step_name;
                std::cout << "    " << (si + 1) << ". " << name
                          << "\n";
                std::cout << "       cmd: "
                          << cli::truncate(s.command, 60) << "\n";
                if (!s.working_dir.empty()) {
                    std::cout << "       cwd: "
                              << s.working_dir.string() << "\n";
                }
                if (!s.env.empty()) {
                    std::cout << "       env: " << s.env.size()
                              << " var(s)\n";
                }
            }

            // Recent runs section.
            if (!recent_runs.empty()) {
                std::cout << "\n  Recent Runs:\n\n";
                cli::Table rt({"RUN ID", "STATUS", "WHEN",
                                "DURATION"});
                for (const auto& r : recent_runs) {
                    std::string status_str = r.status;
                    if (use_color) {
                        status_str = cli::status_icon(r.status, true)
                            + " " + cli::colorize_status(r.status, true);
                    }
                    rt.add_row({
                        cli::truncate(r.run_id, 16),
                        status_str,
                        format_relative_ts(r.start_ts),
                        cli::format_duration(r.duration_ms)
                    });
                }
                std::cout << "  ";
                rt.render(std::cout, use_color);
            } else {
                std::cout << "\n  Recent Runs: (no runs recorded)\n";
            }
            std::cout << "\n";
        }
        return 0;
    }

    // ── jobs run ─────────────────────────────────────────────────
    if (jobs_run->parsed()) {
        setup_logging(log_level, json_output, false);

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        auto registry = load_registry_from_yaml(
            cfg, spdlog::default_logger());

        // Find standalone job by ID or name.
        const engine::JobDef* found_job = nullptr;
        for (const auto* j : registry->standalone_jobs()) {
            if (j->job_id == jobs_run_id ||
                j->job_name == jobs_run_id) {
                found_job = j;
                break;
            }
        }

        if (!found_job) {
            if (json_output) {
                std::cout << json{
                    {"error", "Job not found"},
                    {"id", jobs_run_id}
                }.dump(2) << "\n";
            } else {
                std::cerr << "Standalone job '" << jobs_run_id
                          << "' not found.\n";
            }
            return static_cast<int>(ExitCode::kNotFound);
        }

        // Open the database.
        std::unique_ptr<SQLite::Database> db;
        try {
            db = persist::open_database(cfg->db_path);
        } catch (const std::exception& e) {
            std::cerr << "Failed to open database: " << e.what()
                      << "\nRun 'kairos init-db' first.\n";
            return 1;
        }

        // Create standalone pipeline dependencies (§23.10).
        SystemClockSource clock;
        persist::DBWriterConfig db_writer_cfg;
        persist::DBWriter db_writer(*db, db_writer_cfg);

        std::stop_source stop_source;
        auto stop_token = stop_source.get_token();

        db_writer.start(stop_token);

        exec::RunnerPoolConfig pool_cfg{
            .worker_count = 4,
            .queue_capacity = 256,
        };
        exec::RunnerPool runner_pool(pool_cfg);
        runner_pool.set_process_handle_factory([](const exec::ProcessSpec&) {
            return exec::create_process_handle();
        });
        runner_pool.start(stop_token);

        engine::TriggerBus trigger_bus(64);
        persist::QueryReader query_reader(*db);
        engine::ActiveRunTracker active_runs;

        engine::PipelineConfig pipeline_cfg;
        engine::Pipeline pipeline(pipeline_cfg, engine::Pipeline::Dependencies{
            .clock = &clock,
            .trigger_bus = &trigger_bus,
            .runner_pool = &runner_pool,
            .registry = registry,
            .active_runs = &active_runs,
            .db_writer = &db_writer,
            .query_reader = &query_reader,
            .data_dir = cfg->data_dir,
            .notification_process_factory = []() {
                return exec::create_process_handle();
            },
        });

        // Create a manual trigger event for this standalone job.
        auto correlation_id = core::generate_correlation_id();
        auto event = engine::TriggerEvent::make_manual_run(
            found_job->job_id,
            engine::TriggerEvent::TargetKind::StandaloneJob,
            correlation_id, "cli");
        db_writer.enqueue(persist::InsertAuditEntry{
            .actor = "cli",
            .action = "run_job",
            .target = found_job->job_id,
            .detail_json = "{}",
        });

        // Execute synchronously via the pipeline.
        auto status = pipeline.process_event(event, stop_token);

        // Retrieve the run ID.
        std::string run_id;
        try {
            SQLite::Statement q(*db,
                "SELECT run_id FROM runs WHERE correlation_id = ? "
                "ORDER BY start_ts DESC LIMIT 1");
            q.bind(1, correlation_id);
            if (q.executeStep()) {
                run_id = q.getColumn(0).getString();
            }
        } catch (...) {}

        // Flush DB writer.
        stop_source.request_stop();
        db_writer.flush();
        runner_pool.shutdown();

        // If --follow, display log output (§23.8).
        if (jobs_run_follow && !run_id.empty()) {
            auto chunks = query_reader.get_log_chunks(run_id, 0, 10000);
            for (const auto& c : chunks) {
                if (json_output) {
                    std::cout << json{
                        {"id", c.id},
                        {"job_id", c.job_id},
                        {"step_id", c.step_id},
                        {"stream", c.stream},
                        {"content", c.content},
                        {"timestamp", c.created_at}
                    }.dump() << "\n";
                } else {
                    std::string prefix =
                        c.stream == "stderr" ? "ERR| " : "   | ";
                    std::cout << prefix << c.content;
                    if (!c.content.empty() && c.content.back() != '\n') {
                        std::cout << "\n";
                    }
                }
            }
            if (!json_output && chunks.empty()) {
                std::cout << "(no log output)\n";
            }
        }

        if (json_output) {
            std::cout << json{
                {"run_id", run_id},
                {"status", std::string(engine::run_status_to_string(status))},
                {"job", found_job->job_name}
            }.dump(2) << "\n";
        } else {
            std::string status_str(engine::run_status_to_string(status));
            std::string indicator =
                status == engine::RunStatus::Success ? "[ok]" :
                status == engine::RunStatus::Failure ? "[!!]" : "[--]";

            std::cout << indicator << " Job '" << found_job->job_name
                      << "' completed: " << status_str << "\n";
            if (!run_id.empty()) {
                std::cout << "  Run ID: " << run_id << "\n";
                std::cout << "  View details: kairos runs show "
                          << run_id << "\n";
                std::cout << "  View logs: kairos logs "
                          << run_id << "\n";
            }
        }

        return status == engine::RunStatus::Success ? 0 : 1;
    }

    // ── stop ─────────────────────────────────────────────────────
    if (cmd_stop->parsed()) {
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
                    std::cerr << "Daemon not running (no PID file at "
                              << lock_path.string() << ")\n";
                }
                return static_cast<int>(ExitCode::kNotRunning);
            }
            std::getline(pf, pid_str);
        }

        if (pid_str.empty()) {
            if (json_output) {
                std::cout << json{
                    {"success", false},
                    {"error", "PID file is empty"}
                }.dump(2) << "\n";
            } else {
                std::cerr << "PID file is empty.\n";
            }
            return static_cast<int>(ExitCode::kNotRunning);
        }

#ifndef _WIN32
        pid_t pid = std::stoi(pid_str);

        // Check if the process is alive.
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
            // Clean up stale PID file.
            std::error_code ec;
            fs::remove(lock_path, ec);
            return static_cast<int>(ExitCode::kNotRunning);
        }

        // Send SIGTERM for graceful shutdown.
        if (::kill(pid, SIGTERM) != 0) {
            int err = errno;
            if (json_output) {
                std::cout << json{
                    {"success", false},
                    {"error", "Failed to send SIGTERM: " +
                              std::string(std::strerror(err))}
                }.dump(2) << "\n";
            } else {
                std::cerr << "Failed to send SIGTERM to PID "
                          << pid_str << ": "
                          << std::strerror(err) << "\n";
            }
            return 1;
        }

        if (json_output) {
            std::cout << json{
                {"success", true},
                {"pid", pid},
                {"signal", "SIGTERM"}
            }.dump(2) << "\n";
        } else {
            std::cout << "Stop signal sent to daemon (PID "
                      << pid_str << ")\n";
            std::cout << "  Waiting for graceful shutdown...\n";
        }

        // Optionally wait for the daemon to exit (up to 10 seconds).
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (::kill(pid, 0) != 0) {
                if (!json_output) {
                    std::cout << "  Daemon stopped.\n";
                }
                return 0;
            }
        }

        if (!json_output) {
            std::cout << "  Daemon still running after 10s. "
                      << "Check manually.\n";
        }
        return 0;
#else
        // Windows: no SIGTERM equivalent via kill().
        // Future: use TerminateProcess or a named event.
        if (json_output) {
            std::cout << json{
                {"success", false},
                {"error", "Stop via signal not supported on Windows. "
                          "Use task manager or the MCP interface."}
            }.dump(2) << "\n";
        } else {
            std::cerr << "Stop via signal is not supported on Windows.\n"
                      << "Use task manager or the MCP interface.\n";
        }
        return 1;
#endif
    }

    // ── prune ────────────────────────────────────────────────────
    if (cmd_prune->parsed()) {
        setup_logging(log_level, json_output, false);

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        std::unique_ptr<SQLite::Database> db;
        try {
            db = persist::open_database(cfg->db_path);
        } catch (const std::exception& e) {
            std::cerr << "Failed to open database: " << e.what()
                      << "\nRun 'kairos init-db' first.\n";
            return 1;
        }

        persist::QueryReader reader(*db);

        // Preview what would be pruned (§16.8).
        auto preview = reader.query_prune_preview(prune_days);

        int64_t total = preview.runs_to_delete +
                        preview.run_jobs_to_delete +
                        preview.run_steps_to_delete +
                        preview.log_chunks_to_delete +
                        preview.watch_events_to_delete +
                        preview.watch_samples_to_delete +
                        preview.metrics_snapshots_to_delete;

        if (json_output) {
            json result = {
                {"older_than_days", prune_days},
                {"dry_run", prune_dry_run},
                {"preview", {
                    {"runs", preview.runs_to_delete},
                    {"run_jobs", preview.run_jobs_to_delete},
                    {"run_steps", preview.run_steps_to_delete},
                    {"log_chunks", preview.log_chunks_to_delete},
                    {"watch_events", preview.watch_events_to_delete},
                    {"watch_samples", preview.watch_samples_to_delete},
                    {"metrics_snapshots", preview.metrics_snapshots_to_delete},
                    {"total_records", total}
                }}
            };

            if (!prune_dry_run && (prune_force || total > 0)) {
                // Execute the prune via direct SQL in a transaction.
                std::string cutoff =
                    "datetime('now', '-" +
                    std::to_string(prune_days) + " days')";

                int total_deleted = 0;
                try {
                    SQLite::Transaction txn(*db);

                    // Log chunks first (FK dependency).
                    SQLite::Statement del_chunks(*db,
                        "DELETE FROM log_chunks WHERE run_id IN "
                        "(SELECT run_id FROM runs WHERE start_ts < " +
                        cutoff + ")");
                    total_deleted += del_chunks.exec();

                    // Run steps.
                    SQLite::Statement del_steps(*db,
                        "DELETE FROM step_runs WHERE run_id IN "
                        "(SELECT run_id FROM runs WHERE start_ts < " +
                        cutoff + ")");
                    total_deleted += del_steps.exec();

                    // Run jobs.
                    SQLite::Statement del_jobs(*db,
                        "DELETE FROM job_runs WHERE run_id IN "
                        "(SELECT run_id FROM runs WHERE start_ts < " +
                        cutoff + ")");
                    total_deleted += del_jobs.exec();

                    // Runs.
                    SQLite::Statement del_runs(*db,
                        "DELETE FROM runs WHERE start_ts < " + cutoff);
                    total_deleted += del_runs.exec();

                    // Watch events.
                    SQLite::Statement del_events(*db,
                        "DELETE FROM watch_events WHERE created_at < " +
                        cutoff);
                    total_deleted += del_events.exec();

                    // Watch samples.
                    SQLite::Statement del_samples(*db,
                        "DELETE FROM watch_samples WHERE collected_at < " +
                        cutoff);
                    total_deleted += del_samples.exec();

                    // Metrics snapshots.
                    SQLite::Statement del_metrics(*db,
                        "DELETE FROM metrics_snapshots WHERE recorded_at < " +
                        cutoff);
                    total_deleted += del_metrics.exec();

                    txn.commit();

                    // WAL checkpoint after pruning.
                    db->exec("PRAGMA wal_checkpoint(TRUNCATE)");
                } catch (const std::exception& e) {
                    result["error"] = e.what();
                }

                result["deleted_total"] = total_deleted;
                result["pruned"] = true;
            }

            std::cout << result.dump(2) << "\n";
        } else {
            // Human-friendly output.
            std::cout << "\n  Prune Preview (records older than "
                      << prune_days << " days):\n\n";

            bool use_color = cli::supports_color();
            cli::Table table({"TABLE", "RECORDS"});
            table.add_row({"Runs", std::to_string(preview.runs_to_delete)});
            table.add_row({"Run Jobs",
                std::to_string(preview.run_jobs_to_delete)});
            table.add_row({"Run Steps",
                std::to_string(preview.run_steps_to_delete)});
            table.add_row({"Log Chunks",
                std::to_string(preview.log_chunks_to_delete)});
            table.add_row({"Watch Events",
                std::to_string(preview.watch_events_to_delete)});
            table.add_row({"Watch Samples",
                std::to_string(preview.watch_samples_to_delete)});
            table.add_row({"Metrics Snapshots",
                std::to_string(preview.metrics_snapshots_to_delete)});
            table.render(std::cout, use_color);
            std::cout << "\n  Total: " << total << " record(s)\n\n";

            if (total == 0) {
                std::cout << "  Nothing to prune.\n\n";
                return 0;
            }

            if (prune_dry_run) {
                std::cout << "  (dry-run: no records deleted)\n\n";
                return 0;
            }

            // Confirmation prompt (unless --force).
            if (!prune_force) {
                std::cout << "  Delete " << total
                          << " record(s)? [y/N] ";
                std::string answer;
                std::getline(std::cin, answer);
                if (answer != "y" && answer != "Y") {
                    std::cout << "  Aborted.\n";
                    return 0;
                }
            }

            // Execute the prune.
            std::string cutoff =
                "datetime('now', '-" +
                std::to_string(prune_days) + " days')";
            int total_deleted = 0;

            try {
                SQLite::Transaction txn(*db);

                SQLite::Statement del_chunks(*db,
                    "DELETE FROM log_chunks WHERE run_id IN "
                    "(SELECT run_id FROM runs WHERE start_ts < " +
                    cutoff + ")");
                total_deleted += del_chunks.exec();

                SQLite::Statement del_steps(*db,
                    "DELETE FROM step_runs WHERE run_id IN "
                    "(SELECT run_id FROM runs WHERE start_ts < " +
                    cutoff + ")");
                total_deleted += del_steps.exec();

                SQLite::Statement del_jobs(*db,
                    "DELETE FROM job_runs WHERE run_id IN "
                    "(SELECT run_id FROM runs WHERE start_ts < " +
                    cutoff + ")");
                total_deleted += del_jobs.exec();

                SQLite::Statement del_runs(*db,
                    "DELETE FROM runs WHERE start_ts < " + cutoff);
                total_deleted += del_runs.exec();

                SQLite::Statement del_events(*db,
                    "DELETE FROM watch_events WHERE created_at < " +
                    cutoff);
                total_deleted += del_events.exec();

                SQLite::Statement del_samples(*db,
                    "DELETE FROM watch_samples WHERE collected_at < " +
                    cutoff);
                total_deleted += del_samples.exec();

                SQLite::Statement del_metrics(*db,
                    "DELETE FROM metrics_snapshots WHERE recorded_at < " +
                    cutoff);
                total_deleted += del_metrics.exec();

                txn.commit();

                // WAL checkpoint.
                db->exec("PRAGMA wal_checkpoint(TRUNCATE)");

                std::cout << "  Pruned " << total_deleted
                          << " record(s).\n";
                std::cout << "  Run 'kairos status' to verify.\n\n";
            } catch (const std::exception& e) {
                std::cerr << "  Prune failed: " << e.what() << "\n";
                return 1;
            }
        }
        return 0;
    }

    // ── events tail ──────────────────────────────────────────────
    if (events_tail->parsed()) {
        setup_logging(log_level, json_output, false);

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        std::unique_ptr<SQLite::Database> db;
        try {
            db = persist::open_database(cfg->db_path);
        } catch (const std::exception& e) {
            std::cerr << "Failed to open database: " << e.what()
                      << "\nRun 'kairos init-db' first.\n";
            return 1;
        }

        persist::QueryReader reader(*db);
        bool use_color = !json_output && cli::supports_color();

        // Get the current max event ID as our starting cursor.
        int64_t cursor = reader.query_max_event_id();

        if (!json_output) {
            std::cout << "Tailing watch events";
            if (!events_tail_group.empty()) {
                std::cout << " (group: " << events_tail_group << ")";
            }
            std::cout << "... (Ctrl+C to stop)\n\n";
        }

        // Poll loop at 500ms (§23.2 events tail).
        while (true) {
            auto events = reader.query_watch_events_since(
                cursor, 100, events_tail_group);

            for (const auto& e : events) {
                int64_t row_id = std::stoll(e.event_uid);
                cursor = std::max(cursor, row_id);

                if (json_output) {
                    std::cout << json{
                        {"event_uid", e.event_uid},
                        {"watch_group", e.watch_group},
                        {"rule_name", e.rule_name},
                        {"event_type", e.event_type},
                        {"severity", e.severity},
                        {"created_at", format_display_ts(e.created_at)}
                    }.dump() << "\n";
                    std::cout.flush();
                } else {
                    // Colorize severity.
                    std::string sev = e.severity;
                    if (use_color) {
                        if (sev == "critical") {
                            sev = cli::colorize(sev, cli::ansi::red, true);
                        } else if (sev == "warning") {
                            sev = cli::colorize(sev, cli::ansi::yellow, true);
                        } else {
                            sev = cli::colorize(sev, cli::ansi::green, true);
                        }
                    }

                    std::string ts = e.created_at.size() > 19
                        ? e.created_at.substr(0, 19) : e.created_at;

                    std::cout << ts << "  "
                              << fmt::format("{:<15}", e.watch_group)
                              << fmt::format("{:<15}", e.rule_name)
                              << fmt::format("{:<12}", e.event_type)
                              << sev << "\n";
                    std::cout.flush();
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // Unreachable (Ctrl+C terminates).
        return 0;
    }

    // ── completions ──────────────────────────────────────────────
    if (cmd_completions->parsed()) {
        // Shell completion script generation (§23.9).
        // Usage:
        //   eval "$(kairos completions bash)"
        //   eval "$(kairos completions zsh)"
        //   kairos completions fish | source

        if (completions_shell == "bash") {
            std::cout << R"BASH(# Kairos bash completion — generated by `kairos completions bash`
# Install: eval "$(kairos completions bash)"
# Or:      kairos completions bash > /etc/bash_completion.d/kairos

_kairos_completions() {
    local cur prev words cword
    _init_completion || return

    local -a top_cmds=(start stop status mcp version init-db prune
                       workflows jobs runs logs events watches config
                       completions dashboard kel triggers migrate-config
                       migrate-db)

    local -a wf_cmds=(list show run explain graph)
    local -a jobs_cmds=(list show run)
    local -a runs_cmds=(list show cancel)
    local -a events_cmds=(list tail)
    local -a watches_cmds=(list show scan-once)
    local -a config_cmds=(show validate reload)
    local -a completions_cmds=(bash zsh fish)
    local -a kel_cmds=(eval repl)
    local -a triggers_cmds=(list next preview)

    # Dynamic completions: when the 3rd word is needed (entity name/ID),
    # call the hidden __complete_* subcommands to get candidates from the DB.
    if [[ $cword -ge 3 ]]; then
        case "${words[1]}" in
            workflows)
                case "${words[2]}" in
                    show|run|explain|graph)
                        local names
                        names=$(kairos __complete_workflows 2>/dev/null)
                        COMPREPLY=($(compgen -W "$names" -- "$cur"))
                        return
                        ;;
                esac
                ;;
            jobs)
                case "${words[2]}" in
                    show|run)
                        local names
                        names=$(kairos __complete_jobs 2>/dev/null)
                        COMPREPLY=($(compgen -W "$names" -- "$cur"))
                        return
                        ;;
                esac
                ;;
            runs)
                case "${words[2]}" in
                    show|cancel)
                        local ids
                        ids=$(kairos __complete_runs 2>/dev/null)
                        COMPREPLY=($(compgen -W "$ids" -- "$cur"))
                        return
                        ;;
                esac
                ;;
            watches)
                case "${words[2]}" in
                    show|scan-once)
                        local names
                        names=$(kairos __complete_watches 2>/dev/null)
                        COMPREPLY=($(compgen -W "$names" -- "$cur"))
                        return
                        ;;
                esac
                ;;
            logs)
                local ids
                ids=$(kairos __complete_runs 2>/dev/null)
                COMPREPLY=($(compgen -W "$ids" -- "$cur"))
                return
                ;;
        esac
    fi

    case "${words[1]}" in
        workflows) COMPREPLY=($(compgen -W "${wf_cmds[*]}" -- "$cur")) ;;
        jobs)      COMPREPLY=($(compgen -W "${jobs_cmds[*]}" -- "$cur")) ;;
        runs)      COMPREPLY=($(compgen -W "${runs_cmds[*]}" -- "$cur")) ;;
        events)    COMPREPLY=($(compgen -W "${events_cmds[*]}" -- "$cur")) ;;
        watches)   COMPREPLY=($(compgen -W "${watches_cmds[*]}" -- "$cur")) ;;
        config)    COMPREPLY=($(compgen -W "${config_cmds[*]}" -- "$cur")) ;;
        completions) COMPREPLY=($(compgen -W "${completions_cmds[*]}" -- "$cur")) ;;
        kel)       COMPREPLY=($(compgen -W "${kel_cmds[*]}" -- "$cur")) ;;
        triggers)  COMPREPLY=($(compgen -W "${triggers_cmds[*]}" -- "$cur")) ;;
        *)
            case "$cur" in
                -*)
                    local -a flags=(--config -c --json --log-level --help)
                    COMPREPLY=($(compgen -W "${flags[*]}" -- "$cur"))
                    ;;
                *)
                    COMPREPLY=($(compgen -W "${top_cmds[*]}" -- "$cur"))
                    ;;
            esac
            ;;
    esac
}
complete -F _kairos_completions kairos
)BASH";
        } else if (completions_shell == "zsh") {
            std::cout << R"ZSH(#compdef kairos
# Kairos zsh completion — generated by `kairos completions zsh`
# Install: eval "$(kairos completions zsh)"
# Or:      kairos completions zsh > "${fpath[1]}/_kairos"

_kairos() {
    local -a top_cmds
    top_cmds=(
        'start:Start the daemon'
        'stop:Stop the running daemon'
        'status:Show daemon and engine status'
        'mcp:Start MCP stdio server'
        'version:Show version info'
        'init-db:Initialize database'
        'prune:Prune old records from the database'
        'workflows:Workflow management'
        'jobs:Manage standalone jobs'
        'runs:Run history'
        'logs:View logs for a run'
        'events:Watch events'
        'watches:Watch group management'
        'config:Configuration management'
        'completions:Generate shell completion scripts'
        'dashboard:Live TUI dashboard'
        'kel:KEL expression evaluation'
        'triggers:Schedule triggers'
        'migrate-config:Migrate legacy tool config'
        'migrate-db:Migrate legacy tool database'
    )

    if (( CURRENT == 2 )); then
        _describe 'command' top_cmds
        return
    fi

    # Dynamic entity completions from the DB/YAML via the hidden
    # __complete_* subcommands (empty output → fall through to static).
    if (( CURRENT == 3 )) && [[ "${words[2]}" == logs ]]; then
        local -a run_ids
        run_ids=("${(@f)$(kairos __complete_runs 2>/dev/null)}")
        if (( ${#run_ids} )); then
            _describe 'run' run_ids
            return
        fi
    fi
    if (( CURRENT >= 4 )); then
        local -a candidates
        case "${words[2]}:${words[3]}" in
            workflows:show|workflows:run|workflows:explain|workflows:graph)
                candidates=("${(@f)$(kairos __complete_workflows 2>/dev/null)}") ;;
            jobs:show|jobs:run)
                candidates=("${(@f)$(kairos __complete_jobs 2>/dev/null)}") ;;
            runs:show|runs:cancel)
                candidates=("${(@f)$(kairos __complete_runs 2>/dev/null)}") ;;
            watches:show|watches:scan-once)
                candidates=("${(@f)$(kairos __complete_watches 2>/dev/null)}") ;;
        esac
        if (( ${#candidates} )); then
            _describe 'entity' candidates
            return
        fi
    fi

    case "${words[2]}" in
        workflows)
            local -a wf_cmds=('list:List all workflows' 'show:Show workflow detail'
                               'run:Trigger a workflow' 'explain:Explain execution plan'
                               'graph:Render the DAG as ASCII art')
            _describe 'subcommand' wf_cmds ;;
        jobs)
            local -a j_cmds=('list:List standalone jobs' 'show:Show job detail'
                              'run:Run a standalone job')
            _describe 'subcommand' j_cmds ;;
        runs)
            local -a r_cmds=('list:List recent runs' 'show:Show run detail'
                              'cancel:Cancel a running run')
            _describe 'subcommand' r_cmds ;;
        events)
            local -a e_cmds=('list:List recent events' 'tail:Stream events')
            _describe 'subcommand' e_cmds ;;
        watches)
            local -a w_cmds=('list:List watch groups' 'show:Show watch group detail'
                              'scan-once:Run a single watch scan')
            _describe 'subcommand' w_cmds ;;
        config)
            local -a c_cmds=('show:Show effective config' 'validate:Validate config'
                              'reload:Reload config')
            _describe 'subcommand' c_cmds ;;
        completions)
            local -a sh_cmds=('bash' 'zsh' 'fish')
            _describe 'shell' sh_cmds ;;
        kel)
            local -a k_cmds=('eval:Evaluate one expression' 'repl:Interactive REPL')
            _describe 'subcommand' k_cmds ;;
        triggers)
            local -a t_cmds=('list:List triggers' 'next:Next triggers to fire'
                              'preview:Preview a cron schedule')
            _describe 'subcommand' t_cmds ;;
        *)
            _arguments '*:options:(-c --config --json --log-level)' ;;
    esac
}
_kairos "$@"
)ZSH";
        } else if (completions_shell == "fish") {
            std::cout << R"FISH(# Kairos fish completion — generated by `kairos completions fish`
# Install: kairos completions fish | source
# Or:      kairos completions fish > ~/.config/fish/completions/kairos.fish

# Top-level commands
set -l cmds start stop status mcp version init-db prune workflows jobs runs logs events watches config completions dashboard kel triggers migrate-config migrate-db

# Dynamic entity candidates from the DB/YAML via hidden __complete_*
# subcommands (empty output → no suggestions, no error).
function __kairos_workflows; kairos __complete_workflows 2>/dev/null; end
function __kairos_jobs;       kairos __complete_jobs 2>/dev/null; end
function __kairos_runs;       kairos __complete_runs 2>/dev/null; end
function __kairos_watches;    kairos __complete_watches 2>/dev/null; end

# Disable file completion by default
complete -c kairos -f

# Top-level
complete -c kairos -n "not __fish_seen_subcommand_from $cmds" -a start -d "Start the daemon"
complete -c kairos -n "not __fish_seen_subcommand_from $cmds" -a stop -d "Stop the running daemon"
complete -c kairos -n "not __fish_seen_subcommand_from $cmds" -a status -d "Show daemon and engine status"
complete -c kairos -n "not __fish_seen_subcommand_from $cmds" -a mcp -d "Start MCP stdio server"
complete -c kairos -n "not __fish_seen_subcommand_from $cmds" -a version -d "Show version info"
complete -c kairos -n "not __fish_seen_subcommand_from $cmds" -a init-db -d "Initialize database"
complete -c kairos -n "not __fish_seen_subcommand_from $cmds" -a prune -d "Prune old records"
complete -c kairos -n "not __fish_seen_subcommand_from $cmds" -a workflows -d "Workflow management"
complete -c kairos -n "not __fish_seen_subcommand_from $cmds" -a jobs -d "Job management"
complete -c kairos -n "not __fish_seen_subcommand_from $cmds" -a runs -d "Run history"
complete -c kairos -n "not __fish_seen_subcommand_from $cmds" -a logs -d "View logs"
complete -c kairos -n "not __fish_seen_subcommand_from $cmds" -a events -d "Watch events"
complete -c kairos -n "not __fish_seen_subcommand_from $cmds" -a watches -d "Watch group management"
complete -c kairos -n "not __fish_seen_subcommand_from $cmds" -a config -d "Configuration management"
complete -c kairos -n "not __fish_seen_subcommand_from $cmds" -a completions -d "Generate completions"

# Global flags
complete -c kairos -l config -s c -d "Config file path" -rF
complete -c kairos -l json -d "JSON output"
complete -c kairos -l log-level -d "Log level" -ra "trace debug info warn error"

# Subcommands
complete -c kairos -n "__fish_seen_subcommand_from workflows" -a "list show run explain graph" -f
complete -c kairos -n "__fish_seen_subcommand_from jobs" -a "list show run" -f
complete -c kairos -n "__fish_seen_subcommand_from runs" -a "list show cancel" -f
complete -c kairos -n "__fish_seen_subcommand_from events" -a "list tail" -f
complete -c kairos -n "__fish_seen_subcommand_from watches" -a "list show scan-once" -f
complete -c kairos -n "__fish_seen_subcommand_from config" -a "show validate reload" -f
complete -c kairos -n "__fish_seen_subcommand_from completions" -a "bash zsh fish" -f
complete -c kairos -n "__fish_seen_subcommand_from kel" -a "eval repl" -f
complete -c kairos -n "__fish_seen_subcommand_from triggers" -a "list next preview" -f

# Dynamic entity arguments (3rd word)
complete -c kairos -n "__fish_seen_subcommand_from workflows; and __fish_seen_subcommand_from show run explain graph" -a "(__kairos_workflows)" -f
complete -c kairos -n "__fish_seen_subcommand_from jobs; and __fish_seen_subcommand_from show run" -a "(__kairos_jobs)" -f
complete -c kairos -n "__fish_seen_subcommand_from runs; and __fish_seen_subcommand_from show cancel" -a "(__kairos_runs)" -f
complete -c kairos -n "__fish_seen_subcommand_from watches; and __fish_seen_subcommand_from show scan-once" -a "(__kairos_watches)" -f
complete -c kairos -n "__fish_seen_subcommand_from logs" -a "(__kairos_runs)" -f

# Workflows run/explain flags
complete -c kairos -n "__fish_seen_subcommand_from workflows; and __fish_seen_subcommand_from run" -s f -l follow -d "Follow log output"
complete -c kairos -n "__fish_seen_subcommand_from workflows; and __fish_seen_subcommand_from run" -l dry-run -d "Show plan only"

# Runs list flags
complete -c kairos -n "__fish_seen_subcommand_from runs; and __fish_seen_subcommand_from list" -s n -l limit -d "Number of runs"
complete -c kairos -n "__fish_seen_subcommand_from runs; and __fish_seen_subcommand_from list" -l status -d "Filter by status" -ra "SUCCESS FAILURE RUNNING CANCELLED INTERRUPTED"
complete -c kairos -n "__fish_seen_subcommand_from runs; and __fish_seen_subcommand_from list" -l workflow -d "Filter by workflow"

# Prune flags
complete -c kairos -n "__fish_seen_subcommand_from prune" -l older-than -d "Days to keep"
complete -c kairos -n "__fish_seen_subcommand_from prune" -l dry-run -d "Preview only"
complete -c kairos -n "__fish_seen_subcommand_from prune" -l force -d "Skip confirmation"

# Logs flags
complete -c kairos -n "__fish_seen_subcommand_from logs" -s f -l follow -d "Follow log output"
complete -c kairos -n "__fish_seen_subcommand_from logs" -l job -d "Filter by job"
complete -c kairos -n "__fish_seen_subcommand_from logs" -l step -d "Filter by step"
)FISH";
        } else {
            std::cerr << "Unknown shell: " << completions_shell
                      << "\nSupported: bash, zsh, fish\n";
            return 1;
        }
        return 0;
    }

    // ── Dynamic completion handlers (Phase 8.5, §2.3) ────────────
    // These hidden subcommands output one name/ID per line for shell
    // completion scripts. They open the DB read-only, query names,
    // and exit. No logging, no color — pure stdout for consumption.

    auto complete_helper = [&](auto* cmd, auto query_fn) -> std::optional<int> {
        if (!cmd->parsed()) return std::nullopt;
        try {
            setup_logging("off", false, false);
            auto cfg = load_config_or_die(config_path, {});
            if (!cfg) return 1;
            auto db = try_open_db(cfg);
            if (!db) return 1;
            query_fn(*db, cfg);
        } catch (...) {
            // Silently fail — completions should never produce errors.
        }
        return 0;
    };

    // __complete_workflows: emit workflow names.
    if (auto rc = complete_helper(cpl_workflows,
        [](SQLite::Database& /*db*/,
           const std::shared_ptr<const config::ConfigState>& cfg) {
            // Load registry from YAML for workflow names.
            auto log = spdlog::default_logger();
            auto registry = load_registry_from_yaml(cfg, log);
            for (const auto* wf : registry->workflows()) {
                std::cout << wf->workflow_name << "\n";
            }
        })) {
        return *rc;
    }

    // __complete_jobs: emit standalone job names.
    if (auto rc = complete_helper(cpl_jobs,
        [](SQLite::Database& /*db*/,
           const std::shared_ptr<const config::ConfigState>& cfg) {
            auto log = spdlog::default_logger();
            auto registry = load_registry_from_yaml(cfg, log);
            for (const auto* sj : registry->standalone_jobs()) {
                std::cout << sj->job_name << "\n";
            }
        })) {
        return *rc;
    }

    // __complete_runs: emit recent run IDs (last 50).
    if (auto rc = complete_helper(cpl_runs,
        [](SQLite::Database& db,
           const std::shared_ptr<const config::ConfigState>&) {
            try {
                SQLite::Statement q(db,
                    "SELECT run_id FROM runs "
                    "ORDER BY start_ts DESC LIMIT 50");
                while (q.executeStep()) {
                    std::cout << q.getColumn(0).getString() << "\n";
                }
            } catch (...) {}
        })) {
        return *rc;
    }

    // __complete_watches: emit watch group names.
    if (auto rc = complete_helper(cpl_watches,
        [](SQLite::Database& /*db*/,
           const std::shared_ptr<const config::ConfigState>& cfg) {
            auto log = spdlog::default_logger();
            auto registry = load_registry_from_yaml(cfg, log);
            for (const auto& wg : registry->watch_groups()) {
                std::cout << wg.group_name << "\n";
            }
        })) {
        return *rc;
    }

    // __complete_triggers: emit trigger IDs.
    if (auto rc = complete_helper(cpl_triggers,
        [](SQLite::Database& /*db*/,
           const std::shared_ptr<const config::ConfigState>& cfg) {
            auto log = spdlog::default_logger();
            auto registry = load_registry_from_yaml(cfg, log);
            for (const auto& t : registry->triggers()) {
                std::cout << t.trigger_id << "\n";
            }
        })) {
        return *rc;
    }

    // ── migrate-config ───────────────────────────────────────────────
    if (cmd_migrate_config->parsed()) {
        return handle_migrate_config(mc_source, mc_source_config,
            mc_source_watches, mc_source_workflows, mc_output_dir,
            mc_dry_run, json_output);
    }

    // ── migrate-db ───────────────────────────────────────────────────
    if (cmd_migrate_db->parsed()) {
        return handle_migrate_db(md_source, md_source_db, md_target_db,
            md_dry_run, json_output);
    }

    // ── dashboard (TUI) ──────────────────────────────────────────────
    if (cmd_dashboard->parsed()) {
        kairos::tui::DashboardConfig dc;
        if (!dashboard_db.empty()) {
            dc.db_path = dashboard_db;
        } else {
            // Resolve db_path from config file.
            setup_logging(log_level, json_output, false);
            auto cfg = load_config_or_die(config_path, {});
            if (!cfg) return static_cast<int>(ExitCode::kConfigError);
            dc.db_path = cfg->db_path;
            dc.config_path = cfg->config_file_path;
            dc.data_dir = cfg->data_dir;
            dc.timezone = cfg->global.get<std::string>(
                "kairos.timezone", "local");
        }
        dc.refresh_ms = dashboard_refresh;
        return kairos::tui::run_dashboard(dc);
    }

    // ── service install/uninstall (Windows only) ─────────────────────
    if (svc_install->parsed()) {
#ifdef _WIN32
        auto exe = fs::canonical(argv[0]);
        auto cfg_path = svc_install_config.empty()
            ? fs::path() : fs::path(svc_install_config);
        bool ok = daemon::install_service(exe, cfg_path);
        return ok ? 0 : 1;
#else
        std::cerr << "Service management is only available on Windows.\n"
                  << "On Linux, use: sudo systemctl enable kairos\n"
                  << "On macOS, use: launchctl load "
                     "~/Library/LaunchAgents/com.kairos.daemon.plist\n";
        return 1;
#endif
    }

    if (svc_uninstall->parsed()) {
#ifdef _WIN32
        bool ok = daemon::uninstall_service();
        return ok ? 0 : 1;
#else
        std::cerr << "Service management is only available on Windows.\n";
        return 1;
#endif
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

        // Load the workflow/watch registry from YAML so the workflow, job, and
        // watch-group tools actually work (without this the handler's registry is
        // null and every such tool returns "Workflow registry not available").
        // Reuses the same loader the daemon and the CLI query commands use.
        try {
            handler.update_registry(load_registry_from_yaml(cfg, logger));
        } catch (const std::exception& e) {
            spdlog::warn("MCP: failed to load workflow registry: {}", e.what());
        }

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

    // ── kel eval ─────────────────────────────────────────────────
    // Roadmap §7: Single-expression evaluation.
    if (kel_eval->parsed()) {
        setup_logging(log_level, json_output, false);

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        try {
            // Build evaluation context with DB bindings.
            kel::EvalContext ctx = kel::make_default_context();

            // Open DB for job() function bindings (best-effort).
            auto db = try_open_db(cfg);
            std::unique_ptr<persist::QueryReader> reader;
            std::unique_ptr<persist::TagStore> tag_store_ptr;
            if (db) {
                reader = std::make_unique<persist::QueryReader>(*db);
                reader->register_kel_bindings(ctx);
                reader->register_watch_kel_bindings(ctx);

                // Phase 8.5: Register has_tag() for CLI kel eval.
                tag_store_ptr = std::make_unique<persist::TagStore>(*db);
                auto* ts = tag_store_ptr.get();
                ctx.functions["has_tag"] = [ts](
                    const std::vector<kel::KelValue>& args) -> kel::KelValue
                {
                    if (args.size() != 2 || !args[0].is_string() ||
                        !args[1].is_string())
                        throw kel::KelEvalError(
                            "has_tag() requires two string arguments "
                            "(entity_id, tag)");
                    const auto& eid = args[0].as_string();
                    const auto& tag = args[1].as_string();
                    static const std::string types[] = {
                        "workflow", "job", "watch_group", "trigger"};
                    for (const auto& t : types) {
                        if (ts->has_tag(t, eid, tag))
                            return kel::KelValue(true);
                    }
                    return kel::KelValue(false);
                };
            }

            kel::EvalLimits limits;
            auto result = kel::eval_expression(kel_eval_expr, ctx, limits);

            if (json_output) {
                json j;
                j["expression"] = kel_eval_expr;
                j["type"] = result.type_name();
                // Serialize based on type.
                if (result.is_bool()) {
                    j["value"] = result.as_bool();
                } else if (result.is_int()) {
                    j["value"] = result.as_int();
                } else if (result.is_float()) {
                    j["value"] = result.as_float();
                } else if (result.is_string()) {
                    j["value"] = result.as_string();
                } else {
                    j["value"] = result.to_display_string();
                }
                std::cout << j.dump(2) << "\n";
            } else {
                bool use_color = cli::supports_color();
                std::string val = result.to_display_string();
                std::string type = result.type_name();
                if (use_color) {
                    // Color value by type.
                    if (result.is_bool()) {
                        val = cli::colorize(val,
                            result.as_bool() ? cli::ansi::green
                                             : cli::ansi::red, true);
                    } else if (result.is_int() ||
                               result.is_float()) {
                        val = cli::colorize(val, cli::ansi::cyan, true);
                    } else if (result.is_string()) {
                        val = cli::colorize(val, cli::ansi::yellow, true);
                    }
                    type = cli::colorize(type, cli::ansi::gray, true);
                }
                std::cout << val << "  " << type << "\n";
            }
        } catch (const kel::KelError& e) {
            if (json_output) {
                std::cout << json{
                    {"expression", kel_eval_expr},
                    {"error", e.what()}
                }.dump(2) << "\n";
            } else {
                std::cerr << "KEL error: " << e.what() << "\n";
            }
            return 1;
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    // ── kel repl ─────────────────────────────────────────────────
    // Roadmap §7: Interactive REPL mode.
    if (kel_repl->parsed()) {
        setup_logging("error", false, false);  // Quiet logging for REPL.

        auto cfg = load_config_or_die(config_path, {});
        if (!cfg) return static_cast<int>(ExitCode::kConfigError);

        bool use_color = cli::supports_color();

        // Build evaluation context with DB bindings.
        kel::EvalContext ctx = kel::make_default_context();

        auto db = try_open_db(cfg);
        std::unique_ptr<persist::QueryReader> reader;
        if (db) {
            reader = std::make_unique<persist::QueryReader>(*db);
            reader->register_kel_bindings(ctx);
            reader->register_watch_kel_bindings(ctx);
        }

        kel::EvalLimits limits;

        // Print banner.
        if (use_color) {
            std::cout << cli::colorize("Kairos KEL REPL",
                                       cli::ansi::bold, true)
                      << " — type expressions, 'help' for info, "
                      << "'exit' or Ctrl-D to quit\n";
            if (db) {
                std::cout << cli::colorize(
                    "  Database connected — job() functions available",
                    cli::ansi::green, true) << "\n";
            } else {
                std::cout << cli::colorize(
                    "  No database — job() functions unavailable",
                    cli::ansi::yellow, true) << "\n";
            }
        } else {
            std::cout << "Kairos KEL REPL — type expressions, "
                      << "'help' for info, 'exit' or Ctrl-D to quit\n";
            if (db) std::cout << "  Database connected\n";
            else    std::cout << "  No database\n";
        }
        std::cout << "\n";

        std::string line;
        while (true) {
            // Prompt.
            if (use_color) {
                std::cout << cli::colorize("KEL", cli::ansi::cyan, true)
                          << "> ";
            } else {
                std::cout << "KEL> ";
            }
            std::cout.flush();

            if (!std::getline(std::cin, line)) break;  // EOF / Ctrl-D.

            // Trim whitespace.
            auto start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            auto end = line.find_last_not_of(" \t\r\n");
            line = line.substr(start, end - start + 1);

            if (line.empty()) continue;
            if (line == "exit" || line == "quit") break;

            if (line == "help") {
                std::cout << "KEL REPL commands:\n"
                    << "  <expression>   Evaluate a KEL expression\n"
                    << "  help           Show this help\n"
                    << "  exit / quit    Exit the REPL\n"
                    << "\n"
                    << "Examples:\n"
                    << "  2 + 3 * 4\n"
                    << "  \"hello\" + \" world\"\n"
                    << "  true and not false\n"
                    << "  now()\n";
                if (db) {
                    std::cout
                        << "  job(\"backup\").last_success\n"
                        << "  job(\"backup\").finished_within(24h)\n"
                        << "  job(\"backup\").last_status\n"
                        << "  job(\"backup\").run_count\n";
                }
                std::cout << "\n";
                continue;
            }

            try {
                auto result = kel::eval_expression(line, ctx, limits);
                std::string val = result.to_display_string();
                std::string type = result.type_name();
                if (use_color) {
                    if (result.is_bool()) {
                        val = cli::colorize(val,
                            result.as_bool() ? cli::ansi::green
                                             : cli::ansi::red, true);
                    } else if (result.is_int() ||
                               result.is_float()) {
                        val = cli::colorize(val, cli::ansi::cyan, true);
                    } else if (result.is_string()) {
                        val = cli::colorize(val, cli::ansi::yellow, true);
                    }
                    type = cli::colorize(type, cli::ansi::gray, true);
                }
                std::cout << val << "  " << type << "\n";
            } catch (const kel::KelError& e) {
                if (use_color) {
                    std::cout << cli::colorize("error: ",
                                               cli::ansi::red, true)
                              << e.what() << "\n";
                } else {
                    std::cout << "error: " << e.what() << "\n";
                }
            } catch (const std::exception& e) {
                std::cout << "error: " << e.what() << "\n";
            }
        }

        std::cout << "\n";
        return 0;
    }

    // Should not reach here (require_subcommand is set).
    std::cerr << "No subcommand specified. Use --help for usage.\n";
    return 1;
}

}  // namespace kairos::cli
