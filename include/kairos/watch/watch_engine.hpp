/// include/kairos/watch/watch_engine.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/watch/watch_engine.hpp — Watch engine (hybrid + sample+diff)     ║
// ║                                                                           ║
// ║  Runs on a dedicated thread. For each watch group:                       ║
// ║    - Starts native watcher sub-thread (inotify/FSEvents/RDCW) if        ║
// ║      mode is native or hybrid.                                           ║
// ║    - Native events → BoundedQueue → DebounceBuffer → targeted rescan.   ║
// ║    - Periodically collects a full sample (filesystem snapshot).          ║
// ║    - Computes diff against previous sample.                              ║
// ║    - Evaluates KEL rules against the diff.                               ║
// ║    - Deduplicates events between native and periodic scans.             ║
// ║    - Emits TriggerEvents for triggered rules.                            ║
// ║    - Persists samples and events via DBWriter.                           ║
// ║                                                                           ║
// ║  Spec reference: §12.1–§12.15                                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/core/bounded_queue.hpp"
#include "kairos/engine/trigger_event.hpp"
#include "kairos/observability/tracer.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/testing/fake_clock.hpp"
#include "kairos/watch/debounce_buffer.hpp"
#include "kairos/watch/file_watcher.hpp"
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
#include <unordered_set>
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
        IFileWatcher* native_watcher = nullptr;       ///< Optional: native backend.
        observability::Tracer* tracer = nullptr;       ///< Optional: OTel tracing.
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

    /// Process any pending native events through debounce (for testing).
    void process_native_events(engine::TriggerSink& sink);

    /// Reload with new watch group definitions.
    void request_reload(std::vector<WatchGroupDef> new_groups);

    /// Get status of all watch groups.
    [[nodiscard]] std::vector<WatchGroupStatus> get_status() const;

    /// Get recent triggered events across all groups (for diagnostics/MCP).
    /// Returns up to `limit` most recent events.
    [[nodiscard]] std::vector<WatchTriggerResult> get_recent_events(
        int limit = 50) const;

    /// Get recent triggered events for a specific watch group.
    [[nodiscard]] std::vector<WatchTriggerResult> get_recent_events(
        const std::string& group_name, int limit = 50) const;

    /// Number of configured watch groups.
    [[nodiscard]] size_t group_count() const;

    /// Number of pending debounced events.
    [[nodiscard]] size_t debounce_pending() const;

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

        /// Recently-reported paths for deduplication (§12.7).
        /// Maps path → epoch when last reported via native event.
        /// Entries older than 2×sample_rate are pruned.
        std::unordered_map<std::string, int64_t> recently_reported;
    };

    /// Main coordinator loop.
    void coordinator_loop(std::stop_token stop, engine::TriggerSink sink);

    /// Run a scan cycle for one group state.
    ScanResult run_scan(GroupState& state, engine::TriggerSink& sink);

    /// Collect a sample from the scanner for a watch group.
    Sample collect_sample(const WatchGroupDef& group, std::stop_token stop);

    /// Apply hash computation per hash_policy (§12.6.3).
    /// Computes hashes only for files where size/mtime changed vs previous.
    void apply_hashes(Sample& sample, const Sample& previous,
                      HashPolicy policy, std::stop_token stop);

    /// Apply pattern regex matching per §12.6.1.
    /// Reads file content (bounded by max_pattern_scan_bytes), applies
    /// the watch group's pattern regex, and stores pattern_found.
    /// Skips binary files (NUL byte in first 8KB) and files > threshold.
    void apply_patterns(Sample& sample,
                        const std::optional<std::string>& pattern,
                        std::stop_token stop);

    /// Evaluate watch rules against a diff (with KEL evaluation).
    std::vector<WatchTriggerResult> evaluate_rules(
        const WatchGroupDef& group,
        const Sample& current,
        const Sample& previous,
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

    // ── Hybrid mode methods (§12.7) ───────────────────────────────────

    /// Drain native events from the bounded queue into the debounce buffer.
    void drain_native_queue();

    /// Process settled debounced events: targeted re-scan + rules + emit.
    void process_debounced_events(engine::TriggerSink& sink);

    /// Targeted re-scan for a single file path (native event).
    /// Returns the single-file diff, or empty if no change detected.
    SampleDiff targeted_rescan(const NativeEvent& event,
                               GroupState& state);

    /// Find which watch group a path belongs to.
    /// Returns nullptr if no match.
    GroupState* find_group_for_path(const fs::path& path);

    /// Check if a path was recently reported (for dedup).
    bool is_recently_reported(const GroupState& state,
                              const std::string& path) const;

    /// Mark a path as recently reported.
    void mark_reported(GroupState& state, const std::string& path);

    /// Prune old entries from the recently-reported set.
    void prune_recently_reported(GroupState& state);

    /// Start native watcher sub-thread (if native watcher is available).
    void start_native_watcher(std::stop_token stop);

    /// Stop native watcher sub-thread.
    void stop_native_watcher();

    /// Format a time_point as ISO 8601.
    static std::string format_iso8601(
        std::chrono::system_clock::time_point tp);

    /// Find the earliest next_scan_time across all groups.
    std::chrono::steady_clock::time_point earliest_scan_time() const;

    /// Compute sleep duration considering both scan timer and debounce.
    std::chrono::milliseconds compute_sleep_duration() const;

    WatchEngineConfig config_;
    Dependencies deps_;

    // Per-group state. Protected by groups_mu_ for reload.
    std::unordered_map<std::string, GroupState> groups_;
    mutable std::mutex groups_mu_;

    // Reload coordination.
    std::atomic<bool> reload_requested_{false};
    std::vector<WatchGroupDef> pending_groups_;
    std::mutex reload_mu_;

    // ── Hybrid mode state (§12.13) ────────────────────────────────────

    /// Bounded MPSC queue for native events (sub-thread → coordinator).
    /// Capacity 4096 per spec §12.13.
    core::BoundedQueue<NativeEvent> native_event_queue_{4096};

    /// Debounce buffer for coalescing rapid native events.
    DebounceBuffer debounce_buffer_;

    /// Native watcher sub-thread.
    std::jthread native_watcher_thread_;

    /// Whether native watcher is active.
    std::atomic<bool> native_watcher_active_{false};

    // ── Recent events ring buffer (§12.14) ────────────────────────────
    /// Stores the most recent N triggered events for diagnostics/MCP.
    static constexpr size_t kRecentEventsCapacity = 200;
    mutable std::mutex recent_events_mu_;
    std::vector<WatchTriggerResult> recent_events_;  ///< Ring buffer.
    size_t recent_events_head_ = 0;                  ///< Write position.
    size_t recent_events_count_ = 0;                 ///< Current count.

    /// Record a triggered event in the ring buffer.
    void record_recent_event(const WatchTriggerResult& result);

    // ── Pattern matching constants (§12.6.1) ──────────────────────────
    /// Max file size to read for pattern matching (1 MB).
    static constexpr int64_t kMaxPatternScanBytes = 1 * 1024 * 1024;
    /// Number of bytes to probe for binary detection (NUL in first 8 KB).
    static constexpr size_t kBinaryProbeBytes = 8192;

    // Thread.
    std::jthread thread_;
};

}  // namespace kairos::watch
