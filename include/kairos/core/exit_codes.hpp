/// include/kairos/core/exit_codes.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/core/exit_codes.hpp — Process and daemon exit code semantics      ║
// ║                                                                           ║
// ║  Standard POSIX exit codes 0–127 retain their usual meaning.              ║
// ║  Kairos reserves 200–204 for orchestration-specific conditions.           ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <string_view>

namespace kairos {

/// Standard exit codes used by the kairos daemon and CLI.
enum class ExitCode : int {
    // ── Success ───────────────────────────────────────────────────────
    kSuccess             = 0,

    // ── Generic failure ───────────────────────────────────────────────
    kGenericError        = 1,

    // ── Configuration errors ──────────────────────────────────────────
    kConfigError         = 2,    // Missing/invalid config or mandatory key

    // ── CLI-specific (§23.3) ───────────────────────────────────────
    kNotFound            = 4,    // Workflow/job/run ID not found
    kAlreadyRunning      = 5,    // Daemon already started
    kNotRunning          = 6,    // Daemon not started (for commands requiring it)

    // ── Standard POSIX conventions ────────────────────────────────────
    kCommandNotExecutable = 126,  // Permission denied on command
    kCommandNotFound      = 127,  // Missing interpreter or binary

    // ── Signal-based termination (128 + signal number) ────────────────
    // e.g., 137 = 128 + 9 (SIGKILL)

    // ── Kairos-specific (200–204) ─────────────────────────────────────
    kTimeoutSoft         = 200,  // Timeout exceeded (SIGTERM sent)
    kTimeoutHard         = 201,  // Timeout exceeded (SIGKILL sent)
    kRunnerConfigError   = 202,  // Runner/executor misconfiguration
    kDependencyFailure   = 203,  // Upstream job failed
    kConditionError      = 204,  // KEL condition evaluation error
};

/// Human-readable description for an exit code.
constexpr std::string_view exit_code_description(int code) {
    switch (code) {
        case 0:   return "Success";
        case 1:   return "Generic error";
        case 2:   return "Configuration error";
        case 4:   return "Not found";
        case 5:   return "Already running";
        case 6:   return "Not running";
        case 126: return "Command not executable (permissions)";
        case 127: return "Command not found";
        case 200: return "Timeout exceeded (soft kill)";
        case 201: return "Timeout exceeded (hard kill)";
        case 202: return "Runner configuration error";
        case 203: return "Dependency failure (upstream job failed)";
        case 204: return "Condition evaluation error";
        default:
            if (code >= 128 && code < 200)
                return "Killed by signal";
            return "Unknown exit code";
    }
}

}  // namespace kairos
