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
#include "kairos/kel/errors.hpp"
#include "kairos/watch/hash_util.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>

namespace kairos::watch {

using namespace std::chrono_literals;
namespace engine = kairos::engine;
namespace fs = std::filesystem;

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
    , debounce_buffer_(config_.debounce_ms)
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
    // Start native watcher sub-thread if a native backend is available
    // and any group uses native or hybrid mode.
    start_native_watcher(stop);

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

void WatchEngine::process_native_events(engine::TriggerSink& sink) {
    drain_native_queue();
    process_debounced_events(sink);
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

size_t WatchEngine::debounce_pending() const {
    return debounce_buffer_.pending_count();
}

void WatchEngine::stop() {
    // Stop native watcher sub-thread first.
    stop_native_watcher();

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

        // ── Process native events (hybrid mode) ────────────────────
        // Drain the bounded queue into the debounce buffer, then
        // process any settled events via targeted re-scan + rules.
        drain_native_queue();
        process_debounced_events(sink);

        // ── Determine sleep duration ───────────────────────────────
        // Sleep is the minimum of:
        //   - Time until next periodic scan is due.
        //   - Time until next debounce entry settles.
        //   - Maximum 1 second (for stop responsiveness).
        auto target = earliest_scan_time();
        auto now_steady = deps_.clock ? deps_.clock->steady_now()
                                      : std::chrono::steady_clock::now();

        auto sleep_dur = std::chrono::milliseconds(1000);
        if (target > now_steady) {
            auto until_scan = std::chrono::duration_cast<
                std::chrono::milliseconds>(target - now_steady);
            sleep_dur = std::min(sleep_dur, until_scan);
        } else {
            sleep_dur = std::chrono::milliseconds(0);
        }

        // Check debounce timer.
        auto debounce_wait = debounce_buffer_.time_until_next_settle(
            std::chrono::steady_clock::now());
        if (debounce_wait < sleep_dur) {
            sleep_dur = debounce_wait;
        }

        // Sleep if there's time to wait.
        if (sleep_dur > std::chrono::milliseconds(0)) {
            if (deps_.clock) {
                deps_.clock->sleep_for(sleep_dur);
            } else {
                std::this_thread::sleep_for(sleep_dur);
            }
        }

        if (stop.stop_requested()) break;

        // ── Run periodic scans for all due groups ──────────────────
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

    // ── OTel span: kairos.watch_scan (§21.2, §21.5) ─────────────
    std::unique_ptr<observability::SpanHandle> scan_span;
    if (deps_.tracer) {
        std::string mode_str;
        switch (state.def.mode) {
            case WatchMode::Native: mode_str = "native"; break;
            case WatchMode::Sample: mode_str = "sample"; break;
            case WatchMode::Hybrid: mode_str = "hybrid"; break;
        }
        scan_span = deps_.tracer->start_span("kairos.watch_scan", {
            {"watch_group", state.def.group_name},
            {"scan_mode", mode_str},
        });
    }

    // Collect current sample.
    std::stop_source temp_stop;

    // Child span: kairos.snapshot
    std::unique_ptr<observability::SpanHandle> snap_span;
    if (deps_.tracer && scan_span) {
        snap_span = deps_.tracer->start_child_span(
            *scan_span, "kairos.snapshot");
    }

    auto current = collect_sample(state.def, temp_stop.get_token());
    current.epoch = ++state.sample_epoch;

    // Apply hash computation per hash_policy (§12.6.3).
    apply_hashes(current, state.last_sample,
                 state.def.hash_policy, temp_stop.get_token());

    // Apply pattern regex matching (§12.6.1).
    apply_patterns(current, state.def.pattern, temp_stop.get_token());

    result.sample = current;

    if (snap_span) {
        snap_span->set_attribute("files_scanned",
            static_cast<int64_t>(current.entries.size()));
        snap_span->end();
    }

    // Record scan time.
    auto wall_now = deps_.clock ? deps_.clock->now()
                                : std::chrono::system_clock::now();
    state.last_scan_iso = format_iso8601(wall_now);

    // Compute diff (only if we have a previous sample — EventWatcher parity).
    int64_t changes_detected = 0;
    if (!state.last_sample.empty()) {
        {
            // Child span: kairos.diff
            std::unique_ptr<observability::SpanHandle> diff_span;
            if (deps_.tracer && scan_span) {
                diff_span = deps_.tracer->start_child_span(
                    *scan_span, "kairos.diff");
            }

            result.diff = compute_diff(result.sample, state.last_sample);

            // ── Hybrid mode deduplication (§12.7) ──────────────────────
            if (!result.diff.empty() &&
                !state.recently_reported.empty())
            {
                auto remove_reported = [&](std::vector<std::string>& paths) {
                    paths.erase(
                        std::remove_if(paths.begin(), paths.end(),
                            [&](const std::string& p) {
                                return is_recently_reported(state, p);
                            }),
                        paths.end());
                };
                remove_reported(result.diff.created);
                remove_reported(result.diff.deleted);

                for (auto it = result.diff.modified.begin();
                     it != result.diff.modified.end(); )
                {
                    if (is_recently_reported(state, it->first)) {
                        it = result.diff.modified.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            prune_recently_reported(state);

            changes_detected =
                static_cast<int64_t>(result.diff.created.size()) +
                static_cast<int64_t>(result.diff.deleted.size()) +
                static_cast<int64_t>(result.diff.modified.size());

            if (diff_span) {
                diff_span->set_attribute("changes_detected", changes_detected);
                diff_span->end();
            }
        }

        // Evaluate rules if there are (non-deduplicated) changes.
        if (!result.diff.empty()) {
            // Child span: kairos.rule_eval
            std::unique_ptr<observability::SpanHandle> rule_span;
            if (deps_.tracer && scan_span) {
                rule_span = deps_.tracer->start_child_span(
                    *scan_span, "kairos.rule_eval");
            }

            result.triggered = evaluate_rules(
                state.def, result.sample, state.last_sample, result.diff);

            if (rule_span) {
                rule_span->set_attribute("rules_evaluated",
                    static_cast<int64_t>(state.def.rules.size()));
                rule_span->set_attribute("events_emitted",
                    static_cast<int64_t>(result.triggered.size()));
                rule_span->end();
            }

            // Emit TriggerEvents.
            if (!result.triggered.empty()) {
                emit_triggers(state.def, result.triggered, sink);
                state.events_emitted +=
                    static_cast<int>(result.triggered.size());

                // Record in diagnostics ring buffer (§12.14).
                for (const auto& tr : result.triggered) {
                    record_recent_event(tr);
                }
            }

            // Persist events.
            for (const auto& tr : result.triggered) {
                persist_event(tr, result.sample.epoch);
            }
        }
    }

    // Persist sample.
    persist_sample(state.def.group_name, result.sample);

    // Capture sample size before moving into state (span needs it).
    int64_t files_in_sample =
        static_cast<int64_t>(current.entries.size());

    // Update state. Move current (not result.sample — caller needs it).
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

    // Finalize the scan span.
    if (scan_span) {
        scan_span->set_attribute("scan_duration_ms",
            static_cast<int64_t>(result.scan_duration.count()));
        scan_span->set_attribute("files_scanned", files_in_sample);
        scan_span->set_attribute("changes_detected", changes_detected);
        scan_span->set_attribute("events_emitted",
            static_cast<int64_t>(result.triggered.size()));
        if (result.incomplete) {
            scan_span->set_error("scan incomplete — bound exceeded");
        }
        scan_span->end();
    }

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

            // Hash computation is handled by apply_hashes() after
            // sample collection, so it can compare against the
            // previous sample for the SizePlusMtime optimization.

            sample.entries[metrics.path] = std::move(metrics);
        }
    }

    return sample;
}

// ── Hash computation (§12.6.3) ─────────────────────────────────────────

void WatchEngine::apply_hashes(
    Sample& sample, const Sample& previous,
    HashPolicy policy, std::stop_token stop)
{
    if (policy == HashPolicy::MtimeOnly) return;

    for (auto& [path, metrics] : sample.entries) {
        if (stop.stop_requested()) break;

        // Skip directories — we only hash regular files.
        if (metrics.entry_type != "file") continue;

        if (policy == HashPolicy::SizePlusMtime) {
            // Only compute hash if size or mtime changed vs previous.
            auto prev_it = previous.entries.find(path);
            if (prev_it != previous.entries.end()) {
                const auto& prev = prev_it->second;
                if (prev.size == metrics.size &&
                    prev.last_modified == metrics.last_modified) {
                    // No change — copy hashes from previous sample.
                    metrics.md5 = prev.md5;
                    metrics.sha256 = prev.sha256;
                    continue;
                }
            }
        }

        // Compute fresh hashes (both MD5 and SHA-256 in one pass).
        auto result = compute_file_hashes(fs::path(path), stop);
        if (result) {
            metrics.md5 = std::move(result->md5);
            metrics.sha256 = std::move(result->sha256);
        }
    }
}

// ── Pattern matching (§12.6.1) ────────────────────────────────────────

void WatchEngine::apply_patterns(
    Sample& sample,
    const std::optional<std::string>& pattern,
    std::stop_token stop)
{
    if (!pattern.has_value() || pattern->empty()) return;

    // Compile regex once (with timeout protection per §7.7).
    std::regex re;
    try {
        re = std::regex(*pattern, std::regex::ECMAScript | std::regex::optimize);
    } catch (const std::regex_error&) {
        // Invalid regex — log and skip all pattern matching for this cycle.
        // In production, this is logged via spdlog.
        return;
    }

    for (auto& [path, metrics] : sample.entries) {
        if (stop.stop_requested()) break;

        // Only apply to regular files.
        if (metrics.entry_type != "file") continue;

        // Skip files larger than threshold.
        if (metrics.size > kMaxPatternScanBytes) {
            metrics.pattern_found = false;
            continue;
        }

        // Skip zero-length files.
        if (metrics.size == 0) {
            metrics.pattern_found = false;
            continue;
        }

        // Read file content.
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            metrics.pattern_found = false;
            continue;
        }

        // Read up to kMaxPatternScanBytes.
        std::string content;
        content.resize(static_cast<size_t>(
            std::min(metrics.size, kMaxPatternScanBytes)));
        file.read(content.data(), static_cast<std::streamsize>(content.size()));
        auto bytes_read = file.gcount();
        content.resize(static_cast<size_t>(bytes_read));

        if (content.empty()) {
            metrics.pattern_found = false;
            continue;
        }

        // Binary detection: check for NUL byte in first 8KB.
        size_t probe_len = std::min(content.size(), kBinaryProbeBytes);
        bool is_binary = (std::memchr(content.data(), '\0', probe_len) != nullptr);
        if (is_binary) {
            metrics.pattern_found = false;
            continue;
        }

        // Apply regex search.
        try {
            metrics.pattern_found = std::regex_search(content, re);
        } catch (const std::regex_error&) {
            // Pathological backtracking or other error.
            metrics.pattern_found = false;
        }
    }
}

// ── Diagnostics: recent events ring buffer (§12.14) ──────────────────

void WatchEngine::record_recent_event(const WatchTriggerResult& result) {
    std::lock_guard lock(recent_events_mu_);

    if (recent_events_.size() < kRecentEventsCapacity) {
        recent_events_.push_back(result);
    } else {
        recent_events_[recent_events_head_] = result;
    }
    recent_events_head_ = (recent_events_head_ + 1) % kRecentEventsCapacity;
    if (recent_events_count_ < kRecentEventsCapacity) {
        ++recent_events_count_;
    }
}

std::vector<WatchTriggerResult> WatchEngine::get_recent_events(
    int limit) const
{
    std::lock_guard lock(recent_events_mu_);

    std::vector<WatchTriggerResult> result;
    size_t count = std::min(recent_events_count_,
                            static_cast<size_t>(limit));
    result.reserve(count);

    // Read from ring buffer in reverse-insertion order (newest first).
    for (size_t i = 0; i < count; ++i) {
        size_t idx;
        if (recent_events_count_ < kRecentEventsCapacity) {
            // Buffer not full: read backwards from count-1.
            idx = recent_events_count_ - 1 - i;
        } else {
            // Buffer full: head points to next write position.
            // Most recent is at head - 1 (mod capacity).
            idx = (recent_events_head_ + kRecentEventsCapacity - 1 - i)
                  % kRecentEventsCapacity;
        }
        result.push_back(recent_events_[idx]);
    }

    return result;
}

std::vector<WatchTriggerResult> WatchEngine::get_recent_events(
    const std::string& group_name, int limit) const
{
    auto all = get_recent_events(
        static_cast<int>(kRecentEventsCapacity));

    std::vector<WatchTriggerResult> filtered;
    for (auto& ev : all) {
        if (ev.watch_group_name == group_name) {
            filtered.push_back(std::move(ev));
            if (static_cast<int>(filtered.size()) >= limit) break;
        }
    }
    return filtered;
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
        } catch (const std::exception& e) {
            // KEL evaluation error → rule does not fire.
            // Log the error so users can diagnose broken conditions.
            spdlog::warn("Watch rule '{}' KEL error in condition '{}': {}",
                         rule.rule_name, rule.condition, e.what());
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

// ── Hybrid mode: native event processing (§12.7, §12.13) ──────────────

void WatchEngine::drain_native_queue() {
    // Non-blocking drain of all available native events from the
    // bounded queue into the debounce buffer.
    while (true) {
        auto event = native_event_queue_.try_pop();
        if (!event) break;

        debounce_buffer_.add(std::move(*event));
    }
}

void WatchEngine::process_debounced_events(engine::TriggerSink& sink) {
    auto now = std::chrono::steady_clock::now();
    auto settled = debounce_buffer_.drain_settled(now);

    if (settled.empty()) return;

    std::lock_guard lock(groups_mu_);

    for (const auto& event : settled) {
        // ── Handle overflow: trigger full re-scan ──────────────
        if (event.type == NativeEventType::Overflow ||
            event.type == NativeEventType::Error) {
            // On overflow, schedule immediate re-scan for all groups
            // that watch the affected path's parent directory.
            for (auto& [name, state] : groups_) {
                auto now_steady = deps_.clock ? deps_.clock->steady_now()
                    : std::chrono::steady_clock::now();
                state.next_scan_time = now_steady;
            }
            continue;
        }

        // ── Find the relevant watch group for this path ────────
        auto* state = find_group_for_path(event.path);
        if (!state) continue;

        // Skip if mode is sample-only.
        if (state->def.mode == WatchMode::Sample) continue;

        // ── Targeted re-scan (§12.7 step 2–4) ─────────────────
        // Skip if we don't have a previous sample yet (baseline).
        if (state->last_sample.empty()) continue;

        auto diff = targeted_rescan(event, *state);
        if (diff.empty()) continue;

        // Evaluate rules against the single-file diff.
        auto triggered = evaluate_rules(
            state->def, state->last_sample, state->last_sample, diff);

        // Emit trigger events and persist.
        if (!triggered.empty()) {
            emit_triggers(state->def, triggered, sink);
            state->events_emitted +=
                static_cast<int>(triggered.size());

            for (const auto& tr : triggered) {
                persist_event(tr, state->sample_epoch);
                record_recent_event(tr);
            }
        }

        // Mark paths as recently reported for dedup.
        for (const auto& p : diff.created) mark_reported(*state, p);
        for (const auto& p : diff.deleted) mark_reported(*state, p);
        for (const auto& [p, _] : diff.modified) mark_reported(*state, p);
    }
}

SampleDiff WatchEngine::targeted_rescan(
    const NativeEvent& event, GroupState& state)
{
    SampleDiff diff;

    if (!deps_.scanner) return diff;

    std::string path_str = event.path.string();

    // Handle deletion events.
    if (event.type == NativeEventType::Deleted) {
        auto prev_it = state.last_sample.entries.find(path_str);
        if (prev_it != state.last_sample.entries.end()) {
            diff.deleted.push_back(path_str);
            // Remove from current sample.
            state.last_sample.entries.erase(prev_it);
        }
        return diff;
    }

    // For created/modified/renamed: stat the file and compare.
    auto entry = deps_.scanner->stat_file(event.path);
    if (!entry) {
        // File disappeared between event and stat — treat as deleted.
        auto prev_it = state.last_sample.entries.find(path_str);
        if (prev_it != state.last_sample.entries.end()) {
            diff.deleted.push_back(path_str);
            state.last_sample.entries.erase(prev_it);
        }
        return diff;
    }

    // Build current metrics.
    FileMetrics metrics;
    metrics.path = entry->path;
    metrics.entry_type = entry->entry_type;
    metrics.size = entry->size;
    metrics.last_modified = entry->mtime;
    metrics.permissions = entry->permissions;
    metrics.uid = entry->uid;
    metrics.gid = entry->gid;

    if (entry->is_directory) {
        metrics.files_count = entry->files_count;
        metrics.subdirs_count = entry->subdirs_count;
    }

    // Hash computation for the single file (if policy requires it).
    if (state.def.hash_policy != HashPolicy::MtimeOnly &&
        metrics.entry_type == "file") {
        auto hash = compute_file_hashes(event.path);
        if (hash) {
            metrics.md5 = std::move(hash->md5);
            metrics.sha256 = std::move(hash->sha256);
        }
    }

    // Pattern matching for the single file (§12.6.1).
    if (state.def.pattern.has_value() && !state.def.pattern->empty() &&
        metrics.entry_type == "file" &&
        metrics.size > 0 && metrics.size <= kMaxPatternScanBytes)
    {
        try {
            std::regex re(*state.def.pattern,
                          std::regex::ECMAScript | std::regex::optimize);
            std::ifstream file(event.path, std::ios::binary);
            if (file) {
                std::string content;
                content.resize(static_cast<size_t>(
                    std::min(metrics.size, kMaxPatternScanBytes)));
                file.read(content.data(),
                          static_cast<std::streamsize>(content.size()));
                content.resize(static_cast<size_t>(file.gcount()));

                // Binary detection.
                size_t probe = std::min(content.size(), kBinaryProbeBytes);
                bool is_binary =
                    (std::memchr(content.data(), '\0', probe) != nullptr);

                metrics.pattern_found = (!is_binary && !content.empty())
                    ? std::regex_search(content, re)
                    : false;
            } else {
                metrics.pattern_found = false;
            }
        } catch (const std::regex_error&) {
            metrics.pattern_found = false;
        }
    }

    // Compare against previous sample.
    auto prev_it = state.last_sample.entries.find(metrics.path);
    if (prev_it == state.last_sample.entries.end()) {
        // New file.
        diff.created.push_back(metrics.path);
    } else {
        // Build a mini-sample for diff computation.
        Sample mini_current;
        mini_current.entries[metrics.path] = metrics;
        Sample mini_previous;
        mini_previous.entries[metrics.path] = prev_it->second;

        auto mini_diff = compute_diff(mini_current, mini_previous);
        if (!mini_diff.empty()) {
            // Merge into our result diff.
            for (auto& p : mini_diff.created) diff.created.push_back(std::move(p));
            for (auto& p : mini_diff.deleted) diff.deleted.push_back(std::move(p));
            for (auto& [p, changes] : mini_diff.modified) {
                diff.modified[p] = std::move(changes);
            }
        }
    }

    // Update the last_sample with fresh metrics.
    state.last_sample.entries[metrics.path] = std::move(metrics);

    return diff;
}

WatchEngine::GroupState* WatchEngine::find_group_for_path(
    const fs::path& path)
{
    // Check which watch group's watch_items match this path.
    // A path matches a group if it starts with one of the group's
    // watch_items (directory prefix match).
    std::string path_str = path.string();

    for (auto& [name, state] : groups_) {
        if (state.def.mode == WatchMode::Sample) continue;

        for (const auto& item : state.def.watch_items) {
            // Check if the event path is under this watch item's directory.
            if (path_str.find(item) == 0) {
                return &state;
            }
            // Also check with generic (forward-slash) paths.
            std::string generic_item =
                fs::path(item).generic_string();
            std::string generic_path = path.generic_string();
            if (generic_path.find(generic_item) == 0) {
                return &state;
            }
        }
    }
    return nullptr;
}

bool WatchEngine::is_recently_reported(
    const GroupState& state, const std::string& path) const
{
    return state.recently_reported.count(path) > 0;
}

void WatchEngine::mark_reported(
    GroupState& state, const std::string& path)
{
    state.recently_reported[path] = state.sample_epoch;
}

void WatchEngine::prune_recently_reported(GroupState& state) {
    // Remove entries older than 2× sample_rate scans.
    // Since sample_epoch increments once per scan, entries older than
    // (current_epoch - 2) are stale.
    int64_t cutoff = state.sample_epoch - 2;
    for (auto it = state.recently_reported.begin();
         it != state.recently_reported.end(); )
    {
        if (it->second < cutoff) {
            it = state.recently_reported.erase(it);
        } else {
            ++it;
        }
    }
}

// ── Native watcher lifecycle (§12.13) ──────────────────────────────────

void WatchEngine::start_native_watcher(std::stop_token stop) {
    if (!deps_.native_watcher) return;

    // Check if any group uses native or hybrid mode.
    bool needs_native = false;
    {
        std::lock_guard lock(groups_mu_);
        for (const auto& [name, state] : groups_) {
            if (state.def.mode == WatchMode::Native ||
                state.def.mode == WatchMode::Hybrid) {
                needs_native = true;
                break;
            }
        }
    }

    if (!needs_native) return;

    // Add watches for all native/hybrid groups.
    {
        std::lock_guard lock(groups_mu_);
        for (const auto& [name, state] : groups_) {
            if (state.def.mode == WatchMode::Sample) continue;
            for (const auto& item : state.def.watch_items) {
                bool recursive = (state.def.max_depth > 0);
                if (!deps_.native_watcher->add_watch(
                        fs::path(item), recursive)) {
                    // Non-fatal: log and continue with sample mode.
                }
            }
        }
    }

    // Start the native watcher sub-thread.
    // Events are pushed to the bounded queue for the coordinator.
    native_watcher_active_.store(true, std::memory_order_release);
    native_watcher_thread_ = std::jthread(
        [this, stop](std::stop_token jthread_stop) {
            auto callback = [this](const NativeEvent& event) {
                // Push to bounded queue (non-blocking, drop on full).
                if (!native_event_queue_.try_push(event)) {
                    // Queue full — backpressure. The coordinator will
                    // catch up via periodic full scans.
                }
            };

            deps_.native_watcher->run(stop, callback);
            native_watcher_active_.store(false, std::memory_order_release);
        });
}

void WatchEngine::stop_native_watcher() {
    if (native_watcher_thread_.joinable()) {
        native_watcher_thread_.request_stop();
        native_watcher_thread_.join();
    }
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
