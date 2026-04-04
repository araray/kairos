/// tests/unit/persist/stale_recovery_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  stale_recovery_test.cpp — Unit tests for startup stale run recovery     ║
// ║                                                                          ║
// ║  Verifies that recover_stale_runs() correctly marks orphaned             ║
// ║  RUNNING/PENDING rows as INTERRUPTED across runs, job_runs, and         ║
// ║  step_runs tables.                                                       ║
// ║                                                                          ║
// ║  Spec reference: §27.2 (startup), §16.6 (run history)                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/stale_recovery.hpp"
#include "kairos/persist/migration.hpp"

#include <gtest/gtest.h>

namespace kairos::persist {
namespace {

/// Test fixture: in-memory DB with all migrations applied.
class StaleRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = std::make_unique<SQLite::Database>(":memory:",
            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        apply_migrations(*db_, get_migrations());
    }

    /// Insert a run with the given status.
    void insert_run(const std::string& run_id,
                    const std::string& status) {
        SQLite::Statement stmt(*db_,
            "INSERT INTO runs (run_id, target_type, target_id, target_name, "
            "trigger_type, trigger_id, correlation_id, status, start_ts) "
            "VALUES (?, 'job', 'job-abc', 'test-job', 'schedule', "
            "'trg-1', 'corr-1', ?, '2026-04-03T02:55:00Z')");
        stmt.bind(1, run_id);
        stmt.bind(2, status);
        stmt.exec();
    }

    /// Insert a job_run with the given status.
    void insert_job_run(const std::string& run_id,
                        const std::string& job_id,
                        const std::string& status) {
        SQLite::Statement stmt(*db_,
            "INSERT INTO job_runs (run_id, job_id, job_name, status, "
            "start_ts, condition_result) "
            "VALUES (?, ?, 'test-job', ?, '2026-04-03T02:55:00Z', 'true')");
        stmt.bind(1, run_id);
        stmt.bind(2, job_id);
        stmt.bind(3, status);
        stmt.exec();
    }

    /// Insert a step_run with the given status.
    void insert_step_run(const std::string& run_id,
                         const std::string& job_id,
                         const std::string& step_id,
                         const std::string& status) {
        SQLite::Statement stmt(*db_,
            "INSERT INTO step_runs (run_id, job_id, step_id, step_name, "
            "status, start_ts, command) "
            "VALUES (?, ?, ?, 'step-1', ?, '2026-04-03T02:55:00Z', "
            "'echo hello')");
        stmt.bind(1, run_id);
        stmt.bind(2, job_id);
        stmt.bind(3, step_id);
        stmt.bind(4, status);
        stmt.exec();
    }

    /// Query a run's status by run_id.
    std::string get_run_status(const std::string& run_id) {
        SQLite::Statement q(*db_,
            "SELECT status FROM runs WHERE run_id = ?");
        q.bind(1, run_id);
        if (q.executeStep()) return q.getColumn(0).getString();
        return "";
    }

    /// Query a run's error_message by run_id.
    std::string get_run_error(const std::string& run_id) {
        SQLite::Statement q(*db_,
            "SELECT error_message FROM runs WHERE run_id = ?");
        q.bind(1, run_id);
        if (q.executeStep()) {
            auto col = q.getColumn(0);
            return col.isNull() ? "" : col.getString();
        }
        return "";
    }

    /// Query a run's end_ts by run_id.
    std::string get_run_end_ts(const std::string& run_id) {
        SQLite::Statement q(*db_,
            "SELECT end_ts FROM runs WHERE run_id = ?");
        q.bind(1, run_id);
        if (q.executeStep()) {
            auto col = q.getColumn(0);
            return col.isNull() ? "" : col.getString();
        }
        return "";
    }

    /// Query a job_run's status.
    std::string get_job_run_status(const std::string& run_id,
                                   const std::string& job_id) {
        SQLite::Statement q(*db_,
            "SELECT status FROM job_runs WHERE run_id = ? AND job_id = ?");
        q.bind(1, run_id);
        q.bind(2, job_id);
        if (q.executeStep()) return q.getColumn(0).getString();
        return "";
    }

    /// Query a step_run's status.
    std::string get_step_run_status(const std::string& run_id,
                                    const std::string& job_id,
                                    const std::string& step_id) {
        SQLite::Statement q(*db_,
            "SELECT status FROM step_runs "
            "WHERE run_id = ? AND job_id = ? AND step_id = ?");
        q.bind(1, run_id);
        q.bind(2, job_id);
        q.bind(3, step_id);
        if (q.executeStep()) return q.getColumn(0).getString();
        return "";
    }

    /// Count runs with a given status.
    int count_runs_with_status(const std::string& status) {
        SQLite::Statement q(*db_,
            "SELECT COUNT(*) FROM runs WHERE status = ?");
        q.bind(1, status);
        q.executeStep();
        return q.getColumn(0).getInt();
    }

    std::unique_ptr<SQLite::Database> db_;
};

// ── Basic recovery ──────────────────────────────────────────────────────

TEST_F(StaleRecoveryTest, RecoversSingleRunningRun) {
    insert_run("run-001", "RUNNING");
    insert_job_run("run-001", "job-a", "RUNNING");
    insert_step_run("run-001", "job-a", "step-1", "RUNNING");

    auto result = recover_stale_runs(*db_);

    EXPECT_EQ(result.runs_recovered, 1);
    EXPECT_EQ(result.jobs_recovered, 1);
    EXPECT_EQ(result.steps_recovered, 1);

    EXPECT_EQ(get_run_status("run-001"), "INTERRUPTED");
    EXPECT_EQ(get_job_run_status("run-001", "job-a"), "INTERRUPTED");
    EXPECT_EQ(get_step_run_status("run-001", "job-a", "step-1"),
              "INTERRUPTED");
}

TEST_F(StaleRecoveryTest, RecoversPendingRuns) {
    insert_run("run-002", "PENDING");

    auto result = recover_stale_runs(*db_);

    EXPECT_EQ(result.runs_recovered, 1);
    EXPECT_EQ(get_run_status("run-002"), "INTERRUPTED");
}

TEST_F(StaleRecoveryTest, SetsEndTsAndErrorMessage) {
    insert_run("run-003", "RUNNING");

    auto result = recover_stale_runs(*db_, "test recovery reason");

    EXPECT_EQ(result.runs_recovered, 1);
    EXPECT_EQ(get_run_error("run-003"), "test recovery reason");

    // end_ts should be set (non-empty ISO-8601 timestamp).
    std::string end_ts = get_run_end_ts("run-003");
    EXPECT_FALSE(end_ts.empty());
    // Basic format check: starts with "20" and contains "T".
    EXPECT_EQ(end_ts.substr(0, 2), "20");
    EXPECT_NE(end_ts.find('T'), std::string::npos);
}

// ── Does not touch completed runs ───────────────────────────────────────

TEST_F(StaleRecoveryTest, DoesNotTouchSuccessfulRuns) {
    insert_run("run-ok", "SUCCESS");
    insert_run("run-fail", "FAILURE");
    insert_run("run-cancel", "CANCELLED");
    insert_run("run-stuck", "RUNNING");

    auto result = recover_stale_runs(*db_);

    // Only the RUNNING one should be recovered.
    EXPECT_EQ(result.runs_recovered, 1);

    EXPECT_EQ(get_run_status("run-ok"), "SUCCESS");
    EXPECT_EQ(get_run_status("run-fail"), "FAILURE");
    EXPECT_EQ(get_run_status("run-cancel"), "CANCELLED");
    EXPECT_EQ(get_run_status("run-stuck"), "INTERRUPTED");
}

// ── Multiple stale runs ────────────────────────────────────────────────

TEST_F(StaleRecoveryTest, RecoversMultipleStaleRuns) {
    insert_run("run-a", "RUNNING");
    insert_run("run-b", "RUNNING");
    insert_run("run-c", "PENDING");

    insert_job_run("run-a", "job-1", "RUNNING");
    insert_job_run("run-a", "job-2", "PENDING");
    insert_job_run("run-b", "job-3", "RUNNING");

    auto result = recover_stale_runs(*db_);

    EXPECT_EQ(result.runs_recovered, 3);
    EXPECT_EQ(result.jobs_recovered, 3);

    EXPECT_EQ(count_runs_with_status("RUNNING"), 0);
    EXPECT_EQ(count_runs_with_status("PENDING"), 0);
    EXPECT_EQ(count_runs_with_status("INTERRUPTED"), 3);
}

// ── Idempotent — second call is a no-op ─────────────────────────────────

TEST_F(StaleRecoveryTest, IdempotentOnSecondCall) {
    insert_run("run-x", "RUNNING");
    insert_job_run("run-x", "job-1", "RUNNING");

    auto first = recover_stale_runs(*db_);
    EXPECT_EQ(first.runs_recovered, 1);
    EXPECT_EQ(first.jobs_recovered, 1);

    auto second = recover_stale_runs(*db_);
    EXPECT_EQ(second.runs_recovered, 0);
    EXPECT_EQ(second.jobs_recovered, 0);
    EXPECT_EQ(second.steps_recovered, 0);

    // Status should still be INTERRUPTED.
    EXPECT_EQ(get_run_status("run-x"), "INTERRUPTED");
}

// ── Empty database — no-op ──────────────────────────────────────────────

TEST_F(StaleRecoveryTest, NoOpOnEmptyDatabase) {
    auto result = recover_stale_runs(*db_);

    EXPECT_EQ(result.runs_recovered, 0);
    EXPECT_EQ(result.jobs_recovered, 0);
    EXPECT_EQ(result.steps_recovered, 0);
}

// ── Mixed: only RUNNING/PENDING affected, others untouched ─────────────

TEST_F(StaleRecoveryTest, MixedStatusesOnlyStaleAffected) {
    insert_run("run-success", "SUCCESS");
    insert_run("run-running", "RUNNING");

    insert_job_run("run-success", "job-done", "SUCCESS");
    insert_job_run("run-running", "job-active", "RUNNING");
    insert_job_run("run-running", "job-pending", "PENDING");
    insert_job_run("run-running", "job-skipped", "SKIPPED");

    insert_step_run("run-running", "job-active", "step-ok", "SUCCESS");
    insert_step_run("run-running", "job-active", "step-stuck", "RUNNING");

    auto result = recover_stale_runs(*db_);

    EXPECT_EQ(result.runs_recovered, 1);   // run-running
    EXPECT_EQ(result.jobs_recovered, 2);   // job-active, job-pending
    EXPECT_EQ(result.steps_recovered, 1);  // step-stuck

    // Untouched rows.
    EXPECT_EQ(get_run_status("run-success"), "SUCCESS");
    EXPECT_EQ(get_job_run_status("run-success", "job-done"), "SUCCESS");
    EXPECT_EQ(get_job_run_status("run-running", "job-skipped"), "SKIPPED");
    EXPECT_EQ(get_step_run_status("run-running", "job-active", "step-ok"),
              "SUCCESS");

    // Recovered rows.
    EXPECT_EQ(get_run_status("run-running"), "INTERRUPTED");
    EXPECT_EQ(get_job_run_status("run-running", "job-active"), "INTERRUPTED");
    EXPECT_EQ(get_job_run_status("run-running", "job-pending"), "INTERRUPTED");
    EXPECT_EQ(get_step_run_status("run-running", "job-active", "step-stuck"),
              "INTERRUPTED");
}

}  // namespace
}  // namespace kairos::persist
