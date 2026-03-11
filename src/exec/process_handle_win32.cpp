/// src/exec/process_handle_win32.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Win32 ProcessHandle — CreateProcessW, job objects, pipe capture          ║
// ║                                                                          ║
// ║  Implements the same ProcessHandle interface as the POSIX variant but     ║
// ║  using Windows APIs:                                                      ║
// ║    - CreateProcessW (suspended) + AssignProcessToJobObject + ResumeThread ║
// ║    - CreatePipe for stdout/stderr capture                                 ║
// ║    - GenerateConsoleCtrlEvent for soft kill, TerminateJobObject for hard  ║
// ║    - NTSTATUS → POSIX-style exit code normalization                       ║
// ║                                                                          ║
// ║  Spec reference: §14.4, §14.5                                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef _WIN32

#include "kairos/exec/process_handle.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <array>
#include <atomic>
#include <string>
#include <thread>

namespace kairos::exec {

// ── Exit code normalization (Win32) ──────────────────────────────────────

int normalize_win32_exit_code(unsigned long raw_code) {
    // Normal exit codes (0–255) pass through.
    if (raw_code <= 255) {
        return static_cast<int>(raw_code);
    }

    // NTSTATUS codes (0xC0000000+) map to POSIX signal equivalents.
    switch (raw_code) {
        case 0xC0000005:  // STATUS_ACCESS_VIOLATION
        case 0xC00000FD:  // STATUS_STACK_OVERFLOW
            return 128 + 11;  // SIGSEGV equivalent (139)

        case 0xC000001D:  // STATUS_ILLEGAL_INSTRUCTION
            return 128 + 4;   // SIGILL equivalent (132)

        case 0xC0000094:  // STATUS_INTEGER_DIVIDE_BY_ZERO
            return 128 + 8;   // SIGFPE equivalent (136)

        case 0x40010004:  // STATUS_CONTROL_C_EXIT
            return 128 + 2;   // SIGINT equivalent (130)

        default:
            // Any code above 0x80000000 is an abnormal termination.
            if (raw_code > 0x80000000u) {
                return 128 + 9;  // Map to SIGKILL equivalent (137)
            }
            // Large but non-NTSTATUS codes: truncate to byte range.
            return static_cast<int>(raw_code & 0xFF);
    }
}

// ── Internal helpers ─────────────────────────────────────────────────────

namespace {

/// Convert a UTF-8 std::string to UTF-16 std::wstring for Win32 APIs.
std::wstring utf8_to_utf16(const std::string& utf8) {
    if (utf8.empty()) return {};
    int needed = ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        result.data(), needed);
    return result;
}

/// Quote a single argument per Windows CommandLineToArgvW rules.
/// Backslash is literal unless immediately before a double-quote.
/// Arguments with spaces, quotes, or tabs must be quoted.
std::wstring quote_arg_win(const std::string& arg) {
    std::wstring warg = utf8_to_utf16(arg);

    // If no special characters, return as-is.
    if (!warg.empty() &&
        warg.find_first_of(L" \t\"") == std::wstring::npos) {
        return warg;
    }

    // Surround with quotes and escape internal quotes/backslashes.
    std::wstring quoted = L"\"";
    for (auto it = warg.begin(); ; ++it) {
        std::size_t num_backslashes = 0;

        while (it != warg.end() && *it == L'\\') {
            ++it;
            ++num_backslashes;
        }

        if (it == warg.end()) {
            // End of argument: double backslashes before closing quote.
            quoted.append(num_backslashes * 2, L'\\');
            break;
        } else if (*it == L'"') {
            // Before a quote: double backslashes + escaped quote.
            quoted.append(num_backslashes * 2 + 1, L'\\');
            quoted.push_back(*it);
        } else {
            // Normal character: backslashes are literal.
            quoted.append(num_backslashes, L'\\');
            quoted.push_back(*it);
        }
    }
    quoted.push_back(L'"');
    return quoted;
}

/// Build a Windows environment block: double-null-terminated UTF-16.
/// Each entry is KEY=VALUE\0, terminated by an extra \0.
std::wstring build_env_block(
    const std::unordered_map<std::string, std::string>& env)
{
    std::wstring block;
    for (const auto& [k, v] : env) {
        block += utf8_to_utf16(k);
        block += L'=';
        block += utf8_to_utf16(v);
        block += L'\0';
    }
    block += L'\0';  // Double null terminator.
    return block;
}

/// Read all data from a Win32 pipe HANDLE until the pipe breaks.
/// Calls the output callback with chunks as they arrive.
/// Appends to the accumulator string with truncation.
void pipe_reader_loop(
    HANDLE pipe,
    bool is_stderr,
    OutputCallback& callback,
    std::string& accumulator,
    std::size_t max_bytes,
    std::atomic<bool>& output_truncated)
{
    constexpr DWORD kBufSize = 4096;
    std::array<char, kBufSize> buf{};

    while (true) {
        DWORD bytes_read = 0;
        BOOL ok = ::ReadFile(
            pipe, buf.data(), kBufSize, &bytes_read, nullptr);

        if (!ok || bytes_read == 0) {
            // Pipe broken (child exited) or error.
            break;
        }

        std::string_view chunk(buf.data(), static_cast<std::size_t>(bytes_read));

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
            std::size_t to_append = (std::min)(
                static_cast<std::size_t>(bytes_read), remaining);
            accumulator.append(buf.data(), to_append);
            if (to_append < static_cast<std::size_t>(bytes_read)) {
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

}  // anonymous namespace

// ── Win32 ProcessHandle implementation ───────────────────────────────────

class Win32ProcessHandle final : public ProcessHandle {
public:
    ~Win32ProcessHandle() override {
        // Safety net: kill the process if still running.
        if (is_running()) {
            kill();
            ::WaitForSingleObject(process_info_.hProcess, 5000);
        }
        // Join reader threads before closing handles they use.
        if (stdout_reader_.joinable()) stdout_reader_.join();
        if (stderr_reader_.joinable()) stderr_reader_.join();
        cleanup_handles();
    }

    void set_output_callback(OutputCallback cb) override {
        callback_ = std::move(cb);
    }

    bool spawn(const ProcessSpec& spec) override {
        spec_ = spec;
        start_time_ = std::chrono::steady_clock::now();

        // ── Create pipes for stdout/stderr ─────────────────────
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        HANDLE stdout_read  = INVALID_HANDLE_VALUE;
        HANDLE stdout_write = INVALID_HANDLE_VALUE;
        HANDLE stderr_read  = INVALID_HANDLE_VALUE;
        HANDLE stderr_write = INVALID_HANDLE_VALUE;

        if (!::CreatePipe(&stdout_read, &stdout_write, &sa, 0) ||
            !::CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
            result_.exit_code = 202;
            result_.termination =
                ProcessResult::TerminationKind::SpawnFailed;
            result_.termination_reason = "CreatePipe failed: " +
                std::to_string(::GetLastError());
            if (stdout_read != INVALID_HANDLE_VALUE)  ::CloseHandle(stdout_read);
            if (stdout_write != INVALID_HANDLE_VALUE) ::CloseHandle(stdout_write);
            return false;
        }

        // Ensure the read handles are NOT inherited by the child.
        ::SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
        ::SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

        stdout_read_ = stdout_read;
        stderr_read_ = stderr_read;

        // ── Create a job object for kill-tree ──────────────────
        job_object_ = ::CreateJobObjectW(nullptr, nullptr);
        if (job_object_ == nullptr) {
            result_.exit_code = 202;
            result_.termination =
                ProcessResult::TerminationKind::SpawnFailed;
            result_.termination_reason = "CreateJobObject failed: " +
                std::to_string(::GetLastError());
            ::CloseHandle(stdout_read);  ::CloseHandle(stdout_write);
            ::CloseHandle(stderr_read);  ::CloseHandle(stderr_write);
            stdout_read_ = INVALID_HANDLE_VALUE;
            stderr_read_ = INVALID_HANDLE_VALUE;
            return false;
        }

        // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: when the job handle
        // closes, all processes in the job are terminated.
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
        job_limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job_object_,
            JobObjectExtendedLimitInformation,
            &job_limits, sizeof(job_limits));

        // ── Build command line ─────────────────────────────────
        std::wstring cmd_line;
        if (spec.use_shell) {
            std::string shell = spec.shell.empty()
                ? "cmd.exe" : spec.shell;
            // cmd.exe /C "command"
            // The outer quotes around the entire command handle
            // internal quoting correctly per cmd.exe rules.
            cmd_line = utf8_to_utf16(shell) + L" /C \"" +
                       utf8_to_utf16(spec.command_line) + L"\"";
        } else {
            // Direct execution: build a flat command line from args.
            for (std::size_t i = 0; i < spec.args.size(); ++i) {
                if (i > 0) cmd_line += L' ';
                cmd_line += quote_arg_win(spec.args[i]);
            }
        }

        if (cmd_line.empty()) {
            result_.exit_code = 202;
            result_.termination =
                ProcessResult::TerminationKind::SpawnFailed;
            result_.termination_reason = "Empty command";
            ::CloseHandle(stdout_read);  ::CloseHandle(stdout_write);
            ::CloseHandle(stderr_read);  ::CloseHandle(stderr_write);
            ::CloseHandle(job_object_);
            stdout_read_ = INVALID_HANDLE_VALUE;
            stderr_read_ = INVALID_HANDLE_VALUE;
            job_object_ = nullptr;
            return false;
        }

        // ── Build environment block ────────────────────────────
        std::wstring env_block;
        if (!spec.environment.empty()) {
            env_block = build_env_block(spec.environment);
        }

        // ── Validate working directory ─────────────────────────
        std::wstring work_dir;
        if (!spec.working_dir.empty()) {
            std::error_code ec;
            if (!std::filesystem::is_directory(spec.working_dir, ec)) {
                result_.exit_code = 202;
                result_.termination =
                    ProcessResult::TerminationKind::SpawnFailed;
                result_.termination_reason =
                    "Working directory does not exist: " +
                    spec.working_dir.string();
                ::CloseHandle(stdout_read);  ::CloseHandle(stdout_write);
                ::CloseHandle(stderr_read);  ::CloseHandle(stderr_write);
                ::CloseHandle(job_object_);
                stdout_read_ = INVALID_HANDLE_VALUE;
                stderr_read_ = INVALID_HANDLE_VALUE;
                job_object_ = nullptr;
                return false;
            }
            work_dir = spec.working_dir.wstring();
        }

        // ── Set up STARTUPINFO ─────────────────────────────────
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = stdout_write;
        si.hStdError  = stderr_write;
        si.hStdInput  = ::GetStdHandle(STD_INPUT_HANDLE);

        // ── CreateProcess (suspended) ──────────────────────────
        // CREATE_SUSPENDED: assign to job before the child runs.
        // CREATE_NEW_PROCESS_GROUP: for GenerateConsoleCtrlEvent.
        // CREATE_UNICODE_ENVIRONMENT: we pass a WCHAR env block.
        DWORD flags = CREATE_SUSPENDED |
                      CREATE_NEW_PROCESS_GROUP |
                      CREATE_UNICODE_ENVIRONMENT;

        BOOL ok = ::CreateProcessW(
            nullptr,              // lpApplicationName (null = parse cmdline)
            cmd_line.data(),      // lpCommandLine (mutable buffer)
            nullptr,              // lpProcessAttributes
            nullptr,              // lpThreadAttributes
            TRUE,                 // bInheritHandles
            flags,
            env_block.empty() ? nullptr : env_block.data(),
            work_dir.empty()  ? nullptr : work_dir.c_str(),
            &si,
            &process_info_
        );

        // Close child-side pipe handles (parent only reads).
        ::CloseHandle(stdout_write);
        ::CloseHandle(stderr_write);

        if (!ok) {
            DWORD err = ::GetLastError();
            result_.exit_code =
                (err == ERROR_FILE_NOT_FOUND ||
                 err == ERROR_PATH_NOT_FOUND) ? 127 : 126;
            result_.termination =
                ProcessResult::TerminationKind::SpawnFailed;
            result_.termination_reason = "CreateProcessW failed: error " +
                std::to_string(err);
            ::CloseHandle(stdout_read_); stdout_read_ = INVALID_HANDLE_VALUE;
            ::CloseHandle(stderr_read_); stderr_read_ = INVALID_HANDLE_VALUE;
            ::CloseHandle(job_object_);  job_object_ = nullptr;
            return false;
        }

        // Assign child to job object BEFORE resuming.
        ::AssignProcessToJobObject(job_object_, process_info_.hProcess);

        // Resume the main thread — child starts executing now.
        ::ResumeThread(process_info_.hThread);

        // Start pipe reader threads.
        stdout_reader_ = std::thread(
            [this] {
                pipe_reader_loop(
                    stdout_read_, false, callback_,
                    result_.stdout_data,
                    spec_.max_output_bytes,
                    output_truncated_);
            });

        stderr_reader_ = std::thread(
            [this] {
                pipe_reader_loop(
                    stderr_read_, true, callback_,
                    result_.stderr_data,
                    spec_.max_output_bytes,
                    output_truncated_);
            });

        running_.store(true, std::memory_order_release);
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

            // Check if child has exited (non-blocking poll).
            DWORD wait_result = ::WaitForSingleObject(
                process_info_.hProcess,
                static_cast<DWORD>(kPollInterval.count()));

            if (wait_result == WAIT_OBJECT_0) {
                // Process exited normally.
                DWORD raw_code = 0;
                ::GetExitCodeProcess(process_info_.hProcess, &raw_code);
                result_.exit_code = normalize_win32_exit_code(raw_code);
                result_.termination =
                    ProcessResult::TerminationKind::Normal;
                break;
            }

            // WAIT_TIMEOUT: child still running — loop continues.
            // WAIT_FAILED: shouldn't happen, treat as exit.
            if (wait_result == WAIT_FAILED) {
                result_.exit_code = 1;
                result_.termination =
                    ProcessResult::TerminationKind::Normal;
                result_.termination_reason =
                    "WaitForSingleObject failed: " +
                    std::to_string(::GetLastError());
                break;
            }
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
        // Soft kill: send CTRL_BREAK_EVENT to the process group.
        // This is the Windows equivalent of SIGTERM.
        // The child process must be in its own process group
        // (CREATE_NEW_PROCESS_GROUP ensures this).
        ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT,
                                   process_info_.dwProcessId);
    }

    void kill() override {
        // Hard kill: terminate ALL processes in the job object.
        // This is the Windows equivalent of SIGKILL to a process group.
        if (job_object_ != nullptr) {
            ::TerminateJobObject(job_object_, 1);
        }
    }

    [[nodiscard]] bool is_running() const override {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] int64_t pid() const override {
        return static_cast<int64_t>(process_info_.dwProcessId);
    }

    [[nodiscard]] const ProcessResult& result() const override {
        return result_;
    }

private:
    /// Soft then hard kill sequence.
    /// @return true if hard kill was needed.
    bool soft_then_hard_kill(const char* /*reason*/) {
        // Step 1: soft kill (CTRL_BREAK_EVENT).
        if (process_info_.dwProcessId != 0) {
            ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT,
                                       process_info_.dwProcessId);
        }

        // Wait for grace period.
        DWORD grace_ms = static_cast<DWORD>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                spec_.kill_timeout).count());

        DWORD wait_result = ::WaitForSingleObject(
            process_info_.hProcess, grace_ms);

        if (wait_result == WAIT_OBJECT_0) {
            // Child responded to soft kill.
            DWORD raw_code = 0;
            ::GetExitCodeProcess(process_info_.hProcess, &raw_code);
            result_.exit_code = normalize_win32_exit_code(raw_code);
            return false;  // Soft kill was sufficient.
        }

        // Step 2: hard kill (TerminateJobObject).
        if (job_object_ != nullptr) {
            ::TerminateJobObject(job_object_, 1);
        }
        ::WaitForSingleObject(process_info_.hProcess, 5000);

        return true;  // Hard kill was needed.
    }

    /// Close all Win32 handles.
    void cleanup_handles() {
        if (stdout_read_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(stdout_read_);
            stdout_read_ = INVALID_HANDLE_VALUE;
        }
        if (stderr_read_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(stderr_read_);
            stderr_read_ = INVALID_HANDLE_VALUE;
        }
        if (process_info_.hProcess) {
            ::CloseHandle(process_info_.hProcess);
            process_info_.hProcess = nullptr;
        }
        if (process_info_.hThread) {
            ::CloseHandle(process_info_.hThread);
            process_info_.hThread = nullptr;
        }
        if (job_object_ != nullptr) {
            ::CloseHandle(job_object_);
            job_object_ = nullptr;
        }
    }

    ProcessSpec spec_;
    ProcessResult result_;
    OutputCallback callback_;
    PROCESS_INFORMATION process_info_{};
    HANDLE job_object_ = nullptr;
    HANDLE stdout_read_ = INVALID_HANDLE_VALUE;
    HANDLE stderr_read_ = INVALID_HANDLE_VALUE;
    std::atomic<bool> running_{false};
    std::atomic<bool> output_truncated_{false};
    std::chrono::steady_clock::time_point start_time_;
    std::thread stdout_reader_;
    std::thread stderr_reader_;
};

// ── Factory ──────────────────────────────────────────────────────────────

std::unique_ptr<ProcessHandle> create_process_handle() {
    return std::make_unique<Win32ProcessHandle>();
}

}  // namespace kairos::exec

#endif  // _WIN32
