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
#include "kairos/testing/fake_clock.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

namespace kairos::engine {

/// Configuration for the Scheduler.
struct SchedulerConfig {
    std::chrono::seconds misfire_grace{60};
    MisfirePolicy misfire_policy = MisfirePolicy::Coalesce;
    int run_all_max_catch_up = 10;
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

    /// Number of registered timers.
    [[nodiscard]] size_t timer_count() const;

    /// Stop the scheduler (joins thread if running).
    void stop();

private:
    void build_heap();
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

    std::jthread thread_;
};

}  // namespace kairos::engine
