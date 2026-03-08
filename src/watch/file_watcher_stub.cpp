/// src/watch/file_watcher_stub.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  file_watcher_stub.cpp — Factory stubs for unsupported platforms        ║
// ║                                                                          ║
// ║  Linux uses inotify_watcher.cpp; macOS uses fsevents_watcher.cpp;       ║
// ║  Windows uses rdcw_watcher.cpp. This file is compiled only on           ║
// ║  platforms without a native backend.                                    ║
// ║                                                                          ║
// ║  Spec reference: §12.2 (factory)                                        ║
// ╚════════════════════════════════════════════════════════════════════════════╝

// Guard: only compile on platforms without a native backend.
#if !defined(__linux__) && !defined(__APPLE__) && !defined(_WIN32)

#include "kairos/watch/file_watcher.hpp"

namespace kairos::watch {

std::unique_ptr<IFileWatcher> create_native_watcher() {
    // No native backend available on this platform.
    // WatchEngine will fall back to sample-only mode.
    return nullptr;
}

bool has_native_watcher() {
    return false;
}

}  // namespace kairos::watch

#endif  // !__linux__ && !__APPLE__ && !_WIN32
