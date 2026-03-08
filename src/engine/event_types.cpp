/// src/engine/event_types.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  event_types.cpp — WatchEventType ↔ string conversion                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/watch/event_types.hpp"

#include <array>
#include <sstream>
#include <string_view>

namespace kairos::watch {

namespace {

struct FlagEntry {
    WatchEventType flag;
    std::string_view name;
};

constexpr std::array<FlagEntry, 16> kFlagTable = {{
    {WatchEventType::FileCreated,        "file_created"},
    {WatchEventType::FileDeleted,        "file_deleted"},
    {WatchEventType::SizeChanged,        "size_changed"},
    {WatchEventType::ContentModified,    "content_modified"},
    {WatchEventType::ContentChanged,     "content_changed"},
    {WatchEventType::PermissionsChanged, "permissions_changed"},
    {WatchEventType::OwnerChanged,       "owner_changed"},
    {WatchEventType::PatternFound,       "pattern_found"},
    {WatchEventType::PatternRemoved,     "pattern_removed"},
    {WatchEventType::FilesChanged,       "files_changed"},
    {WatchEventType::SubdirsChanged,     "subdirs_changed"},
    {WatchEventType::DirSizeChanged,     "dir_size_changed"},
    {WatchEventType::StructureChanged,   "structure_changed"},
    {WatchEventType::ScanTimeout,        "scan_timeout"},
    {WatchEventType::QueueOverflow,      "queue_overflow"},
    {WatchEventType::WatchError,         "watch_error"},
}};

}  // namespace

std::string event_type_to_string(WatchEventType type) {
    if (type == WatchEventType::Unknown) return "unknown";

    std::string result;
    for (const auto& [flag, name] : kFlagTable) {
        if (has_flag(type, flag)) {
            if (!result.empty()) result += ',';
            result += name;
        }
    }
    return result.empty() ? "unknown" : result;
}

WatchEventType string_to_event_type(std::string_view str) {
    auto result = WatchEventType::Unknown;

    // Split on commas and match each token.
    size_t start = 0;
    while (start < str.size()) {
        auto comma = str.find(',', start);
        auto token = str.substr(start,
            comma == std::string_view::npos ? std::string_view::npos
                                           : comma - start);

        // Trim whitespace.
        while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
        while (!token.empty() && token.back() == ' ') token.remove_suffix(1);

        for (const auto& [flag, name] : kFlagTable) {
            if (token == name) {
                result |= flag;
                break;
            }
        }

        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }

    return result;
}

}  // namespace kairos::watch
