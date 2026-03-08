/// include/kairos/testing/fake_filesystem.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/testing/fake_filesystem.hpp — Fake filesystem for watch tests    ║
// ║                                                                           ║
// ║  Provides deterministic filesystem state that the watch engine's         ║
// ║  snapshot scanner can query without touching the real filesystem.          ║
// ║                                                                           ║
// ║  Spec reference: §30.4                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace kairos::testing {

/// Simulated file metadata for test snapshots.
struct FakeFileEntry {
    std::filesystem::path path;
    std::uintmax_t size = 0;
    std::chrono::system_clock::time_point mtime;
    std::filesystem::perms permissions = std::filesystem::perms::owner_read;
    std::string content_hash;   ///< SHA-256 hex if known.
    bool is_directory = false;
    bool is_symlink = false;
    std::filesystem::path symlink_target;  ///< Only set if is_symlink.
};

/// A fake filesystem that the watch engine's snapshot scanner can query.
///
/// The WatchEngine's snapshot function is parameterized by a
/// FilesystemScanner concept/interface. In production, it calls
/// std::filesystem::recursive_directory_iterator and stat().
/// In tests, it queries this FakeFilesystem instead.
///
/// Usage:
///   FakeFilesystem fs;
///   fs.add_file("/watched/a.txt", {.size = 100, .mtime = T0});
///   // ... take snapshot via watch engine ...
///   fs.modify_file("/watched/a.txt", {.size = 200, .mtime = T1});
///   // ... take second snapshot, diff detects size_changed ...
class FakeFilesystem {
public:
    /// Add a file to the fake filesystem.
    void add_file(const std::filesystem::path& path, FakeFileEntry entry) {
        entry.path = path;
        files_[path] = std::move(entry);
    }

    /// Add a directory to the fake filesystem.
    void add_directory(const std::filesystem::path& path,
                        std::chrono::system_clock::time_point mtime = {}) {
        FakeFileEntry entry;
        entry.path = path;
        entry.is_directory = true;
        entry.mtime = mtime;
        files_[path] = std::move(entry);
    }

    /// Modify an existing file's metadata.
    void modify_file(const std::filesystem::path& path,
                      FakeFileEntry entry) {
        entry.path = path;
        files_[path] = std::move(entry);
    }

    /// Remove a file from the fake filesystem.
    void remove_file(const std::filesystem::path& path) {
        files_.erase(path);
    }

    /// Clear all files.
    void clear() { files_.clear(); }

    /// List all files under `root` up to `max_depth`.
    /// Respects exclude globs if provided.
    [[nodiscard]] std::vector<FakeFileEntry> scan(
        const std::filesystem::path& root,
        int max_depth = -1,
        const std::vector<std::string>& exclude_globs = {}) const
    {
        std::vector<FakeFileEntry> result;
        for (const auto& [path, entry] : files_) {
            // Check if under root.
            auto rel = path.lexically_relative(root);
            if (rel.empty() || rel.string().starts_with("..")) continue;

            // Check max_depth.
            if (max_depth >= 0) {
                int depth = 0;
                for (auto it = rel.begin(); it != rel.end(); ++it) ++depth;
                // depth counts path components; file at root/a.txt = depth 1.
                if (depth > max_depth + 1) continue;
            }

            // Check exclude globs (simple prefix/suffix matching).
            bool excluded = false;
            for (const auto& glob : exclude_globs) {
                if (path.string().find(glob) != std::string::npos) {
                    excluded = true;
                    break;
                }
            }
            if (excluded) continue;

            result.push_back(entry);
        }
        return result;
    }

    /// Check if a path exists.
    [[nodiscard]] bool exists(const std::filesystem::path& path) const {
        return files_.count(path) > 0;
    }

    /// Get metadata for a specific file.
    [[nodiscard]] std::optional<FakeFileEntry> stat(
        const std::filesystem::path& path) const
    {
        auto it = files_.find(path);
        if (it == files_.end()) return std::nullopt;
        return it->second;
    }

    /// Total number of entries.
    [[nodiscard]] size_t entry_count() const { return files_.size(); }

private:
    std::map<std::filesystem::path, FakeFileEntry> files_;
};

}  // namespace kairos::testing
