/// include/kairos/watch/rdcw_watcher.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/watch/rdcw_watcher.hpp — Windows ReadDirectoryChangesW backend   ║
// ║                                                                          ║
// ║  Uses I/O Completion Ports (IOCP) for efficient multiplexing of         ║
// ║  multiple directory watches. Each watched directory has an outstanding   ║
// ║  overlapped ReadDirectoryChangesW call.                                 ║
// ║                                                                          ║
// ║  Stop mechanism: manual-reset event signaled from stop_callback;        ║
// ║  GQCS loop checks both IOCP and stop event.                            ║
// ║                                                                          ║
// ║  Spec reference: §12.5                                                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/watch/file_watcher.hpp"

#ifdef _WIN32

#include <windows.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace kairos::watch {

/// Windows file watcher using ReadDirectoryChangesW + IOCP.
///
/// Each watched directory is opened with CreateFileW (no-sharing conflict),
/// associated with an IOCP, and has a pending overlapped RDC call. The
/// IOCP delivers completion packets when changes are detected.
///
/// Thread model:
///   - run() blocks on GetQueuedCompletionStatus in a loop.
///   - stop_callback posts a custom completion key to wake the loop.
///   - add_watch/remove_watch must be called before run().
///
/// Buffer overflow:
///   If ReadDirectoryChangesW returns ERROR_NOTIFY_ENUM_DIR (buffer too
///   small), an Overflow event is emitted. The caller should trigger a
///   full re-scan.
///
/// Unicode:
///   Notification buffers contain FILE_NOTIFY_INFORMATION with UTF-16
///   filenames. We convert to std::filesystem::path (wchar_t on Windows)
///   and normalize to UTF-8 for internal storage.
class RDCWWatcher : public IFileWatcher {
public:
    RDCWWatcher();
    ~RDCWWatcher() override;

    // Non-copyable, non-movable (owns HANDLEs).
    RDCWWatcher(const RDCWWatcher&) = delete;
    RDCWWatcher& operator=(const RDCWWatcher&) = delete;
    RDCWWatcher(RDCWWatcher&&) = delete;
    RDCWWatcher& operator=(RDCWWatcher&&) = delete;

    bool add_watch(const fs::path& path, bool recursive) override;
    void remove_watch(const fs::path& path) override;
    void run(std::stop_token stop, NativeEventCallback callback) override;
    [[nodiscard]] std::string diagnostic_info() const override;
    [[nodiscard]] std::string_view platform_name() const override {
        return "ReadDirectoryChangesW";
    }
    [[nodiscard]] std::size_t watch_count() const override;

private:
    /// Per-directory watch state.
    struct WatchEntry {
        HANDLE dir_handle = INVALID_HANDLE_VALUE;
        OVERLAPPED overlapped{};
        std::vector<BYTE> buffer;       ///< Notification buffer (64 KB).
        fs::path path;                  ///< Canonical directory path.
        bool recursive = true;          ///< Watch subtree flag.
    };

    /// Sentinel completion key for stop signaling.
    static constexpr ULONG_PTR kStopKey = 0xDEADBEEF;

    /// Default notification buffer size per watch.
    static constexpr DWORD kBufferSize = 65536;  // 64 KB per spec.

    /// Notification filter mask — all file changes we care about.
    static constexpr DWORD kNotifyFilter =
        FILE_NOTIFY_CHANGE_FILE_NAME |
        FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_SIZE |
        FILE_NOTIFY_CHANGE_LAST_WRITE |
        FILE_NOTIFY_CHANGE_ATTRIBUTES |
        FILE_NOTIFY_CHANGE_SECURITY;

    /// Issue an overlapped ReadDirectoryChangesW for a watch entry.
    /// Returns true on success, false on failure (entry should be removed).
    bool issue_read(WatchEntry& entry);

    /// Parse FILE_NOTIFY_INFORMATION from a completed buffer and invoke
    /// the callback for each event.
    void process_notifications(WatchEntry& entry,
                               DWORD bytes_transferred,
                               NativeEventCallback& callback);

    /// Map a FILE_ACTION_* constant to our NativeEventType.
    static NativeEventType map_action(DWORD action);

    /// Convert a UTF-16 wstring to UTF-8 string.
    static std::string utf16_to_utf8(const std::wstring& wstr);

    /// Check if a path is on a network drive (DRIVE_REMOTE).
    static bool is_network_path(const fs::path& path);

    HANDLE iocp_ = INVALID_HANDLE_VALUE;  ///< I/O Completion Port.

    /// All active watches, keyed by canonical path string.
    std::unordered_map<std::string, std::unique_ptr<WatchEntry>> watches_;
};

}  // namespace kairos::watch

#endif  // _WIN32
