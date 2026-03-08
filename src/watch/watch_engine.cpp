/// src/watch/watch_engine.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  watch_engine.cpp — Watch engine coordinator                              ║
// ║                                                                           ║
// ║  Main loop: sleep until next group's sample time → collect sample →      ║
// ║  compute diff → evaluate rules → emit TriggerEvents → persist.           ║
// ║                                                                           ║
// ║  EventWatcher parity: first scan is baseline (no events). Events start   ║
// ║  from the second scan onwards.                                            ║
// ║                                                                           ║
// ║  Spec reference: §12.1–§12.15                                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/watch/watch_engine.hpp"
#include "kairos/core/id_generator.hpp"
#include "kairos/kel/evaluator.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace kairos::watch {

using namespace std::chrono_literals;
namespace engine = kairos::engine;

// ── Internal helpers (anonymous namespace) ──────────────────────────────

namespace {

/// Check if a rule's event_types filter matches the given event string.
/// Empty event_types means "match all".
bool rule_matches_event(const WatchRuleDef& rule,
                        const std::string& event_str)
{
    if (rule.event_types.empty()) return true;

    for (const auto& et : rule.event_types) {
        if (event_str.find(et) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// Simple glob matching: * matches any chars, ? matches one char.
/// No ** or character classes in v1 (per spec §12.6).
bool glob_match_simple(const std::string& pattern, const std::string& str) {
    size_t pi = 0, si = 0;
    size_t star_pi = std::string::npos, star_si = 0;

    while (si < str.size()) {
        if (pi < pattern.size() &&
            (pattern[pi] == str[si] || pattern[pi] == '?')) {
            ++pi;
            ++si;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            star_pi = pi;
            star_si = si;
            ++pi;
        } else if (star_pi != std::string::npos) {
            pi = star_pi + 1;
            ++star_si;
            si = star_si;
        } else {
            return false;
        }
    }

    while (pi < pattern.size() && pattern[pi] == '*') ++pi;
    return pi == pattern.size();
}

/// Build a KEL evaluation context for a watch rule.
/// Binds: watch_group (string), event (string), file (map-like via members),
/// prev_file (map-like via members).
///
/// Per spec §12.11, the context type 2 includes:
///   data, event, watch_group, file, prev_file
///
/// Also registers aggregate(data, glob, metric, func) which operates on
/// the sample passed by reference through the closure.
kel::EvalContext build_watch_kel_context(
    const std::string& group_name,
    const std::string& event_type,
    const FileMetrics& file_metrics,
    const FileMetrics* prev_metrics,
    const Sample* current_sample = nullptr)
{
    auto ctx = kel::make_default_context();

    // Scalar variables.
    ctx.variables["watch_group"] = kel::KelValue(group_name);
    ctx.variables["event"] = kel::KelValue(event_type);

    // File metrics as flat variables for KEL expressions like
    // "file_size > X", "file_type == 'file'", etc.
    // KEL v1 doesn't support dot-access on maps; flat variables
    // provide equivalent functionality per spec §12.11.
    ctx.variables["file_size"] = kel::KelValue(file_metrics.size);
    ctx.variables["file_type"] = kel::KelValue(file_metrics.entry_type);
    ctx.variables["file_path"] = kel::KelValue(file_metrics.path);
    ctx.variables["file_permissions"] = kel::KelValue(file_metrics.permissions);

    if (file_metrics.pattern_found.has_value()) {
        ctx.variables["file_pattern_found"] =
            kel::KelValue(*file_metrics.pattern_found);
    } else {
        ctx.variables["file_pattern_found"] = kel::KelValue(false);
    }

    if (file_metrics.md5.has_value()) {
        ctx.variables["file_md5"] = kel::KelValue(*file_metrics.md5);
    }
    if (file_metrics.sha256.has_value()) {
        ctx.variables["file_sha256"] = kel::KelValue(*file_metrics.sha256);
    }

    // Prev file metrics (if available).
    if (prev_metrics) {
        ctx.variables["prev_file_size"] = kel::KelValue(prev_metrics->size);
        ctx.variables["prev_file_type"] = kel::KelValue(prev_metrics->entry_type);
        if (prev_metrics->pattern_found.has_value()) {
            ctx.variables["prev_file_pattern_found"] =
                kel::KelValue(*prev_metrics->pattern_found);
        }
    }

    // Data sentinel — per spec §7.8 context 2, `data` is a variable
    // passed as the first arg to aggregate(). The actual sample data
    // is captured by the aggregate() closure below.
    ctx.variables["data"] = kel::KelValue(std::string("__sample_data__"));

    // aggregate(data, glob, metric, func) — spec §7.7 category 3.
    // Operates on the captured current_sample.
    if (current_sample) {
        ctx.functions["aggregate"] =
            [current_sample](const std::vector<kel::KelValue>& args)
                -> kel::KelValue
        {
            if (args.size() != 4) {
                throw kel::KelEvalError(
                    "aggregate() requires 4 arguments: "
                    "(data, glob, metric, func)");
            }
            // args[0] is `data` (sentinel, ignored — sample is captured).
            if (!args[1].is_string() || !args[2].is_string() ||
                !args[3].is_string()) {
                throw kel::KelEvalError(
                    "aggregate() arguments 2-4 must be strings");
            }

            const auto& glob = args[1].as_string();
            const auto& metric = args[2].as_string();
            const auto& func = args[3].as_string();

            // Collect values from matching files.
            std::vector<double> values;
            for (const auto& [path, fm] : current_sample->entries) {
                // Match glob against filename or full path.
                std::string fname = path;
                auto sep = path.find_last_of("/\\");
                if (sep != std::string::npos)
                    fname = path.substr(sep + 1);

                if (!glob_match_simple(glob, fname) &&
                    !glob_match_simple(glob, path)) {
                    continue;
                }

                if (metric == "size") {
                    values.push_back(static_cast<double>(fm.size));
                } else if (metric == "mtime") {
                    // Convert mtime to epoch seconds for aggregation.
                    auto epoch = std::chrono::duration_cast<
                        std::chrono::seconds>(
                            fm.last_modified.time_since_epoch()).count();
                    values.push_back(static_cast<double>(epoch));
                } else if (metric == "pattern_found") {
                    values.push_back(
                        (fm.pattern_found.has_value() && *fm.pattern_found)
                            ? 1.0 : 0.0);
                } else if (metric == "hash") {
                    // Count non-empty hashes.
                    values.push_back(
                        (fm.sha256.has_value() || fm.md5.has_value())
                            ? 1.0 : 0.0);
                } else {
                    throw kel::KelEvalError(
                        "aggregate(): unknown metric '" + metric + "'");
                }
            }

            // Apply aggregation function.
            double result = 0.0;
            if (!values.empty()) {
                if (func == "sum") {
                    for (double v : values) result += v;
                } else if (func == "count") {
                    result = static_cast<double>(values.size());
                } else if (func == "min") {
                    result = *std::min_element(
                        values.begin(), values.end());
                } else if (func == "max") {
                    result = *std::max_element(
                        values.begin(), values.end());
                } else if (func == "avg") {
                    for (double v : values) result += v;
                    result /= static_cast<double>(values.size());
                } else {
                    throw kel::KelEvalError(
                        "aggregate(): unknown func '" + func + "'");
                }
            }

            // Return as int if whole number.
            if (result == static_cast<double>(static_cast<int64_t>(result))) {
                return kel::KelValue(static_cast<int64_t>(result));
            }
            return kel::KelValue(result);
        };
    }

    return ctx;
}

}  // namespace

// ── Constructor / Destructor ────────────────────────────────────────────

WatchEngine::WatchEngine(
    WatchEngineConfig config,
    Dependencies deps,
    std::vector<WatchGroupDef> groups)
    : config_(std::move(config))
    , deps_(deps)
{
    auto now_steady = deps_.clock ? deps_.clock->steady_now()
                                  : std::chrono::steady_clock::now();

    for (auto& g : groups) {
        if (!g.enabled) continue;
        GroupState state;
        state.def = std::move(g);
        // First scan happens immediately (offset 0).
        state.next_scan_time = now_steady;
        groups_[state.def.group_name] = std::move(state);
    }
}

WatchEngine::~WatchEngine() {
    stop();
}

// ── Public API ──────────────────────────────────────────────────────────

void WatchEngine::start(std::stop_token stop, engine::TriggerSink sink) {
    thread_ = std::jthread([this, stop, sink = std::move(sink)](
                                std::stop_token) mutable {
        coordinator_loop(stop, std::move(sink));
    });
}

void WatchEngine::run(std::stop_token stop, engine::TriggerSink sink) {
    coordinator_loop(stop, std::move(sink));
}

std::vector<ScanResult> WatchEngine::scan_once(engine::TriggerSink& sink) {
    std::vector<ScanResult> results;
    std::lock_guard lock(groups_mu_);
    for (auto& [name, state] : groups_) {
        results.push_back(run_scan(state, sink));
    }
    return results;
}

ScanResult WatchEngine::scan_group(
    const std::string& group_name, engine::TriggerSink& sink)
{
    std::lock_guard lock(groups_mu_);
    auto it = groups_.find(group_name);
    if (it == groups_.end()) {
        return {};  // Unknown group.
    }
    return run_scan(it->second, sink);
}

void WatchEngine::request_reload(std::vector<WatchGroupDef> new_groups) {
    std::lock_guard lock(reload_mu_);
    pending_groups_ = std::move(new_groups);
    reload_requested_.store(true, std::memory_order_release);
    if (deps_.clock) deps_.clock->wake();
}

std::vector<WatchGroupStatus> WatchEngine::get_status() const {
    std::vector<WatchGroupStatus> statuses;
    std::lock_guard lock(groups_mu_);
    for (const auto& [name, state] : groups_) {
        WatchGroupStatus s;
        s.group_name = name;
        switch (state.def.mode) {
            case WatchMode::Native: s.mode = "native"; break;
            case WatchMode::Sample: s.mode = "sample"; break;
            case WatchMode::Hybrid: s.mode = "hybrid"; break;
        }
        s.watched_paths = static_cast<int>(state.def.watch_items.size());
        s.files_in_last_sample = static_cast<int>(state.last_sample.size());
        s.last_scan_time = state.last_scan_iso;
        s.events_last_hour = state.events_emitted;
        s.status = "active";
        statuses.push_back(std::move(s));
    }
    return statuses;
}

size_t WatchEngine::group_count() const {
    std::lock_guard lock(groups_mu_);
    return groups_.size();
}

void WatchEngine::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        if (deps_.clock) deps_.clock->wake();
        thread_.join();
    }
}

// ── Coordinator loop ────────────────────────────────────────────────────

void WatchEngine::coordinator_loop(
    std::stop_token stop, engine::TriggerSink sink)
{
    while (!stop.stop_requested()) {
        // Handle reload.
        if (reload_requested_.exchange(false, std::memory_order_acquire)) {
            std::lock_guard rlock(reload_mu_);
            std::lock_guard glock(groups_mu_);

            auto now_steady = deps_.clock ? deps_.clock->steady_now()
                                          : std::chrono::steady_clock::now();

            // Rebuild group state. Preserve last_sample for groups that
            // still exist (so the diff can detect changes since last scan).
            std::unordered_map<std::string, GroupState> new_groups;
            for (auto& g : pending_groups_) {
                if (!g.enabled) continue;
                GroupState state;
                state.def = std::move(g);

                // Preserve existing state if group existed.
                auto old = groups_.find(state.def.group_name);
                if (old != groups_.end()) {
                    state.last_sample = std::move(old->second.last_sample);
                    state.sample_epoch = old->second.sample_epoch;
                }

                state.next_scan_time = now_steady;  // Re-scan now.
                new_groups[state.def.group_name] = std::move(state);
            }

            groups_ = std::move(new_groups);
            pending_groups_.clear();
        }

        // Find next scan time.
        auto target = earliest_scan_time();
        auto now_steady = deps_.clock ? deps_.clock->steady_now()
                                      : std::chrono::steady_clock::now();

        // Sleep until the next scan is due.
        if (target > now_steady) {
            if (deps_.clock) {
                deps_.clock->sleep_until(target);
            } else {
                std::this_thread::sleep_for(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        target - now_steady));
            }
        }

        if (stop.stop_requested()) break;

        // Run scans for all groups that are due.
        now_steady = deps_.clock ? deps_.clock->steady_now()
                                 : std::chrono::steady_clock::now();

        std::lock_guard lock(groups_mu_);
        for (auto& [name, state] : groups_) {
            if (state.next_scan_time <= now_steady) {
                run_scan(state, sink);
            }
        }
    }
}

// ── Single group scan ───────────────────────────────────────────────────

ScanResult WatchEngine::run_scan(
    GroupState& state, engine::TriggerSink& sink)
{
    ScanResult result;
    auto scan_start = std::chrono::steady_clock::now();

    // Collect current sample.
    std::stop_source temp_stop;
    auto current = collect_sample(state.def, temp_stop.get_token());
    current.epoch = ++state.sample_epoch;

    result.sample = current;

    // Record scan time.
    auto wall_now = deps_.clock ? deps_.clock->now()
                                : std::chrono::system_clock::now();
    state.last_scan_iso = format_iso8601(wall_now);

    // Compute diff (only if we have a previous sample — EventWatcher parity).
    if (!state.last_sample.empty()) {
        result.diff = compute_diff(current, state.last_sample);

        // Evaluate rules if there are changes.
        if (!result.diff.empty()) {
            result.triggered = evaluate_rules(
                state.def, current, state.last_sample, result.diff);

            // Emit TriggerEvents.
            if (!result.triggered.empty()) {
                emit_triggers(state.def, result.triggered, sink);
                state.events_emitted +=
                    static_cast<int>(result.triggered.size());
            }

            // Persist events.
            for (const auto& tr : result.triggered) {
                persist_event(tr, current.epoch);
            }
        }
    }

    // Persist sample.
    persist_sample(state.def.group_name, current);

    // Update state.
    state.last_sample = std::move(current);

    // Schedule next scan.
    auto now_steady = deps_.clock ? deps_.clock->steady_now()
                                  : std::chrono::steady_clock::now();
    state.next_scan_time =
        now_steady + state.def.sample_rate;

    auto scan_end = std::chrono::steady_clock::now();
    result.scan_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            scan_end - scan_start);

    return result;
}

// ── Sample collection ───────────────────────────────────────────────────

Sample WatchEngine::collect_sample(
    const WatchGroupDef& group, std::stop_token stop)
{
    Sample sample;

    if (!deps_.scanner) return sample;

    for (const auto& item : group.watch_items) {
        auto entries = deps_.scanner->scan(
            fs::path(item),
            group.max_depth,
            group.exclude_globs,
            stop);

        for (auto& entry : entries) {
            if (stop.stop_requested()) break;

            FileMetrics metrics;
            metrics.path = entry.path;
            metrics.entry_type = entry.entry_type;
            metrics.size = entry.size;
            metrics.last_modified = entry.mtime;
            metrics.permissions = entry.permissions;
            metrics.uid = entry.uid;
            metrics.gid = entry.gid;

            if (entry.is_directory) {
                metrics.files_count = entry.files_count;
                metrics.subdirs_count = entry.subdirs_count;
            }

            // Hash computation deferred per hash_policy — in v1 the
            // FakeFilesystem provides pre-set hashes, and real scanning
            // will compute them in a later batch.

            sample.entries[metrics.path] = std::move(metrics);
        }
    }

    return sample;
}

// ── Rule evaluation ─────────────────────────────────────────────────────

std::vector<WatchTriggerResult> WatchEngine::evaluate_rules(
    const WatchGroupDef& group,
    const Sample& current,
    const Sample& previous,
    const SampleDiff& diff)
{
    std::vector<WatchTriggerResult> results;

    kel::EvalLimits limits;  // Default limits — safe for watch rules.

    // Helper lambda: evaluate a single rule against a single file event.
    // Returns true if the rule fires (event-type match AND KEL condition true).
    auto try_rule = [&](const WatchRuleDef& rule,
                        const std::string& path,
                        const std::string& event_str) -> bool
    {
        // Step 1: Event-type filter (fast path).
        if (!rule_matches_event(rule, event_str)) return false;

        // Step 2: KEL condition evaluation.
        // If condition is empty or "true", the rule always fires.
        if (rule.condition.empty() || rule.condition == "true") {
            return true;
        }

        // Build KEL context with file metrics and event info.
        auto entry_it = current.entries.find(path);
        FileMetrics empty_metrics;
        empty_metrics.path = path;
        const FileMetrics& file_metrics =
            (entry_it != current.entries.end())
                ? entry_it->second : empty_metrics;

        auto prev_it = previous.entries.find(path);
        const FileMetrics* prev_metrics =
            (prev_it != previous.entries.end())
                ? &prev_it->second : nullptr;

        auto ctx = build_watch_kel_context(
            group.group_name, event_str, file_metrics, prev_metrics,
            &current);

        // Evaluate the KEL expression.
        try {
            auto result = kel::eval_expression(
                rule.condition, ctx, limits);
            return result.is_truthy();
        } catch (const std::exception&) {
            // KEL evaluation error → rule does not fire.
            // In production, this would be logged. For now, silently skip.
            return false;
        }
    };

    // Helper lambda: check a rule against a file and add result if matched.
    auto check_and_add = [&](const WatchRuleDef& rule,
                             const std::string& path,
                             const std::string& event_str) {
        if (try_rule(rule, path, event_str)) {
            WatchTriggerResult tr;
            tr.rule_name = rule.rule_name;
            tr.watch_group_name = group.group_name;
            tr.affected_paths = {path};
            tr.event_type = event_str;
            tr.severity = rule.severity;
            tr.trigger_target = rule.trigger_target;
            tr.trigger_is_workflow = rule.trigger_is_workflow;
            results.push_back(std::move(tr));
        }
    };

    // Process created files.
    for (const auto& path : diff.created) {
        auto entry_it = current.entries.find(path);
        bool is_dir = (entry_it != current.entries.end() &&
                       entry_it->second.entry_type == "directory");

        std::string event_str =
            event_type_to_string(is_dir ? WatchEventType::StructureChanged
                                        : WatchEventType::FileCreated);

        for (const auto& rule : group.rules) {
            check_and_add(rule, path, event_str);
        }
    }

    // Process deleted files.
    for (const auto& path : diff.deleted) {
        std::string event_str = event_type_to_string(WatchEventType::FileDeleted);

        for (const auto& rule : group.rules) {
            check_and_add(rule, path, event_str);
        }
    }

    // Process modified files.
    for (const auto& [path, changes] : diff.modified) {
        auto entry_it = current.entries.find(path);
        std::string entry_type = "file";
        if (entry_it != current.entries.end()) {
            entry_type = entry_it->second.entry_type;
        }

        auto event_flags = classify_changes(changes, entry_type);
        std::string event_str = event_type_to_string(event_flags);

        for (const auto& rule : group.rules) {
            check_and_add(rule, path, event_str);
        }
    }

    return results;
}

// ── Trigger emission ────────────────────────────────────────────────────

void WatchEngine::emit_triggers(
    const WatchGroupDef& group,
    const std::vector<WatchTriggerResult>& results,
    engine::TriggerSink& sink)
{
    for (const auto& tr : results) {
        // Only emit a TriggerEvent if the rule has a trigger target.
        if (!tr.trigger_target.has_value()) continue;

        auto correlation_id = core::generate_correlation_id();
        auto target_kind = tr.trigger_is_workflow
            ? engine::TriggerEvent::TargetKind::Workflow
            : engine::TriggerEvent::TargetKind::StandaloneJob;

        engine::FileDiffPayload payload;
        payload.watch_group = group.group_name;
        payload.affected_paths = tr.affected_paths;
        payload.diff_summary = tr.event_type;
        // Count by diff type from the affected paths.
        payload.files_modified =
            static_cast<int>(tr.affected_paths.size());

        auto event = engine::TriggerEvent{
            .type = engine::TriggerType::FileDiff,
            .trigger_id = group.group_id,
            .target_id = *tr.trigger_target,
            .target_kind = target_kind,
            .fire_time = deps_.clock ? deps_.clock->now()
                                     : std::chrono::system_clock::now(),
            .mono_time = deps_.clock ? deps_.clock->steady_now()
                                     : std::chrono::steady_clock::now(),
            .correlation_id = std::move(correlation_id),
            .payload = std::move(payload),
        };

        sink(std::move(event));
    }
}

// ── Persistence helpers ─────────────────────────────────────────────────

void WatchEngine::persist_sample(
    const std::string& group_name, const Sample& sample)
{
    if (!deps_.db_writer) return;

    for (const auto& [path, metrics] : sample.entries) {
        persist::InsertWatchSample req;
        req.watch_group = group_name;
        req.sample_epoch = sample.epoch;
        req.file_path = path;
        req.is_dir = (metrics.entry_type == "directory");
        req.size = metrics.size;

        // Format mtime as ISO-8601 string.
        req.mtime = format_iso8601(metrics.last_modified);

        // Use whichever hash is available (prefer SHA256).
        if (metrics.sha256.has_value()) {
            req.hash = *metrics.sha256;
        } else if (metrics.md5.has_value()) {
            req.hash = *metrics.md5;
        }

        deps_.db_writer->enqueue(
            persist::DBWriteRequest{std::move(req)},
            std::chrono::milliseconds(100));
    }
}

void WatchEngine::persist_event(
    const WatchTriggerResult& result, int64_t sample_epoch)
{
    if (!deps_.db_writer) return;

    // Build a deterministic event UID from group + rule + epoch + paths.
    std::string uid_input = result.watch_group_name + ":"
        + result.rule_name + ":"
        + std::to_string(sample_epoch);
    for (const auto& p : result.affected_paths) {
        uid_input += ":" + p;
    }
    auto event_uid = core::generate_content_id(
        core::EntityType::kWatchRule, uid_input);

    // Build JSON array of affected paths.
    std::string paths_json = "[";
    for (size_t i = 0; i < result.affected_paths.size(); ++i) {
        if (i > 0) paths_json += ",";
        paths_json += "\"" + result.affected_paths[i] + "\"";
    }
    paths_json += "]";

    persist::InsertWatchEvent req;
    req.event_uid = event_uid;
    req.watch_group = result.watch_group_name;
    req.rule_name = result.rule_name;
    req.event_type = result.event_type;
    req.severity = result.severity;
    req.affected_files_json = paths_json;
    req.sample_epoch = sample_epoch;
    req.details_json = "{}";

    deps_.db_writer->enqueue(
        persist::DBWriteRequest{std::move(req)},
        std::chrono::milliseconds(100));
}

// ── Utility ─────────────────────────────────────────────────────────────

std::string WatchEngine::format_iso8601(
    std::chrono::system_clock::time_point tp)
{
    auto time_t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &time_t);
#else
    gmtime_r(&time_t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::chrono::steady_clock::time_point WatchEngine::earliest_scan_time() const {
    auto earliest = std::chrono::steady_clock::time_point::max();
    std::lock_guard lock(groups_mu_);
    for (const auto& [_, state] : groups_) {
        if (state.next_scan_time < earliest) {
            earliest = state.next_scan_time;
        }
    }
    return earliest;
}

}  // namespace kairos::watch
