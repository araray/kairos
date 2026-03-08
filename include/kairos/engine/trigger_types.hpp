/// include/kairos/engine/trigger_types.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/engine/trigger_types.hpp — Schedule trigger type definitions      ║
// ║                                                                           ║
// ║  Defines CronTrigger, IntervalTrigger, DateTrigger, and the TimerEntry   ║
// ║  struct used in the scheduler's min-heap priority queue.                  ║
// ║                                                                           ║
// ║  Spec reference: §10.2 (timer queue), §10.3 (trigger types)             ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/engine/trigger_event.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace kairos::engine {

using SteadyTimePoint = std::chrono::steady_clock::time_point;
using SystemTimePoint = std::chrono::system_clock::time_point;

// ── Trigger type definitions ────────────────────────────────────────────

/// Misfire handling policy.
enum class MisfirePolicy {
    Coalesce,   ///< Fire once, skip missed occurrences (default).
    Skip,       ///< Skip all missed fires entirely.
    RunAll,     ///< Fire once for each missed occurrence.
};

/// Parse a misfire policy string.
[[nodiscard]] inline MisfirePolicy parse_misfire_policy(
    std::string_view str) noexcept
{
    if (str == "skip")    return MisfirePolicy::Skip;
    if (str == "run_all") return MisfirePolicy::RunAll;
    return MisfirePolicy::Coalesce;  // Default
}

/// Cron trigger: fires according to a standard cron expression.
/// Uses system_clock because cron is defined in civil/wall time.
///
/// The cron expression is pre-parsed at config load time; only the
/// `next_fire_after` computation happens at runtime.
struct CronTrigger {
    std::string expression;   ///< Original string, e.g., "0 2 * * *"

    /// Compute next fire time after `after`.
    /// Uses croncpp internally. Returns system_clock::time_point.
    ///
    /// Implementation note: actual croncpp integration is deferred to
    /// the .cpp file. This function is the wrapper.
    [[nodiscard]] SystemTimePoint
    next_fire_after(SystemTimePoint after) const;
};

/// Interval trigger: fires every `interval` duration from a start point.
/// Uses steady_clock for drift immunity.
struct IntervalTrigger {
    std::chrono::milliseconds interval{60000};  ///< Fire interval.
    bool align_to_start = false;  ///< If true, fire immediately at daemon start.

    /// Compute next fire time after `after`.
    [[nodiscard]] SteadyTimePoint
    next_fire_after(SteadyTimePoint after) const {
        // Next fire is the smallest multiple of interval strictly after `after`.
        // For align_to_start on first fire, caller passes `start_time - 1ns`.
        return after + interval;
    }
};

/// Date trigger: fires once at a specific wall-clock time.
/// After firing, the trigger is removed from the heap.
struct DateTrigger {
    SystemTimePoint fire_at;   ///< When to fire.
    bool fired = false;        ///< True after the single fire.

    /// Check if this trigger is exhausted (already fired).
    [[nodiscard]] bool is_exhausted() const { return fired; }
};

/// Union of all trigger schedule types.
using TriggerSpec = std::variant<CronTrigger, IntervalTrigger, DateTrigger>;

/// Return a human-readable name for a TriggerSpec variant.
[[nodiscard]] inline std::string trigger_spec_type_name(
    const TriggerSpec& spec)
{
    return std::visit([](const auto& t) -> std::string {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, CronTrigger>)     return "cron";
        if constexpr (std::is_same_v<T, IntervalTrigger>) return "interval";
        if constexpr (std::is_same_v<T, DateTrigger>)     return "date";
    }, spec);
}

// ── Timer entry for the scheduler's priority queue ──────────────────────

/// A timer entry in the scheduler's min-heap.
/// Ordered by a unified "next fire time" projected into milliseconds-from-now.
struct TimerEntry {
    /// Unique identifier: the trigger's content-addressable ID (trg-xxx).
    std::string trigger_id;

    /// Which job or workflow this trigger targets.
    std::string target_id;

    /// Human-readable target name (for logging).
    std::string target_name;

    /// Is this a standalone job or workflow?
    TriggerEvent::TargetKind target_kind = TriggerEvent::TargetKind::Workflow;

    /// The schedule definition.
    TriggerSpec spec;

    /// Misfire handling policy.
    MisfirePolicy misfire_policy = MisfirePolicy::Coalesce;

    /// Maximum concurrent instances of this target.
    int max_instances = 1;

    /// Whether this trigger is enabled.
    bool enabled = true;

    /// Grace period for misfire detection (default: 60 seconds).
    std::chrono::seconds misfire_grace{60};

    // ── Next fire time in both clock domains ────────────────────────

    /// Next wall-clock fire time (for cron/date triggers).
    /// For interval triggers, this is a projection from steady_clock.
    SystemTimePoint next_fire_wall{};

    /// Next monotonic fire time (for interval triggers).
    /// For cron/date triggers, this is a projection from system_clock.
    SteadyTimePoint next_fire_mono{};

    /// Compute the duration from "now" until the next fire.
    /// Uses steady_clock for interval, system_clock for cron/date,
    /// and projects into a common milliseconds duration.
    [[nodiscard]] std::chrono::milliseconds time_until_fire(
        SystemTimePoint wall_now,
        SteadyTimePoint mono_now) const
    {
        return std::visit([&](const auto& t) -> std::chrono::milliseconds {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, IntervalTrigger>) {
                auto diff = next_fire_mono - mono_now;
                return std::chrono::duration_cast<std::chrono::milliseconds>(diff);
            } else {
                // Cron or Date: use wall-clock difference.
                auto diff = next_fire_wall - wall_now;
                return std::chrono::duration_cast<std::chrono::milliseconds>(diff);
            }
        }, spec);
    }
};

/// Comparator for the min-heap: smallest next_fire_mono first.
struct TimerEntryGreater {
    bool operator()(const TimerEntry& a, const TimerEntry& b) const {
        // Use monotonic time for ordering to avoid clock skew issues.
        return a.next_fire_mono > b.next_fire_mono;
    }
};

/// The scheduler's timer heap: a min-heap of TimerEntry.
using TimerHeap = std::priority_queue<
    TimerEntry,
    std::vector<TimerEntry>,
    TimerEntryGreater
>;

// ── Timer snapshot for diagnostics ──────────────────────────────────────

/// Read-only snapshot of a timer entry, for `kairos explain` / MCP.
struct TimerSnapshot {
    std::string trigger_id;
    std::string target_id;
    std::string target_name;
    std::string trigger_type;   ///< "cron" | "interval" | "date"
    std::string expression;     ///< Cron expr, interval duration, ISO date
    std::string next_fire;      ///< ISO 8601 of next projected fire
    std::string last_fire;      ///< ISO 8601 of last fire (or "never")
    int64_t ms_until_fire = 0;  ///< Milliseconds until next fire
    int active_instances = 0;
    int max_instances = 1;
    bool enabled = true;
};

// ── Active run tracker ──────────────────────────────────────────────────

/// Tracks the number of active (in-flight) runs per target,
/// for max_instances enforcement.
///
/// Thread safety: protected by internal mutex. Access frequency is low
/// (proportional to job count, not event rate).
class ActiveRunTracker {
public:
    /// Increment active count for a target. Returns new count.
    int increment(const std::string& target_id) {
        std::lock_guard lock(mu_);
        return ++counts_[target_id];
    }

    /// Decrement active count. Returns new count (clamped to 0).
    int decrement(const std::string& target_id) {
        std::lock_guard lock(mu_);
        auto it = counts_.find(target_id);
        if (it == counts_.end() || it->second <= 0) return 0;
        int val = --it->second;
        if (val == 0) counts_.erase(it);
        return val;
    }

    /// Current count for a target.
    [[nodiscard]] int count(const std::string& target_id) const {
        std::lock_guard lock(mu_);
        auto it = counts_.find(target_id);
        return it != counts_.end() ? it->second : 0;
    }

    /// Check if a target can fire (active < max_instances).
    [[nodiscard]] bool can_fire(const std::string& target_id,
                                 int max_instances) const {
        return count(target_id) < max_instances;
    }

    /// Reset all counts (for reconciliation).
    void reset() {
        std::lock_guard lock(mu_);
        counts_.clear();
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, int> counts_;
};

}  // namespace kairos::engine
