/// tests/unit/persist/query_reader_cli_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for QueryReader CLI query methods                                  ║
// ║  Deliverable 12.2–12.4: query_recent_runs, get_run_detail,              ║
// ║                          get_log_chunks, get_run_summary,                ║
// ║                          query_metrics_snapshots                         ║
// ║  Spec reference: §16.6, §23.2, §23.8, §20.5                           ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/query_reader.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/migration.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace kairos::persist::test {

// ── Test fixture ──────────────────────────────────────────────────────────

class QueryReaderCliTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = fs::temp_directory_path() /
                   ("kairos_qr_cli_test_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this)) +
                    ".db");
        db_ = open_database(db_path_);
        init_database(db_path_);
    }

    void TearDown() override {
        db_.reset();
        std::error_code ec;
        fs::remove(db_path_, ec);
        // Also remove WAL/SHM files.
        fs::remove(fs::path(db_path_.string() + "-wal"), ec);
        fs::remove(fs::path(db_path_.string() + "-shm"), ec);
    }

    /// Insert a run record directly.
    void insert_run(const std::string& run_id,
                    const std::string& target_name,
                    const std::string& trigger_type,
                    const std::string& status,
                    const std::string& start_ts,
                    const std::string& end_ts = "",
                    int64_t duration_ms = 0,
                    int exit_code = 0) {
        SQLite::Statement stmt(*db_,
            "INSERT INTO runs (run_id, target_type, target_id, target_name, "
            "trigger_type, trigger_id, correlation_id, status, exit_code, "
            "start_ts, end_ts, duration_ms) "
            "VALUES (?, 'workflow', ?, ?, ?, 'trg-1', 'cor-1', ?, ?, ?, ?, ?)");
        stmt.bind(1, run_id);
        stmt.bind(2, "wfl-" + target_name);
        stmt.bind(3, target_name);
        stmt.bind(4, trigger_type);
        stmt.bind(5, status);
        stmt.bind(6, exit_code);
        stmt.bind(7, start_ts);
        stmt.bind(8, end_ts.empty() ? nullptr : end_ts.c_str());
        stmt.bind(9, duration_ms);
        stmt.exec();
    }

    /// Insert a job_run record directly.
    void insert_job_run(const std::string& run_id,
                        const std::string& job_id,
                        const std::string& job_name,
                        const std::string& status,
                        int exit_code = 0,
                        int64_t duration_ms = 0) {
        SQLite::Statement stmt(*db_,
            "INSERT INTO job_runs (run_id, job_id, job_name, status, "
            "exit_code, start_ts, end_ts, duration_ms) "
            "VALUES (?, ?, ?, ?, ?, "
            "strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
            "strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), ?)");
        stmt.bind(1, run_id);
        stmt.bind(2, job_id);
        stmt.bind(3, job_name);
        stmt.bind(4, status);
        stmt.bind(5, exit_code);
        stmt.bind(6, duration_ms);
        stmt.exec();
    }

    /// Insert a step_run record directly.
    void insert_step_run(const std::string& run_id,
                         const std::string& job_id,
                         const std::string& step_id,
                         const std::string& step_name,
                         const std::string& status,
                         int exit_code = 0) {
        SQLite::Statement stmt(*db_,
            "INSERT INTO step_runs (run_id, job_id, step_id, step_name, "
            "command, status, exit_code, start_ts, end_ts, duration_ms) "
            "VALUES (?, ?, ?, ?, 'echo hello', ?, ?, "
            "strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
            "strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), 100)");
        stmt.bind(1, run_id);
        stmt.bind(2, job_id);
        stmt.bind(3, step_id);
        stmt.bind(4, step_name);
        stmt.bind(5, status);
        stmt.bind(6, exit_code);
        stmt.exec();
    }

    /// Insert a log_chunk record directly.
    void insert_log_chunk(const std::string& run_id,
                          const std::string& job_id,
                          const std::string& step_id,
                          const std::string& stream,
                          int64_t chunk_index,
                          const std::string& content) {
        SQLite::Statement stmt(*db_,
            "INSERT INTO log_chunks (run_id, job_id, step_id, stream, "
            "chunk_index, content) VALUES (?, ?, ?, ?, ?, ?)");
        stmt.bind(1, run_id);
        stmt.bind(2, job_id);
        stmt.bind(3, step_id);
        stmt.bind(4, stream);
        stmt.bind(5, chunk_index);
        stmt.bind(6, content);
        stmt.exec();
    }

    /// Insert a metrics_snapshot record directly.
    void insert_metrics_snapshot(const std::string& name,
                                const std::string& type,
                                double value,
                                const std::string& labels = "") {
        SQLite::Statement stmt(*db_,
            "INSERT INTO metrics_snapshots (metric_name, metric_type, "
            "value, labels_json) VALUES (?, ?, ?, ?)");
        stmt.bind(1, name);
        stmt.bind(2, type);
        stmt.bind(3, value);
        stmt.bind(4, labels);
        stmt.exec();
    }

    fs::path db_path_;
    std::unique_ptr<SQLite::Database> db_;
};

// ═══════════════════════════════════════════════════════════════════════════
// query_recent_runs
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(QueryReaderCliTest, RecentRuns_EmptyDB) {
    QueryReader reader(*db_);
    auto runs = reader.query_recent_runs();
    EXPECT_TRUE(runs.empty());
}

TEST_F(QueryReaderCliTest, RecentRuns_ReturnsOrderedByStartDesc) {
    insert_run("run-001", "deploy", "schedule", "SUCCESS",
               "2026-03-01T10:00:00Z", "2026-03-01T10:00:05Z", 5000);
    insert_run("run-002", "backup", "schedule", "FAILURE",
               "2026-03-02T10:00:00Z", "2026-03-02T10:01:00Z", 60000, 1);
    insert_run("run-003", "deploy", "manual", "SUCCESS",
               "2026-03-03T10:00:00Z", "2026-03-03T10:00:03Z", 3000);

    QueryReader reader(*db_);
    auto runs = reader.query_recent_runs();

    ASSERT_EQ(runs.size(), 3u);
    EXPECT_EQ(runs[0].run_id, "run-003");  // Most recent first.
    EXPECT_EQ(runs[1].run_id, "run-002");
    EXPECT_EQ(runs[2].run_id, "run-001");
}

TEST_F(QueryReaderCliTest, RecentRuns_LimitWorks) {
    for (int i = 0; i < 10; ++i) {
        insert_run("run-" + std::to_string(i), "wf", "schedule", "SUCCESS",
                    "2026-03-0" + std::to_string(i+1) + "T10:00:00Z");
    }

    QueryReader reader(*db_);
    auto runs = reader.query_recent_runs(3);
    ASSERT_EQ(runs.size(), 3u);
}

TEST_F(QueryReaderCliTest, RecentRuns_StatusFilter) {
    insert_run("run-001", "deploy", "schedule", "SUCCESS",
               "2026-03-01T10:00:00Z");
    insert_run("run-002", "deploy", "schedule", "FAILURE",
               "2026-03-02T10:00:00Z");
    insert_run("run-003", "deploy", "schedule", "SUCCESS",
               "2026-03-03T10:00:00Z");

    QueryReader reader(*db_);
    auto runs = reader.query_recent_runs(20, "FAILURE");

    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0].run_id, "run-002");
    EXPECT_EQ(runs[0].status, "FAILURE");
}

TEST_F(QueryReaderCliTest, RecentRuns_TargetFilter) {
    insert_run("run-001", "deploy", "schedule", "SUCCESS",
               "2026-03-01T10:00:00Z");
    insert_run("run-002", "backup", "schedule", "SUCCESS",
               "2026-03-02T10:00:00Z");

    QueryReader reader(*db_);
    auto runs = reader.query_recent_runs(20, "", "backup");

    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0].target_name, "backup");
}

TEST_F(QueryReaderCliTest, RecentRuns_SinceFilter) {
    insert_run("run-001", "deploy", "schedule", "SUCCESS",
               "2026-01-01T10:00:00Z");
    insert_run("run-002", "deploy", "schedule", "SUCCESS",
               "2026-03-01T10:00:00Z");

    QueryReader reader(*db_);
    auto runs = reader.query_recent_runs(20, "", "", "2026-02-01T00:00:00Z");

    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0].run_id, "run-002");
}

TEST_F(QueryReaderCliTest, RecentRuns_FieldsPopulated) {
    insert_run("run-001", "deploy", "manual", "SUCCESS",
               "2026-03-01T10:00:00Z", "2026-03-01T10:00:05Z", 5000, 0);

    QueryReader reader(*db_);
    auto runs = reader.query_recent_runs();

    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0].run_id, "run-001");
    EXPECT_EQ(runs[0].target_type, "workflow");
    EXPECT_EQ(runs[0].target_name, "deploy");
    EXPECT_EQ(runs[0].trigger_type, "manual");
    EXPECT_EQ(runs[0].status, "SUCCESS");
    EXPECT_EQ(runs[0].exit_code, 0);
    EXPECT_EQ(runs[0].duration_ms, 5000);
}

// ═══════════════════════════════════════════════════════════════════════════
// get_run_summary
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(QueryReaderCliTest, GetRunSummary_Found) {
    insert_run("run-001", "deploy", "manual", "SUCCESS",
               "2026-03-01T10:00:00Z");

    QueryReader reader(*db_);
    auto opt = reader.get_run_summary("run-001");

    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->run_id, "run-001");
    EXPECT_EQ(opt->target_name, "deploy");
}

TEST_F(QueryReaderCliTest, GetRunSummary_NotFound) {
    QueryReader reader(*db_);
    auto opt = reader.get_run_summary("nonexistent");
    EXPECT_FALSE(opt.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// get_run_detail
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(QueryReaderCliTest, GetRunDetail_NotFound) {
    QueryReader reader(*db_);
    auto opt = reader.get_run_detail("nonexistent");
    EXPECT_FALSE(opt.has_value());
}

TEST_F(QueryReaderCliTest, GetRunDetail_WithJobsAndSteps) {
    insert_run("run-001", "deploy", "schedule", "SUCCESS",
               "2026-03-01T10:00:00Z", "2026-03-01T10:00:10Z", 10000);
    insert_job_run("run-001", "job-build", "build", "SUCCESS", 0, 5000);
    insert_job_run("run-001", "job-test", "test", "SUCCESS", 0, 3000);
    insert_step_run("run-001", "job-build", "stp-1", "compile", "SUCCESS");
    insert_step_run("run-001", "job-build", "stp-2", "link", "SUCCESS");
    insert_step_run("run-001", "job-test", "stp-3", "run-tests", "SUCCESS");

    QueryReader reader(*db_);
    auto detail = reader.get_run_detail("run-001");

    ASSERT_TRUE(detail.has_value());
    EXPECT_EQ(detail->run.run_id, "run-001");
    EXPECT_EQ(detail->run.status, "SUCCESS");

    ASSERT_EQ(detail->jobs.size(), 2u);
    EXPECT_EQ(detail->jobs[0].job_name, "build");
    EXPECT_EQ(detail->jobs[1].job_name, "test");

    EXPECT_EQ(detail->jobs[0].steps.size(), 2u);
    EXPECT_EQ(detail->jobs[1].steps.size(), 1u);

    EXPECT_EQ(detail->jobs[0].steps[0].step_name, "compile");
    EXPECT_EQ(detail->jobs[0].steps[1].step_name, "link");
}

TEST_F(QueryReaderCliTest, GetRunDetail_NoJobs) {
    insert_run("run-001", "deploy", "manual", "SUCCESS",
               "2026-03-01T10:00:00Z");

    QueryReader reader(*db_);
    auto detail = reader.get_run_detail("run-001");

    ASSERT_TRUE(detail.has_value());
    EXPECT_EQ(detail->run.run_id, "run-001");
    EXPECT_TRUE(detail->jobs.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// get_log_chunks
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(QueryReaderCliTest, GetLogChunks_Empty) {
    QueryReader reader(*db_);
    auto chunks = reader.get_log_chunks("run-nonexistent");
    EXPECT_TRUE(chunks.empty());
}

TEST_F(QueryReaderCliTest, GetLogChunks_OrderedById) {
    insert_log_chunk("run-001", "job-1", "stp-1", "stdout", 0, "line 1\n");
    insert_log_chunk("run-001", "job-1", "stp-1", "stdout", 1, "line 2\n");
    insert_log_chunk("run-001", "job-1", "stp-1", "stderr", 0, "err 1\n");

    QueryReader reader(*db_);
    auto chunks = reader.get_log_chunks("run-001");

    ASSERT_EQ(chunks.size(), 3u);
    EXPECT_EQ(chunks[0].content, "line 1\n");
    EXPECT_EQ(chunks[1].content, "line 2\n");
    EXPECT_EQ(chunks[2].content, "err 1\n");
}

TEST_F(QueryReaderCliTest, GetLogChunks_CursorPagination) {
    insert_log_chunk("run-001", "j", "s", "stdout", 0, "chunk-a\n");
    insert_log_chunk("run-001", "j", "s", "stdout", 1, "chunk-b\n");
    insert_log_chunk("run-001", "j", "s", "stdout", 2, "chunk-c\n");

    QueryReader reader(*db_);

    // First page.
    auto page1 = reader.get_log_chunks("run-001", 0, 2);
    ASSERT_EQ(page1.size(), 2u);
    EXPECT_EQ(page1[0].content, "chunk-a\n");
    EXPECT_EQ(page1[1].content, "chunk-b\n");

    // Second page using cursor from last item.
    auto cursor = page1.back().id;
    auto page2 = reader.get_log_chunks("run-001", cursor, 2);
    ASSERT_EQ(page2.size(), 1u);
    EXPECT_EQ(page2[0].content, "chunk-c\n");
}

TEST_F(QueryReaderCliTest, GetLogChunks_FieldsPopulated) {
    insert_log_chunk("run-001", "job-a", "stp-x", "stderr", 5, "err msg");

    QueryReader reader(*db_);
    auto chunks = reader.get_log_chunks("run-001");

    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].run_id, "run-001");
    EXPECT_EQ(chunks[0].job_id, "job-a");
    EXPECT_EQ(chunks[0].step_id, "stp-x");
    EXPECT_EQ(chunks[0].stream, "stderr");
    EXPECT_EQ(chunks[0].chunk_index, 5);
    EXPECT_EQ(chunks[0].content, "err msg");
    EXPECT_FALSE(chunks[0].created_at.empty());
    EXPECT_GT(chunks[0].id, 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// query_metrics_snapshots
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(QueryReaderCliTest, MetricsSnapshots_Empty) {
    QueryReader reader(*db_);
    auto rows = reader.query_metrics_snapshots();
    EXPECT_TRUE(rows.empty());
}

TEST_F(QueryReaderCliTest, MetricsSnapshots_ReturnsEntries) {
    insert_metrics_snapshot("kairos_uptime_seconds", "gauge", 42.5);
    insert_metrics_snapshot("kairos_runs_total", "counter", 100.0,
                            R"({"status":"success"})");

    QueryReader reader(*db_);
    auto rows = reader.query_metrics_snapshots();

    ASSERT_EQ(rows.size(), 2u);
    // Ordered by created_at DESC, metric_name ASC.
    // Since both are inserted at same time, alphabetical order.
    EXPECT_EQ(rows[0].metric_name, "kairos_runs_total");
    EXPECT_EQ(rows[0].metric_type, "counter");
    EXPECT_DOUBLE_EQ(rows[0].value, 100.0);

    EXPECT_EQ(rows[1].metric_name, "kairos_uptime_seconds");
    EXPECT_EQ(rows[1].metric_type, "gauge");
    EXPECT_DOUBLE_EQ(rows[1].value, 42.5);
}

TEST_F(QueryReaderCliTest, MetricsSnapshots_LimitWorks) {
    // Insert snapshots with different timestamps.
    db_->exec(
        "INSERT INTO metrics_snapshots (metric_name, metric_type, value, "
        "created_at) VALUES "
        "('m1', 'gauge', 1.0, '2026-03-01T10:00:00Z'),"
        "('m2', 'gauge', 2.0, '2026-03-01T10:01:00Z'),"
        "('m3', 'gauge', 3.0, '2026-03-01T10:02:00Z')");

    QueryReader reader(*db_);
    // Limit to 2 most recent distinct epochs.
    auto rows = reader.query_metrics_snapshots(2);
    ASSERT_EQ(rows.size(), 2u);
    // Should be m3 and m2 (most recent two).
    EXPECT_EQ(rows[0].metric_name, "m3");
    EXPECT_EQ(rows[1].metric_name, "m2");
}

}  // namespace kairos::persist::test
