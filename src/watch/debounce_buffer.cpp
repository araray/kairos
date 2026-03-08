/// src/watch/debounce_buffer.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  debounce_buffer.cpp — Native event debouncing/coalescing                ║
// ║                                                                           ║
// ║  Implements the coalescing state machine from spec §12.9.                ║
// ║                                                                           ║
// ║  Key design choices:                                                      ║
// ║    - Keyed by path string (canonical UTF-8).                              ║
// ║    - Overflow events bypass debouncing (immediately emitted).             ║
// ║    - Created→Deleted produces nothing (file appeared and vanished).       ║
// ║    - Deleted→Created becomes Modified (file was replaced).               ║
// ║    - First event type is preserved for Created (so the watch engine      ║
// ║      knows it's a new file, not a modification to an existing one).      ║
// ║                                                                           ║
// ║  Spec reference: §12.9                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/watch/debounce_buffer.hpp"

#include <algorithm>

namespace kairos::watch {

// ── Constructor ─────────────────────────────────────────────────────────

DebounceBuffer::DebounceBuffer(std::chrono::milliseconds debounce_duration)
    : debounce_duration_(debounce_duration)
{}

// ── add ─────────────────────────────────────────────────────────────────

void DebounceBuffer::add(NativeEvent event) {
    add(std::move(event), std::chrono::steady_clock::now());
}

void DebounceBuffer::add(
    NativeEvent event,
    std::chrono::steady_clock::time_point now)
{
    // Overflow and Error events are never debounced — they're special
    // signals that require immediate processing.
    if (event.type == NativeEventType::Overflow ||
        event.type == NativeEventType::Error) {
        // These are handled separately by the watch engine coordinator.
        // We still store them so drain_settled() will return them immediately.
        auto key = event.path.string();
        PendingEntry entry;
        entry.first_event = event;
        entry.latest_event = std::move(event);
        entry.last_activity = now - debounce_duration_;  // Already settled.
        entry.event_count = 1;
        pending_[key] = std::move(entry);
        return;
    }

    auto key = event.path.string();
    auto it = pending_.find(key);

    if (it == pending_.end()) {
        // New path: insert fresh entry.
        PendingEntry entry;
        entry.first_event = event;
        entry.latest_event = std::move(event);
        entry.last_activity = now;
        entry.event_count = 1;
        pending_[key] = std::move(entry);
    } else {
        // Existing path: apply coalescing rules.
        if (!coalesce(it->second, event)) {
            // Coalescing determined this path should be removed
            // (e.g., Created + Deleted = nothing happened).
            pending_.erase(it);
        } else {
            // Update timestamp to extend the debounce window.
            it->second.last_activity = now;
        }
    }
}

// ── drain_settled ───────────────────────────────────────────────────────

std::vector<NativeEvent> DebounceBuffer::drain_settled(
    std::chrono::steady_clock::time_point now)
{
    std::vector<NativeEvent> result;

    auto it = pending_.begin();
    while (it != pending_.end()) {
        auto deadline = it->second.last_activity + debounce_duration_;
        if (deadline <= now) {
            // This entry has settled — emit the coalesced event.
            auto& entry = it->second;

            // The emitted event type depends on the coalesced state:
            // If the first event was Created and the latest is Modified,
            // emit as Created (the file is new).
            if (entry.first_event.type == NativeEventType::Created &&
                entry.latest_event.type == NativeEventType::Modified) {
                entry.latest_event.type = NativeEventType::Created;
            }

            result.push_back(std::move(entry.latest_event));
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }

    return result;
}

std::vector<NativeEvent> DebounceBuffer::drain_settled() {
    return drain_settled(std::chrono::steady_clock::now());
}

// ── pending_count ───────────────────────────────────────────────────────

size_t DebounceBuffer::pending_count() const {
    return pending_.size();
}

// ── clear ───────────────────────────────────────────────────────────────

void DebounceBuffer::clear() {
    pending_.clear();
}

// ── time_until_next_settle ──────────────────────────────────────────────

std::chrono::milliseconds DebounceBuffer::time_until_next_settle(
    std::chrono::steady_clock::time_point now) const
{
    if (pending_.empty()) {
        return std::chrono::milliseconds::max();
    }

    auto earliest_deadline =
        std::chrono::steady_clock::time_point::max();

    for (const auto& [_, entry] : pending_) {
        auto deadline = entry.last_activity + debounce_duration_;
        if (deadline < earliest_deadline) {
            earliest_deadline = deadline;
        }
    }

    if (earliest_deadline <= now) {
        return std::chrono::milliseconds(0);
    }

    return std::chrono::duration_cast<std::chrono::milliseconds>(
        earliest_deadline - now);
}

// ── coalesce ────────────────────────────────────────────────────────────
//
// Coalescing state machine (spec §12.9):
//
// ┌───────────────────┬───────────────────┬───────────────────────────┐
// │ Existing first    │ New event type    │ Result                    │
// ├───────────────────┼───────────────────┼───────────────────────────┤
// │ Created           │ Modified          │ Keep (first=Created)      │
// │ Created           │ Deleted           │ REMOVE (appeared+vanished)│
// │ Created           │ Created           │ Keep (first=Created)      │
// │ Modified          │ Modified          │ Keep (first=Modified)     │
// │ Modified          │ Deleted           │ Keep as Deleted           │
// │ Modified          │ Created           │ Keep (first=Modified)     │
// │ Deleted           │ Created           │ Keep as Modified          │
// │ Deleted           │ Modified          │ Keep as Modified          │
// │ Deleted           │ Deleted           │ Keep as Deleted           │
// │ Renamed           │ Modified          │ Keep (first=Renamed)      │
// │ Renamed           │ Deleted           │ Keep as Deleted           │
// │ * (any)           │ Renamed           │ Keep as Renamed           │
// └───────────────────┴───────────────────┴───────────────────────────┘

bool DebounceBuffer::coalesce(PendingEntry& entry,
                               const NativeEvent& new_event)
{
    entry.event_count++;

    auto first_type = entry.first_event.type;
    auto new_type = new_event.type;

    // Created → Deleted = nothing (file appeared and vanished).
    if (first_type == NativeEventType::Created &&
        new_type == NativeEventType::Deleted) {
        return false;  // Remove this entry.
    }

    // Deleted → Created = Modified (file was replaced).
    if (first_type == NativeEventType::Deleted &&
        new_type == NativeEventType::Created) {
        entry.first_event.type = NativeEventType::Modified;
        entry.latest_event = new_event;
        entry.latest_event.type = NativeEventType::Modified;
        return true;
    }

    // Deleted → Modified = Modified (file was replaced with modification).
    if (first_type == NativeEventType::Deleted &&
        new_type == NativeEventType::Modified) {
        entry.first_event.type = NativeEventType::Modified;
        entry.latest_event = new_event;
        entry.latest_event.type = NativeEventType::Modified;
        return true;
    }

    // For all other combinations: keep the first_event type but update
    // the latest_event with the new event's data (path, timestamp, etc).
    // The drain_settled() method determines the final emitted type.
    entry.latest_event = new_event;

    // Special case: if new event is Deleted, override the emitted type.
    if (new_type == NativeEventType::Deleted) {
        entry.first_event.type = NativeEventType::Deleted;
    }

    // Special case: if new event is Renamed, track it.
    if (new_type == NativeEventType::Renamed) {
        entry.first_event.type = NativeEventType::Renamed;
    }

    return true;
}

}  // namespace kairos::watch
