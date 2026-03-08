/// include/kairos/watch/inotify_watcher.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/watch/inotify_watcher.hpp — Linux inotify backend                ║
// ║                                                                           ║
// ║  Uses inotify + poll() with a self-pipe for cooperative shutdown.        ║
// ║  Handles recursive watches, IN_Q_OVERFLOW, and wd→path mapping.          ║
// ║                                                                           ║
// ║  Spec reference: §12.3                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/watch/file_watcher.hpp"

#ifdef __linux__

#include <string>
#include <unordered_map>

namespace kairos::watch {

class InotifyWatcher : public IFileWatcher {
public:
    InotifyWatcher();
    ~InotifyWatcher() override;

    // Non-copyable, non-movable (owns file descriptors).
    InotifyWatcher(const InotifyWatcher&) = delete;
    InotifyWatcher& operator=(const InotifyWatcher&) = delete;
    InotifyWatcher(InotifyWatcher&&) = delete;
    InotifyWatcher& operator=(InotifyWatcher&&) = delete;

    bool add_watch(const fs::path& path, bool recursive) override;
    void remove_watch(const fs::path& path) override;
    void run(std::stop_token stop, NativeEventCallback callback) override;
    [[nodiscard]] std::string diagnostic_info() const override;
    [[nodiscard]] std::string_view platform_name() const override {
        return "inotify";
    }
    [[nodiscard]] std::size_t watch_count() const override;

private:
    /// Add an inotify watch for a single directory.
    /// Returns the watch descriptor, or -1 on failure.
    int add_single_watch(const fs::path& dir);

    /// Recursively add watches for all subdirectories of dir.
    void add_recursive(const fs::path& dir);

    /// Handle a single inotify event from the buffer.
    void handle_event(const struct inotify_event* event,
                      NativeEventCallback& callback);

    /// Signal the poll() to wake up (for stop or new watches).
    void wake_poll();

    int inotify_fd_ = -1;          ///< inotify instance file descriptor.
    int stop_pipe_[2] = {-1, -1};  ///< Self-pipe for waking poll().

    /// wd → directory path mapping (inotify returns WDs, not paths).
    std::unordered_map<int, std::string> wd_to_path_;

    /// Canonical path → wd mapping (for remove_watch).
    std::unordered_map<std::string, int> path_to_wd_;
};

}  // namespace kairos::watch

#endif  // __linux__
