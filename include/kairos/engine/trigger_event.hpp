/// include/kairos/engine/trigger_event.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/engine/trigger_event.hpp — Unified trigger event type             ║
// ║                                                                           ║
// ║  Every trigger source (scheduler, watch engine, CLI, MCP) produces a      ║
// ║  TriggerEvent. The Pipeline thread consumes these from the trigger bus     ║
// ║  and dispatches work accordingly.                                         ║
// ║                                                                           ║
// ║  This is the fundamental unification type: schedule ticks, file events,   ║
// ║  file diffs, manual runs, and MCP calls all become TriggerEvents.         ║
// ║                                                                           ║
// ║  Spec reference: §3.1 (unification pipeline), §11.2 (trigger bus)       ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/core/bounded_queue.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace kairos::engine {

// ── Payload types for different trigger sources ─────────────────────────

/// Payload for schedule-based triggers (cron, interval, date).
struct ScheduleTickPayload {
    std::string trigger_type;    ///< "cron", "interval", or "date"
    std::string expression;      ///< Original schedule expression
    bool is_misfire = false;     ///< True if this is a catch-up fire
};

/// Payload for native filesystem events (inotify, FSEvents, RDCW).
struct FileEventPayload {
    std::string watch_group;     ///< Which watch-group detected this
    std::vector<std::string> changed_paths;  ///< Affected file paths
    std::string event_type;      ///< Stringified WatchEventType flags
};

/// Payload for sample+diff filesystem events.
struct FileDiffPayload {
    std::string watch_group;
    int files_added = 0;
    int files_removed = 0;
    int files_modified = 0;
    std::vector<std::string> affected_paths;
    std::string diff_summary;    ///< Human-readable diff summary
};

/// Payload for manual CLI runs.
struct ManualRunPayload {
    std::string invoked_by;      ///< "cli" or "mcp"
    std::string extra_args;      ///< Any additional CLI arguments
};

/// Payload for config reload events.
struct ConfigReloadPayload {
    std::string source;          ///< "signal", "mcp", "cli", or "watch"
};

/// Union of all payload types.
using TriggerPayload = std::variant<
    ScheduleTickPayload,
    FileEventPayload,
    FileDiffPayload,
    ManualRunPayload,
    ConfigReloadPayload
>;

// ── The TriggerEvent itself ─────────────────────────────────────────────

/// Discriminant for the trigger source.
enum class TriggerType {
    ScheduleTick,    ///< Timer fired (cron, interval, or date)
    FileEvent,       ///< Native filesystem notification
    FileDiff,        ///< Sample+diff detected changes
    ManualRun,       ///< CLI `kairos run` or MCP `kairos.runWorkflow`
    ConfigReload,    ///< Configuration reload request
};

/// Convert TriggerType to a string for logging/persistence.
[[nodiscard]] constexpr std::string_view trigger_type_to_string(
    TriggerType type) noexcept
{
    switch (type) {
        case TriggerType::ScheduleTick: return "schedule";
        case TriggerType::FileEvent:    return "watch";
        case TriggerType::FileDiff:     return "watch";
        case TriggerType::ManualRun:    return "manual";
        case TriggerType::ConfigReload: return "config_reload";
    }
    return "unknown";
}

/// Parse a trigger type string back to enum.
[[nodiscard]] inline TriggerType string_to_trigger_type(
    std::string_view str) noexcept
{
    if (str == "schedule")       return TriggerType::ScheduleTick;
    if (str == "watch")          return TriggerType::FileEvent;
    if (str == "manual")         return TriggerType::ManualRun;
    if (str == "config_reload")  return TriggerType::ConfigReload;
    return TriggerType::ManualRun;  // Safe default
}

/// A trigger event: the fundamental message in the Kairos pipeline.
///
/// Produced by: Scheduler, WatchEngine, CLI, MCP.
/// Consumed by: Pipeline thread.
/// Transport:   Trigger Bus (BoundedQueue<TriggerEvent>).
struct TriggerEvent {
    /// What kind of trigger produced this event.
    TriggerType type = TriggerType::ManualRun;

    /// Which trigger definition produced this event.
    /// For schedules: the trigger's content-addressable ID (trg-xxx).
    /// For watches: the watch-group name.
    /// For manual: "cli" or "mcp".
    std::string trigger_id;

    /// Which job or workflow this trigger targets.
    std::string target_id;

    /// Is this a standalone job or a workflow?
    enum class TargetKind { Workflow, StandaloneJob };
    TargetKind target_kind = TargetKind::Workflow;

    /// Wall-clock time when the event was produced.
    std::chrono::system_clock::time_point fire_time{};

    /// Monotonic time when the event was produced (for ordering).
    std::chrono::steady_clock::time_point mono_time{};

    /// Correlation ID for end-to-end tracing.
    /// Propagated through the entire pipeline: trigger → plan → jobs → steps.
    std::string correlation_id;

    /// Trigger-specific payload data.
    TriggerPayload payload = ManualRunPayload{};

    // ── Factory methods for common trigger types ─────────────────────

    /// Create a schedule tick event.
    static TriggerEvent make_schedule_tick(
        std::string trigger_id,
        std::string target_id,
        TargetKind target_kind,
        std::chrono::system_clock::time_point fire_time,
        std::string correlation_id,
        ScheduleTickPayload payload)
    {
        return TriggerEvent{
            .type = TriggerType::ScheduleTick,
            .trigger_id = std::move(trigger_id),
            .target_id = std::move(target_id),
            .target_kind = target_kind,
            .fire_time = fire_time,
            .mono_time = std::chrono::steady_clock::now(),
            .correlation_id = std::move(correlation_id),
            .payload = std::move(payload),
        };
    }

    /// Create a manual run event.
    static TriggerEvent make_manual_run(
        std::string target_id,
        TargetKind target_kind,
        std::string correlation_id,
        std::string invoked_by = "cli")
    {
        return TriggerEvent{
            .type = TriggerType::ManualRun,
            .trigger_id = invoked_by,
            .target_id = std::move(target_id),
            .target_kind = target_kind,
            .fire_time = std::chrono::system_clock::now(),
            .mono_time = std::chrono::steady_clock::now(),
            .correlation_id = std::move(correlation_id),
            .payload = ManualRunPayload{
                .invoked_by = std::move(invoked_by)},
        };
    }

    /// Create a file event.
    static TriggerEvent make_file_event(
        std::string trigger_id,
        std::string target_id,
        TargetKind target_kind,
        std::string correlation_id,
        FileEventPayload payload)
    {
        return TriggerEvent{
            .type = TriggerType::FileEvent,
            .trigger_id = std::move(trigger_id),
            .target_id = std::move(target_id),
            .target_kind = target_kind,
            .fire_time = std::chrono::system_clock::now(),
            .mono_time = std::chrono::steady_clock::now(),
            .correlation_id = std::move(correlation_id),
            .payload = std::move(payload),
        };
    }

    /// Create a config reload event.
    static TriggerEvent make_config_reload(
        std::string source,
        std::string correlation_id)
    {
        return TriggerEvent{
            .type = TriggerType::ConfigReload,
            .trigger_id = "config_reload",
            .target_id = {},
            .target_kind = TargetKind::Workflow,
            .fire_time = std::chrono::system_clock::now(),
            .mono_time = std::chrono::steady_clock::now(),
            .correlation_id = std::move(correlation_id),
            .payload = ConfigReloadPayload{.source = std::move(source)},
        };
    }
};

// ── Trigger Bus typedef ─────────────────────────────────────────────────

/// The trigger bus: a bounded MPMC queue of TriggerEvents.
/// Capacity 1024 per spec §3.3.
using TriggerBus = core::BoundedQueue<TriggerEvent>;

/// Callback type for trigger producers to emit events.
using TriggerSink = std::function<bool(TriggerEvent)>;

}  // namespace kairos::engine
