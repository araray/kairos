/// include/kairos/watch/watch_group_def.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/watch/watch_group_def.hpp — Watch group and rule definitions     ║
// ║                                                                           ║
// ║  Parsed from YAML watch-group configuration. Each watch group defines    ║
// ║  paths to monitor, scanning parameters, and KEL rules that trigger       ║
// ║  events when matched.                                                    ║
// ║                                                                           ║
// ║  Spec reference: §12.11, §12.12                                          ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/watch/sample.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace kairos::watch {

/// A single rule within a watch group.
/// Rules are KEL expressions evaluated against each diff entry.
struct WatchRuleDef {
    std::string rule_name;           ///< Human-readable name.
    std::string condition;           ///< KEL expression.
    std::string severity = "info";   ///< "info" | "warning" | "critical"
    std::string description;         ///< Human-readable description.

    /// Which event types this rule applies to (empty = all).
    std::vector<std::string> event_types;

    /// Target workflow/job to trigger when this rule fires.
    std::optional<std::string> trigger_target;

    /// Whether the target is a workflow or standalone job.
    bool trigger_is_workflow = true;
};

/// Watch mode: how the watch group detects changes.
enum class WatchMode {
    Native,   ///< Native OS events only (inotify/FSEvents/RDCW).
    Sample,   ///< Periodic sample+diff only.
    Hybrid,   ///< Both native + periodic (default per spec §12.7).
};

/// Parse watch mode from string.
[[nodiscard]] inline WatchMode parse_watch_mode(std::string_view s) {
    if (s == "native") return WatchMode::Native;
    if (s == "sample") return WatchMode::Sample;
    if (s == "hybrid") return WatchMode::Hybrid;
    return WatchMode::Hybrid;  // Default per spec.
}

/// Symlink follow policy per spec §12.10.
enum class SymlinkPolicy {
    Follow,    ///< Resolve symlinks (default). Risk: symlink cycles.
    NoFollow,  ///< Monitor symlinks themselves.
};

/// A complete watch group definition.
struct WatchGroupDef {
    std::string group_id;            ///< Content-addressable ID (wg-xxx).
    std::string group_name;          ///< Human-readable name.

    /// Paths or glob patterns to watch.
    std::vector<std::string> watch_items;

    /// Monitoring mode.
    WatchMode mode = WatchMode::Hybrid;

    /// Scanning parameters.
    int max_depth = 10;
    std::chrono::seconds sample_rate{300};  ///< Seconds between scans.
    int max_files = 100000;

    /// Hash computation policy.
    HashPolicy hash_policy = HashPolicy::SizePlusMtime;

    /// Optional regex pattern for pattern_found detection.
    std::optional<std::string> pattern;

    /// Exclude globs.
    std::vector<std::string> exclude_globs;

    /// Symlink handling.
    SymlinkPolicy symlink_policy = SymlinkPolicy::Follow;

    /// Rules evaluated against diffs.
    std::vector<WatchRuleDef> rules;

    /// Whether the group is enabled.
    bool enabled = true;

    /// §6 Tags from YAML `tags:` array.
    std::vector<std::string> tags;
};

/// Result of a triggered watch rule, ready for persistence and
/// TriggerEvent emission per spec §12.14.
struct WatchTriggerResult {
    std::string rule_name;
    std::string watch_group_name;
    std::vector<std::string> affected_paths;
    std::string event_type;         ///< Stringified WatchEventType flags.
    std::string severity;           ///< From rule definition.
    std::optional<std::string> trigger_target;
    bool trigger_is_workflow = true;
};

/// Status of a single watch group (for diagnostics/MCP).
struct WatchGroupStatus {
    std::string group_name;
    std::string mode;               ///< "native" | "sample" | "hybrid"
    int watched_paths = 0;
    int files_in_last_sample = 0;
    std::string last_scan_time;     ///< ISO 8601.
    std::string next_scan_time;     ///< ISO 8601.
    int events_last_hour = 0;
    std::string status;             ///< "active" | "error" | "overflow_recovery"
};

}  // namespace kairos::watch
