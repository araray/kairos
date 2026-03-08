/// src/watch/sample.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  sample.cpp — Sample collection, diff computation, change classification ║
// ║                                                                           ║
// ║  Direct C++ translation of EventWatcher's collect_sample(),              ║
// ║  compare_samples(), and get_event_type().                                ║
// ║                                                                           ║
// ║  Spec reference: §12.6                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/watch/sample.hpp"

#include <algorithm>
#include <sstream>

namespace kairos::watch {

// ── compute_diff ────────────────────────────────────────────────────────

SampleDiff compute_diff(const Sample& current, const Sample& previous) {
    SampleDiff diff;

    // 1. Find created entries: in current but not in previous.
    for (const auto& [path, _] : current.entries) {
        if (previous.entries.find(path) == previous.entries.end()) {
            diff.created.push_back(path);
        }
    }

    // 2. Find deleted entries: in previous but not in current.
    for (const auto& [path, _] : previous.entries) {
        if (current.entries.find(path) == current.entries.end()) {
            diff.deleted.push_back(path);
        }
    }

    // 3. Find modified entries: in both, with changed fields.
    for (const auto& [path, cur] : current.entries) {
        auto prev_it = previous.entries.find(path);
        if (prev_it == previous.entries.end()) continue;
        const auto& prev = prev_it->second;

        std::vector<SampleDiff::FieldChange> changes;

        // Size.
        if (cur.size != prev.size) {
            changes.push_back({
                .field = "size",
                .old_value = std::to_string(prev.size),
                .new_value = std::to_string(cur.size),
            });
        }

        // Mtime (compare as epoch seconds for human-readable diffs).
        auto cur_mtime = std::chrono::duration_cast<std::chrono::seconds>(
            cur.last_modified.time_since_epoch()).count();
        auto prev_mtime = std::chrono::duration_cast<std::chrono::seconds>(
            prev.last_modified.time_since_epoch()).count();
        if (cur_mtime != prev_mtime) {
            changes.push_back({
                .field = "mtime",
                .old_value = std::to_string(prev_mtime),
                .new_value = std::to_string(cur_mtime),
            });
        }

        // Permissions.
        if (cur.permissions != prev.permissions) {
            changes.push_back({
                .field = "permissions",
                .old_value = prev.permissions,
                .new_value = cur.permissions,
            });
        }

        // MD5 hash (if both present).
        if (cur.md5.has_value() && prev.md5.has_value() &&
            *cur.md5 != *prev.md5) {
            changes.push_back({
                .field = "md5",
                .old_value = *prev.md5,
                .new_value = *cur.md5,
            });
        }

        // SHA256 hash (if both present).
        if (cur.sha256.has_value() && prev.sha256.has_value() &&
            *cur.sha256 != *prev.sha256) {
            changes.push_back({
                .field = "sha256",
                .old_value = *prev.sha256,
                .new_value = *cur.sha256,
            });
        }

        // Pattern found.
        if (cur.pattern_found.has_value() && prev.pattern_found.has_value() &&
            *cur.pattern_found != *prev.pattern_found) {
            changes.push_back({
                .field = "pattern_found",
                .old_value = *prev.pattern_found ? "true" : "false",
                .new_value = *cur.pattern_found ? "true" : "false",
            });
        }

        // Owner UID.
        if (cur.uid.has_value() && prev.uid.has_value() &&
            *cur.uid != *prev.uid) {
            changes.push_back({
                .field = "uid",
                .old_value = std::to_string(*prev.uid),
                .new_value = std::to_string(*cur.uid),
            });
        }

        // Owner GID.
        if (cur.gid.has_value() && prev.gid.has_value() &&
            *cur.gid != *prev.gid) {
            changes.push_back({
                .field = "gid",
                .old_value = std::to_string(*prev.gid),
                .new_value = std::to_string(*cur.gid),
            });
        }

        // Directory: files_count.
        if (cur.files_count.has_value() && prev.files_count.has_value() &&
            *cur.files_count != *prev.files_count) {
            changes.push_back({
                .field = "files_count",
                .old_value = std::to_string(*prev.files_count),
                .new_value = std::to_string(*cur.files_count),
            });
        }

        // Directory: subdirs_count.
        if (cur.subdirs_count.has_value() && prev.subdirs_count.has_value() &&
            *cur.subdirs_count != *prev.subdirs_count) {
            changes.push_back({
                .field = "subdirs_count",
                .old_value = std::to_string(*prev.subdirs_count),
                .new_value = std::to_string(*cur.subdirs_count),
            });
        }

        // Directory: total_size.
        if (cur.total_size.has_value() && prev.total_size.has_value() &&
            *cur.total_size != *prev.total_size) {
            changes.push_back({
                .field = "total_size",
                .old_value = std::to_string(*prev.total_size),
                .new_value = std::to_string(*cur.total_size),
            });
        }

        if (!changes.empty()) {
            diff.modified[path] = std::move(changes);
        }
    }

    // Sort for determinism.
    std::sort(diff.created.begin(), diff.created.end());
    std::sort(diff.deleted.begin(), diff.deleted.end());

    return diff;
}

// ── classify_changes ────────────────────────────────────────────────────

WatchEventType classify_changes(
    const std::vector<SampleDiff::FieldChange>& changes,
    const std::string& entry_type)
{
    auto result = WatchEventType::Unknown;

    for (const auto& change : changes) {
        if (change.field == "size") {
            if (entry_type == "directory") {
                result |= WatchEventType::DirSizeChanged;
            } else {
                result |= WatchEventType::SizeChanged;
            }
        } else if (change.field == "mtime") {
            result |= WatchEventType::ContentModified;
        } else if (change.field == "md5" || change.field == "sha256") {
            result |= WatchEventType::ContentChanged;
        } else if (change.field == "permissions") {
            result |= WatchEventType::PermissionsChanged;
        } else if (change.field == "uid" || change.field == "gid") {
            result |= WatchEventType::OwnerChanged;
        } else if (change.field == "pattern_found") {
            // Determine if pattern was found or removed.
            if (change.new_value == "true") {
                result |= WatchEventType::PatternFound;
            } else {
                result |= WatchEventType::PatternRemoved;
            }
        } else if (change.field == "files_count") {
            result |= WatchEventType::FilesChanged;
        } else if (change.field == "subdirs_count") {
            result |= WatchEventType::SubdirsChanged;
        } else if (change.field == "total_size") {
            result |= WatchEventType::DirSizeChanged;
        }
    }

    return result;
}

std::string classify_changes_string(
    const std::vector<SampleDiff::FieldChange>& changes,
    const std::string& entry_type)
{
    return event_type_to_string(classify_changes(changes, entry_type));
}

}  // namespace kairos::watch
