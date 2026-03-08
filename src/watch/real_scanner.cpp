/// src/watch/real_scanner.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  real_scanner.cpp — Production IFilesystemScanner                         ║
// ║                                                                           ║
// ║  Uses std::filesystem for cross-platform directory iteration.             ║
// ║  Handles: depth limits, exclude globs, symlinks, permissions,            ║
// ║  and cooperative cancellation via stop_token.                             ║
// ║                                                                           ║
// ║  Spec reference: §12.6                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/watch/real_scanner.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <system_error>
#include <unordered_set>

#ifndef _WIN32
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace kairos::watch {

// ── Path normalization ──────────────────────────────────────────────────

std::string RealFilesystemScanner::normalize_path(const fs::path& p) {
    std::error_code ec;

    // Try canonical (resolves symlinks) first.
    auto canonical = fs::canonical(p, ec);
    if (!ec) {
        // Use generic_string for forward-slash normalization.
        return canonical.generic_string();
    }

    // Fallback: weakly_canonical (exists check with resolve).
    auto weak = fs::weakly_canonical(p, ec);
    if (!ec) {
        return weak.generic_string();
    }

    // Last resort: just generic-string the input.
    return p.generic_string();
}

// ── Glob matching ───────────────────────────────────────────────────────

bool RealFilesystemScanner::glob_match(std::string_view pattern,
                                        std::string_view text) {
    size_t pi = 0, ti = 0;
    size_t star_pi = std::string_view::npos;
    size_t star_ti = 0;

    while (ti < text.size()) {
        if (pi < pattern.size() && pattern[pi] == '*') {
            // Check for ** (matches across directory boundaries).
            if (pi + 1 < pattern.size() && pattern[pi + 1] == '*') {
                // ** matches everything including /.
                pi += 2;
                if (pi < pattern.size() && pattern[pi] == '/') {
                    ++pi;  // Skip the trailing / after **.
                }
                star_pi = pi;
                star_ti = ti;
                // Try matching rest from every position.
                while (ti <= text.size()) {
                    if (glob_match(
                            pattern.substr(star_pi),
                            text.substr(ti))) {
                        return true;
                    }
                    ++ti;
                }
                return false;
            }

            // Single * — matches anything except /.
            star_pi = pi++;
            star_ti = ti;

        } else if (pi < pattern.size() && pattern[pi] == '?') {
            // ? matches any single character except /.
            if (text[ti] == '/') return false;
            ++pi;
            ++ti;

        } else if (pi < pattern.size() && pattern[pi] == '[') {
            // Character class.
            ++pi;
            bool negate = (pi < pattern.size() && pattern[pi] == '!');
            if (negate) ++pi;

            bool matched = false;
            while (pi < pattern.size() && pattern[pi] != ']') {
                if (text[ti] == pattern[pi]) matched = true;
                ++pi;
            }
            if (pi < pattern.size()) ++pi;  // Skip ']'.

            if (negate ? matched : !matched) {
                if (star_pi != std::string_view::npos) {
                    pi = star_pi;
                    ti = ++star_ti;
                } else {
                    return false;
                }
            } else {
                ++ti;
            }

        } else if (pi < pattern.size() && pattern[pi] == text[ti]) {
            ++pi;
            ++ti;

        } else {
            // Mismatch — backtrack to last *.
            if (star_pi != std::string_view::npos) {
                pi = star_pi + 1;  // After the *.
                ti = ++star_ti;
            } else {
                return false;
            }
        }
    }

    // Consume trailing *s.
    while (pi < pattern.size() && pattern[pi] == '*') ++pi;

    return pi == pattern.size();
}

bool RealFilesystemScanner::matches_exclude(
    const std::string& rel_path,
    const std::vector<std::string>& exclude_globs)
{
    for (const auto& glob : exclude_globs) {
        if (glob_match(glob, rel_path)) return true;

        // Also match against the filename component alone.
        auto slash_pos = rel_path.rfind('/');
        std::string_view filename = (slash_pos != std::string::npos)
            ? std::string_view(rel_path).substr(slash_pos + 1)
            : std::string_view(rel_path);
        if (glob_match(glob, filename)) return true;
    }
    return false;
}

// ── Build ScannedEntry from directory_entry ─────────────────────────────

ScannedEntry RealFilesystemScanner::entry_from_dir_entry(
    const fs::directory_entry& dir_entry,
    const fs::path& root)
{
    ScannedEntry entry;
    std::error_code ec;

    // Normalize the path.
    entry.path = normalize_path(dir_entry.path());

    // File type.
    auto status = dir_entry.symlink_status(ec);
    if (ec) {
        // Cannot stat — return minimal entry.
        entry.entry_type = "unknown";
        return entry;
    }

    entry.is_symlink = fs::is_symlink(status);

    // For symlinks, follow to get the real type.
    auto resolved_status = dir_entry.status(ec);
    if (ec) {
        // Broken symlink.
        entry.entry_type = "symlink";
        return entry;
    }

    entry.is_directory = fs::is_directory(resolved_status);
    if (entry.is_directory) {
        entry.entry_type = "directory";
    } else if (fs::is_regular_file(resolved_status)) {
        entry.entry_type = "file";
    } else if (entry.is_symlink) {
        entry.entry_type = "symlink";
    } else {
        entry.entry_type = "other";
    }

    // Size.
    if (!entry.is_directory) {
        entry.size = static_cast<int64_t>(dir_entry.file_size(ec));
        if (ec) entry.size = 0;
    }

    // Modification time.
    auto lwt = dir_entry.last_write_time(ec);
    if (!ec) {
        // Convert file_time_type to system_clock.
        // C++20 provides clock_cast on conforming implementations.
        // Fallback: use duration arithmetic.
        auto file_dur = lwt.time_since_epoch();
        auto sys_dur = std::chrono::duration_cast<
            std::chrono::system_clock::duration>(file_dur);

        // On most implementations, file_clock epoch matches system_clock.
        // Use a known reference point for safety.
        entry.mtime = std::chrono::system_clock::time_point{sys_dur};
    }

    // Permissions (octal string).
#ifndef _WIN32
    {
        auto perms = resolved_status.permissions();
        unsigned perm_val = static_cast<unsigned>(perms) & 0777;
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%04o", perm_val);
        entry.permissions = buf;
    }

    // UID/GID (POSIX only).
    struct stat st;
    if (::stat(dir_entry.path().c_str(), &st) == 0) {
        entry.uid = st.st_uid;
        entry.gid = st.st_gid;
    }
#else
    entry.permissions = "0000";  // Not meaningful on Windows.
#endif

    // Directory stats.
    if (entry.is_directory) {
        int file_count = 0, subdir_count = 0;
        for (auto it = fs::directory_iterator(dir_entry.path(), ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            auto child_status = it->status(ec);
            if (ec) continue;
            if (fs::is_directory(child_status)) ++subdir_count;
            else ++file_count;
        }
        entry.files_count = file_count;
        entry.subdirs_count = subdir_count;
    }

    return entry;
}

// ── Scan implementation ─────────────────────────────────────────────────

std::vector<ScannedEntry> RealFilesystemScanner::scan(
    const fs::path& root,
    int max_depth,
    const std::vector<std::string>& exclude_globs,
    std::stop_token stop) const
{
    std::vector<ScannedEntry> results;
    std::error_code ec;

    // Verify root exists.
    if (!fs::exists(root, ec)) {
        return results;
    }

    // If root is a file (not a directory), scan just that file.
    if (fs::is_regular_file(root, ec)) {
        auto entry = stat_file(root);
        if (entry) results.push_back(std::move(*entry));
        return results;
    }

    // Resolve root for relative path computation.
    auto root_canonical = fs::canonical(root, ec);
    if (ec) root_canonical = root;

    // Manual recursive scan with depth tracking.
    // We use a stack-based DFS instead of recursive_directory_iterator
    // because we need fine-grained depth control and exclude matching.
    //
    // Symlink cycle detection (§12.10): we track canonical paths visited
    // during this scan. If a resolved path is already in the set, we
    // skip it and log a warning. This prevents infinite recursion when
    // symlinks form cycles (e.g., A → B → A).
    struct DirFrame {
        fs::path dir;
        int depth;
    };

    std::vector<DirFrame> stack;
    std::unordered_set<std::string> visited_canonical;

    // Register root as visited.
    visited_canonical.insert(root_canonical.string());
    stack.push_back({root_canonical, 0});

    while (!stack.empty() && !stop.stop_requested()) {
        auto [dir, depth] = stack.back();
        stack.pop_back();

        auto dir_it = fs::directory_iterator(dir, ec);
        if (ec) continue;

        for (auto it = dir_it; !ec && it != fs::directory_iterator();
             it.increment(ec))
        {
            if (stop.stop_requested()) break;

            // Compute relative path for exclude matching.
            auto rel = fs::relative(it->path(), root_canonical, ec);
            std::string rel_str = (ec) ? it->path().filename().string()
                                       : rel.generic_string();

            // Check excludes.
            if (matches_exclude(rel_str, exclude_globs)) continue;

            // Build entry.
            auto entry = entry_from_dir_entry(*it, root_canonical);
            results.push_back(entry);

            // Recurse into directories if within depth limit.
            if (entry.is_directory && depth < max_depth) {
                // ── Symlink cycle detection ──────────────────────
                // Resolve the canonical path. If we've already visited
                // this canonical path, it's a cycle — skip with warning.
                fs::path child_canonical;
                if (entry.is_symlink) {
                    child_canonical = fs::canonical(it->path(), ec);
                    if (ec) {
                        // Broken symlink target — skip recursion.
                        continue;
                    }
                } else {
                    child_canonical = it->path();
                    // Normalize for consistent comparison.
                    auto maybe = fs::canonical(child_canonical, ec);
                    if (!ec) child_canonical = maybe;
                }

                std::string canonical_str = child_canonical.string();
                if (visited_canonical.count(canonical_str)) {
                    // Cycle detected — skip this directory.
                    spdlog::warn(
                        "Symlink cycle detected: '{}' resolves to "
                        "already-visited '{}' — skipping",
                        it->path().string(), canonical_str);
                    continue;
                }

                visited_canonical.insert(canonical_str);
                stack.push_back({it->path(), depth + 1});
            }

            // Safety limit.
            if (results.size() >= 100000) break;
        }

        if (results.size() >= 100000) break;
    }

    return results;
}

// ── Stat a single file ──────────────────────────────────────────────────

std::optional<ScannedEntry> RealFilesystemScanner::stat_file(
    const fs::path& path) const
{
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        return std::nullopt;
    }

    auto dir_entry = fs::directory_entry(path, ec);
    if (ec) return std::nullopt;

    return entry_from_dir_entry(dir_entry, path.parent_path());
}

}  // namespace kairos::watch
