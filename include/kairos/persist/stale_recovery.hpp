/// include/kairos/persist/stale_recovery.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/persist/stale_recovery.hpp — Startup recovery for orphaned runs  ║
// ║                                                                          ║
// ║  When the daemon is killed (SIGKILL, OOM, power loss, systemd timeout)  ║
// ║  any in-flight runs are left with status='RUNNING' in the database.     ║
// ║  On restart, recover_stale_runs() marks them as INTERRUPTED so that:    ║
// ║    - `kairos status` reports correct Active Runs count                   ║
// ║    - KEL `job("x").last_status` returns accurate information            ║
// ║    - max_instances constraints are not permanently exhausted             ║
// ║                                                                          ║
// ║  This function runs BEFORE any engines start, directly on the open DB.  ║
// ║  It does NOT use DBWriter (which hasn't been created yet).              ║
// ║                                                                          ║
// ║  Spec reference: §27.2 (startup), §16.6 (run history)                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <SQLiteCpp/SQLiteCpp.h>
#include <string>

namespace kairos::persist {

/// Result of stale run recovery.
struct StaleRecoveryResult {
    int runs_recovered  = 0;   ///< Number of top-level runs marked INTERRUPTED.
    int jobs_recovered  = 0;   ///< Number of job_runs marked INTERRUPTED.
    int steps_recovered = 0;   ///< Number of step_runs marked INTERRUPTED.
};

/// Scan for and fix orphaned RUNNING/PENDING rows.
///
/// Sets status to 'INTERRUPTED' and end_ts to the current UTC timestamp.
/// The reason string is stored in a new `interrupt_reason` column if it
/// exists, otherwise appended to the target_name field is not modified.
///
/// Call this between database open and engine start.
///
/// @param db      Open database (WAL mode, read-write).
/// @param reason  Human-readable reason (e.g., "daemon restarted").
/// @return        Counts of recovered rows.
StaleRecoveryResult recover_stale_runs(
    SQLite::Database& db,
    const std::string& reason = "daemon restarted — run was interrupted");

}  // namespace kairos::persist
