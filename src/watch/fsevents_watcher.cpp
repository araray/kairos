/// src/watch/fsevents_watcher.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  fsevents_watcher.cpp — macOS FSEvents native backend                    ║
// ║                                                                           ║
// ║  Implementation notes:                                                    ║
// ║    - kFSEventStreamCreateFlagFileEvents for per-file events (10.7+)      ║
// ║    - kFSEventStreamCreateFlagNoDefer for immediate first event           ║
// ║    - 0.1s latency for near-real-time delivery                             ║
// ║    - CFRunLoopStop() for cooperative shutdown via stop_callback           ║
// ║    - kFSEventStreamEventFlagMustScanSubDirs → Overflow event             ║
// ║                                                                           ║
// ║  Spec reference: §12.4                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef __APPLE__

#include "kairos/watch/fsevents_watcher.hpp"

#include <cstring>
#include <sstream>

namespace kairos::watch {

// ── Constructor / Destructor ────────────────────────────────────────────

FSEventsWatcher::FSEventsWatcher() = default;

FSEventsWatcher::~FSEventsWatcher() {
    // If the stream is still alive (shouldn't happen if run() exited
    // cleanly), invalidate it.
    if (stream_) {
        FSEventStreamStop(stream_);
        FSEventStreamInvalidate(stream_);
        FSEventStreamRelease(stream_);
        stream_ = nullptr;
    }
}

// ── add_watch / remove_watch ────────────────────────────────────────────

bool FSEventsWatcher::add_watch(const fs::path& path, bool recursive) {
    std::lock_guard lock(paths_mu_);

    // FSEvents watches must be directories.
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        return false;
    }

    // Canonicalize to avoid duplicates.
    auto canonical = fs::canonical(path, ec);
    if (ec) {
        return false;
    }

    auto canonical_str = canonical.string();

    // Deduplicate.
    for (const auto& existing : watched_paths_) {
        if (existing == canonical_str) {
            return true;  // Already watched.
        }
    }

    watched_paths_.push_back(canonical_str);
    watched_recursive_.push_back(recursive);
    return true;
}

void FSEventsWatcher::remove_watch(const fs::path& path) {
    std::lock_guard lock(paths_mu_);

    std::error_code ec;
    auto canonical = fs::canonical(path, ec);
    auto target = ec ? path.string() : canonical.string();

    for (size_t i = 0; i < watched_paths_.size(); ++i) {
        if (watched_paths_[i] == target) {
            watched_paths_.erase(watched_paths_.begin()
                + static_cast<ptrdiff_t>(i));
            watched_recursive_.erase(watched_recursive_.begin()
                + static_cast<ptrdiff_t>(i));
            return;
        }
    }
}

// ── run ─────────────────────────────────────────────────────────────────

void FSEventsWatcher::run(std::stop_token stop, NativeEventCallback callback) {
    callback_ = std::move(callback);

    // Build CFArray of paths.
    std::vector<CFStringRef> cf_paths;
    {
        std::lock_guard lock(paths_mu_);
        if (watched_paths_.empty()) {
            return;  // Nothing to watch.
        }

        for (const auto& p : watched_paths_) {
            cf_paths.push_back(CFStringCreateWithCString(
                kCFAllocatorDefault, p.c_str(), kCFStringEncodingUTF8));
        }
    }

    CFArrayRef paths_to_watch = CFArrayCreate(
        kCFAllocatorDefault,
        reinterpret_cast<const void**>(cf_paths.data()),
        static_cast<CFIndex>(cf_paths.size()),
        &kCFTypeArrayCallBacks);

    // Context: pass `this` as the info pointer.
    FSEventStreamContext context{};
    context.info = this;

    // Create the stream.
    // Flags:
    //   kFSEventStreamCreateFlagFileEvents — per-file events (not dir-level)
    //   kFSEventStreamCreateFlagNoDefer    — deliver first event immediately
    //   kFSEventStreamCreateFlagUseCFTypes — event_paths as CFArrayRef
    FSEventStreamCreateFlags flags =
        kFSEventStreamCreateFlagFileEvents |
        kFSEventStreamCreateFlagNoDefer |
        kFSEventStreamCreateFlagUseCFTypes;

    stream_ = FSEventStreamCreate(
        kCFAllocatorDefault,
        &FSEventsWatcher::fs_events_callback,
        &context,
        paths_to_watch,
        kFSEventStreamEventIdSinceNow,
        kLatency,
        flags);

    // Release path CFStrings — the stream has retained them.
    CFRelease(paths_to_watch);
    for (auto cf : cf_paths) {
        CFRelease(cf);
    }

    if (!stream_) {
        callback_ = nullptr;
        return;
    }

    // Capture the run loop reference for the stop callback.
    run_loop_ = CFRunLoopGetCurrent();

    // Schedule the stream on this thread's run loop.
    FSEventStreamScheduleWithRunLoop(
        stream_, run_loop_, kCFRunLoopDefaultMode);
    FSEventStreamStart(stream_);

    // Install stop callback: when stop is requested, wake the run loop.
    std::stop_callback stop_cb(stop, [this]() {
        if (run_loop_) {
            CFRunLoopStop(run_loop_);
        }
    });

    // Block here until CFRunLoopStop() is called.
    if (!stop.stop_requested()) {
        CFRunLoopRun();
    }

    // Cleanup.
    FSEventStreamStop(stream_);
    FSEventStreamInvalidate(stream_);
    FSEventStreamRelease(stream_);
    stream_ = nullptr;
    run_loop_ = nullptr;
    callback_ = nullptr;
}

// ── FSEvents callback ───────────────────────────────────────────────────

void FSEventsWatcher::fs_events_callback(
    ConstFSEventStreamRef /*stream*/,
    void* context,
    size_t num_events,
    void* event_paths,
    const FSEventStreamEventFlags event_flags[],
    const FSEventStreamEventId /*event_ids*/[])
{
    auto* self = static_cast<FSEventsWatcher*>(context);
    if (!self->callback_) return;

    auto paths_array = static_cast<CFArrayRef>(event_paths);
    auto now = std::chrono::system_clock::now();

    for (size_t i = 0; i < num_events; ++i) {
        FSEventStreamEventFlags flags = event_flags[i];

        // Extract path string from CFArray.
        auto cf_path = static_cast<CFStringRef>(
            CFArrayGetValueAtIndex(paths_array, static_cast<CFIndex>(i)));

        // Convert CFString to std::string.
        const char* c_str = CFStringGetCStringPtr(
            cf_path, kCFStringEncodingUTF8);

        std::string path_str;
        if (c_str) {
            path_str = c_str;
        } else {
            // Fallback: CFString didn't give us a direct pointer.
            CFIndex len = CFStringGetLength(cf_path);
            CFIndex max_size = CFStringGetMaximumSizeForEncoding(
                len, kCFStringEncodingUTF8) + 1;
            path_str.resize(static_cast<size_t>(max_size));
            if (CFStringGetCString(cf_path, path_str.data(),
                                    max_size, kCFStringEncodingUTF8)) {
                path_str.resize(std::strlen(path_str.c_str()));
            } else {
                continue;  // Cannot convert — skip this event.
            }
        }

        // Check for overflow / must-scan-subdirs.
        if (flags & kFSEventStreamEventFlagMustScanSubDirs) {
            NativeEvent evt;
            evt.type = NativeEventType::Overflow;
            evt.path = fs::path(path_str);
            evt.timestamp = now;
            self->callback_(evt);
            continue;
        }

        // Skip root-change and history-done meta-events.
        if (flags & kFSEventStreamEventFlagRootChanged) continue;
        if (flags & kFSEventStreamEventFlagHistoryDone) continue;

        // Determine if this is a directory event.
        bool is_dir = (flags & kFSEventStreamEventFlagItemIsDir) != 0;

        // Classify the event type.
        NativeEventType event_type = classify_flags(flags);

        // Non-recursive filtering: if a watched path was added with
        // recursive=false, skip events that are deeper than the
        // immediate children of the watched root.
        // (FSEvents is inherently recursive; we filter here.)
        {
            std::lock_guard lock(self->paths_mu_);
            bool skip = false;
            for (size_t w = 0; w < self->watched_paths_.size(); ++w) {
                const auto& root = self->watched_paths_[w];
                if (path_str.starts_with(root) && !self->watched_recursive_[w]) {
                    // Count path separators after the root.
                    size_t extra = 0;
                    for (size_t c = root.size(); c < path_str.size(); ++c) {
                        if (path_str[c] == '/') ++extra;
                    }
                    // Immediate child has at most 1 separator.
                    if (extra > 1) {
                        skip = true;
                        break;
                    }
                }
            }
            if (skip) continue;
        }

        NativeEvent evt;
        evt.type = event_type;
        evt.path = fs::path(path_str);
        evt.is_directory = is_dir;
        evt.timestamp = now;

        // FSEvents doesn't provide old_path for renames.
        // Renamed events will have empty old_path — the watch engine
        // handles this by treating it as delete+create if needed.

        self->callback_(evt);
    }
}

// ── classify_flags ──────────────────────────────────────────────────────

NativeEventType FSEventsWatcher::classify_flags(
    FSEventStreamEventFlags flags)
{
    // Priority order: Created > Deleted > Renamed > Modified.
    // These flags can be combined; we pick the most significant.
    if (flags & kFSEventStreamEventFlagItemCreated) {
        return NativeEventType::Created;
    }
    if (flags & kFSEventStreamEventFlagItemRemoved) {
        return NativeEventType::Deleted;
    }
    if (flags & kFSEventStreamEventFlagItemRenamed) {
        return NativeEventType::Renamed;
    }
    if (flags & (kFSEventStreamEventFlagItemModified |
                 kFSEventStreamEventFlagItemInodeMetaMod |
                 kFSEventStreamEventFlagItemFinderInfoMod |
                 kFSEventStreamEventFlagItemChangeOwner |
                 kFSEventStreamEventFlagItemXattrMod)) {
        return NativeEventType::Modified;
    }

    // Fallback: treat as Modified (some event happened).
    return NativeEventType::Modified;
}

// ── diagnostic_info ─────────────────────────────────────────────────────

std::string FSEventsWatcher::diagnostic_info() const {
    std::lock_guard lock(paths_mu_);
    std::ostringstream oss;
    oss << "FSEventsWatcher: " << watched_paths_.size() << " path(s)";
    oss << ", latency=" << kLatency << "s";
    oss << ", flags=FileEvents|NoDefer|UseCFTypes";
    for (size_t i = 0; i < watched_paths_.size(); ++i) {
        oss << "\n  [" << i << "] " << watched_paths_[i]
            << (watched_recursive_[i] ? " (recursive)" : " (shallow)");
    }
    return oss.str();
}

// ── watch_count ─────────────────────────────────────────────────────────

std::size_t FSEventsWatcher::watch_count() const {
    std::lock_guard lock(paths_mu_);
    return watched_paths_.size();
}

// ── Factory functions (macOS) ────────────────────────────────────────────

std::unique_ptr<IFileWatcher> create_native_watcher() {
    return std::make_unique<FSEventsWatcher>();
}

bool has_native_watcher() {
    return true;
}

}  // namespace kairos::watch

#endif // __APPLE__
