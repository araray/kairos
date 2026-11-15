/// include/kairos/engine/scheduler.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/engine/scheduler.hpp — Scheduling engine (timer priority queue)  ║
// ║                                                                           ║
// ║  Single thread: sleeps until the next timer fires, emits TriggerEvents   ║
// ║  to the trigger bus. Does NOT evaluate conditions or execute jobs.        ║
// ║                                                                           ║
// ║  Uses ClockSource abstraction for deterministic testing with FakeClock.  ║
// ║                                                                           ║
// ║  Spec reference: §10 (full section)                                      ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/engine/trigger_event.hpp"
#include "kairos/engine/trigger_types.hpp"
#include "kairos/engine/workflow_registry.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/occurrence_store.hpp"
#include "kairos/testing/fake_clock.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <stop_token>
#include <thread>
#include <unordered_set>
#include <vector>

namespace kairos::engine {

/// Configuration for the Scheduler.
struct SchedulerConfig {
    std::chrono::seconds misfire_grace{60};
    MisfirePolicy misfire_policy = MisfirePolicy::Coalesce;
    int run_all_max_catch_up = 10;
};

/// Villa integration v1: one active timer as seen by the status endpoint.
struct IntegrationTimer {
    std::string workflow;
    std::string trigger_id;
    std::string trigger_type;
    std::string schedule_id;            ///< Empty → JSON null.
    int64_t schedule_generation = -1;   ///< -1 → JSON null.
    std::string trigger_digest;
    std::string timer_epoch;
    std::string next_due_at;            ///< RFC 3339 UTC ms.
};

/// Coherent snapshot published by the scheduler thread after every heap
/// change: the registry digest it has applied plus its live timers.
struct IntegrationSnapshot {
    bool applied = false;               ///< False until the first build.
    std::string applied_digest;
    std::vector<IntegrationTimer> timers;
};

/// The Scheduler engine.
///
/// Thread safety:
///   - start() must be called once.
///   - request_reload() is thread-safe.
///   - All other methods are internal to the scheduler thread.
class Scheduler {
public:
    struct Dependencies {
        ClockSource* clock = nullptr;
        std::shared_ptr<const WorkflowRegistry> registry;
        ActiveRunTracker* active_runs = nullptr;
        persist::DBWriter* db_writer = nullptr;
        /// Villa integration v1: durable occurrences. Null → no records
        /// are written and no occurrence identity is attached.
        persist::OccurrenceStore* occurrences = nullptr;
    };

    explicit Scheduler(SchedulerConfig config, Dependencies deps);
    ~Scheduler();

    /// Start the scheduler thread.
    void start(std::stop_token stop, TriggerSink sink);

    /// Run synchronously (for testing).
    void run(std::stop_token stop, TriggerSink sink);

    /// Request config reload. Thread-safe.
    void request_reload(
        std::shared_ptr<const WorkflowRegistry> new_registry);

    /// Get a snapshot of all timers (for diagnostics).
    [[nodiscard]] std::vector<TimerSnapshot> snapshot() const;

    /// Villa integration v1 snapshot (thread-safe, coherent).
    [[nodiscard]] IntegrationSnapshot integration_snapshot() const;

    /// Number of registered timers.
    [[nodiscard]] size_t timer_count() const;

    /// Stop the scheduler (joins thread if running).
    void stop();

private:
    /// Rebuild the heap from the current registry. On a reload
    /// (initial=false), timers whose continuity key is unchanged keep
    /// their deadline/phase and epoch; consumed one-shots stay consumed.
    void build_heap(bool initial);
    void publish_snapshot();
    void record_occurrence(const TimerEntry& entry,
                           const std::string& scheduled_for,
                           std::string_view disposition);
    std::vector<TriggerEvent> resolve_misfires();
    void compute_next_fire(TimerEntry& entry);
    bool reschedule(TimerEntry& entry);
    void persist_fire(const std::string& trigger_id,
                      ClockSource::time_point fire_time);

    SchedulerConfig config_;
    Dependencies deps_;
    TimerHeap heap_;

    std::atomic<bool> reload_requested_{false};
    std::shared_ptr<const WorkflowRegistry> pending_registry_;
    std::mutex reload_mu_;

    /// Continuity keys of one-shot timers that already fired / were
    /// skipped (scheduler thread only).
    std::unordered_set<std::string> consumed_;

    mutable std::mutex snap_mu_;
    IntegrationSnapshot snap_;

    std::jthread thread_;
};

}  // namespace kairos::engine
