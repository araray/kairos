/// include/kairos/watch/file_watcher.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/watch/file_watcher.hpp — Platform abstraction for native FS      ║
// ║  watchers (inotify, FSEvents, ReadDirectoryChangesW).                    ║
// ║                                                                           ║
// ║  Each platform implements IFileWatcher; the factory function              ║
// ║  create_native_watcher() selects the correct backend at compile time.    ║
// ║                                                                           ║
// ║  Spec reference: §12.2                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace kairos::watch {

namespace fs = std::filesystem;

// ── Native event types ──────────────────────────────────────────────────

/// Types of raw filesystem events from the OS.
enum class NativeEventType {
    Created,     ///< File or directory created.
    Modified,    ///< File content or metadata changed.
    Deleted,     ///< File or directory deleted.
    Renamed,     ///< File moved/renamed (old_path → path).
    Overflow,    ///< Platform queue overflow (inotify, RDCW buffer full).
    Error        ///< Watch error (e.g., directory deleted while watched).
};

/// Convert NativeEventType to human-readable string.
[[nodiscard]] constexpr std::string_view native_event_type_str(
    NativeEventType t) noexcept
{
    switch (t) {
        case NativeEventType::Created:  return "created";
        case NativeEventType::Modified: return "modified";
        case NativeEventType::Deleted:  return "deleted";
        case NativeEventType::Renamed:  return "renamed";
        case NativeEventType::Overflow: return "overflow";
        case NativeEventType::Error:    return "error";
    }
    return "unknown";
}

// ── Native event struct ─────────────────────────────────────────────────

/// A raw filesystem event from the OS.
struct NativeEvent {
    NativeEventType type;
    fs::path path;
    fs::path old_path;                ///< For Renamed events only.
    bool is_directory = false;
    std::chrono::system_clock::time_point timestamp;
};

/// Callback signature for native filesystem events.
/// Called from the watcher's run thread — must be thread-safe.
using NativeEventCallback = std::function<void(const NativeEvent&)>;

// ── IFileWatcher interface ──────────────────────────────────────────────

/// Abstract interface for platform-specific filesystem watchers.
///
/// Lifecycle:
///   1. Construct via create_native_watcher()
///   2. add_watch() for each directory
///   3. run() blocks until stop is requested (runs on a dedicated thread)
///   4. Destructor removes all watches
///
/// Thread safety:
///   - add_watch() / remove_watch() must be called before run(), or from
///     the same thread as run().
///   - callback is invoked from the run() thread.
class IFileWatcher {
public:
    virtual ~IFileWatcher() = default;

    /// Add a directory to watch.
    /// @param path       Absolute directory path.
    /// @param recursive  Whether to watch subdirectories.
    /// @return true on success, false on error (logged internally).
    [[nodiscard]] virtual bool add_watch(
        const fs::path& path,
        bool recursive = true
    ) = 0;

    /// Remove a previously added watch.
    virtual void remove_watch(const fs::path& path) = 0;

    /// Start the event loop. Blocks until stop is requested.
    /// Events are delivered via the callback from this thread.
    virtual void run(std::stop_token stop, NativeEventCallback callback) = 0;

    /// Get platform-specific diagnostic info (for troubleshooting).
    [[nodiscard]] virtual std::string diagnostic_info() const = 0;

    /// Platform backend name for logging.
    [[nodiscard]] virtual std::string_view platform_name() const = 0;

    /// Number of currently active watches.
    [[nodiscard]] virtual std::size_t watch_count() const = 0;
};

// ── Factory ─────────────────────────────────────────────────────────────

/// Create the appropriate IFileWatcher for the current platform.
/// Returns nullptr if no native backend is available (e.g., unsupported OS).
[[nodiscard]] std::unique_ptr<IFileWatcher> create_native_watcher();

/// Check if a native watcher backend is available on this platform.
[[nodiscard]] bool has_native_watcher();

}  // namespace kairos::watch
