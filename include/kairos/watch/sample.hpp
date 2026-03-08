/// include/kairos/watch/sample.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/watch/sample.hpp — File metrics, samples, and diffs              ║
// ║                                                                           ║
// ║  Core data types for the sample+diff engine. Mirrors EventWatcher's      ║
// ║  collect_sample() / compare_samples() / get_event_type() but in C++.     ║
// ║                                                                           ║
// ║  Spec reference: §12.6                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/watch/event_types.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

namespace kairos::watch {

namespace fs = std::filesystem;

// ── File Metrics (per-file) ─────────────────────────────────────────────

/// Metrics collected for a single filesystem entry.
/// Matches EventWatcher's sample data format: size, mtime, permissions,
/// ownership, md5, sha256, pattern_found.
struct FileMetrics {
    std::string path;               ///< Normalized absolute path (UTF-8).
    std::string entry_type;         ///< "file" | "directory" | "symlink"

    // Common metrics:
    int64_t size = 0;               ///< File size in bytes (or total dir size).
    std::chrono::system_clock::time_point last_modified;
    std::string permissions;        ///< Octal string, e.g., "0644".

    // File-specific:
    std::optional<std::string> md5;
    std::optional<std::string> sha256;
    std::optional<bool> pattern_found;  ///< Regex match result.

    // Directory-specific:
    std::optional<int> files_count;
    std::optional<int> subdirs_count;
    std::optional<int64_t> total_size;  ///< Recursive size.

    // Ownership (POSIX only):
    std::optional<uint32_t> uid;
    std::optional<uint32_t> gid;
    std::optional<std::string> owner;
    std::optional<std::string> group;
};

// ── Sample (full snapshot) ──────────────────────────────────────────────

/// A complete sample: path → metrics.
/// Represents one snapshot of a watch group's filesystem state.
struct Sample {
    int64_t epoch = 0;  ///< Incrementing counter per group.
    std::unordered_map<std::string, FileMetrics> entries;

    [[nodiscard]] bool empty() const { return entries.empty(); }
    [[nodiscard]] size_t size() const { return entries.size(); }
};

// ── Sample Diff ─────────────────────────────────────────────────────────

/// Differences between two samples.
struct SampleDiff {
    std::vector<std::string> created;    ///< New entries (paths).
    std::vector<std::string> deleted;    ///< Removed entries (paths).

    /// A single field change within a modified entry.
    struct FieldChange {
        std::string field;
        std::string old_value;
        std::string new_value;
    };

    /// Modified entries: path → list of field changes.
    std::unordered_map<std::string, std::vector<FieldChange>> modified;

    [[nodiscard]] bool empty() const {
        return created.empty() && deleted.empty() && modified.empty();
    }

    /// Total number of changes (created + deleted + modified entries).
    [[nodiscard]] size_t change_count() const {
        return created.size() + deleted.size() + modified.size();
    }
};

// ── Scan bounds ─────────────────────────────────────────────────────────

/// Scanning bounds per spec §12.6.2.
struct ScanBounds {
    int max_depth = 10;                                ///< Max recursion depth.
    std::chrono::milliseconds time_budget{30000};      ///< Abort if exceeded.
    int max_files = 100000;                            ///< Safety limit.
    std::vector<std::string> exclude_globs;            ///< Patterns to skip.
};

/// Hash computation policy per spec §12.6.3.
enum class HashPolicy {
    MtimeOnly,      ///< No hashes; rely on mtime alone.
    SizePlusMtime,  ///< Hash only when size or mtime changed (default).
    Full,           ///< Always compute hashes (expensive).
};

/// Parse hash policy from string.
[[nodiscard]] inline HashPolicy parse_hash_policy(std::string_view s) {
    if (s == "mtime")       return HashPolicy::MtimeOnly;
    if (s == "size+mtime")  return HashPolicy::SizePlusMtime;
    if (s == "full")        return HashPolicy::Full;
    return HashPolicy::SizePlusMtime;  // Safe default.
}

// ── Scanner abstraction (for testability) ───────────────────────────────

/// Result of scanning a single filesystem entry.
struct ScannedEntry {
    std::string path;
    std::string entry_type;         ///< "file" | "directory" | "symlink"
    int64_t size = 0;
    std::chrono::system_clock::time_point mtime;
    std::string permissions;
    bool is_directory = false;
    bool is_symlink = false;
    std::optional<uint32_t> uid;
    std::optional<uint32_t> gid;

    // Directory stats (populated for directories).
    std::optional<int> files_count;
    std::optional<int> subdirs_count;
};

/// Abstract filesystem scanner interface.
/// Production uses RealFilesystemScanner; tests use FakeFilesystemScanner.
class IFilesystemScanner {
public:
    virtual ~IFilesystemScanner() = default;

    /// Scan all entries under root, up to max_depth.
    /// Respects exclude_globs. Aborts if stop_token is triggered.
    [[nodiscard]] virtual std::vector<ScannedEntry> scan(
        const fs::path& root,
        int max_depth,
        const std::vector<std::string>& exclude_globs,
        std::stop_token stop) const = 0;

    /// Stat a single file (for targeted re-scan).
    [[nodiscard]] virtual std::optional<ScannedEntry> stat_file(
        const fs::path& path) const = 0;
};

// ── Core algorithms ─────────────────────────────────────────────────────

/// Compute the difference between current and previous samples.
/// Per spec §12.6: compares all fields, produces FieldChange entries
/// for each differing field.
[[nodiscard]] SampleDiff compute_diff(
    const Sample& current,
    const Sample& previous);

/// Determine the event type from a set of field changes.
/// Returns a WatchEventType bitmask.
/// Maps to EventWatcher's get_event_type().
[[nodiscard]] WatchEventType classify_changes(
    const std::vector<SampleDiff::FieldChange>& changes,
    const std::string& entry_type);

/// Classify changes and return the string representation.
[[nodiscard]] std::string classify_changes_string(
    const std::vector<SampleDiff::FieldChange>& changes,
    const std::string& entry_type);

}  // namespace kairos::watch
