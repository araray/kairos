/// include/kairos/watch/fsevents_watcher.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/watch/fsevents_watcher.hpp — macOS FSEvents native backend       ║
// ║                                                                           ║
// ║  Uses CFRunLoop + FSEventStream with kFSEventStreamCreateFlagFileEvents  ║
// ║  for per-file event delivery (macOS 10.7+). Inherently recursive.         ║
// ║                                                                           ║
// ║  Thread model: run() blocks inside CFRunLoopRun(). Stop is achieved by   ║
// ║  calling CFRunLoopStop() via a std::stop_callback.                        ║
// ║                                                                           ║
// ║  Spec reference: §12.4                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/watch/file_watcher.hpp"

#ifdef __APPLE__

#include <CoreServices/CoreServices.h>

#include <mutex>
#include <string>
#include <vector>

namespace kairos::watch {

class FSEventsWatcher : public IFileWatcher {
public:
    FSEventsWatcher();
    ~FSEventsWatcher() override;

    // Non-copyable, non-movable (CFRunLoop reference semantics).
    FSEventsWatcher(const FSEventsWatcher&) = delete;
    FSEventsWatcher& operator=(const FSEventsWatcher&) = delete;

    /// Add a directory to watch.
    /// FSEvents is inherently recursive — the `recursive` param is noted
    /// but non-recursive filtering is done in the callback by comparing
    /// path depth.
    [[nodiscard]] bool add_watch(
        const fs::path& path,
        bool recursive = true) override;

    /// Remove a watch. Takes effect on next start/restart.
    void remove_watch(const fs::path& path) override;

    /// Start the FSEvents run loop. Blocks until stop is requested.
    /// Creates the FSEventStream from all registered paths, schedules
    /// it on a new CFRunLoop, and calls CFRunLoopRun().
    void run(std::stop_token stop, NativeEventCallback callback) override;

    /// Diagnostic info: paths watched, latency, stream flags.
    [[nodiscard]] std::string diagnostic_info() const override;

    [[nodiscard]] std::string_view platform_name() const override {
        return "fsevents";
    }

    [[nodiscard]] std::size_t watch_count() const override;

private:
    /// FSEvents callback — dispatched from the CFRunLoop thread.
    /// `context` is a pointer to this FSEventsWatcher instance.
    static void fs_events_callback(
        ConstFSEventStreamRef stream,
        void* context,
        size_t num_events,
        void* event_paths,
        const FSEventStreamEventFlags event_flags[],
        const FSEventStreamEventId event_ids[]);

    /// Translate FSEvents flags to NativeEventType.
    static NativeEventType classify_flags(FSEventStreamEventFlags flags);

    /// Paths registered via add_watch (before stream creation).
    std::vector<std::string> watched_paths_;

    /// Whether each path was added with recursive=true.
    std::vector<bool> watched_recursive_;

    /// User callback — set during run(), cleared on exit.
    NativeEventCallback callback_;

    /// CFRunLoop reference — captured inside run() for stop_callback use.
    CFRunLoopRef run_loop_ = nullptr;

    /// The event stream (created in run(), invalidated on stop).
    FSEventStreamRef stream_ = nullptr;

    /// Protects watched_paths_ during add/remove (called before run).
    mutable std::mutex paths_mu_;

    /// Latency for FSEventStream batching (seconds). Lower = more CPU.
    /// Per spec: 0.1s with kFSEventStreamCreateFlagNoDefer.
    static constexpr CFAbsoluteTime kLatency = 0.1;
};

}  // namespace kairos::watch

#endif // __APPLE__
