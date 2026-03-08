/// src/watch/inotify_watcher.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  inotify_watcher.cpp — Linux inotify backend implementation              ║
// ║                                                                           ║
// ║  Event loop: poll(fds=[inotify_fd, stop_pipe_read], timeout=1000ms)      ║
// ║  ├── inotify_fd ready: read() events → parse → dispatch callback         ║
// ║  ├── stop_pipe ready: stop requested → exit loop                         ║
// ║  └── timeout: check stop_token, continue                                 ║
// ║                                                                           ║
// ║  Spec reference: §12.3                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef __linux__

#include "kairos/watch/inotify_watcher.hpp"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace kairos::watch {

// ── inotify event mask for watched directories ──────────────────────────

/// IN_CLOSE_WRITE is preferred over IN_MODIFY because it fires once per
/// write session (close-after-write), not per every write() call.
/// IN_CREATE + IN_ISDIR triggers recursive watch addition.
/// IN_DELETE_SELF handles the watched directory itself being removed.
static constexpr uint32_t kWatchMask =
    IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO |
    IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF;

// Aligned buffer size for reading inotify events.
// Each event is sizeof(inotify_event) + NAME_MAX + 1 ≈ 280 bytes.
// 8 KB allows ~28 events per read, sufficient for typical bursts.
static constexpr size_t kEventBufSize = 8192;

// ── Constructor / Destructor ────────────────────────────────────────────

InotifyWatcher::InotifyWatcher() {
    // Create inotify instance.
    inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd_ < 0) {
        spdlog::error("inotify_init1 failed: {} ({})",
                      std::strerror(errno), errno);
        throw std::runtime_error("Failed to create inotify instance");
    }

    // Create self-pipe for waking poll(). Must be non-blocking so the
    // drain loop in run() terminates after consuming all pending bytes.
    if (pipe2(stop_pipe_, O_NONBLOCK | O_CLOEXEC) < 0) {
        close(inotify_fd_);
        inotify_fd_ = -1;
        spdlog::error("pipe() failed: {} ({})",
                      std::strerror(errno), errno);
        throw std::runtime_error("Failed to create stop pipe");
    }

    spdlog::debug("InotifyWatcher created: fd={}", inotify_fd_);
}

InotifyWatcher::~InotifyWatcher() {
    // Remove all watches.
    for (const auto& [wd, _] : wd_to_path_) {
        inotify_rm_watch(inotify_fd_, wd);
    }

    if (inotify_fd_ >= 0) close(inotify_fd_);
    if (stop_pipe_[0] >= 0) close(stop_pipe_[0]);
    if (stop_pipe_[1] >= 0) close(stop_pipe_[1]);

    spdlog::debug("InotifyWatcher destroyed");
}

// ── add_watch / remove_watch ────────────────────────────────────────────

bool InotifyWatcher::add_watch(const fs::path& path, bool recursive) {
    std::error_code ec;
    auto canonical = fs::canonical(path, ec);
    if (ec) {
        spdlog::warn("add_watch: cannot canonicalize '{}': {}",
                     path.string(), ec.message());
        return false;
    }

    if (!fs::is_directory(canonical, ec) || ec) {
        spdlog::warn("add_watch: '{}' is not a directory", canonical.string());
        return false;
    }

    int wd = add_single_watch(canonical);
    if (wd < 0) return false;

    if (recursive) {
        add_recursive(canonical);
    }

    spdlog::info("Watching '{}' (recursive={}), total watches: {}",
                 canonical.string(), recursive, wd_to_path_.size());
    return true;
}

void InotifyWatcher::remove_watch(const fs::path& path) {
    std::error_code ec;
    auto canonical = fs::canonical(path, ec);
    std::string key = ec ? path.string() : canonical.string();

    auto it = path_to_wd_.find(key);
    if (it == path_to_wd_.end()) return;

    int wd = it->second;
    inotify_rm_watch(inotify_fd_, wd);
    wd_to_path_.erase(wd);
    path_to_wd_.erase(it);

    spdlog::debug("Removed watch for '{}'", key);
}

// ── run() — main event loop ─────────────────────────────────────────────

void InotifyWatcher::run(std::stop_token stop, NativeEventCallback callback) {
    spdlog::info("InotifyWatcher run loop starting, {} watches active",
                 wd_to_path_.size());

    // Register stop callback to wake poll() when stop is requested.
    std::stop_callback stop_cb(stop, [this] { wake_poll(); });

    // Aligned buffer for inotify events.
    alignas(struct inotify_event) char buf[kEventBufSize];

    struct pollfd fds[2];
    fds[0].fd = inotify_fd_;
    fds[0].events = POLLIN;
    fds[1].fd = stop_pipe_[0];
    fds[1].events = POLLIN;

    while (!stop.stop_requested()) {
        int ret = poll(fds, 2, 1000);  // 1s timeout for stop check.

        if (ret < 0) {
            if (errno == EINTR) continue;
            spdlog::error("poll() failed: {} ({})",
                          std::strerror(errno), errno);
            break;
        }

        if (ret == 0) continue;  // Timeout.

        // Check stop pipe first.
        if (fds[1].revents & POLLIN) {
            // Drain the pipe.
            char drain[64];
            while (read(stop_pipe_[0], drain, sizeof(drain)) > 0) {}
            break;
        }

        // Read inotify events.
        if (fds[0].revents & POLLIN) {
            ssize_t len = read(inotify_fd_, buf, sizeof(buf));
            if (len < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                spdlog::error("read(inotify_fd) failed: {} ({})",
                              std::strerror(errno), errno);
                break;
            }

            // Parse events from the buffer.
            const char* ptr = buf;
            while (ptr < buf + len) {
                const auto* event =
                    reinterpret_cast<const struct inotify_event*>(ptr);

                handle_event(event, callback);

                ptr += sizeof(struct inotify_event) + event->len;
            }
        }
    }

    spdlog::info("InotifyWatcher run loop exiting");
}

// ── diagnostic_info / watch_count ───────────────────────────────────────

std::string InotifyWatcher::diagnostic_info() const {
    std::string info = "inotify backend\n";
    info += "  fd: " + std::to_string(inotify_fd_) + "\n";
    info += "  watches: " + std::to_string(wd_to_path_.size()) + "\n";

    // Try to read the system limit.
    FILE* f = fopen("/proc/sys/fs/inotify/max_user_watches", "r");
    if (f) {
        int limit = 0;
        if (fscanf(f, "%d", &limit) == 1) {
            info += "  max_user_watches: " + std::to_string(limit) + "\n";
        }
        fclose(f);
    }

    return info;
}

std::size_t InotifyWatcher::watch_count() const {
    return wd_to_path_.size();
}

// ── Internal helpers ────────────────────────────────────────────────────

int InotifyWatcher::add_single_watch(const fs::path& dir) {
    std::string path_str = dir.string();

    // Skip if already watched.
    if (path_to_wd_.count(path_str)) {
        return path_to_wd_[path_str];
    }

    int wd = inotify_add_watch(inotify_fd_, path_str.c_str(), kWatchMask);
    if (wd < 0) {
        if (errno == ENOSPC) {
            spdlog::error(
                "inotify watch limit reached for '{}'. "
                "Increase with: echo 65536 | sudo tee "
                "/proc/sys/fs/inotify/max_user_watches",
                path_str);
        } else {
            spdlog::warn("inotify_add_watch('{}') failed: {} ({})",
                         path_str, std::strerror(errno), errno);
        }
        return -1;
    }

    wd_to_path_[wd] = path_str;
    path_to_wd_[path_str] = wd;
    return wd;
}

void InotifyWatcher::add_recursive(const fs::path& dir) {
    std::error_code ec;
    for (auto& entry : fs::recursive_directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec))
    {
        if (ec) break;
        if (entry.is_directory(ec) && !ec) {
            add_single_watch(entry.path());
        }
    }
}

void InotifyWatcher::handle_event(
    const struct inotify_event* event,
    NativeEventCallback& callback)
{
    auto now = std::chrono::system_clock::now();

    // ── Queue overflow ──────────────────────────────────────────────
    if (event->mask & IN_Q_OVERFLOW) {
        spdlog::warn("inotify queue overflow detected — "
                     "full re-scan recommended");
        NativeEvent ev;
        ev.type = NativeEventType::Overflow;
        ev.timestamp = now;
        callback(ev);
        return;
    }

    // ── Watch removed (directory deleted/moved) ─────────────────────
    if (event->mask & IN_IGNORED) {
        // inotify automatically removes the watch; clean our maps.
        auto it = wd_to_path_.find(event->wd);
        if (it != wd_to_path_.end()) {
            path_to_wd_.erase(it->second);
            wd_to_path_.erase(it);
        }
        return;
    }

    // ── Resolve event path ──────────────────────────────────────────
    auto wd_it = wd_to_path_.find(event->wd);
    if (wd_it == wd_to_path_.end()) return;  // Stale wd.

    fs::path event_path = wd_it->second;
    if (event->len > 0) {
        event_path /= event->name;
    }

    bool is_dir = (event->mask & IN_ISDIR) != 0;

    // ── Classify the event ──────────────────────────────────────────

    NativeEvent ev;
    ev.path = event_path;
    ev.is_directory = is_dir;
    ev.timestamp = now;

    if (event->mask & IN_CREATE) {
        ev.type = NativeEventType::Created;

        // Auto-add recursive watch for new subdirectories.
        if (is_dir) {
            add_single_watch(event_path);
        }

    } else if (event->mask & IN_DELETE) {
        ev.type = NativeEventType::Deleted;

    } else if (event->mask & (IN_CLOSE_WRITE | IN_ATTRIB)) {
        ev.type = NativeEventType::Modified;

    } else if (event->mask & IN_MOVED_FROM) {
        ev.type = NativeEventType::Renamed;
        ev.old_path = event_path;
        // Note: IN_MOVED_TO follows with the new path. We emit
        // Renamed for the FROM event. The TO event becomes a Created.

    } else if (event->mask & IN_MOVED_TO) {
        ev.type = NativeEventType::Created;
        // For files moved INTO the watched tree, treat as creation.
        // Auto-add watch for moved-in directories.
        if (is_dir) {
            add_single_watch(event_path);
        }

    } else if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
        ev.type = NativeEventType::Error;
        spdlog::warn("Watched directory '{}' was deleted or moved",
                     wd_it->second);
    } else {
        return;  // Unknown mask bits — skip.
    }

    callback(ev);
}

void InotifyWatcher::wake_poll() {
    char c = 1;
    // Best-effort; if the pipe is full, poll() will timeout instead.
    [[maybe_unused]] auto ret = write(stop_pipe_[1], &c, 1);
}

// ── Factory implementation (Linux) ──────────────────────────────────────

std::unique_ptr<IFileWatcher> create_native_watcher() {
    return std::make_unique<InotifyWatcher>();
}

bool has_native_watcher() {
    return true;
}

}  // namespace kairos::watch

#endif  // __linux__
