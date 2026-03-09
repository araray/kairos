/// tests/unit/persist/runs_cancel_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for the SQL operations used by `kairos runs cancel`              ║
// ║                                                                          ║
// ║  Phase 3 Batch 15: Cancel marks RUNNING/PENDING runs + jobs as          ║
// ║  CANCELLED with end_ts set. Completed runs are not affected.           ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/query_reader.hpp"
#include "kairos/persist/database.hpp"

#include <gtest/gtest.h>
#include <SQLiteCpp/SQLiteCpp.h>

#include <memory>
#include <string>

namespace kairos::persist::test {

namespace {

class RunsCancelTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = std::make_unique<SQLite::Database>(
            ":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db_->exec("PRAGMA journal_mode = WAL");
        db_->exec("PRAGMA foreign_keys = ON");

        db_->exec(R"(
            CREATE TABLE runs (
                run_id TEXT PRIMARY KEY,
                target_type TEXT NOT NULL,
                target_id TEXT NOT NULL,
                target_name TEXT NOT NULL,
                trigger_type TEXT NOT NULL,
                trigger_id TEXT DEFAULT '',
                correlation_id TEXT DEFAULT '',
                status TEXT NOT NULL,
                exit_code INTEGER DEFAULT 0,
                start_ts TEXT NOT NULL,
                end_ts TEXT DEFAULT '',
                duration_ms INTEGER DEFAULT 0,
                plan_json TEXT DEFAULT '',
                error_message TEXT DEFAULT '',
                created_at TEXT DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
            )
        )");
        db_->exec(R"(
            CREATE TABLE job_runs (
                run_id TEXT NOT NULL,
                job_id TEXT NOT NULL,
                job_name TEXT DEFAULT '',
                status TEXT NOT NULL,
                start_ts TEXT DEFAULT '',
                end_ts TEXT DEFAULT '',
                exit_code INTEGER DEFAULT 0,
                duration_ms INTEGER DEFAULT 0,
                condition_result TEXT DEFAULT '',
                PRIMARY KEY (run_id, job_id)
            )
        )");
    }

    void insert_run(const std::string& run_id,
                    const std::string& name,
                    const std::string& status) {
        SQLite::Statement stmt(*db_,
            "INSERT INTO runs (run_id, target_type, target_id, target_name, "
            "trigger_type, status, start_ts) VALUES (?, 'workflow', 'tid', "
            "?, 'manual', ?, '2026-01-01T00:00:00Z')");
        stmt.bind(1, run_id);
        stmt.bind(2, name);
        stmt.bind(3, status);
        stmt.exec();
    }

    void insert_job_run(const std::string& run_id,
                        const std::string& job_id,
                        const std::string& status) {
        SQLite::Statement stmt(*db_,
            "INSERT INTO job_runs (run_id, job_id, job_name, status) "
            "VALUES (?, ?, ?, ?)");
        stmt.bind(1, run_id);
        stmt.bind(2, job_id);
        stmt.bind(3, job_id);
        stmt.bind(4, status);
        stmt.exec();
    }

    std::string get_status(const std::string& run_id) {
        SQLite::Statement stmt(*db_,
            "SELECT status FROM runs WHERE run_id = ?");
        stmt.bind(1, run_id);
        if (stmt.executeStep()) return stmt.getColumn(0).getString();
        return "";
    }

    std::string get_job_status(const std::string& run_id,
                               const std::string& job_id) {
        SQLite::Statement stmt(*db_,
            "SELECT status FROM job_runs WHERE run_id = ? AND job_id = ?");
        stmt.bind(1, run_id);
        stmt.bind(2, job_id);
        if (stmt.executeStep()) return stmt.getColumn(0).getString();
        return "";
    }

    bool has_end_ts(const std::string& run_id) {
        SQLite::Statement stmt(*db_,
            "SELECT end_ts FROM runs WHERE run_id = ?");
        stmt.bind(1, run_id);
        if (stmt.executeStep()) {
            return !stmt.getColumn(0).getString().empty();
        }
        return false;
    }

    /// Execute the same SQL the CLI `runs cancel` uses.
    int cancel_run(const std::string& run_id) {
        SQLite::Statement stmt(*db_,
            "UPDATE runs SET status = 'CANCELLED', "
            "end_ts = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
            "WHERE run_id = ? AND status IN ('RUNNING', 'PENDING')");
        stmt.bind(1, run_id);
        return stmt.exec();
    }

    int cancel_run_jobs(const std::string& run_id) {
        SQLite::Statement stmt(*db_,
            "UPDATE job_runs SET status = 'CANCELLED', "
            "end_ts = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
            "WHERE run_id = ? AND status IN ('RUNNING', 'PENDING')");
        stmt.bind(1, run_id);
        return stmt.exec();
    }

    std::unique_ptr<SQLite::Database> db_;
};

// ── Cancel a running run ───────────────────────────────────────────────

TEST_F(RunsCancelTest, CancelRunningRun) {
    insert_run("r1", "build", "RUNNING");
    insert_job_run("r1", "j1", "RUNNING");
    insert_job_run("r1", "j2", "PENDING");

    int updated = cancel_run("r1");
    EXPECT_EQ(updated, 1);
    EXPECT_EQ(get_status("r1"), "CANCELLED");
    EXPECT_TRUE(has_end_ts("r1"));

    int jobs_updated = cancel_run_jobs("r1");
    EXPECT_EQ(jobs_updated, 2);
    EXPECT_EQ(get_job_status("r1", "j1"), "CANCELLED");
    EXPECT_EQ(get_job_status("r1", "j2"), "CANCELLED");
}

// ── Cancel a pending run ───────────────────────────────────────────────

TEST_F(RunsCancelTest, CancelPendingRun) {
    insert_run("r2", "deploy", "PENDING");

    int updated = cancel_run("r2");
    EXPECT_EQ(updated, 1);
    EXPECT_EQ(get_status("r2"), "CANCELLED");
}

// ── Cancel a completed run does nothing ────────────────────────────────

TEST_F(RunsCancelTest, CancelCompletedRunNoOp) {
    insert_run("r3", "test", "SUCCESS");

    int updated = cancel_run("r3");
    EXPECT_EQ(updated, 0);
    EXPECT_EQ(get_status("r3"), "SUCCESS");
}

// ── Cancel a failed run does nothing ───────────────────────────────────

TEST_F(RunsCancelTest, CancelFailedRunNoOp) {
    insert_run("r4", "lint", "FAILURE");

    int updated = cancel_run("r4");
    EXPECT_EQ(updated, 0);
    EXPECT_EQ(get_status("r4"), "FAILURE");
}

// ── Cancel non-existent run returns 0 ──────────────────────────────────

TEST_F(RunsCancelTest, CancelNonexistentRunNoOp) {
    int updated = cancel_run("does_not_exist");
    EXPECT_EQ(updated, 0);
}

// ── Mixed job statuses: only active jobs cancelled ─────────────────────

TEST_F(RunsCancelTest, OnlyActiveJobsCancelled) {
    insert_run("r5", "multi", "RUNNING");
    insert_job_run("r5", "j1", "SUCCESS");     // Completed — keep.
    insert_job_run("r5", "j2", "RUNNING");     // Active — cancel.
    insert_job_run("r5", "j3", "FAILURE");     // Completed — keep.
    insert_job_run("r5", "j4", "PENDING");     // Active — cancel.

    cancel_run("r5");
    int jobs_cancelled = cancel_run_jobs("r5");

    EXPECT_EQ(jobs_cancelled, 2);
    EXPECT_EQ(get_job_status("r5", "j1"), "SUCCESS");
    EXPECT_EQ(get_job_status("r5", "j2"), "CANCELLED");
    EXPECT_EQ(get_job_status("r5", "j3"), "FAILURE");
    EXPECT_EQ(get_job_status("r5", "j4"), "CANCELLED");
}

// ── Cancel already-cancelled run is idempotent ─────────────────────────

TEST_F(RunsCancelTest, CancelAlreadyCancelledIsNoOp) {
    insert_run("r6", "build", "RUNNING");
    cancel_run("r6");
    EXPECT_EQ(get_status("r6"), "CANCELLED");

    // Cancel again — no rows updated.
    int updated = cancel_run("r6");
    EXPECT_EQ(updated, 0);
    EXPECT_EQ(get_status("r6"), "CANCELLED");
}

// ── QueryReader confirms cancel via get_run_summary ────────────────────

TEST_F(RunsCancelTest, QueryReaderSeesCancelledStatus) {
    insert_run("r7", "deploy", "RUNNING");
    cancel_run("r7");

    QueryReader reader(*db_);
    auto summary = reader.get_run_summary("r7");
    ASSERT_TRUE(summary.has_value());
    EXPECT_EQ(summary->status, "CANCELLED");
    EXPECT_FALSE(summary->end_ts.empty());
}

}  // namespace

}  // namespace kairos::persist::test
