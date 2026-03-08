/// include/kairos/watch/event_types.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/watch/event_types.hpp — Filesystem event type taxonomy            ║
// ║                                                                           ║
// ║  Unified event type bitflags, compatible with EventWatcher's              ║
// ║  get_event_type(). Multiple types can be combined via operator|.          ║
// ║                                                                           ║
// ║  Spec reference: §12.8 (event typing)                                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kairos::watch {

/// Filesystem event types, matching EventWatcher's taxonomy.
/// Multiple types can be combined (e.g., "size_changed,content_changed").
enum class WatchEventType : uint32_t {
    // File events
    FileCreated         = 1u << 0,
    FileDeleted         = 1u << 1,
    SizeChanged         = 1u << 2,
    ContentModified     = 1u << 3,   ///< mtime changed
    ContentChanged      = 1u << 4,   ///< hash changed
    PermissionsChanged  = 1u << 5,
    OwnerChanged        = 1u << 6,
    PatternFound        = 1u << 7,
    PatternRemoved      = 1u << 8,

    // Directory events
    FilesChanged        = 1u << 9,   ///< files_count changed
    SubdirsChanged      = 1u << 10,  ///< subdirs_count changed
    DirSizeChanged      = 1u << 11,  ///< total directory size changed
    StructureChanged    = 1u << 12,  ///< files added/removed within dir

    // Meta events
    ScanTimeout         = 1u << 13,  ///< Scan exceeded time budget
    QueueOverflow       = 1u << 14,  ///< Native watcher queue overflow
    WatchError          = 1u << 15,  ///< Watch setup/maintenance error

    Unknown             = 0
};

/// Bitwise OR for combining event types.
constexpr WatchEventType operator|(WatchEventType a, WatchEventType b) {
    return static_cast<WatchEventType>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

/// Bitwise OR assignment.
constexpr WatchEventType& operator|=(WatchEventType& a, WatchEventType b) {
    a = a | b;
    return a;
}

/// Bitwise AND for testing flags.
constexpr WatchEventType operator&(WatchEventType a, WatchEventType b) {
    return static_cast<WatchEventType>(
        static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

/// Check if a combined type contains a specific flag.
constexpr bool has_flag(WatchEventType combined, WatchEventType flag) {
    return (static_cast<uint32_t>(combined) &
            static_cast<uint32_t>(flag)) != 0;
}

/// Convert event type flags to human-readable comma-separated string.
/// E.g., "size_changed,content_changed"
[[nodiscard]] std::string event_type_to_string(WatchEventType type);

/// Parse event type string back to flags.
[[nodiscard]] WatchEventType string_to_event_type(std::string_view str);

}  // namespace kairos::watch
