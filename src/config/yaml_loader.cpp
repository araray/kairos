/// src/config/yaml_loader.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  yaml_loader.cpp — Parse workflow/watch-group YAML files                  ║
// ║                                                                           ║
// ║  Uses yaml-cpp to parse, then maps to engine types. Validates schema,     ║
// ║  generates content-addressable IDs, constructs DAGs, extracts triggers.   ║
// ║                                                                           ║
// ║  Spec reference: §4.5 (workflow), §4.6 (watch groups), §6.7 (init)      ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/config/yaml_loader.hpp"
#include "kairos/core/id_generator.hpp"
#include "kairos/engine/dag.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace kairos::config {

using namespace kairos::engine;
using namespace kairos::watch;
using namespace kairos::core;

// ── Helpers ─────────────────────────────────────────────────────────────

namespace {

/// Read an optional string field, return default if missing.
std::string opt_string(const YAML::Node& node, const std::string& key,
                       const std::string& def = "")
{
    if (node[key] && node[key].IsScalar()) {
        return node[key].as<std::string>();
    }
    return def;
}

/// Read an optional int field.
int opt_int(const YAML::Node& node, const std::string& key, int def = 0) {
    if (node[key] && node[key].IsScalar()) {
        return node[key].as<int>();
    }
    return def;
}

/// Read an optional bool field.
bool opt_bool(const YAML::Node& node, const std::string& key, bool def = false) {
    if (node[key] && node[key].IsScalar()) {
        return node[key].as<bool>();
    }
    return def;
}

/// Read a string vector from a YAML sequence node.
std::vector<std::string> read_string_vec(const YAML::Node& node) {
    std::vector<std::string> result;
    if (node && node.IsSequence()) {
        for (const auto& item : node) {
            if (item.IsScalar()) {
                result.push_back(item.as<std::string>());
            }
        }
    }
    return result;
}

/// Read an env map (string → string) from a YAML mapping node.
std::unordered_map<std::string, std::string> read_env_map(
    const YAML::Node& node)
{
    std::unordered_map<std::string, std::string> result;
    if (node && node.IsMap()) {
        for (const auto& kv : node) {
            result[kv.first.as<std::string>()] = kv.second.as<std::string>();
        }
    }
    return result;
}

/// Parse a duration string like "300", "5m", "1h", "30s" into seconds.
/// Returns seconds. Falls back to interpreting as raw seconds.
std::chrono::seconds parse_duration_seconds(const std::string& s) {
    if (s.empty()) return std::chrono::seconds{0};

    // Try regex: number + optional unit suffix.
    static const std::regex re(R"((\d+)\s*(s|m|h|d)?)");
    std::smatch match;
    if (std::regex_match(s, match, re)) {
        int64_t val = std::stoll(match[1].str());
        std::string unit = match[2].str();
        if (unit == "m") return std::chrono::seconds{val * 60};
        if (unit == "h") return std::chrono::seconds{val * 3600};
        if (unit == "d") return std::chrono::seconds{val * 86400};
        return std::chrono::seconds{val};  // "s" or no suffix.
    }

    // Fallback: parse as integer seconds.
    try {
        return std::chrono::seconds{std::stoll(s)};
    } catch (...) {
        return std::chrono::seconds{0};
    }
}

/// Parse a timeout that might be int or string.
std::optional<std::chrono::seconds> parse_timeout(
    const YAML::Node& node, const std::string& key)
{
    if (!node[key]) return std::nullopt;
    if (node[key].IsScalar()) {
        auto s = node[key].as<std::string>();
        // Try as pure integer first.
        try {
            auto val = std::stoll(s);
            return std::chrono::seconds{val};
        } catch (...) {}
        // Try as duration string.
        auto dur = parse_duration_seconds(s);
        if (dur.count() > 0) return dur;
    }
    return std::nullopt;
}

/// Generate the canonical string for a step's content-hash input.
/// Combines command + working_dir + env to produce a deterministic ID.
std::string step_hash_input(const StepDef& step, const std::string& job_id) {
    std::string buf;
    buf += job_id;
    buf += '\0';
    buf += step.step_name;
    buf += '\0';
    buf += step.command;
    buf += '\0';
    buf += step.working_dir.string();
    return buf;
}

/// Generate the canonical string for a job's content-hash input.
std::string job_hash_input(const JobDef& job, const std::string& workflow_id) {
    std::string buf;
    buf += workflow_id;
    buf += '\0';
    buf += job.job_name;
    // Include step commands so different steps produce different job IDs.
    for (const auto& step : job.steps) {
        buf += '\0';
        buf += step.command;
    }
    return buf;
}

/// Generate the canonical string for a workflow's content-hash input.
std::string workflow_hash_input(const std::string& name,
                                 const std::vector<std::string>& job_names)
{
    std::string buf;
    buf += name;
    for (const auto& jn : job_names) {
        buf += '\0';
        buf += jn;
    }
    return buf;
}

}  // anonymous namespace

// ── Parse a single step from YAML ───────────────────────────────────────

namespace {

StepDef parse_step(const YAML::Node& node, const std::string& job_id,
                   int step_index,
                   std::vector<YamlError>& errors,
                   const std::string& file)
{
    StepDef step;

    std::string path = "jobs." + job_id + ".steps[" +
                       std::to_string(step_index) + "]";

    // Name is required per spec — but fall back to "step_N".
    step.step_name = opt_string(node, "name",
                                "step_" + std::to_string(step_index));

    // Command: "run" key.
    if (node["run"] && node["run"].IsScalar()) {
        step.command = node["run"].as<std::string>();
    } else {
        errors.push_back({file, path + ".run",
                          "Step must have a 'run' field with a command string"});
    }

    // Working directory.
    step.working_dir = opt_string(node, "working_dir", "");

    // Environment variables.
    step.env = read_env_map(node["env"]);

    // Timeout.
    step.timeout = parse_timeout(node, "timeout_seconds");

    // Shell mode (default true per spec §14.4).
    step.use_shell = opt_bool(node, "use_shell", true);

    // Generate content-addressable ID.
    step.step_id = generate_content_id(
        EntityType::kStep, step_hash_input(step, job_id));

    return step;
}

// ── Parse a single job from YAML ────────────────────────────────────────

JobDef parse_job(const std::string& job_key, const YAML::Node& node,
                 const std::string& workflow_id,
                 std::vector<YamlError>& errors,
                 const std::string& file)
{
    JobDef job;

    std::string path = "jobs." + job_key;

    // Name: use explicit 'name' or fall back to the key.
    job.job_name = opt_string(node, "name", job_key);

    // Needs (dependencies).
    job.needs = read_string_vec(node["needs"]);

    // Condition (KEL expression).
    if (node["condition"] && node["condition"].IsScalar()) {
        job.condition_expr = node["condition"].as<std::string>();
    }

    // continue_on_error.
    job.continue_on_error = opt_bool(node, "continue_on_error", false);

    // Job-level env.
    job.env = read_env_map(node["env"]);

    // Working directory.
    job.working_dir = opt_string(node, "working_dir", "");

    // Steps — required.
    if (!node["steps"] || !node["steps"].IsSequence()) {
        errors.push_back({file, path + ".steps",
                          "Job must have a 'steps' sequence"});
    } else {
        int idx = 0;
        for (const auto& step_node : node["steps"]) {
            job.steps.push_back(
                parse_step(step_node, job_key, idx, errors, file));
            ++idx;
        }
        if (job.steps.empty()) {
            errors.push_back({file, path + ".steps",
                              "Job must have at least one step"});
        }
    }

    // Generate content-addressable ID.
    job.job_id = generate_content_id(
        EntityType::kJob, job_hash_input(job, workflow_id));

    return job;
}

// ── Parse triggers from YAML ────────────────────────────────────────────

std::vector<TimerEntry> parse_triggers(
    const YAML::Node& triggers_node,
    const std::string& target_id,
    const std::string& target_name,
    TriggerEvent::TargetKind target_kind,
    std::vector<YamlError>& errors,
    const std::string& file)
{
    std::vector<TimerEntry> entries;

    if (!triggers_node || !triggers_node.IsSequence()) {
        return entries;  // No triggers — valid (manual-only workflows).
    }

    int idx = 0;
    for (const auto& trig : triggers_node) {
        std::string path = "triggers[" + std::to_string(idx) + "]";
        ++idx;

        TimerEntry entry;
        entry.target_id = target_id;
        entry.target_name = target_name;
        entry.target_kind = target_kind;
        entry.enabled = opt_bool(trig, "enabled", true);

        // Misfire policy.
        entry.misfire_policy = parse_misfire_policy(
            opt_string(trig, "misfire_policy", "coalesce"));

        // Max instances.
        entry.max_instances = opt_int(trig, "max_instances", 1);

        // Determine trigger type.
        std::string type = opt_string(trig, "type", "");

        if (type == "cron" || trig["cron"]) {
            std::string expr;
            if (trig["cron"] && trig["cron"].IsScalar()) {
                expr = trig["cron"].as<std::string>();
            } else if (trig["spec"] && trig["spec"].IsScalar()) {
                expr = trig["spec"].as<std::string>();
            } else {
                errors.push_back({file, path,
                    "Cron trigger must have 'cron' or 'spec' expression"});
                continue;
            }

            CronTrigger ct;
            ct.expression = expr;
            entry.spec = ct;

            // Generate trigger ID from expression + target.
            entry.trigger_id = generate_content_id(
                EntityType::kTrigger,
                std::vector<std::string_view>{"cron", expr, target_id});

        } else if (type == "interval" || trig["interval"]) {
            std::string interval_str;
            if (trig["interval"] && trig["interval"].IsScalar()) {
                interval_str = trig["interval"].as<std::string>();
            } else {
                errors.push_back({file, path,
                    "Interval trigger must have 'interval' duration"});
                continue;
            }

            auto secs = parse_duration_seconds(interval_str);
            IntervalTrigger it;
            it.interval = std::chrono::duration_cast<
                std::chrono::milliseconds>(secs);
            it.align_to_start = opt_bool(trig, "align_to_start", false);
            entry.spec = it;

            entry.trigger_id = generate_content_id(
                EntityType::kTrigger,
                std::vector<std::string_view>{
                    "interval", interval_str, target_id});

        } else if (type == "date" || trig["date"]) {
            // Date triggers are parsed but not commonly used.
            // Accept ISO 8601 string.
            std::string date_str;
            if (trig["date"] && trig["date"].IsScalar()) {
                date_str = trig["date"].as<std::string>();
            } else if (trig["fire_at"] && trig["fire_at"].IsScalar()) {
                date_str = trig["fire_at"].as<std::string>();
            } else {
                errors.push_back({file, path,
                    "Date trigger must have 'date' or 'fire_at' ISO 8601 time"});
                continue;
            }

            // Parse ISO 8601 → time_t → system_clock::time_point.
            // Simplified parser: expects "YYYY-MM-DDTHH:MM:SS" or similar.
            std::tm tm_val{};
            std::istringstream iss(date_str);
            iss >> std::get_time(&tm_val, "%Y-%m-%dT%H:%M:%S");
            if (iss.fail()) {
                errors.push_back({file, path,
                    "Cannot parse date '" + date_str +
                    "' (expected ISO 8601: YYYY-MM-DDTHH:MM:SS)"});
                continue;
            }
            auto tt = std::mktime(&tm_val);

            DateTrigger dt;
            dt.fire_at = std::chrono::system_clock::from_time_t(tt);
            entry.spec = dt;

            entry.trigger_id = generate_content_id(
                EntityType::kTrigger,
                std::vector<std::string_view>{"date", date_str, target_id});

        } else if (type == "manual") {
            // Manual triggers don't create TimerEntry entries.
            // They're implicit (every workflow supports manual runs).
            continue;

        } else if (type == "watch_group") {
            // Watch-group triggers are handled by the watch engine,
            // not the scheduler. Skip here.
            continue;

        } else {
            errors.push_back({file, path,
                "Unknown trigger type '" + type + "'"});
            continue;
        }

        entries.push_back(std::move(entry));
    }

    return entries;
}

}  // anonymous namespace

// ── Parse a single workflow from a YAML root node ───────────────────────

namespace {

YamlLoadResult parse_workflow_node(
    const YAML::Node& root,
    const std::string& file)
{
    YamlLoadResult result;

    // ── Required fields ─────────────────────────────────────────
    std::string wf_name = opt_string(root, "name", "");
    if (wf_name.empty()) {
        result.errors.push_back({file, "name",
            "Workflow must have a 'name' field"});
        return result;
    }

    // ── Jobs ────────────────────────────────────────────────────
    if (!root["jobs"] || !root["jobs"].IsMap()) {
        result.errors.push_back({file, "jobs",
            "Workflow must have a 'jobs' mapping"});
        return result;
    }

    // Collect job names for the workflow hash.
    std::vector<std::string> job_names;
    for (const auto& kv : root["jobs"]) {
        job_names.push_back(kv.first.as<std::string>());
    }

    // Generate workflow ID early (needed for job hashing).
    std::string wf_id;
    if (root["id"] && root["id"].IsScalar()) {
        // If an explicit ID is provided, use it directly.
        wf_id = root["id"].as<std::string>();
    } else {
        wf_id = generate_content_id(
            EntityType::kWorkflow,
            workflow_hash_input(wf_name, job_names));
    }

    // Parse jobs.
    std::vector<JobDef> jobs;
    // Map from YAML key → job_id (for needs resolution).
    std::unordered_map<std::string, std::string> key_to_id;

    for (const auto& kv : root["jobs"]) {
        std::string key = kv.first.as<std::string>();
        auto job = parse_job(key, kv.second, wf_id,
                             result.errors, file);
        key_to_id[key] = job.job_id;
        jobs.push_back(std::move(job));
    }

    // Resolve `needs` references: YAML uses job keys (e.g., "build"),
    // which must be mapped to content-addressable IDs.
    // If a needs entry matches a YAML key, replace with the ID.
    for (auto& job : jobs) {
        for (auto& need : job.needs) {
            auto it = key_to_id.find(need);
            if (it != key_to_id.end()) {
                need = it->second;
            } else {
                // Check if it's already a valid ID (starts with "job-").
                if (need.substr(0, 4) != "job-") {
                    result.errors.push_back({file,
                        "jobs." + job.job_name + ".needs",
                        "Unknown dependency '" + need +
                        "' — not a known job key in this workflow"});
                }
            }
        }
    }

    // ── Build DAG ───────────────────────────────────────────────
    std::vector<DagNode> dag_nodes;
    dag_nodes.reserve(jobs.size());
    for (const auto& job : jobs) {
        DagNode dn;
        dn.job_id = job.job_id;
        dn.job_name = job.job_name;
        dn.needs = job.needs;
        dn.condition_expr = job.condition_expr;
        dn.continue_on_error = job.continue_on_error;
        dag_nodes.push_back(std::move(dn));
    }

    try {
        auto dag = WorkflowDag::build(dag_nodes);

        // Check for cycles (spec §6.7 step 5).
        auto cycle = dag.find_cycle();
        if (cycle) {
            std::string cycle_str;
            for (const auto& cid : *cycle) {
                if (!cycle_str.empty()) cycle_str += " → ";
                cycle_str += cid;
            }
            result.errors.push_back({file, "jobs",
                "Cycle detected in job dependencies: " + cycle_str});
            return result;
        }

        // ── Build workflow def ──────────────────────────────────
        WorkflowDef wf{
            wf_id,
            wf_name,
            std::move(jobs),
            std::move(dag)
        };

        // ── Extract triggers ────────────────────────────────────
        auto triggers = parse_triggers(
            root["triggers"], wf.workflow_id, wf.workflow_name,
            TriggerEvent::TargetKind::Workflow,
            result.errors, file);

        result.triggers = std::move(triggers);
        result.workflows.push_back(std::move(wf));

    } catch (const std::exception& e) {
        result.errors.push_back({file, "jobs",
            std::string("DAG construction failed: ") + e.what()});
        return result;
    }

    return result;
}

}  // anonymous namespace

// ── Public API: load_workflow_file ───────────────────────────────────────

YamlLoadResult load_workflow_file(const fs::path& path) {
    YamlLoadResult result;

    if (!fs::exists(path)) {
        result.errors.push_back({path.string(), "",
            "File not found: " + path.string()});
        return result;
    }

    try {
        YAML::Node root = YAML::LoadFile(path.string());
        auto r = parse_workflow_node(root, path.string());

        // Merge into result.
        result.workflows = std::move(r.workflows);
        result.triggers = std::move(r.triggers);
        result.errors = std::move(r.errors);

    } catch (const YAML::Exception& e) {
        result.errors.push_back({path.string(), "",
            "YAML parse error: " + std::string(e.what())});
    }

    return result;
}

// ── Public API: load_workflow_string ─────────────────────────────────────

YamlLoadResult load_workflow_string(const std::string& yaml_content,
                                     const std::string& source_name) {
    YamlLoadResult result;

    try {
        YAML::Node root = YAML::Load(yaml_content);
        return parse_workflow_node(root, source_name);

    } catch (const YAML::Exception& e) {
        result.errors.push_back({source_name, "",
            "YAML parse error: " + std::string(e.what())});
    }

    return result;
}

// ── Public API: load_workflows_dir ──────────────────────────────────────

YamlLoadResult load_workflows_dir(const fs::path& dir) {
    YamlLoadResult result;

    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        result.errors.push_back({dir.string(), "",
            "Workflow directory not found: " + dir.string()});
        return result;
    }

    // Collect YAML files, sorted for determinism.
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        auto ext = entry.path().extension().string();
        if (ext == ".yaml" || ext == ".yml") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    // Duplicate ID tracking.
    std::unordered_map<std::string, std::string> seen_ids;  // id → file

    for (const auto& f : files) {
        auto r = load_workflow_file(f);

        // Check for duplicate IDs.
        for (const auto& wf : r.workflows) {
            auto it = seen_ids.find(wf.workflow_id);
            if (it != seen_ids.end()) {
                result.errors.push_back({f.string(), "id",
                    "Duplicate workflow ID '" + wf.workflow_id +
                    "' (also in " + it->second + ")"});
            } else {
                seen_ids[wf.workflow_id] = f.string();
                result.workflows.push_back(wf);
            }
        }

        // Merge triggers and errors.
        result.triggers.insert(result.triggers.end(),
            r.triggers.begin(), r.triggers.end());
        result.errors.insert(result.errors.end(),
            r.errors.begin(), r.errors.end());
    }

    return result;
}

// ── Parse watch group definitions ───────────────────────────────────────

namespace {

WatchGroupDef parse_watch_group_node(
    const YAML::Node& node,
    std::vector<YamlError>& errors,
    const std::string& file)
{
    WatchGroupDef group;

    // Name is required.
    group.group_name = opt_string(node, "name", "");
    if (group.group_name.empty()) {
        errors.push_back({file, "watch_groups[].name",
            "Watch group must have a 'name' field"});
    }

    // Enabled.
    group.enabled = opt_bool(node, "enabled", true);

    // Watch items (paths/globs).
    group.watch_items = read_string_vec(node["watch_items"]);
    // Also accept "paths" as an alias (Kairos migration format).
    if (group.watch_items.empty()) {
        group.watch_items = read_string_vec(node["paths"]);
    }
    if (group.watch_items.empty()) {
        errors.push_back({file,
            "watch_groups." + group.group_name + ".watch_items",
            "Watch group must have at least one watch_item or path"});
    }

    // Mode.
    group.mode = parse_watch_mode(
        opt_string(node, "mode", "hybrid"));

    // Scanning parameters.
    group.max_depth = opt_int(node, "max_depth", 10);

    // Sample rate: accept int (seconds) or string duration.
    if (node["sample_rate"] && node["sample_rate"].IsScalar()) {
        auto sr_str = node["sample_rate"].as<std::string>();
        auto dur = parse_duration_seconds(sr_str);
        group.sample_rate = dur.count() > 0 ? dur
            : std::chrono::seconds{300};
    } else if (node["sample_interval"] && node["sample_interval"].IsScalar()) {
        auto si_str = node["sample_interval"].as<std::string>();
        group.sample_rate = parse_duration_seconds(si_str);
    }

    // Max files.
    group.max_files = opt_int(node, "max_files", 100000);

    // Hash policy.
    group.hash_policy = parse_hash_policy(
        opt_string(node, "hash_policy", "size+mtime"));
    // Also accept compute_hashes: false → MtimeOnly.
    if (node["compute_hashes"] && node["compute_hashes"].IsScalar()) {
        if (!node["compute_hashes"].as<bool>()) {
            group.hash_policy = HashPolicy::MtimeOnly;
        }
    }

    // Pattern.
    if (node["pattern"] && node["pattern"].IsScalar()) {
        group.pattern = node["pattern"].as<std::string>();
    }

    // Exclude globs.
    group.exclude_globs = read_string_vec(node["exclude_globs"]);
    // Also accept "excludes".
    if (group.exclude_globs.empty()) {
        group.exclude_globs = read_string_vec(node["excludes"]);
    }

    // Symlink policy.
    auto sym_str = opt_string(node, "symlink_policy", "follow");
    if (sym_str == "follow" || sym_str == "true") {
        group.symlink_policy = SymlinkPolicy::Follow;
    } else {
        group.symlink_policy = SymlinkPolicy::NoFollow;
    }
    // Also accept "follow_symlinks" boolean.
    if (node["follow_symlinks"] && node["follow_symlinks"].IsScalar()) {
        group.symlink_policy = node["follow_symlinks"].as<bool>()
            ? SymlinkPolicy::Follow : SymlinkPolicy::NoFollow;
    }

    // Rules.
    if (node["rules"] && node["rules"].IsSequence()) {
        for (const auto& rule_node : node["rules"]) {
            WatchRuleDef rule;
            rule.rule_name = opt_string(rule_node, "name", "");
            if (rule.rule_name.empty()) {
                errors.push_back({file,
                    "watch_groups." + group.group_name + ".rules[].name",
                    "Watch rule must have a 'name' field"});
                continue;
            }

            rule.condition = opt_string(rule_node, "condition", "true");
            rule.severity = opt_string(rule_node, "severity", "info");
            rule.description = opt_string(rule_node, "description", "");

            // Event types filter.
            rule.event_types = read_string_vec(rule_node["event_types"]);

            // Trigger target.
            if (rule_node["trigger"] && rule_node["trigger"].IsMap()) {
                auto& trig = rule_node["trigger"];
                if (trig["workflow"]) {
                    rule.trigger_target = trig["workflow"].as<std::string>();
                    rule.trigger_is_workflow = true;
                } else if (trig["job"]) {
                    rule.trigger_target = trig["job"].as<std::string>();
                    rule.trigger_is_workflow = false;
                }
            } else if (rule_node["target"] && rule_node["target"].IsScalar()) {
                // Shorthand: "target: workflow_name".
                rule.trigger_target = rule_node["target"].as<std::string>();
                rule.trigger_is_workflow = true;
            }

            group.rules.push_back(std::move(rule));
        }
    }

    // Generate content-addressable ID.
    std::string hash_input = group.group_name;
    for (const auto& item : group.watch_items) {
        hash_input += '\0';
        hash_input += item;
    }
    group.group_id = generate_content_id(
        EntityType::kWatchGroup, hash_input);

    return group;
}

}  // anonymous namespace

// ── Public API: load_watch_groups_file ───────────────────────────────────

YamlLoadResult load_watch_groups_file(const fs::path& path) {
    YamlLoadResult result;

    if (!fs::exists(path)) {
        result.errors.push_back({path.string(), "",
            "File not found: " + path.string()});
        return result;
    }

    try {
        YAML::Node root = YAML::LoadFile(path.string());

        // Expect top-level "watch_groups" array.
        YAML::Node groups_node;
        if (root["watch_groups"] && root["watch_groups"].IsSequence()) {
            groups_node = root["watch_groups"];
        } else if (root.IsSequence()) {
            // Allow bare array at top level.
            groups_node = root;
        } else {
            result.errors.push_back({path.string(), "",
                "Expected 'watch_groups' sequence at top level"});
            return result;
        }

        for (const auto& node : groups_node) {
            auto group = parse_watch_group_node(
                node, result.errors, path.string());
            result.watch_groups.push_back(std::move(group));
        }

    } catch (const YAML::Exception& e) {
        result.errors.push_back({path.string(), "",
            "YAML parse error: " + std::string(e.what())});
    }

    return result;
}

// ── Public API: load_watch_groups_string ─────────────────────────────────

YamlLoadResult load_watch_groups_string(const std::string& yaml_content,
                                         const std::string& source_name) {
    YamlLoadResult result;

    try {
        YAML::Node root = YAML::Load(yaml_content);

        YAML::Node groups_node;
        if (root["watch_groups"] && root["watch_groups"].IsSequence()) {
            groups_node = root["watch_groups"];
        } else if (root.IsSequence()) {
            groups_node = root;
        } else {
            result.errors.push_back({source_name, "",
                "Expected 'watch_groups' sequence"});
            return result;
        }

        for (const auto& node : groups_node) {
            auto group = parse_watch_group_node(
                node, result.errors, source_name);
            result.watch_groups.push_back(std::move(group));
        }

    } catch (const YAML::Exception& e) {
        result.errors.push_back({source_name, "",
            "YAML parse error: " + std::string(e.what())});
    }

    return result;
}

// ── Public API: load_watch_groups_dir ────────────────────────────────────

YamlLoadResult load_watch_groups_dir(const fs::path& dir) {
    YamlLoadResult result;

    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        result.errors.push_back({dir.string(), "",
            "Watch groups directory not found: " + dir.string()});
        return result;
    }

    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        auto ext = entry.path().extension().string();
        if (ext == ".yaml" || ext == ".yml") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    for (const auto& f : files) {
        auto r = load_watch_groups_file(f);
        result.watch_groups.insert(result.watch_groups.end(),
            r.watch_groups.begin(), r.watch_groups.end());
        result.errors.insert(result.errors.end(),
            r.errors.begin(), r.errors.end());
    }

    return result;
}

// ── Public API: load_all ────────────────────────────────────────────────

YamlLoadResult load_all(const fs::path& workflows_dir,
                          const fs::path& watch_groups_path) {
    YamlLoadResult result;

    // Load workflows.
    if (fs::exists(workflows_dir) && fs::is_directory(workflows_dir)) {
        auto wr = load_workflows_dir(workflows_dir);
        result.workflows = std::move(wr.workflows);
        result.triggers = std::move(wr.triggers);
        result.errors = std::move(wr.errors);
    }

    // Load watch groups: path can be a file or directory.
    if (fs::exists(watch_groups_path)) {
        YamlLoadResult wgr;
        if (fs::is_directory(watch_groups_path)) {
            wgr = load_watch_groups_dir(watch_groups_path);
        } else {
            wgr = load_watch_groups_file(watch_groups_path);
        }
        result.watch_groups = std::move(wgr.watch_groups);
        result.errors.insert(result.errors.end(),
            wgr.errors.begin(), wgr.errors.end());
    }

    return result;
}

}  // namespace kairos::config
