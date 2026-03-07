/// src/exec/process_handle_posix.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  POSIX ProcessHandle — fork/exec with process groups and pipe capture    ║
// ║                                                                          ║
// ║  Spec reference: §14.3 (POSIX implementation), §14.5 (exit codes)       ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifndef _WIN32

#include "kairos/exec/process_handle.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <thread>

namespace kairos::exec {

// ── Exit code normalization (POSIX) ──────────────────────────────────────

int normalize_posix_status(int raw_status) {
    if (WIFEXITED(raw_status)) {
        return WEXITSTATUS(raw_status);
    }
    if (WIFSIGNALED(raw_status)) {
        return 128 + WTERMSIG(raw_status);
    }
    return 1;  // Unknown status.
}

// ── Pipe reader utility ─────────────────────────────────────────────────

/// Read all data from a file descriptor until EOF.
/// Calls the output callback with chunks as they arrive.
/// Appends to the accumulator string (with truncation).
static void pipe_reader_loop(
    int fd,
    bool is_stderr,
    OutputCallback& callback,
    std::string& accumulator,
    std::size_t max_bytes,
    std::atomic<bool>& output_truncated)
{
    constexpr std::size_t kBufSize = 4096;
    std::array<char, kBufSize> buf{};

    while (true) {
        ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n <= 0) {
            // n == 0: EOF. n < 0 && errno != EINTR: error.
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }

        std::string_view chunk(buf.data(), static_cast<std::size_t>(n));

        // Deliver to streaming callback.
        if (callback) {
            try {
                callback(chunk, is_stderr);
            } catch (...) {
                // OutputCallback must not throw, but be defensive.
            }
        }

        // Accumulate (with truncation).
        if (accumulator.size() < max_bytes) {
            std::size_t remaining = max_bytes - accumulator.size();
            std::size_t to_append = std::min(
                static_cast<std::size_t>(n), remaining);
            accumulator.append(buf.data(), to_append);
            if (to_append < static_cast<std::size_t>(n)) {
                output_truncated.store(true, std::memory_order_relaxed);
            }
        }
    }

    // Signal end-of-stream.
    if (callback) {
        try {
            callback({}, is_stderr);
        } catch (...) {}
    }
}

// ── POSIX ProcessHandle implementation ───────────────────────────────────

class PosixProcessHandle final : public ProcessHandle {
public:
    ~PosixProcessHandle() override {
        // Safety net: kill the process if still running.
        if (is_running()) {
            kill();
            int status = 0;
            ::waitpid(child_pid_, &status, 0);
        }
        // Join reader threads.
        if (stdout_reader_.joinable()) stdout_reader_.join();
        if (stderr_reader_.joinable()) stderr_reader_.join();
    }

    void set_output_callback(OutputCallback cb) override {
        callback_ = std::move(cb);
    }

    bool spawn(const ProcessSpec& spec) override {
        spec_ = spec;
        start_time_ = std::chrono::steady_clock::now();

        // ── Create pipes ─────────────────────────────────────────
        std::array<int, 2> stdout_pipe{};
        std::array<int, 2> stderr_pipe{};

        if (::pipe(stdout_pipe.data()) != 0 ||
            ::pipe(stderr_pipe.data()) != 0) {
            result_.exit_code = 202;
            result_.termination = ProcessResult::TerminationKind::SpawnFailed;
            result_.termination_reason =
                std::string("pipe() failed: ") + std::strerror(errno);
            return false;
        }

        // ── Build argv ───────────────────────────────────────────
        std::vector<std::string> argv_storage;
        if (spec.use_shell) {
            std::string shell = spec.shell.empty() ? "/bin/sh" : spec.shell;
            argv_storage = {shell, "-c", spec.command_line};
        } else {
            argv_storage = spec.args;
        }

        if (argv_storage.empty()) {
            result_.exit_code = 202;
            result_.termination = ProcessResult::TerminationKind::SpawnFailed;
            result_.termination_reason = "Empty command";
            ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
            ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
            return false;
        }

        // Convert to C-style argv.
        std::vector<char*> argv;
        argv.reserve(argv_storage.size() + 1);
        for (auto& s : argv_storage) {
            argv.push_back(s.data());
        }
        argv.push_back(nullptr);

        // ── Build envp ───────────────────────────────────────────
        std::vector<std::string> env_storage;
        env_storage.reserve(spec.environment.size());
        for (const auto& [k, v] : spec.environment) {
            env_storage.push_back(k + "=" + v);
        }
        std::vector<char*> envp;
        envp.reserve(env_storage.size() + 1);
        for (auto& s : env_storage) {
            envp.push_back(s.data());
        }
        envp.push_back(nullptr);

        // ── Validate working directory ───────────────────────────
        if (!spec.working_dir.empty()) {
            std::error_code ec;
            if (!std::filesystem::is_directory(spec.working_dir, ec)) {
                result_.exit_code = 202;
                result_.termination =
                    ProcessResult::TerminationKind::SpawnFailed;
                result_.termination_reason =
                    "Working directory does not exist: " +
                    spec.working_dir.string();
                ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
                ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
                return false;
            }
        }

        // ── Fork ─────────────────────────────────────────────────
        pid_t pid = ::fork();
        if (pid < 0) {
            result_.exit_code = 202;
            result_.termination = ProcessResult::TerminationKind::SpawnFailed;
            result_.termination_reason =
                std::string("fork() failed: ") + std::strerror(errno);
            ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
            ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
            return false;
        }

        if (pid == 0) {
            // ── Child process ────────────────────────────────────

            // Create a new process group for kill-tree.
            ::setpgid(0, 0);

            // Redirect stdout and stderr to pipes.
            ::dup2(stdout_pipe[1], STDOUT_FILENO);
            ::dup2(stderr_pipe[1], STDERR_FILENO);

            // Close all pipe file descriptors (child doesn't need them).
            ::close(stdout_pipe[0]);
            ::close(stdout_pipe[1]);
            ::close(stderr_pipe[0]);
            ::close(stderr_pipe[1]);

            // Change working directory.
            if (!spec.working_dir.empty()) {
                if (::chdir(spec.working_dir.c_str()) != 0) {
                    ::_exit(202);
                }
            }

            // Execute.
            if (spec.environment.empty()) {
                ::execvp(argv[0], argv.data());
            } else {
                ::execve(argv[0], argv.data(), envp.data());
            }

            // If we reach here, exec failed.
            // Distinguish "not found" (ENOENT) from "not executable" (EACCES).
            int exit_code = (errno == ENOENT) ? 127 : 126;
            ::_exit(exit_code);
        }

        // ── Parent process ───────────────────────────────────────
        child_pid_ = pid;
        running_.store(true, std::memory_order_release);

        // Close write ends (parent only reads).
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[1]);

        // Set read ends to close-on-exec (defensive).
        ::fcntl(stdout_pipe[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(stderr_pipe[0], F_SETFD, FD_CLOEXEC);

        // Start reader threads.
        stdout_reader_ = std::thread([this, fd = stdout_pipe[0]] {
            pipe_reader_loop(fd, false, callback_,
                             result_.stdout_data,
                             spec_.max_output_bytes,
                             output_truncated_);
            ::close(fd);
        });

        stderr_reader_ = std::thread([this, fd = stderr_pipe[0]] {
            pipe_reader_loop(fd, true, callback_,
                             result_.stderr_data,
                             spec_.max_output_bytes,
                             output_truncated_);
            ::close(fd);
        });

        return true;
    }

    ProcessResult wait(std::stop_token stop) override {
        auto deadline = std::chrono::steady_clock::time_point::max();
        if (spec_.timeout.has_value()) {
            deadline = start_time_ + *spec_.timeout;
        }

        constexpr auto kPollInterval = std::chrono::milliseconds(50);

        while (true) {
            // Check cancellation.
            if (stop.stop_requested()) {
                soft_then_hard_kill("Cancelled via stop_token");
                result_.termination =
                    ProcessResult::TerminationKind::Cancelled;
                result_.termination_reason = "Run cancelled";
                break;
            }

            // Check timeout.
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                bool hard = soft_then_hard_kill("Timeout");
                result_.termination = hard
                    ? ProcessResult::TerminationKind::HardKill
                    : ProcessResult::TerminationKind::SoftKill;
                result_.exit_code = hard ? 201 : 200;
                result_.termination_reason =
                    "Timeout after " +
                    std::to_string(spec_.timeout->count()) + "s";
                break;
            }

            // Check if child has exited (non-blocking).
            int status = 0;
            pid_t result = ::waitpid(child_pid_, &status, WNOHANG);

            if (result > 0) {
                // Child exited.
                result_.exit_code = normalize_posix_status(status);
                result_.termination =
                    ProcessResult::TerminationKind::Normal;
                break;
            }

            if (result < 0) {
                // waitpid error (e.g., child already reaped).
                if (errno != EINTR) {
                    result_.exit_code = 1;
                    result_.termination =
                        ProcessResult::TerminationKind::Normal;
                    result_.termination_reason =
                        std::string("waitpid error: ") +
                        std::strerror(errno);
                    break;
                }
                continue;
            }

            // result == 0: child still running. Sleep briefly.
            std::this_thread::sleep_for(kPollInterval);
        }

        // Ensure reader threads finish (they drain remaining pipe data).
        if (stdout_reader_.joinable()) stdout_reader_.join();
        if (stderr_reader_.joinable()) stderr_reader_.join();

        running_.store(false, std::memory_order_release);

        // Record duration.
        result_.duration = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time_);

        return result_;
    }

    void terminate() override {
        if (!is_running()) return;
        // Send SIGTERM to the entire process group.
        ::kill(-child_pid_, SIGTERM);
    }

    void kill() override {
        if (!is_running()) return;
        // Send SIGKILL to the entire process group.
        ::kill(-child_pid_, SIGKILL);
    }

    [[nodiscard]] bool is_running() const override {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] int64_t pid() const override {
        return static_cast<int64_t>(child_pid_);
    }

    [[nodiscard]] const ProcessResult& result() const override {
        return result_;
    }

private:
    /// Soft then hard kill sequence.
    /// @return true if hard kill was needed.
    bool soft_then_hard_kill(const char* reason) {
        // Step 1: soft kill (SIGTERM to process group).
        ::kill(-child_pid_, SIGTERM);

        // Wait for grace period.
        auto grace_deadline = std::chrono::steady_clock::now() +
                              spec_.kill_timeout;

        while (std::chrono::steady_clock::now() < grace_deadline) {
            int status = 0;
            pid_t r = ::waitpid(child_pid_, &status, WNOHANG);
            if (r > 0) {
                // Child responded to SIGTERM.
                result_.exit_code = normalize_posix_status(status);
                return false;  // Soft kill was sufficient.
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        // Step 2: hard kill (SIGKILL to process group).
        ::kill(-child_pid_, SIGKILL);

        // Reap the zombie.
        int status = 0;
        ::waitpid(child_pid_, &status, 0);

        return true;  // Hard kill was needed.
    }

    ProcessSpec spec_;
    ProcessResult result_;
    OutputCallback callback_;
    pid_t child_pid_ = -1;
    std::atomic<bool> running_{false};
    std::atomic<bool> output_truncated_{false};
    std::chrono::steady_clock::time_point start_time_;
    std::thread stdout_reader_;
    std::thread stderr_reader_;
};

// ── Factory ──────────────────────────────────────────────────────────────

std::unique_ptr<ProcessHandle> create_process_handle() {
    return std::make_unique<PosixProcessHandle>();
}

}  // namespace kairos::exec

#endif  // !_WIN32
