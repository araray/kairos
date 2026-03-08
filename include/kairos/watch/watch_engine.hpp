/// include/kairos/watch/watch_engine.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/watch/watch_engine.hpp — Watch engine (sample+diff + rules)      ║
// ║                                                                           ║
// ║  Runs on a dedicated thread. For each watch group:                       ║
// ║    - Periodically collects a sample (filesystem snapshot).               ║
// ║    - Computes diff against previous sample.                              ║
// ║    - Evaluates KEL rules against the diff.                               ║
// ║    - Emits TriggerEvents for triggered rules.                            ║
// ║    - Persists samples and events via DBWriter.                           ║
// ║                                                                           ║
// ║  v1 supports sample-only mode. Native backends (inotify, FSEvents,       ║
// ║  RDCW) deferred to a later batch to keep this focused.                   ║
// ║                                                                           ║
// ║  Spec reference: §12.1–§12.15                                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/engine/trigger_event.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/testing/fake_clock.hpp"
#include "kairos/watch/sample.hpp"
#include "kairos/watch/watch_group_def.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kairos::watch {

// ── Scan result (for diagnostics/MCP) ───────────────────────────────────

/// Result of a single scan cycle for one watch group.
struct ScanResult {
    Sample sample;
    SampleDiff diff;
    std::vector<WatchTriggerResult> triggered;
    std::chrono::milliseconds scan_duration{0};
    bool incomplete = false;   ///< True if scan hit a bound.
};

// ── Watch engine config ─────────────────────────────────────────────────

/// Engine-wide configuration extracted from ConfigState.
struct WatchEngineConfig {
    bool enabled = true;
    int scan_thread_count = 2;         ///< Deferred to v2 (single-threaded v1).
    int max_events_per_cycle = 1000;   ///< Backpressure cap.
    std::chrono::milliseconds debounce_ms{200};
};

// ── Watch engine ────────────────────────────────────────────────────────

class WatchEngine {
public:
    /// Dependencies injected at construction.
    struct Dependencies {
        ClockSource* clock = nullptr;
        IFilesystemScanner* scanner = nullptr;       ///< Real or fake.
        persist::DBWriter* db_writer = nullptr;
    };

    explicit WatchEngine(
        WatchEngineConfig config,
        Dependencies deps,
        std::vector<WatchGroupDef> groups);

    ~WatchEngine();

    /// Start the watch engine thread.
    /// Emits triggered events via the provided sink.
    void start(std::stop_token stop, engine::TriggerSink sink);

    /// Run synchronously (for testing).
    void run(std::stop_token stop, engine::TriggerSink sink);

    /// Run a single scan cycle for all groups (for testing).
    /// Returns aggregated results.
    std::vector<ScanResult> scan_once(engine::TriggerSink& sink);

    /// Run a single scan cycle for a specific group.
    ScanResult scan_group(const std::string& group_name,
                          engine::TriggerSink& sink);

    /// Reload with new watch group definitions.
    void request_reload(std::vector<WatchGroupDef> new_groups);

    /// Get status of all watch groups.
    [[nodiscard]] std::vector<WatchGroupStatus> get_status() const;

    /// Number of configured watch groups.
    [[nodiscard]] size_t group_count() const;

    /// Stop the engine (joins thread if running).
    void stop();

private:
    /// Per-group runtime state.
    struct GroupState {
        WatchGroupDef def;
        Sample last_sample;
        int64_t sample_epoch = 0;
        std::chrono::steady_clock::time_point next_scan_time;
        std::string last_scan_iso;
        int events_emitted = 0;
    };

    /// Main coordinator loop.
    void coordinator_loop(std::stop_token stop, engine::TriggerSink sink);

    /// Run a scan cycle for one group state.
    ScanResult run_scan(GroupState& state, engine::TriggerSink& sink);

    /// Collect a sample from the scanner for a watch group.
    Sample collect_sample(const WatchGroupDef& group, std::stop_token stop);

    /// Evaluate watch rules against a diff.
    std::vector<WatchTriggerResult> evaluate_rules(
        const WatchGroupDef& group,
        const Sample& current,
        const SampleDiff& diff);

    /// Emit TriggerEvents for triggered rules.
    void emit_triggers(
        const WatchGroupDef& group,
        const std::vector<WatchTriggerResult>& results,
        engine::TriggerSink& sink);

    /// Persist a sample to SQLite via DBWriter.
    void persist_sample(const std::string& group_name,
                        const Sample& sample);

    /// Persist a watch event to SQLite via DBWriter.
    void persist_event(const WatchTriggerResult& result,
                       int64_t sample_epoch);

    /// Format a time_point as ISO 8601.
    static std::string format_iso8601(
        std::chrono::system_clock::time_point tp);

    /// Find the earliest next_scan_time across all groups.
    std::chrono::steady_clock::time_point earliest_scan_time() const;

    WatchEngineConfig config_;
    Dependencies deps_;

    // Per-group state. Protected by groups_mu_ for reload.
    std::unordered_map<std::string, GroupState> groups_;
    mutable std::mutex groups_mu_;

    // Reload coordination.
    std::atomic<bool> reload_requested_{false};
    std::vector<WatchGroupDef> pending_groups_;
    std::mutex reload_mu_;

    // Thread.
    std::jthread thread_;
};

}  // namespace kairos::watch
