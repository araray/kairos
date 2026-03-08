/// src/watch/rdcw_watcher.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  rdcw_watcher.cpp — Windows ReadDirectoryChangesW backend               ║
// ║                                                                          ║
// ║  Event loop:                                                             ║
// ║    GetQueuedCompletionStatus(iocp, timeout=1s)                          ║
// ║    ├── Completion packet for a watch: parse FILE_NOTIFY_INFORMATION,    ║
// ║    │   dispatch callback, re-issue ReadDirectoryChangesW               ║
// ║    ├── Stop key posted: exit loop                                       ║
// ║    └── Timeout: check stop_token, continue                              ║
// ║                                                                          ║
// ║  Spec reference: §12.5                                                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef _WIN32

#include "kairos/watch/rdcw_watcher.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace kairos::watch {

// ── Constructor / Destructor ────────────────────────────────────────────

RDCWWatcher::RDCWWatcher() {
    // Create an I/O Completion Port (no initial file association).
    iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (iocp_ == INVALID_HANDLE_VALUE || iocp_ == nullptr) {
        DWORD err = GetLastError();
        spdlog::error("CreateIoCompletionPort failed: error {}", err);
        throw std::runtime_error(
            "Failed to create I/O Completion Port (error " +
            std::to_string(err) + ")");
    }

    spdlog::debug("RDCWWatcher created: IOCP handle={}", (void*)iocp_);
}

RDCWWatcher::~RDCWWatcher() {
    // Close all directory handles.
    for (auto& [key, entry] : watches_) {
        if (entry->dir_handle != INVALID_HANDLE_VALUE) {
            // Cancel any pending I/O on this handle.
            CancelIo(entry->dir_handle);
            CloseHandle(entry->dir_handle);
        }
    }
    watches_.clear();

    if (iocp_ != INVALID_HANDLE_VALUE && iocp_ != nullptr) {
        CloseHandle(iocp_);
    }

    spdlog::debug("RDCWWatcher destroyed");
}

// ── add_watch ───────────────────────────────────────────────────────────

bool RDCWWatcher::add_watch(const fs::path& path, bool recursive) {
    std::error_code ec;
    auto canonical = fs::canonical(path, ec);
    if (ec) {
        spdlog::warn("add_watch: cannot canonicalize '{}': {}",
                     path.string(), ec.message());
        return false;
    }

    if (!fs::is_directory(canonical, ec) || ec) {
        spdlog::warn("add_watch: '{}' is not a directory",
                     canonical.string());
        return false;
    }

    std::string key = canonical.string();

    // Skip if already watched.
    if (watches_.count(key)) {
        spdlog::debug("add_watch: '{}' already watched", key);
        return true;
    }

    // Warn about network paths.
    if (is_network_path(canonical)) {
        spdlog::warn(
            "Watching network path '{}' — ReadDirectoryChangesW may be "
            "unreliable on network drives. Consider using sample-only mode.",
            key);
    }

    // Open directory with required flags for overlapped RDC.
    // FILE_FLAG_BACKUP_SEMANTICS is required to open directories.
    // FILE_FLAG_OVERLAPPED enables asynchronous I/O.
    HANDLE dir_handle = CreateFileW(
        canonical.wstring().c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (dir_handle == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        spdlog::error("CreateFileW failed for '{}': error {}", key, err);
        return false;
    }

    // Associate with IOCP. The completion key is the pointer to the
    // WatchEntry (cast to ULONG_PTR) so we can identify which watch
    // completed in the GQCS loop.
    auto entry = std::make_unique<WatchEntry>();
    entry->dir_handle = dir_handle;
    entry->path = canonical;
    entry->recursive = recursive;
    entry->buffer.resize(kBufferSize);
    std::memset(&entry->overlapped, 0, sizeof(OVERLAPPED));

    ULONG_PTR completion_key =
        reinterpret_cast<ULONG_PTR>(entry.get());

    HANDLE result = CreateIoCompletionPort(
        dir_handle, iocp_, completion_key, 0);
    if (result == nullptr) {
        DWORD err = GetLastError();
        spdlog::error("IOCP association failed for '{}': error {}", key, err);
        CloseHandle(dir_handle);
        return false;
    }

    // Issue the first ReadDirectoryChangesW call.
    if (!issue_read(*entry)) {
        CloseHandle(dir_handle);
        return false;
    }

    spdlog::info("Watching '{}' (recursive={})", key, recursive);
    watches_[key] = std::move(entry);
    return true;
}

// ── remove_watch ────────────────────────────────────────────────────────

void RDCWWatcher::remove_watch(const fs::path& path) {
    std::error_code ec;
    auto canonical = fs::canonical(path, ec);
    std::string key = ec ? path.string() : canonical.string();

    auto it = watches_.find(key);
    if (it == watches_.end()) return;

    auto& entry = it->second;
    if (entry->dir_handle != INVALID_HANDLE_VALUE) {
        CancelIo(entry->dir_handle);
        CloseHandle(entry->dir_handle);
    }

    watches_.erase(it);
    spdlog::debug("Removed watch for '{}'", key);
}

// ── run() — main event loop ─────────────────────────────────────────────

void RDCWWatcher::run(std::stop_token stop, NativeEventCallback callback) {
    spdlog::info("RDCWWatcher run loop starting, {} watches active",
                 watches_.size());

    // Register stop callback: post a special completion key to wake GQCS.
    std::stop_callback stop_cb(stop, [this] {
        PostQueuedCompletionStatus(iocp_, 0, kStopKey, nullptr);
    });

    while (!stop.stop_requested()) {
        DWORD bytes_transferred = 0;
        ULONG_PTR completion_key = 0;
        LPOVERLAPPED overlapped = nullptr;

        // Wait for completion, up to 1 second.
        BOOL success = GetQueuedCompletionStatus(
            iocp_,
            &bytes_transferred,
            &completion_key,
            &overlapped,
            1000);  // 1s timeout for stop check.

        if (!success) {
            DWORD err = GetLastError();
            if (err == WAIT_TIMEOUT) {
                continue;  // Timeout — re-check stop.
            }
            if (err == ERROR_OPERATION_ABORTED) {
                // CancelIo was called — this watch is being removed.
                continue;
            }

            if (overlapped != nullptr) {
                // I/O failed for a specific watch.
                // Find the entry and emit an error event.
                auto* entry =
                    reinterpret_cast<WatchEntry*>(completion_key);

                if (err == ERROR_NOTIFY_ENUM_DIR) {
                    // Buffer overflow — too many changes.
                    spdlog::warn("RDCW buffer overflow for '{}' — "
                                 "full re-scan recommended",
                                 entry->path.string());
                    NativeEvent ev;
                    ev.type = NativeEventType::Overflow;
                    ev.path = entry->path;
                    ev.timestamp = std::chrono::system_clock::now();
                    callback(ev);
                } else {
                    spdlog::error(
                        "RDCW I/O error for '{}': error {}",
                        entry->path.string(), err);
                    NativeEvent ev;
                    ev.type = NativeEventType::Error;
                    ev.path = entry->path;
                    ev.timestamp = std::chrono::system_clock::now();
                    callback(ev);
                }

                // Re-issue the read (it may succeed next time).
                issue_read(*entry);
            } else {
                spdlog::error("GQCS failed with no overlapped: error {}",
                              err);
            }
            continue;
        }

        // ── Check for stop signal ───────────────────────────────────
        if (completion_key == kStopKey) {
            spdlog::debug("RDCW stop signal received");
            break;
        }

        // ── Process completion ──────────────────────────────────────
        auto* entry = reinterpret_cast<WatchEntry*>(completion_key);

        if (bytes_transferred > 0) {
            process_notifications(*entry, bytes_transferred, callback);
        }

        // Re-issue ReadDirectoryChangesW for continuous monitoring.
        if (!issue_read(*entry)) {
            spdlog::error("Failed to re-issue RDC for '{}' — "
                          "watch may miss events",
                          entry->path.string());
        }
    }

    spdlog::info("RDCWWatcher run loop exiting");
}

// ── diagnostic_info / watch_count ──────────────────────────────────────

std::string RDCWWatcher::diagnostic_info() const {
    std::string info = "ReadDirectoryChangesW backend\n";
    info += "  watches: " + std::to_string(watches_.size()) + "\n";
    info += "  buffer_size: " + std::to_string(kBufferSize) + " bytes\n";
    for (const auto& [key, entry] : watches_) {
        info += "  - " + key;
        if (entry->recursive) info += " (recursive)";
        info += "\n";
    }
    return info;
}

std::size_t RDCWWatcher::watch_count() const {
    return watches_.size();
}

// ── Internal: issue_read ────────────────────────────────────────────────

bool RDCWWatcher::issue_read(WatchEntry& entry) {
    std::memset(&entry.overlapped, 0, sizeof(OVERLAPPED));

    BOOL ok = ReadDirectoryChangesW(
        entry.dir_handle,
        entry.buffer.data(),
        static_cast<DWORD>(entry.buffer.size()),
        entry.recursive ? TRUE : FALSE,
        kNotifyFilter,
        nullptr,  // bytes_returned unused for overlapped.
        &entry.overlapped,
        nullptr); // No completion routine; using IOCP.

    if (!ok) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            spdlog::error("ReadDirectoryChangesW failed for '{}': error {}",
                          entry.path.string(), err);
            return false;
        }
    }
    return true;
}

// ── Internal: process_notifications ─────────────────────────────────────

void RDCWWatcher::process_notifications(
    WatchEntry& entry,
    DWORD bytes_transferred,
    NativeEventCallback& callback)
{
    auto now = std::chrono::system_clock::now();

    const BYTE* ptr = entry.buffer.data();
    const BYTE* end = ptr + bytes_transferred;

    while (ptr < end) {
        const auto* info =
            reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(ptr);

        // Extract the filename (UTF-16, not null-terminated).
        std::wstring wname(
            info->FileName,
            info->FileNameLength / sizeof(WCHAR));

        // Build the full path.
        fs::path event_path = entry.path / wname;

        NativeEvent ev;
        ev.type = map_action(info->Action);
        ev.path = event_path;
        ev.timestamp = now;

        // Determine if it's a directory (best-effort stat).
        std::error_code ec;
        ev.is_directory = fs::is_directory(event_path, ec) && !ec;

        // Handle renamed events: FILE_ACTION_RENAMED_OLD_NAME carries
        // the old path; FILE_ACTION_RENAMED_NEW_NAME carries the new.
        // We emit Renamed for old_name and Created for new_name.
        if (info->Action == FILE_ACTION_RENAMED_OLD_NAME) {
            ev.type = NativeEventType::Renamed;
            ev.old_path = event_path;
            // The new name should follow in the next notification.
        } else if (info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
            ev.type = NativeEventType::Created;
            // Treat as creation at the new location.
        }

        callback(ev);

        // Advance to next entry.
        if (info->NextEntryOffset == 0) break;
        ptr += info->NextEntryOffset;
    }
}

// ── Internal: map_action ────────────────────────────────────────────────

NativeEventType RDCWWatcher::map_action(DWORD action) {
    switch (action) {
        case FILE_ACTION_ADDED:
            return NativeEventType::Created;
        case FILE_ACTION_REMOVED:
            return NativeEventType::Deleted;
        case FILE_ACTION_MODIFIED:
            return NativeEventType::Modified;
        case FILE_ACTION_RENAMED_OLD_NAME:
            return NativeEventType::Renamed;
        case FILE_ACTION_RENAMED_NEW_NAME:
            return NativeEventType::Created;
        default:
            return NativeEventType::Modified;
    }
}

// ── Internal: utf16_to_utf8 ─────────────────────────────────────────────

std::string RDCWWatcher::utf16_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};

    int len = WideCharToMultiByte(
        CP_UTF8, 0,
        wstr.data(), static_cast<int>(wstr.size()),
        nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};

    std::string result(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0,
        wstr.data(), static_cast<int>(wstr.size()),
        result.data(), len, nullptr, nullptr);
    return result;
}

// ── Internal: is_network_path ───────────────────────────────────────────

bool RDCWWatcher::is_network_path(const fs::path& path) {
    // Get the root path (e.g., "C:\\").
    auto root = path.root_path();
    if (root.empty()) return false;

    UINT drive_type = GetDriveTypeW(root.wstring().c_str());
    return (drive_type == DRIVE_REMOTE);
}

// ── Factory implementation (Windows) ────────────────────────────────────

std::unique_ptr<IFileWatcher> create_native_watcher() {
    return std::make_unique<RDCWWatcher>();
}

bool has_native_watcher() {
    return true;
}

}  // namespace kairos::watch

#endif  // _WIN32
