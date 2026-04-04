/// src/persist/stale_recovery.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  stale_recovery.cpp — Mark orphaned RUNNING/PENDING rows as INTERRUPTED  ║
// ║                                                                          ║
// ║  Called once at startup, before any engines are created.  Operates       ║
// ║  directly on the open database (DBWriter does not exist yet).            ║
// ║                                                                          ║
// ║  The INTERRUPTED status is distinct from CANCELLED (user-initiated)      ║
// ║  and FAILURE (process exited non-zero).  It means "the daemon was       ║
// ║  killed while this run was in progress."                                 ║
// ║                                                                          ║
// ║  All three tables (runs, job_runs, step_runs) are cleaned in a single   ║
// ║  transaction for consistency.                                            ║
// ║                                                                          ║
// ║  Spec reference: §27.2 (startup), §16.6 (run history)                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/stale_recovery.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <ctime>

namespace kairos::persist {

StaleRecoveryResult recover_stale_runs(
    SQLite::Database& db,
    const std::string& reason)
{
    StaleRecoveryResult result;

    // ── Build current UTC timestamp for end_ts ─────────────────────
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char ts_buf[32];
    std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    std::string end_ts(ts_buf);

    try {
        SQLite::Transaction txn(db);

        // ── 1. Mark stale top-level runs ───────────────────────────
        // We set status='INTERRUPTED', populate end_ts, and record
        // the reason in error_message (which exists in the schema).
        // duration_ms is left NULL — we can't know the real duration.
        {
            SQLite::Statement stmt(db,
                "UPDATE runs SET status = 'INTERRUPTED', end_ts = ?, "
                "error_message = ? "
                "WHERE status IN ('RUNNING', 'PENDING')");
            stmt.bind(1, end_ts);
            stmt.bind(2, reason);
            result.runs_recovered = stmt.exec();
        }

        // ── 2. Mark stale job_runs ─────────────────────────────────
        {
            SQLite::Statement stmt(db,
                "UPDATE job_runs SET status = 'INTERRUPTED', end_ts = ?, "
                "error_message = ? "
                "WHERE status IN ('RUNNING', 'PENDING')");
            stmt.bind(1, end_ts);
            stmt.bind(2, reason);
            result.jobs_recovered = stmt.exec();
        }

        // ── 3. Mark stale step_runs ────────────────────────────────
        {
            SQLite::Statement stmt(db,
                "UPDATE step_runs SET status = 'INTERRUPTED', end_ts = ?, "
                "error_message = ? "
                "WHERE status IN ('RUNNING', 'PENDING')");
            stmt.bind(1, end_ts);
            stmt.bind(2, reason);
            result.steps_recovered = stmt.exec();
        }

        txn.commit();
    } catch (const std::exception& e) {
        spdlog::error("Stale run recovery failed: {}", e.what());
        return result;  // Partial results — non-fatal.
    }

    if (result.runs_recovered > 0) {
        spdlog::warn("Stale run recovery: marked {} run(s), {} job(s), "
                     "{} step(s) as INTERRUPTED ({})",
                     result.runs_recovered, result.jobs_recovered,
                     result.steps_recovered, reason);
    } else {
        spdlog::debug("Stale run recovery: no orphaned runs found");
    }

    return result;
}

}  // namespace kairos::persist
