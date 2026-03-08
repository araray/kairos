/// include/kairos/watch/debounce_buffer.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/watch/debounce_buffer.hpp — Native event debouncing/coalescing   ║
// ║                                                                           ║
// ║  Rapid-fire native events (e.g., continuous writes, editor save+rename)  ║
// ║  are collected per-path and coalesced once no new event has arrived for   ║
// ║  `debounce_duration` milliseconds.                                        ║
// ║                                                                           ║
// ║  Coalescing rules (spec §12.9):                                           ║
// ║    Created → Modified → Modified   =  Created (final state)              ║
// ║    Modified → Modified → Modified  =  Modified (single event)            ║
// ║    Created → Deleted               =  Nothing (path appeared and vanished)║
// ║    Deleted → Created               =  Modified (file was replaced)       ║
// ║    Renamed(A→B) → Modified(B)      =  Renamed + Modified (both emitted) ║
// ║                                                                           ║
// ║  Spec reference: §12.9                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/watch/file_watcher.hpp"

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace kairos::watch {

/// Debounce buffer for native filesystem events.
///
/// Events are grouped by path. When a path's events settle
/// (no new events for debounce_duration), the coalesced event is emitted.
///
/// Thread safety: NOT thread-safe. Intended to be used from a single
/// coordinator thread that both adds events and drains settled ones.
class DebounceBuffer {
public:
    /// Construct with a debounce window duration.
    explicit DebounceBuffer(
        std::chrono::milliseconds debounce_duration =
            std::chrono::milliseconds(200));

    /// Add a native event to the buffer.
    /// If the path already has a pending event, coalescing rules apply.
    void add(NativeEvent event);

    /// Add a native event with an explicit timestamp (for testing).
    void add(NativeEvent event,
             std::chrono::steady_clock::time_point now);

    /// Drain events that have settled (no activity for debounce_duration).
    /// Returns events whose last_activity + debounce_duration <= now.
    [[nodiscard]] std::vector<NativeEvent> drain_settled(
        std::chrono::steady_clock::time_point now);

    /// Convenience: drain using steady_clock::now().
    [[nodiscard]] std::vector<NativeEvent> drain_settled();

    /// Number of pending (not yet settled) entries.
    [[nodiscard]] size_t pending_count() const;

    /// Clear all pending entries.
    void clear();

    /// Time until the earliest entry settles (for sleep scheduling).
    /// Returns 0 if any entry is already settled.
    /// Returns max duration if buffer is empty.
    [[nodiscard]] std::chrono::milliseconds time_until_next_settle(
        std::chrono::steady_clock::time_point now) const;

private:
    /// State for a single pending path.
    struct PendingEntry {
        NativeEvent first_event;    ///< The initial event type.
        NativeEvent latest_event;   ///< The most recent event (carries final state).
        std::chrono::steady_clock::time_point last_activity;
        int event_count = 0;
    };

    /// Apply coalescing rules between existing entry and new event.
    /// Returns true if the entry should be kept, false to remove it
    /// (e.g., Created+Deleted = nothing).
    bool coalesce(PendingEntry& entry, const NativeEvent& new_event);

    std::chrono::milliseconds debounce_duration_;
    std::unordered_map<std::string, PendingEntry> pending_;
};

}  // namespace kairos::watch
