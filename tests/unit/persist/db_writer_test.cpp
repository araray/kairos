/// tests/unit/persist/db_writer_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for DBWriter and QueryReader                                      ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/query_reader.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/kel/evaluator.hpp"
#include "test_helpers.hpp"

#include <gtest/gtest.h>

#include <thread>

using namespace kairos::persist;

// ═══════════════════════════════════════════════════════════════════════════
// Fixture: creates an in-memory database with schema v1
// ═══════════════════════════════════════════════════════════════════════════

class DBWriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = std::make_unique<SQLite::Database>(
            ":memory:",
            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db_->exec("PRAGMA journal_mode=WAL");
        db_->exec("PRAGMA foreign_keys=ON");

        // Apply schema v1 migrations.
        kairos::persist::apply_migrations(*db_, kairos::persist::get_migrations());
    }

    std::unique_ptr<SQLite::Database> db_;
};

// ═══════════════════════════════════════════════════════════════════════════
// DBWriter tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(DBWriterTest, InsertAndQueryRun) {
    DBWriterConfig config;
    config.queue_capacity = 64;
    config.max_batch_size = 16;

    DBWriter writer(*db_, config);
    std::stop_source stop;
    writer.start(stop.get_token());

    // Insert a run.
    writer.enqueue(InsertRun{
        .run_id = "run-001",
        .target_type = "workflow",
        .target_id = "wfl-deploy",
        .target_name = "deploy",
        .trigger_type = "manual",
        .trigger_id = "trg-001",
        .correlation_id = "corr-001",
        .status = "RUNNING",
        .start_ts = "2026-03-07T12:00:00Z",
    });

    // Wait for write to complete.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Query it directly.
    SQLite::Statement stmt(*db_,
        "SELECT run_id, status FROM runs WHERE run_id = ?");
    stmt.bind(1, "run-001");
    ASSERT_TRUE(stmt.executeStep());
    EXPECT_EQ(stmt.getColumn(0).getString(), "run-001");
    EXPECT_EQ(stmt.getColumn(1).getString(), "RUNNING");

    stop.request_stop();
    writer.flush();
}

TEST_F(DBWriterTest, UpdateRunComplete) {
    DBWriter writer(*db_);
    std::stop_source stop;
    writer.start(stop.get_token());

    writer.enqueue(InsertRun{
        .run_id = "run-002",
        .target_type = "workflow",
        .target_id = "wfl-build",
        .target_name = "build",
        .trigger_type = "schedule",
        .trigger_id = "trg-002",
        .correlation_id = "corr-002",
        .status = "RUNNING",
        .start_ts = "2026-03-07T12:00:00Z",
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    writer.enqueue(UpdateRunComplete{
        .run_id = "run-002",
        .status = "SUCCESS",
        .end_ts = "2026-03-07T12:01:00Z",
        .exit_code = 0,
        .duration_ms = 60000,
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    SQLite::Statement stmt(*db_,
        "SELECT status, exit_code, duration_ms FROM runs "
        "WHERE run_id = ?");
    stmt.bind(1, "run-002");
    ASSERT_TRUE(stmt.executeStep());
    EXPECT_EQ(stmt.getColumn(0).getString(), "SUCCESS");
    EXPECT_EQ(stmt.getColumn(1).getInt(), 0);
    EXPECT_EQ(stmt.getColumn(2).getInt64(), 60000);

    stop.request_stop();
    writer.flush();
}

TEST_F(DBWriterTest, InsertJobAndStepRuns) {
    DBWriter writer(*db_);
    std::stop_source stop;
    writer.start(stop.get_token());

    // Insert parent run first.
    writer.enqueue(InsertRun{
        .run_id = "run-003",
        .target_type = "workflow",
        .target_id = "wfl-test",
        .target_name = "test",
        .trigger_type = "manual",
        .trigger_id = "trg-003",
        .correlation_id = "corr-003",
        .status = "RUNNING",
        .start_ts = "2026-03-07T12:00:00Z",
    });

    writer.enqueue(InsertJobRun{
        .run_id = "run-003",
        .job_id = "job-build",
        .job_name = "build",
        .status = "RUNNING",
        .start_ts = "2026-03-07T12:00:01Z",
    });

    writer.enqueue(InsertStepRun{
        .run_id = "run-003",
        .job_id = "job-build",
        .step_id = "stp-001",
        .step_name = "compile",
        .command = "make -j4",
        .status = "RUNNING",
        .start_ts = "2026-03-07T12:00:02Z",
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    SQLite::Statement stmt(*db_,
        "SELECT step_name, command FROM step_runs "
        "WHERE run_id = ? AND step_id = ?");
    stmt.bind(1, "run-003");
    stmt.bind(2, "stp-001");
    ASSERT_TRUE(stmt.executeStep());
    EXPECT_EQ(stmt.getColumn(0).getString(), "compile");
    EXPECT_EQ(stmt.getColumn(1).getString(), "make -j4");

    stop.request_stop();
    writer.flush();
}

TEST_F(DBWriterTest, BatchWritesMultipleItems) {
    DBWriterConfig config;
    config.max_batch_size = 64;
    config.batch_timeout = std::chrono::milliseconds(20);

    DBWriter writer(*db_, config);
    std::stop_source stop;
    writer.start(stop.get_token());

    // Enqueue 50 runs rapidly.
    for (int i = 0; i < 50; ++i) {
        writer.enqueue(InsertRun{
            .run_id = "run-batch-" + std::to_string(i),
            .target_type = "workflow",
            .target_id = "wfl-batch",
            .target_name = "batch",
            .trigger_type = "manual",
            .trigger_id = "trg-batch",
            .correlation_id = "corr-batch",
            .status = "SUCCESS",
            .start_ts = "2026-03-07T12:00:00Z",
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    SQLite::Statement count(*db_, "SELECT COUNT(*) FROM runs");
    ASSERT_TRUE(count.executeStep());
    EXPECT_EQ(count.getColumn(0).getInt(), 50);

    stop.request_stop();
    writer.flush();
}

TEST_F(DBWriterTest, InsertLogChunk) {
    DBWriter writer(*db_);
    std::stop_source stop;
    writer.start(stop.get_token());

    // Insert parent run.
    writer.enqueue(InsertRun{
        .run_id = "run-log",
        .target_type = "workflow",
        .target_id = "wfl-x",
        .target_name = "x",
        .trigger_type = "manual",
        .trigger_id = "trg-x",
        .correlation_id = "corr-x",
        .status = "RUNNING",
        .start_ts = "2026-03-07T12:00:00Z",
    });

    writer.enqueue(InsertLogChunk{
        .run_id = "run-log",
        .job_id = "job-x",
        .step_id = "stp-x",
        .chunk_index = 1,
        .stream = "stdout",
        .content = "Build successful\n",
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    SQLite::Statement stmt(*db_,
        "SELECT content, stream FROM log_chunks "
        "WHERE run_id = ?");
    stmt.bind(1, "run-log");
    ASSERT_TRUE(stmt.executeStep());
    EXPECT_EQ(stmt.getColumn(0).getString(), "Build successful\n");
    EXPECT_EQ(stmt.getColumn(1).getString(), "stdout");

    stop.request_stop();
    writer.flush();
}

TEST_F(DBWriterTest, TotalWritesTracking) {
    DBWriter writer(*db_);
    std::stop_source stop;
    writer.start(stop.get_token());

    EXPECT_EQ(writer.total_writes(), 0);

    for (int i = 0; i < 5; ++i) {
        writer.enqueue(InsertRun{
            .run_id = "run-tw-" + std::to_string(i),
            .target_type = "workflow",
            .target_id = "wfl",
            .target_name = "w",
            .trigger_type = "manual",
            .trigger_id = "t",
            .correlation_id = "c",
            .status = "SUCCESS",
            .start_ts = "2026-03-07T12:00:00Z",
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(writer.total_writes(), 5);

    stop.request_stop();
    writer.flush();
}

// ═══════════════════════════════════════════════════════════════════════════
// QueryReader tests
// ═══════════════════════════════════════════════════════════════════════════

class QueryReaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = std::make_unique<SQLite::Database>(
            ":memory:",
            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db_->exec("PRAGMA journal_mode=WAL");
        db_->exec("PRAGMA foreign_keys=ON");
        kairos::persist::apply_migrations(*db_, kairos::persist::get_migrations());

        // Seed some job run data directly.
        seed_data();
    }

    void seed_data() {
        // Insert a run.
        db_->exec(
            "INSERT INTO runs (run_id, target_type, target_id, target_name, "
            "trigger_type, trigger_id, correlation_id, status, "
            "start_ts, end_ts, exit_code, duration_ms) "
            "VALUES ('run-1', 'workflow', 'wfl-1', 'deploy', 'manual', 'trg-1', "
            "'corr-1', 'SUCCESS', '2026-03-07T10:00:00Z', "
            "'2026-03-07T10:01:00Z', 0, 60000)");

        // Successful build job.
        db_->exec(
            "INSERT INTO job_runs (run_id, job_id, job_name, status, "
            "start_ts, end_ts, exit_code, duration_ms) "
            "VALUES ('run-1', 'job-build', 'build', 'SUCCESS', "
            "'2026-03-07T10:00:00Z', '2026-03-07T10:00:30Z', 0, 30000)");

        // Failed test job.
        db_->exec(
            "INSERT INTO job_runs (run_id, job_id, job_name, status, "
            "start_ts, end_ts, exit_code, duration_ms) "
            "VALUES ('run-1', 'job-test', 'test', 'FAILURE', "
            "'2026-03-07T10:00:30Z', '2026-03-07T10:01:00Z', 1, 30000)");

        // Another run — build succeeds again.
        db_->exec(
            "INSERT INTO runs (run_id, target_type, target_id, target_name, "
            "trigger_type, trigger_id, correlation_id, status, "
            "start_ts, end_ts, exit_code, duration_ms) "
            "VALUES ('run-2', 'workflow', 'wfl-1', 'deploy', 'schedule', 'trg-2', "
            "'corr-2', 'SUCCESS', '2026-03-07T11:00:00Z', "
            "'2026-03-07T11:01:00Z', 0, 60000)");

        db_->exec(
            "INSERT INTO job_runs (run_id, job_id, job_name, status, "
            "start_ts, end_ts, exit_code, duration_ms) "
            "VALUES ('run-2', 'job-build-2', 'build', 'SUCCESS', "
            "'2026-03-07T11:00:00Z', '2026-03-07T11:00:45Z', 0, 45000)");
    }

    std::unique_ptr<SQLite::Database> db_;
};

TEST_F(QueryReaderTest, LastSuccessBuild) {
    QueryReader reader(*db_);
    EXPECT_TRUE(reader.last_success("build"));
}

TEST_F(QueryReaderTest, LastSuccessTest) {
    QueryReader reader(*db_);
    EXPECT_FALSE(reader.last_success("test"));
}

TEST_F(QueryReaderTest, LastSuccessNeverRun) {
    QueryReader reader(*db_);
    EXPECT_FALSE(reader.last_success("nonexistent"));
}

TEST_F(QueryReaderTest, LastStatusBuild) {
    QueryReader reader(*db_);
    EXPECT_EQ(reader.last_status("build"), "SUCCESS");
}

TEST_F(QueryReaderTest, LastStatusTest) {
    QueryReader reader(*db_);
    EXPECT_EQ(reader.last_status("test"), "FAILURE");
}

TEST_F(QueryReaderTest, LastStatusNeverRun) {
    QueryReader reader(*db_);
    EXPECT_EQ(reader.last_status("nonexistent"), "NEVER_RUN");
}

TEST_F(QueryReaderTest, LastExitCode) {
    QueryReader reader(*db_);
    EXPECT_EQ(reader.last_exit_code("build"), 0);
    EXPECT_EQ(reader.last_exit_code("test"), 1);
    EXPECT_EQ(reader.last_exit_code("nonexistent"), -1);
}

TEST_F(QueryReaderTest, HasRun) {
    QueryReader reader(*db_);
    EXPECT_TRUE(reader.has_run("build"));
    EXPECT_TRUE(reader.has_run("test"));
    EXPECT_FALSE(reader.has_run("nonexistent"));
}

TEST_F(QueryReaderTest, RunCount) {
    QueryReader reader(*db_);
    // "build" appears in two runs (run-1 and run-2).
    EXPECT_EQ(reader.run_count("build"), 2);
    EXPECT_EQ(reader.run_count("test"), 1);
    EXPECT_EQ(reader.run_count("nonexistent"), 0);
}

TEST_F(QueryReaderTest, SuccessRate) {
    QueryReader reader(*db_);
    // "build" has 2 runs, both SUCCESS → rate = 1.0.
    EXPECT_DOUBLE_EQ(reader.success_rate("build"), 1.0);
    // "test" has 1 run, FAILURE → rate = 0.0.
    EXPECT_DOUBLE_EQ(reader.success_rate("test"), 0.0);
    EXPECT_DOUBLE_EQ(reader.success_rate("nonexistent"), 0.0);
}

TEST_F(QueryReaderTest, FinishedWithin) {
    QueryReader reader(*db_);

    // Build finished at 2026-03-07T11:00:45Z (seeded data).
    // Use a reference time far enough after the data that a large
    // window includes it but a tiny window does not.
    // 2026-03-08T00:00:00Z = 1773014400
    auto ref = std::chrono::system_clock::from_time_t(1773014400);

    // Large window (48 hours) should include the data.
    auto large_window = std::chrono::hours(48);
    EXPECT_TRUE(reader.finished_within("build", large_window, ref));

    // Tiny window (1 second) at this reference time won't reach back
    // ~13 hours to the seeded data.
    auto tiny_window = std::chrono::seconds(1);
    EXPECT_FALSE(reader.finished_within("build", tiny_window, ref));
}

// ═══════════════════════════════════════════════════════════════════════════
// KEL integration tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(QueryReaderTest, KelJobLastSuccess) {
    QueryReader reader(*db_);

    auto ctx = kairos::kel::make_default_context();
    reader.register_kel_bindings(ctx);

    // job("build").last_success should be true.
    auto result = kairos::kel::eval_expression(
        R"(job("build").last_success)", ctx);
    ASSERT_TRUE(std::holds_alternative<bool>(result.data));
    EXPECT_TRUE(std::get<bool>(result.data));

    // job("test").last_success should be false.
    result = kairos::kel::eval_expression(
        R"(job("test").last_success)", ctx);
    ASSERT_TRUE(std::holds_alternative<bool>(result.data));
    EXPECT_FALSE(std::get<bool>(result.data));
}

TEST_F(QueryReaderTest, KelJobLastStatus) {
    QueryReader reader(*db_);

    auto ctx = kairos::kel::make_default_context();
    reader.register_kel_bindings(ctx);

    auto result = kairos::kel::eval_expression(
        R"(job("test").last_status)", ctx);
    ASSERT_TRUE(std::holds_alternative<std::string>(result.data));
    EXPECT_EQ(std::get<std::string>(result.data), "FAILURE");
}

TEST_F(QueryReaderTest, KelJobHasRun) {
    QueryReader reader(*db_);

    auto ctx = kairos::kel::make_default_context();
    reader.register_kel_bindings(ctx);

    auto result = kairos::kel::eval_expression(
        R"(job("build").has_run)", ctx);
    ASSERT_TRUE(std::holds_alternative<bool>(result.data));
    EXPECT_TRUE(std::get<bool>(result.data));

    result = kairos::kel::eval_expression(
        R"(job("nonexistent").has_run)", ctx);
    ASSERT_TRUE(std::holds_alternative<bool>(result.data));
    EXPECT_FALSE(std::get<bool>(result.data));
}

TEST_F(QueryReaderTest, KelJobRunCount) {
    QueryReader reader(*db_);

    auto ctx = kairos::kel::make_default_context();
    reader.register_kel_bindings(ctx);

    auto result = kairos::kel::eval_expression(
        R"(job("build").run_count)", ctx);
    ASSERT_TRUE(std::holds_alternative<int64_t>(result.data));
    EXPECT_EQ(std::get<int64_t>(result.data), 2);
}

TEST_F(QueryReaderTest, KelJobConditionExpression) {
    QueryReader reader(*db_);

    auto ctx = kairos::kel::make_default_context();
    reader.register_kel_bindings(ctx);

    // Complex condition: build succeeded AND test has run.
    auto result = kairos::kel::eval_expression(
        R"(job("build").last_success and job("test").has_run)", ctx);
    ASSERT_TRUE(std::holds_alternative<bool>(result.data));
    EXPECT_TRUE(std::get<bool>(result.data));

    // Build succeeded but test didn't → should be false.
    result = kairos::kel::eval_expression(
        R"(job("build").last_success and job("test").last_success)", ctx);
    ASSERT_TRUE(std::holds_alternative<bool>(result.data));
    EXPECT_FALSE(std::get<bool>(result.data));
}
