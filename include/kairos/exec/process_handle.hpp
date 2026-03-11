/// include/kairos/exec/process_handle.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/exec/process_handle.hpp — Cross-platform process abstraction     ║
// ║                                                                          ║
// ║  Spawns a child process, captures stdout/stderr via streaming pipes,     ║
// ║  enforces timeouts with soft→hard kill sequence, and normalizes exit     ║
// ║  codes across POSIX and Windows.                                         ║
// ║                                                                          ║
// ║  Spec reference: §14.2–§14.5                                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kairos::exec {

// ── Output streaming callback ────────────────────────────────────────────

/// Called with each chunk of data from the child's stdout or stderr.
///
/// Contract:
///   - Called from dedicated reader threads (not the caller's thread).
///   - Must not throw. Must not block for extended periods.
///   - Chunk boundaries are arbitrary (not line-aligned).
///   - An empty chunk signals end-of-stream for that descriptor.
using OutputCallback = std::function<void(
    std::string_view chunk, bool is_stderr)>;

// ── Process specification ────────────────────────────────────────────────

/// Describes how to spawn a child process.
struct ProcessSpec {
    /// The command to execute.
    /// If `use_shell` is true, executed via the shell:
    ///   POSIX:   [shell, "-c", command_line]
    ///   Windows: [shell, "/C", command_line]
    std::string command_line;

    /// Arguments (used when use_shell is false).
    /// The first element is the executable.
    std::vector<std::string> args;

    /// If true, command_line is executed via the configured shell.
    bool use_shell = true;

    /// Shell to use when use_shell is true.
    /// Empty means auto-detect: /bin/sh (POSIX) or cmd.exe (Windows).
    std::string shell;

    /// Working directory. Empty means inherit from the daemon.
    std::filesystem::path working_dir;

    /// Environment variables for the child process.
    /// These are the *complete* environment (not a delta).
    /// Use EnvBuilder (§14.6) to construct this.
    std::unordered_map<std::string, std::string> environment;

    /// Timeout for the entire process. nullopt means no timeout.
    std::optional<std::chrono::seconds> timeout;

    /// Grace period between SIGTERM and SIGKILL during timeout.
    std::chrono::seconds kill_timeout{10};

    /// Maximum combined bytes of captured output.
    /// If exceeded, output is truncated (not the process killed).
    std::size_t max_output_bytes = 10 * 1024 * 1024;  // 10 MiB

    // ── Runner type dispatch (§15.5–§15.6) ────────────────────────────

    /// Runner type for dispatch. Empty or "local_shell" = default runner.
    /// "docker" = DockerProcessHandle. "interpreter" = wrapped shell.
    std::string runner_type;

    // ── Docker runner fields (§15.3) ──────────────────────────────────

    /// Docker container image (e.g., "python:3.11-slim").
    /// Required when runner_type == "docker".
    std::string docker_image;

    /// Volume mounts in Docker bind-mount format: "host_path:container_path[:ro]".
    std::vector<std::string> docker_volumes;

    /// Docker network mode (default: "bridge"). Common: "host", "none", "bridge".
    std::string docker_network = "bridge";

    /// Automatically pull the image if not present locally.
    bool docker_auto_pull = true;

    /// Remove the container after execution completes (even on failure).
    bool docker_remove = true;

    // ── Ansible runner fields (§15.4) ──────────────────────────────

    /// Path to the Ansible playbook file (.yml/.yaml).
    /// Required when runner_type == "ansible".
    std::string ansible_playbook;

    /// Inventory file path or host pattern (e.g., "localhost,").
    /// If empty, defaults to "localhost," with --connection local.
    std::string ansible_inventory;

    /// Extra variables as a JSON string.
    /// Passed to ansible-playbook via --extra-vars.
    std::string ansible_extra_vars;

    /// Vault decryption password. Injected via ANSIBLE_VAULT_PASSWORD
    /// environment variable (never on the command line).
    std::string ansible_vault_password;

    /// Limit execution to specific hosts (--limit).
    std::string ansible_limit;

    /// Run only tasks tagged with these tags (--tags).
    std::string ansible_tags;

    /// Skip tasks tagged with these tags (--skip-tags).
    std::string ansible_skip_tags;

    /// Run in check mode (--check). No changes are made.
    bool ansible_check = false;

    /// Show diffs for template/file changes (--diff).
    bool ansible_diff = false;

    /// Verbosity level (0–4). 0 = normal, 1 = -v, 2 = -vv, etc.
    int ansible_verbosity = 0;
};

// ── Process result ───────────────────────────────────────────────────────

/// Result of a completed process execution.
struct ProcessResult {
    /// How the process terminated.
    enum class TerminationKind {
        Normal,      ///< Process exited on its own.
        SoftKill,    ///< Process killed by SIGTERM / CtrlBreak.
        HardKill,    ///< Process killed by SIGKILL / TerminateJobObject.
        Cancelled,   ///< Cancelled via stop_token.
        SpawnFailed  ///< Process could not be started.
    };

    /// Normalized exit code.
    /// - 0: success
    /// - 1–125: application error
    /// - 126: command not executable
    /// - 127: command not found
    /// - 128+N: killed by signal N (POSIX)
    /// - 200: timeout (soft kill)
    /// - 201: timeout (hard kill)
    /// - 202: runner/spawn error
    int exit_code = -1;

    /// Captured stdout content.
    std::string stdout_data;

    /// Captured stderr content.
    std::string stderr_data;

    /// Wall-clock duration of execution.
    std::chrono::milliseconds duration{0};

    /// How the process was terminated.
    TerminationKind termination = TerminationKind::Normal;

    /// Human-readable termination reason (for diagnostics).
    std::string termination_reason;

    /// True if the process completed successfully (exit_code == 0).
    [[nodiscard]] bool success() const noexcept { return exit_code == 0; }

    /// True if the process timed out.
    [[nodiscard]] bool timed_out() const noexcept {
        return exit_code == 200 || exit_code == 201;
    }
};

// ── ProcessHandle abstract interface ─────────────────────────────────────

/// Platform-agnostic process handle.
///
/// Lifecycle:
///   1. Create via create_process_handle()
///   2. Optionally set_output_callback() for streaming
///   3. spawn() starts the process
///   4. wait() blocks until completion, timeout, or cancellation
///   5. result() returns the ProcessResult
///   6. Destructor ensures cleanup (kill if still running)
///
/// Thread safety:
///   - spawn() must be called from a single thread.
///   - wait() blocks the calling thread.
///   - terminate() and kill() may be called from any thread.
///   - OutputCallback is called from reader threads.
class ProcessHandle {
public:
    virtual ~ProcessHandle() = default;

    /// Spawn the child process.
    /// @return false if spawn failed (result() will have SpawnFailed).
    [[nodiscard]] virtual bool spawn(const ProcessSpec& spec) = 0;

    /// Block until the process exits, times out, or stop_token fires.
    /// @return The process result.
    virtual ProcessResult wait(std::stop_token stop) = 0;

    /// Send a soft termination signal.
    /// POSIX: SIGTERM to the process group.
    /// Windows: GenerateConsoleCtrlEvent to the job.
    virtual void terminate() = 0;

    /// Force-kill the process tree.
    /// POSIX: SIGKILL to the process group.
    /// Windows: TerminateJobObject.
    virtual void kill() = 0;

    /// Check if the process is still running.
    [[nodiscard]] virtual bool is_running() const = 0;

    /// Get the OS process ID (for diagnostics/logging).
    [[nodiscard]] virtual int64_t pid() const = 0;

    /// Attach a callback for streaming output.
    /// Must be called before spawn(). The callback receives chunks
    /// as they arrive from stdout/stderr.
    virtual void set_output_callback(OutputCallback cb) = 0;

    /// Get the result after wait() completes.
    [[nodiscard]] virtual const ProcessResult& result() const = 0;
};

/// Factory function: creates the platform-appropriate ProcessHandle.
std::unique_ptr<ProcessHandle> create_process_handle();

// ── Exit code normalization ──────────────────────────────────────────────

/// Normalize a raw POSIX wait status to a Kairos exit code.
/// - WIFEXITED: return the exit status (0–255).
/// - WIFSIGNALED: return 128 + signal_number.
/// - Otherwise: return 1.
int normalize_posix_status(int raw_status);

#ifdef _WIN32
/// Normalize a Windows process exit code to Kairos semantics.
/// - Normal exit codes (0–255) pass through.
/// - NTSTATUS codes (0xC0000000+) are mapped:
///   - 0xC0000005 (access violation) → 139 (SIGSEGV equivalent)
///   - 0xC00000FD (stack overflow)   → 139
///   - 0xC0000094 (int div by zero)  → 136 (SIGFPE equivalent)
///   - Others → 1
int normalize_win32_exit_code(unsigned long raw_code);
#endif

}  // namespace kairos::exec
