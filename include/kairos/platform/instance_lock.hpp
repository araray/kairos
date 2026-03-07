/// include/kairos/platform/instance_lock.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/platform/instance_lock.hpp — Single-instance enforcement          ║
// ║  Spec reference: §25.7                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace kairos::platform {

/// RAII guard that ensures only one Kairos daemon runs at a time.
///
/// On POSIX: uses flock() on a PID file.
/// On Windows: uses a named mutex.
///
/// The lock is released when the InstanceLock object is destroyed.
class InstanceLock {
public:
    /// Attempt to acquire the instance lock.
    ///
    /// @param lock_path  Path to the PID/lock file.
    /// @return           An InstanceLock if acquisition succeeded, nullptr otherwise.
    static std::unique_ptr<InstanceLock> try_acquire(
        const std::filesystem::path& lock_path);

    ~InstanceLock();

    // Non-copyable, non-movable.
    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    explicit InstanceLock(std::unique_ptr<Impl> impl);
};

}  // namespace kairos::platform
