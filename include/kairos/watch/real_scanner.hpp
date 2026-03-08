/// include/kairos/watch/real_scanner.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/watch/real_scanner.hpp — Production filesystem scanner            ║
// ║                                                                           ║
// ║  Implements IFilesystemScanner using std::filesystem::recursive_          ║
// ║  directory_iterator with bounded depth, time budget, exclude globs,       ║
// ║  and cooperative stop_token cancellation.                                 ║
// ║                                                                           ║
// ║  Spec reference: §12.6 (sample collection), §12.6.3 (hashing)           ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/watch/sample.hpp"

#include <chrono>
#include <optional>
#include <regex>

namespace kairos::watch {

/// Production filesystem scanner.
///
/// Thread safety: stateless after construction. Multiple threads may call
/// scan() / stat_file() concurrently on different paths.
class RealFilesystemScanner : public IFilesystemScanner {
public:
    /// Default constructor.
    RealFilesystemScanner() = default;

    /// Scan all entries under root, up to max_depth.
    /// Respects exclude_globs. Aborts if stop_token is triggered.
    ///
    /// Cross-platform notes:
    ///  - Paths are normalized to UTF-8 forward slashes for consistency.
    ///  - On Windows, uses fs::path::generic_u8string() for normalization.
    ///  - Symlinks: resolved by default. Cycle detection via visited-set.
    ///  - Permissions: octal string on POSIX, "0000" on Windows.
    ///  - UID/GID: populated on POSIX only.
    [[nodiscard]] std::vector<ScannedEntry> scan(
        const fs::path& root,
        int max_depth,
        const std::vector<std::string>& exclude_globs,
        std::stop_token stop) const override;

    /// Stat a single file (for targeted re-scan after native events).
    [[nodiscard]] std::optional<ScannedEntry> stat_file(
        const fs::path& path) const override;

    /// Normalize a path to canonical UTF-8 form.
    /// Used for consistent hashing and comparison across platforms.
    static std::string normalize_path(const fs::path& p);

private:
    /// Check if a path matches any exclude glob pattern.
    static bool matches_exclude(
        const std::string& rel_path,
        const std::vector<std::string>& exclude_globs);

    /// Simple glob matching (supports *, ?, [abc], **).
    static bool glob_match(std::string_view pattern,
                            std::string_view text);

    /// Build a ScannedEntry from a directory_entry.
    static ScannedEntry entry_from_dir_entry(
        const fs::directory_entry& entry,
        const fs::path& root);
};

}  // namespace kairos::watch
