/// src/watch/file_watcher_stub.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  file_watcher_stub.cpp — Factory stubs for platforms without native      ║
// ║  watcher backends yet. macOS (FSEvents) and Windows (RDCW) will get     ║
// ║  implementations in future batches.                                      ║
// ║                                                                           ║
// ║  Spec reference: §12.4, §12.5 (future)                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

// Only compile this file on non-Linux platforms (Linux has the inotify impl).
#if !defined(__linux__)

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

#endif  // !__linux__
