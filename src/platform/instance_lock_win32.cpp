/// src/platform/instance_lock_win32.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  instance_lock_win32.cpp — Single-instance enforcement via named mutex    ║
// ║                                                                           ║
// ║  Uses a global named mutex for system-wide exclusion, plus writes the     ║
// ║  PID to the lock file so `kairos status` can read it.                     ║
// ║                                                                           ║
// ║  Spec reference: §25.7                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef _WIN32

#include "kairos/platform/instance_lock.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace kairos::platform {

struct InstanceLock::Impl {
    HANDLE mutex_handle = nullptr;
    fs::path lock_path;

    ~Impl() {
        if (mutex_handle) {
            ::ReleaseMutex(mutex_handle);
            ::CloseHandle(mutex_handle);
        }
        // Remove PID file on clean shutdown.
        if (!lock_path.empty()) {
            std::error_code ec;
            fs::remove(lock_path, ec);
        }
    }
};

InstanceLock::InstanceLock(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

InstanceLock::~InstanceLock() = default;

std::unique_ptr<InstanceLock> InstanceLock::try_acquire(
    const std::filesystem::path& lock_path)
{
    // Ensure parent directory exists.
    if (lock_path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(lock_path.parent_path(), ec);
    }

    // Attempt to create/acquire a global named mutex.
    // The "Global\\" prefix makes it visible across all sessions
    // (including Windows Service session 0).
    HANDLE h = ::CreateMutexW(nullptr, TRUE, L"Global\\KairosDaemon");
    if (h == nullptr) {
        return nullptr;  // CreateMutex failed entirely.
    }

    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        // Another Kairos instance already holds the mutex.
        ::CloseHandle(h);
        return nullptr;
    }

    // Write PID to the lock file for `kairos status`.
    {
        std::ofstream ofs(lock_path, std::ios::trunc);
        if (ofs.is_open()) {
            ofs << ::GetCurrentProcessId() << "\n";
        }
        // If the write fails, we still hold the mutex — not fatal.
    }

    auto impl = std::make_unique<Impl>();
    impl->mutex_handle = h;
    impl->lock_path = lock_path;

    return std::unique_ptr<InstanceLock>(new InstanceLock(std::move(impl)));
}

}  // namespace kairos::platform

#endif  // _WIN32
