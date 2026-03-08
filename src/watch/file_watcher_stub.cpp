/// src/watch/file_watcher_stub.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  file_watcher_stub.cpp — Factory stubs for platforms without native      ║
// ║  watcher backends yet. Windows (RDCW) gets an implementation in a       ║
// ║  future batch.                                                           ║
// ║                                                                           ║
// ║  Linux uses inotify_watcher.cpp; macOS uses fsevents_watcher.cpp.        ║
// ║  This file is compiled only on Windows and other unsupported platforms.   ║
// ║                                                                           ║
// ║  Spec reference: §12.5 (RDCW — future)                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

// Guard: only compile on platforms without a native backend.
#if !defined(__linux__) && !defined(__APPLE__)

#include "kairos/watch/file_watcher.hpp"

namespace kairos::watch {

std::unique_ptr<IFileWatcher> create_native_watcher() {
    // No native backend available yet on this platform.
    // WatchEngine will fall back to sample-only mode.
    return nullptr;
}

bool has_native_watcher() {
    return false;
}

}  // namespace kairos::watch

#endif  // !__linux__ && !__APPLE__
