// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  instance_lock_posix.cpp — Single-instance enforcement via flock()        ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/platform/instance_lock.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sys/file.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace kairos::platform {

struct InstanceLock::Impl {
    int fd = -1;
    fs::path lock_path;

    ~Impl() {
        if (fd >= 0) {
            ::flock(fd, LOCK_UN);
            ::close(fd);
            // Remove the PID file on clean shutdown.
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

    int fd = ::open(lock_path.c_str(), O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        return nullptr;
    }

    // Non-blocking exclusive lock.
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return nullptr;  // Another instance holds the lock.
    }

    // Write our PID to the file.
    if (::ftruncate(fd, 0) == 0) {
        std::string pid_str = std::to_string(::getpid()) + "\n";
        [[maybe_unused]] auto _ = ::write(fd, pid_str.data(), pid_str.size());
    }

    auto impl = std::make_unique<Impl>();
    impl->fd = fd;
    impl->lock_path = lock_path;

    return std::unique_ptr<InstanceLock>(new InstanceLock(std::move(impl)));
}

}  // namespace kairos::platform
