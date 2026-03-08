/// include/kairos/testing/fake_fs_scanner.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/testing/fake_fs_scanner.hpp — IFilesystemScanner backed by        ║
// ║  FakeFilesystem for deterministic watch engine tests.                     ║
// ║                                                                           ║
// ║  Spec reference: §30.4                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/testing/fake_filesystem.hpp"
#include "kairos/watch/sample.hpp"

namespace kairos::testing {

/// Adapts FakeFilesystem to the IFilesystemScanner interface used by
/// WatchEngine::collect_sample(). Enables fully deterministic testing
/// without touching the real filesystem.
class FakeFilesystemScanner final : public kairos::watch::IFilesystemScanner {
public:
    explicit FakeFilesystemScanner(FakeFilesystem& fs)
        : fs_(fs) {}

    [[nodiscard]] std::vector<kairos::watch::ScannedEntry> scan(
        const std::filesystem::path& root,
        int max_depth,
        const std::vector<std::string>& exclude_globs,
        std::stop_token stop) const override
    {
        std::vector<kairos::watch::ScannedEntry> result;

        auto fake_entries = fs_.scan(root, max_depth, exclude_globs);

        for (const auto& fe : fake_entries) {
            if (stop.stop_requested()) break;

            kairos::watch::ScannedEntry entry;
            entry.path = fe.path.string();
            entry.entry_type = fe.is_directory ? "directory"
                             : fe.is_symlink   ? "symlink"
                                               : "file";
            entry.size = static_cast<int64_t>(fe.size);
            entry.mtime = fe.mtime;
            entry.permissions = permission_to_string(fe.permissions);
            entry.is_directory = fe.is_directory;
            entry.is_symlink = fe.is_symlink;

            result.push_back(std::move(entry));
        }

        return result;
    }

    [[nodiscard]] std::optional<kairos::watch::ScannedEntry> stat_file(
        const std::filesystem::path& path) const override
    {
        auto fe = fs_.stat(path);
        if (!fe.has_value()) return std::nullopt;

        kairos::watch::ScannedEntry entry;
        entry.path = fe->path.string();
        entry.entry_type = fe->is_directory ? "directory"
                         : fe->is_symlink   ? "symlink"
                                            : "file";
        entry.size = static_cast<int64_t>(fe->size);
        entry.mtime = fe->mtime;
        entry.permissions = permission_to_string(fe->permissions);
        entry.is_directory = fe->is_directory;
        entry.is_symlink = fe->is_symlink;

        return entry;
    }

private:
    static std::string permission_to_string(std::filesystem::perms p) {
        auto val = static_cast<unsigned>(p);
        // Format as octal string.
        char buf[8];
        std::snprintf(buf, sizeof(buf), "0%o", val & 0777);
        return buf;
    }

    FakeFilesystem& fs_;
};

}  // namespace kairos::testing
