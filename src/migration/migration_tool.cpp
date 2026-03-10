/// src/migration/migration_tool.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  migration_tool.cpp — Legacy tool config + DB migration                  ║
// ║                                                                         ║
// ║  Implements config translation and database import for:                 ║
// ║    - AVScheduler (§31.2, §31.5)                                        ║
// ║    - EventWatcher (§31.4, §31.5)                                       ║
// ║    - LocalFlow (§31.3)                                                 ║
// ║                                                                         ║
// ║  All source reads are read-only.  No legacy data is modified.          ║
// ║  Spec reference: §31                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/migration/migration_tool.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace kairos::migration {

// ── Helpers ──────────────────────────────────────────────────────────────

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

SourceTool parse_source_tool(const std::string& name) {
    auto lower = to_lower(name);
    if (lower == "avscheduler")  return SourceTool::kAVScheduler;
    if (lower == "eventwatcher") return SourceTool::kEventWatcher;
    if (lower == "localflow")    return SourceTool::kLocalFlow;
    throw std::invalid_argument(
        "Unknown migration source: '" + name + "'. "
        "Expected: avscheduler, eventwatcher, or localflow.");
}

std::string source_tool_name(SourceTool tool) {
    switch (tool) {
        case SourceTool::kAVScheduler:  return "avscheduler";
        case SourceTool::kEventWatcher: return "eventwatcher";
        case SourceTool::kLocalFlow:    return "localflow";
    }
    return "unknown";
}

/// Get current ISO 8601 timestamp for metadata.
static std::string now_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

/// Write a string to a file.
static bool write_file(const fs::path& path, const std::string& content,
                       std::vector<MigrationMessage>& msgs) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        msgs.push_back({MigrationMessage::Level::kError,
                        path.string(),
                        "Failed to create parent directory: " + ec.message()});
        return false;
    }
    std::ofstream out(path);
    if (!out.is_open()) {
        msgs.push_back({MigrationMessage::Level::kError,
                        path.string(),
                        "Failed to open file for writing"});
        return false;
    }
    out << content;
    return true;
}

// ══════════════════════════════════════════════════════════════════════════
// CONFIG MIGRATION
// ══════════════════════════════════════════════════════════════════════════

// ── AVScheduler Config Migration (§31.2) ─────────────────────────────────

/// Parse AVScheduler TOML config and produce Kairos files.
///
/// Input structure:
///   [settings]          → kairos.toml global settings
///   [web_server]        → kairos.http.* settings
///   [interpreters]      → shell mapping in workflow YAML
///   [jobs.<id>]         → workflows/avs_jobs.yaml
static ConfigMigrationResult migrate_avscheduler_config(
    const ConfigMigrationOptions& opts) {

    ConfigMigrationResult result;

    // Parse source TOML using a simple line-based parser.
    // We use YAML-cpp's YAML::LoadFile on TOML (not ideal but TOML is
    // a subset for simple cases).  For robustness, we parse manually.
    std::ifstream in(opts.source_config);
    if (!in.is_open()) {
        result.messages.push_back({MigrationMessage::Level::kError,
            opts.source_config.string(),
            "Cannot open source config file"});
        return result;
    }

    // Simple TOML parser for AVScheduler's flat structure.
    std::string current_section;
    std::string current_job_id;
    std::unordered_map<std::string, std::string> settings;
    std::unordered_map<std::string, std::string> web_server;
    std::unordered_map<std::string, std::string> interpreters;

    struct AvJob {
        std::string id;
        std::string type;         // PYTHON, BASH
        std::string schedule_type; // cron, interval
        std::string schedule;      // cron expr
        std::string interval_seconds;
        std::string command;
        std::string condition;
        std::string env_file;
    };
    std::vector<AvJob> jobs;

    std::string line;
    AvJob* current_av_job = nullptr;

    while (std::getline(in, line)) {
        // Trim whitespace.
        auto start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        // Skip comments.
        if (line[0] == '#') continue;

        // Section header.
        if (line[0] == '[') {
            auto end = line.find(']');
            if (end == std::string::npos) continue;
            current_section = line.substr(1, end - 1);

            // Check for [jobs.xxx].
            if (current_section.starts_with("jobs.")) {
                auto job_id = current_section.substr(5);
                jobs.push_back({});
                jobs.back().id = job_id;
                current_av_job = &jobs.back();
            } else {
                current_av_job = nullptr;
            }
            continue;
        }

        // Key = value.
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq);
        auto val = line.substr(eq + 1);

        // Trim.
        auto trim = [](std::string& s) {
            auto a = s.find_first_not_of(" \t\"");
            auto b = s.find_last_not_of(" \t\"\r\n");
            s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
        };
        trim(key);
        trim(val);

        if (current_av_job) {
            if (key == "type")             current_av_job->type = val;
            else if (key == "schedule_type")    current_av_job->schedule_type = val;
            else if (key == "schedule")         current_av_job->schedule = val;
            else if (key == "interval_seconds") current_av_job->interval_seconds = val;
            else if (key == "command")          current_av_job->command = val;
            else if (key == "condition")        current_av_job->condition = val;
            else if (key == "env_file")         current_av_job->env_file = val;
        } else if (current_section == "settings") {
            settings[key] = val;
        } else if (current_section == "web_server") {
            web_server[key] = val;
        } else if (current_section == "interpreters") {
            interpreters[key] = val;
        }
    }

    // ── Generate kairos.toml ──────────────────────────────────────────
    std::ostringstream toml;
    toml << "# Kairos configuration — migrated from AVScheduler\n"
         << "# Source: " << opts.source_config.string() << "\n"
         << "# Migrated: " << now_iso8601() << "\n\n";

    toml << "[kairos.db]\n";
    if (settings.contains("db_path")) {
        toml << "path = \"" << settings["db_path"] << "\"\n";
    } else {
        toml << "# path = \"/var/lib/kairos/kairos.db\"  # default\n";
    }
    toml << "\n";

    toml << "[kairos.platform]\n";
    auto default_shell_it = interpreters.find("BASH");
    if (default_shell_it != interpreters.end()) {
        toml << "default_shell = \"" << default_shell_it->second << "\"\n";
    }
    toml << "\n";

    if (!web_server.empty()) {
        toml << "[kairos.http]\n";
        if (web_server.contains("host")) {
            toml << "listen_addr = \"" << web_server["host"] << "\"\n";
        }
        if (web_server.contains("port")) {
            toml << "listen_port = " << web_server["port"] << "\n";
        }
        toml << "\n";
    }

    toml << "[kairos.logging]\n"
         << "level = \"info\"\n"
         << "format = \"auto\"\n";

    auto kairos_toml_path = opts.output_dir / "kairos.toml";

    // ── Generate workflow YAML ────────────────────────────────────────
    YAML::Emitter yaml;
    yaml << YAML::Comment("Kairos workflow — migrated from AVScheduler");
    yaml << YAML::Comment("Source: " + opts.source_config.string());
    yaml << YAML::Comment("Migrated: " + now_iso8601());
    yaml << YAML::Newline;

    // Group all AVScheduler jobs as standalone jobs with triggers.
    yaml << YAML::BeginMap;
    yaml << YAML::Key << "triggers" << YAML::Value << YAML::BeginSeq;

    for (const auto& job : jobs) {
        yaml << YAML::BeginMap;
        yaml << YAML::Key << "name" << YAML::Value
             << ("avs_" + job.id + "_trigger");

        if (job.schedule_type == "cron" && !job.schedule.empty()) {
            yaml << YAML::Key << "cron" << YAML::Value << job.schedule;
        } else if (job.schedule_type == "interval" &&
                   !job.interval_seconds.empty()) {
            yaml << YAML::Key << "interval"
                 << YAML::Value << (job.interval_seconds + "s");
        }

        yaml << YAML::Key << "workflow" << YAML::Value
             << ("avs_" + job.id);
        yaml << YAML::EndMap;
    }
    yaml << YAML::EndSeq;

    yaml << YAML::Key << "workflows" << YAML::Value << YAML::BeginSeq;
    for (const auto& job : jobs) {
        yaml << YAML::BeginMap;
        yaml << YAML::Key << "name" << YAML::Value
             << ("avs_" + job.id);

        // Condition.
        if (!job.condition.empty()) {
            yaml << YAML::Key << "condition" << YAML::Value << job.condition;

            // Check if condition needs manual rewriting.
            if (job.condition.find("any(") != std::string::npos ||
                job.condition.find("all(") != std::string::npos ||
                job.condition.find("for ") != std::string::npos) {
                result.messages.push_back({
                    MigrationMessage::Level::kWarning,
                    "job '" + job.id + "'",
                    "Condition may not be KEL-compatible: \"" +
                    job.condition + "\". Please review and rewrite in KEL syntax."
                });
            }
        }

        // Steps.
        yaml << YAML::Key << "steps" << YAML::Value << YAML::BeginSeq;
        yaml << YAML::BeginMap;
        yaml << YAML::Key << "name" << YAML::Value << job.id;

        // Determine shell from type.
        std::string shell;
        auto it = interpreters.find(job.type);
        if (it != interpreters.end()) {
            shell = it->second;
        } else {
            shell = to_lower(job.type);
        }
        yaml << YAML::Key << "shell" << YAML::Value << shell;
        yaml << YAML::Key << "run" << YAML::Value << job.command;

        if (!job.env_file.empty()) {
            yaml << YAML::Key << "env_file" << YAML::Value << job.env_file;
        }

        yaml << YAML::EndMap;
        yaml << YAML::EndSeq;  // steps
        yaml << YAML::EndMap;  // workflow
    }
    yaml << YAML::EndSeq;  // workflows
    yaml << YAML::EndMap;   // root

    auto workflows_dir = opts.output_dir / "workflows";
    auto yaml_path = workflows_dir / "avs_jobs.yaml";

    // ── Write files ──────────────────────────────────────────────────
    result.entities_migrated = static_cast<int>(jobs.size());

    if (!opts.dry_run) {
        if (!write_file(kairos_toml_path, toml.str(), result.messages)) {
            return result;
        }
        result.files_created.push_back(kairos_toml_path);

        if (!jobs.empty()) {
            if (!write_file(yaml_path, yaml.c_str(), result.messages)) {
                return result;
            }
            result.files_created.push_back(yaml_path);
        }
    } else {
        result.files_created.push_back(kairos_toml_path);
        if (!jobs.empty()) {
            result.files_created.push_back(yaml_path);
        }
    }

    result.success = true;
    return result;
}

// ── EventWatcher Config Migration (§31.4) ────────────────────────────────

static ConfigMigrationResult migrate_eventwatcher_config(
    const ConfigMigrationOptions& opts) {

    ConfigMigrationResult result;

    // Parse EventWatcher config.toml (same simple TOML parser).
    std::ifstream in(opts.source_config);
    if (!in.is_open()) {
        result.messages.push_back({MigrationMessage::Level::kError,
            opts.source_config.string(),
            "Cannot open source config file"});
        return result;
    }

    std::string current_section;
    std::unordered_map<std::string, std::string> database;
    std::unordered_map<std::string, std::string> logging;
    std::unordered_map<std::string, std::string> monitoring;

    std::string line;
    while (std::getline(in, line)) {
        auto start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        if (line[0] == '#') continue;

        if (line[0] == '[') {
            auto end = line.find(']');
            if (end != std::string::npos) {
                current_section = line.substr(1, end - 1);
            }
            continue;
        }

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq);
        auto val = line.substr(eq + 1);
        auto trim = [](std::string& s) {
            auto a = s.find_first_not_of(" \t\"");
            auto b = s.find_last_not_of(" \t\"\r\n");
            s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
        };
        trim(key);
        trim(val);

        if (current_section == "database")   database[key] = val;
        else if (current_section == "logging")    logging[key] = val;
        else if (current_section == "monitoring") monitoring[key] = val;
    }

    // ── Generate kairos.toml ──────────────────────────────────────────
    std::ostringstream toml;
    toml << "# Kairos configuration — migrated from EventWatcher\n"
         << "# Source: " << opts.source_config.string() << "\n"
         << "# Migrated: " << now_iso8601() << "\n\n";

    toml << "[kairos.db]\n";
    toml << "# path = \"/var/lib/kairos/kairos.db\"  # default\n\n";

    toml << "[kairos.logging]\n";
    if (logging.contains("level")) {
        toml << "level = \"" << to_lower(logging["level"]) << "\"\n";
    } else {
        toml << "level = \"info\"\n";
    }
    toml << "format = \"auto\"\n";

    auto kairos_toml_path = opts.output_dir / "kairos.toml";

    // ── Translate watch groups YAML ───────────────────────────────────
    int watch_groups_migrated = 0;
    auto yaml_path = opts.output_dir / "watch_groups" / "migrated.yaml";

    std::string yaml_str;
    if (!opts.source_watches.empty() && fs::exists(opts.source_watches)) {
        try {
            YAML::Node source = YAML::LoadFile(opts.source_watches.string());

            YAML::Emitter yaml;
            yaml << YAML::Comment("Kairos watch groups — migrated from EventWatcher");
            yaml << YAML::Comment("Source: " + opts.source_watches.string());
            yaml << YAML::Comment("Migrated: " + now_iso8601());
            yaml << YAML::Newline;

            yaml << YAML::BeginMap;
            yaml << YAML::Key << "watch_groups" << YAML::Value << YAML::BeginSeq;

            if (source["watch_groups"]) {
                for (const auto& wg : source["watch_groups"]) {
                    yaml << YAML::BeginMap;

                    // Name.
                    if (wg["name"]) {
                        yaml << YAML::Key << "name"
                             << YAML::Value << wg["name"].as<std::string>();
                    }

                    // Paths (was watch_items).
                    if (wg["watch_items"]) {
                        yaml << YAML::Key << "paths"
                             << YAML::Value << YAML::BeginSeq;
                        for (const auto& item : wg["watch_items"]) {
                            yaml << item.as<std::string>();
                        }
                        yaml << YAML::EndSeq;
                    }

                    // Sample interval (was sample_rate).
                    if (wg["sample_rate"]) {
                        yaml << YAML::Key << "sample_interval"
                             << YAML::Value
                             << (wg["sample_rate"].as<std::string>() + "s");
                    }

                    // Max snapshots (was max_samples).
                    if (wg["max_samples"]) {
                        yaml << YAML::Key << "max_snapshots"
                             << YAML::Value << wg["max_samples"].as<int>();
                    }

                    // Max depth.
                    if (wg["max_depth"]) {
                        yaml << YAML::Key << "max_depth"
                             << YAML::Value << wg["max_depth"].as<int>();
                    }

                    // Rules.
                    if (wg["rules"]) {
                        yaml << YAML::Key << "rules"
                             << YAML::Value << YAML::BeginSeq;
                        for (const auto& rule : wg["rules"]) {
                            yaml << YAML::BeginMap;
                            if (rule["name"]) {
                                yaml << YAML::Key << "name"
                                     << YAML::Value
                                     << rule["name"].as<std::string>();
                            }
                            if (rule["condition"]) {
                                auto cond = rule["condition"]
                                    .as<std::string>();
                                // Flag Python-eval conditions.
                                if (cond.find("aggregate(") != std::string::npos ||
                                    cond.find("eval(") != std::string::npos ||
                                    cond.find("lambda") != std::string::npos) {
                                    result.messages.push_back({
                                        MigrationMessage::Level::kWarning,
                                        "watch group '" +
                                            (wg["name"]
                                                 ? wg["name"].as<std::string>()
                                                 : "?") +
                                            "', rule '" +
                                            (rule["name"]
                                                 ? rule["name"].as<std::string>()
                                                 : "?") + "'",
                                        "Cannot auto-translate condition: \"" +
                                        cond + "\". Please rewrite in KEL syntax."
                                    });
                                }
                                yaml << YAML::Key << "condition"
                                     << YAML::Value << cond;
                            }
                            if (rule["action"]) {
                                yaml << YAML::Key << "action"
                                     << YAML::Value
                                     << rule["action"].as<std::string>();
                            }
                            yaml << YAML::EndMap;
                        }
                        yaml << YAML::EndSeq;
                    }

                    yaml << YAML::EndMap;
                    ++watch_groups_migrated;
                }
            }

            yaml << YAML::EndSeq;
            yaml << YAML::EndMap;
            yaml_str = yaml.c_str();
        } catch (const YAML::Exception& e) {
            result.messages.push_back({MigrationMessage::Level::kError,
                opts.source_watches.string(),
                "YAML parse error: " + std::string(e.what())});
            return result;
        }
    }

    result.entities_migrated = watch_groups_migrated;

    if (!opts.dry_run) {
        if (!write_file(kairos_toml_path, toml.str(), result.messages)) {
            return result;
        }
        result.files_created.push_back(kairos_toml_path);

        if (!yaml_str.empty()) {
            if (!write_file(yaml_path, yaml_str, result.messages)) {
                return result;
            }
            result.files_created.push_back(yaml_path);
        }
    } else {
        result.files_created.push_back(kairos_toml_path);
        if (!yaml_str.empty()) {
            result.files_created.push_back(yaml_path);
        }
    }

    result.success = true;
    return result;
}

// ── LocalFlow Config Migration (§31.3) ───────────────────────────────────

static ConfigMigrationResult migrate_localflow_config(
    const ConfigMigrationOptions& opts) {

    ConfigMigrationResult result;

    if (opts.source_workflows.empty() ||
        !fs::exists(opts.source_workflows)) {
        result.messages.push_back({MigrationMessage::Level::kError,
            opts.source_workflows.string(),
            "Source workflows directory does not exist"});
        return result;
    }

    auto out_dir = opts.output_dir / "workflows";
    std::error_code ec;

    for (auto& entry : fs::directory_iterator(opts.source_workflows, ec)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext != ".yaml" && ext != ".yml") continue;

        try {
            YAML::Node source = YAML::LoadFile(entry.path().string());

            // LocalFlow YAML is structurally similar to Kairos YAML.
            // Main differences:
            //   1. `eval_condition` → `condition` (KEL)
            //   2. `python_eval` → flagged as needing manual rewrite
            //   3. `needs` → `depends_on` (same semantics)

            YAML::Emitter yaml;
            yaml << YAML::Comment("Kairos workflow — migrated from LocalFlow");
            yaml << YAML::Comment("Source: " + entry.path().string());
            yaml << YAML::Comment("Migrated: " + now_iso8601());
            yaml << YAML::Newline;

            // Copy through with field renaming.
            // For simplicity, we re-emit the YAML with known transformations.
            yaml << YAML::BeginMap;

            // Copy workflows array.
            if (source["workflows"]) {
                yaml << YAML::Key << "workflows"
                     << YAML::Value << YAML::BeginSeq;

                for (const auto& wf : source["workflows"]) {
                    yaml << YAML::BeginMap;

                    for (auto it = wf.begin(); it != wf.end(); ++it) {
                        auto key = it->first.as<std::string>();

                        if (key == "eval_condition" ||
                            key == "python_eval") {
                            // Rename to condition; warn if Python.
                            auto val = it->second.as<std::string>();
                            yaml << YAML::Key << "condition"
                                 << YAML::Value << val;

                            if (key == "python_eval" ||
                                val.find("import ") != std::string::npos ||
                                val.find("def ") != std::string::npos) {
                                result.messages.push_back({
                                    MigrationMessage::Level::kWarning,
                                    "workflow '" +
                                        (wf["name"]
                                             ? wf["name"].as<std::string>()
                                             : "?") + "'",
                                    "Python eval condition not supported in KEL: \""
                                    + val + "\". Rewrite in KEL syntax."
                                });
                            }
                        } else if (key == "needs") {
                            yaml << YAML::Key << "depends_on"
                                 << YAML::Value << it->second;
                        } else {
                            yaml << YAML::Key << key
                                 << YAML::Value << it->second;
                        }
                    }

                    yaml << YAML::EndMap;
                }
                yaml << YAML::EndSeq;
            }

            // Copy triggers if present.
            if (source["triggers"]) {
                yaml << YAML::Key << "triggers"
                     << YAML::Value << source["triggers"];
            }

            yaml << YAML::EndMap;

            auto out_path = out_dir / entry.path().filename();

            if (!opts.dry_run) {
                if (!write_file(out_path, yaml.c_str(), result.messages)) {
                    continue;  // Non-fatal: continue with other files.
                }
            }
            result.files_created.push_back(out_path);
            ++result.entities_migrated;

        } catch (const YAML::Exception& e) {
            result.messages.push_back({MigrationMessage::Level::kWarning,
                entry.path().string(),
                "YAML parse error: " + std::string(e.what()) +
                " — skipping file"});
        }
    }

    result.success = true;
    return result;
}

// ── Config migration dispatch ────────────────────────────────────────────

ConfigMigrationResult migrate_config(const ConfigMigrationOptions& opts) {
    switch (opts.source) {
        case SourceTool::kAVScheduler:
            return migrate_avscheduler_config(opts);
        case SourceTool::kEventWatcher:
            return migrate_eventwatcher_config(opts);
        case SourceTool::kLocalFlow:
            return migrate_localflow_config(opts);
    }
    ConfigMigrationResult r;
    r.messages.push_back({MigrationMessage::Level::kError,
        "", "Unknown source tool"});
    return r;
}

// ══════════════════════════════════════════════════════════════════════════
// DATABASE MIGRATION
// ══════════════════════════════════════════════════════════════════════════

// ── AVScheduler DB Migration (§31.5) ─────────────────────────────────────

static DbMigrationResult migrate_avscheduler_db(
    const DbMigrationOptions& opts) {

    DbMigrationResult result;
    auto migrated_at = now_iso8601();

    try {
        // Open source database (read-only).
        SQLite::Database source_db(opts.source_db.string(),
                                    SQLite::OPEN_READONLY);

        // Open target database (read-write).
        SQLite::Database target_db(opts.target_db.string(),
                                    SQLite::OPEN_READWRITE);

        // AVScheduler schema: job_execution_logs table
        //   id, job_name, started_at, finished_at, exit_code,
        //   stdout, stderr, status (0=fail, 1=success)

        // Check if source table exists.
        SQLite::Statement check(source_db,
            "SELECT count(*) FROM sqlite_master "
            "WHERE type='table' AND name='job_execution_logs'");
        check.executeStep();
        if (check.getColumn(0).getInt() == 0) {
            result.messages.push_back({MigrationMessage::Level::kWarning,
                "job_execution_logs",
                "Table not found in source database — nothing to migrate"});
            result.success = true;
            return result;
        }

        // Prepare target inserts.
        SQLite::Statement insert_run(target_db,
            "INSERT OR IGNORE INTO runs "
            "(id, entity_type, entity_id, trigger_type, status, "
            " started_at, finished_at, metadata) "
            "VALUES (?, 'job', ?, 'migration', ?, ?, ?, ?)");

        SQLite::Statement insert_step(target_db,
            "INSERT OR IGNORE INTO run_steps "
            "(id, run_id, job_name, step_name, status, "
            " started_at, finished_at, exit_code) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");

        SQLite::Statement insert_output(target_db,
            "INSERT OR IGNORE INTO step_outputs "
            "(step_id, stream, data) VALUES (?, ?, ?)");

        // Check for duplicates.
        SQLite::Statement check_dup(target_db,
            "SELECT count(*) FROM runs "
            "WHERE entity_id = ? AND started_at = ? "
            "AND metadata LIKE '%migrated_from%'");

        // Read source records.
        SQLite::Statement query(source_db,
            "SELECT id, job_name, started_at, finished_at, "
            "exit_code, stdout, stderr, status "
            "FROM job_execution_logs ORDER BY id");

        SQLite::Transaction txn(target_db);
        int batch = 0;

        while (query.executeStep()) {
            auto src_id = query.getColumn(0).getInt64();
            auto job_name = query.getColumn(1).getString();
            auto started_at = query.getColumn(2).getString();
            auto finished_at = query.getColumn(3).getString();
            int exit_code = query.getColumn(4).getInt();
            auto stdout_data = query.getColumn(5).getString();
            auto stderr_data = query.getColumn(6).getString();
            int status = query.getColumn(7).getInt();

            // Check for duplicate.
            check_dup.reset();
            check_dup.bind(1, job_name);
            check_dup.bind(2, started_at);
            check_dup.executeStep();
            if (check_dup.getColumn(0).getInt() > 0) {
                ++result.duplicates_skipped;
                continue;
            }

            // Generate IDs.
            auto run_id = "migrated_avs_" + std::to_string(src_id);
            auto step_id = run_id + "_step";

            std::string kairos_status =
                (status == 1) ? "SUCCESS" : "FAILURE";

            json metadata = {
                {"migrated_from", "avscheduler"},
                {"migrated_at", migrated_at},
                {"source_id", src_id}
            };

            // Insert run.
            insert_run.reset();
            insert_run.bind(1, run_id);
            insert_run.bind(2, job_name);
            insert_run.bind(3, kairos_status);
            insert_run.bind(4, started_at);
            insert_run.bind(5, finished_at);
            insert_run.bind(6, metadata.dump());
            insert_run.exec();
            ++result.runs_imported;

            // Insert step.
            insert_step.reset();
            insert_step.bind(1, step_id);
            insert_step.bind(2, run_id);
            insert_step.bind(3, job_name);
            insert_step.bind(4, job_name);
            insert_step.bind(5, kairos_status);
            insert_step.bind(6, started_at);
            insert_step.bind(7, finished_at);
            insert_step.bind(8, exit_code);
            insert_step.exec();
            ++result.steps_imported;

            // Insert stdout/stderr.
            if (!stdout_data.empty()) {
                insert_output.reset();
                insert_output.bind(1, step_id);
                insert_output.bind(2, "stdout");
                insert_output.bind(3, stdout_data);
                insert_output.exec();
            }
            if (!stderr_data.empty()) {
                insert_output.reset();
                insert_output.bind(1, step_id);
                insert_output.bind(2, "stderr");
                insert_output.bind(3, stderr_data);
                insert_output.exec();
            }

            if (++batch >= 1000) {
                txn.commit();
                batch = 0;
                // Start new transaction.
                // Note: SQLiteCpp Transaction doesn't support re-begin,
                // so we commit in batches by using raw exec.
                target_db.exec("BEGIN");
            }
        }

        if (batch > 0) {
            txn.commit();
        }

    } catch (const SQLite::Exception& e) {
        result.messages.push_back({MigrationMessage::Level::kError,
            "database",
            "SQLite error: " + std::string(e.what())});
        return result;
    }

    result.success = true;
    return result;
}

// ── EventWatcher DB Migration (§31.5) ────────────────────────────────────

static DbMigrationResult migrate_eventwatcher_db(
    const DbMigrationOptions& opts) {

    DbMigrationResult result;
    auto migrated_at = now_iso8601();

    try {
        SQLite::Database source_db(opts.source_db.string(),
                                    SQLite::OPEN_READONLY);
        SQLite::Database target_db(opts.target_db.string(),
                                    SQLite::OPEN_READWRITE);

        // ── Migrate events ────────────────────────────────────────────
        {
            SQLite::Statement check(source_db,
                "SELECT count(*) FROM sqlite_master "
                "WHERE type='table' AND name='events'");
            check.executeStep();

            if (check.getColumn(0).getInt() > 0) {
                SQLite::Statement query(source_db,
                    "SELECT id, watch_group, rule_name, event_type, "
                    "timestamp, details FROM events ORDER BY id");

                SQLite::Statement insert(target_db,
                    "INSERT OR IGNORE INTO watch_events "
                    "(id, group_name, rule_name, event_type, "
                    " detected_at, details) "
                    "VALUES (?, ?, ?, ?, ?, ?)");

                SQLite::Transaction txn(target_db);

                while (query.executeStep()) {
                    auto id = "migrated_ew_evt_" +
                        std::to_string(query.getColumn(0).getInt64());
                    insert.reset();
                    insert.bind(1, id);
                    insert.bind(2, query.getColumn(1).getString());
                    insert.bind(3, query.getColumn(2).getString());
                    insert.bind(4, query.getColumn(3).getString());
                    insert.bind(5, query.getColumn(4).getString());
                    insert.bind(6, query.getColumn(5).getString());
                    insert.exec();
                    ++result.events_imported;
                }

                txn.commit();
            }
        }

        // ── Migrate samples ───────────────────────────────────────────
        {
            SQLite::Statement check(source_db,
                "SELECT count(*) FROM sqlite_master "
                "WHERE type='table' AND name='samples_json'");
            check.executeStep();

            if (check.getColumn(0).getInt() > 0) {
                SQLite::Statement query(source_db,
                    "SELECT id, watch_group, sampled_at, data "
                    "FROM samples_json ORDER BY id");

                SQLite::Statement insert(target_db,
                    "INSERT OR IGNORE INTO watch_samples "
                    "(id, group_name, sampled_at, snapshot_data) "
                    "VALUES (?, ?, ?, ?)");

                SQLite::Transaction txn(target_db);

                while (query.executeStep()) {
                    auto id = "migrated_ew_smp_" +
                        std::to_string(query.getColumn(0).getInt64());
                    insert.reset();
                    insert.bind(1, id);
                    insert.bind(2, query.getColumn(1).getString());
                    insert.bind(3, query.getColumn(2).getString());
                    insert.bind(4, query.getColumn(3).getString());
                    insert.exec();
                    ++result.samples_imported;
                }

                txn.commit();
            }
        }

    } catch (const SQLite::Exception& e) {
        result.messages.push_back({MigrationMessage::Level::kError,
            "database",
            "SQLite error: " + std::string(e.what())});
        return result;
    }

    result.success = true;
    return result;
}

// ── DB migration dispatch ────────────────────────────────────────────────

DbMigrationResult migrate_db(const DbMigrationOptions& opts) {
    // Validate inputs.
    DbMigrationResult r;

    if (!fs::exists(opts.source_db)) {
        r.messages.push_back({MigrationMessage::Level::kError,
            opts.source_db.string(),
            "Source database does not exist"});
        return r;
    }

    if (!fs::exists(opts.target_db)) {
        r.messages.push_back({MigrationMessage::Level::kError,
            opts.target_db.string(),
            "Target database does not exist. Run `kairos init-db` first."});
        return r;
    }

    if (opts.dry_run) {
        // For dry run, just count source records.
        try {
            SQLite::Database source_db(opts.source_db.string(),
                                        SQLite::OPEN_READONLY);
            switch (opts.source) {
                case SourceTool::kAVScheduler: {
                    SQLite::Statement q(source_db,
                        "SELECT count(*) FROM job_execution_logs");
                    q.executeStep();
                    r.runs_imported = q.getColumn(0).getInt();
                    r.steps_imported = r.runs_imported;
                    break;
                }
                case SourceTool::kEventWatcher: {
                    try {
                        SQLite::Statement q1(source_db,
                            "SELECT count(*) FROM events");
                        q1.executeStep();
                        r.events_imported = q1.getColumn(0).getInt();
                    } catch (...) {}
                    try {
                        SQLite::Statement q2(source_db,
                            "SELECT count(*) FROM samples_json");
                        q2.executeStep();
                        r.samples_imported = q2.getColumn(0).getInt();
                    } catch (...) {}
                    break;
                }
                case SourceTool::kLocalFlow:
                    r.messages.push_back({MigrationMessage::Level::kInfo,
                        "localflow",
                        "LocalFlow has no database — nothing to migrate"});
                    break;
            }
        } catch (const SQLite::Exception& e) {
            r.messages.push_back({MigrationMessage::Level::kError,
                "database", "SQLite error: " + std::string(e.what())});
            return r;
        }
        r.success = true;
        return r;
    }

    switch (opts.source) {
        case SourceTool::kAVScheduler:
            return migrate_avscheduler_db(opts);
        case SourceTool::kEventWatcher:
            return migrate_eventwatcher_db(opts);
        case SourceTool::kLocalFlow:
            r.messages.push_back({MigrationMessage::Level::kInfo,
                "localflow",
                "LocalFlow has no database — nothing to migrate"});
            r.success = true;
            return r;
    }
    r.messages.push_back({MigrationMessage::Level::kError,
        "", "Unknown source tool"});
    return r;
}

}  // namespace kairos::migration
