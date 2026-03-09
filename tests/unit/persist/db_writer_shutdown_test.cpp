/// tests/unit/persist/db_writer_shutdown_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for DBWriter::stop() — graceful shutdown and statement release    ║
// ║                                                                          ║
// ║  Phase 3 Batch 15: Fix "database is locked" assertion on daemon          ║
// ║  shutdown. Verifies that stop() joins the writer thread and releases    ║
// ║  all prepared statements before the Database is destroyed.              ║
// ║                                                                          ║
// ║  The root cause was that daemon.cpp called db_writer.flush() (which     ║
// ║  only drains the queue) then db.reset() (which destroys the Database)  ║
// ║  while DBWriter still held 11 active prepared statements. SQLiteCpp's  ║
// ║  Database::Deleter asserts !SQLITE_BUSY on sqlite3_close().            ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/migration.hpp"

#include <gtest/gtest.h>
#include <SQLiteCpp/SQLiteCpp.h>

#include <memory>
#include <stop_token>
#include <string>
#include <thread>

namespace kairos::persist::test {

namespace {

/// Helper: format current time as ISO-8601.
static std::string iso_now() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &time_t);
#else
    gmtime_r(&time_t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

/// Test fixture with in-memory SQLite database.
class DBWriterShutdownTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = std::make_unique<SQLite::Database>(
            ":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db_->exec("PRAGMA journal_mode = WAL");
        db_->exec("PRAGMA foreign_keys = ON");

        // Create the minimal schema needed by DBWriter.
        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS runs (
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
                duration_ms INTEGER DEFAULT 0
            )
        )");
        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS job_runs (
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
        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS step_runs (
                run_id TEXT NOT NULL,
                job_id TEXT NOT NULL,
                step_id TEXT NOT NULL,
                step_name TEXT DEFAULT '',
                command TEXT DEFAULT '',
                status TEXT NOT NULL,
                start_ts TEXT DEFAULT '',
                end_ts TEXT DEFAULT '',
                exit_code INTEGER DEFAULT 0,
                duration_ms INTEGER DEFAULT 0,
                PRIMARY KEY (run_id, job_id, step_id)
            )
        )");
        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS log_chunks (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                run_id TEXT NOT NULL,
                job_id TEXT DEFAULT '',
                step_id TEXT DEFAULT '',
                chunk_index INTEGER DEFAULT 0,
                stream TEXT DEFAULT 'stdout',
                content TEXT DEFAULT '',
                created_at TEXT DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
            )
        )");
        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS trigger_history (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                trigger_id TEXT NOT NULL,
                trigger_type TEXT DEFAULT '',
                target_id TEXT DEFAULT '',
                fired_at TEXT DEFAULT '',
                status TEXT DEFAULT '',
                run_id TEXT DEFAULT ''
            )
        )");
        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS watch_samples (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                watch_group TEXT NOT NULL,
                sample_epoch INTEGER NOT NULL,
                file_path TEXT NOT NULL,
                file_size INTEGER DEFAULT 0,
                mtime TEXT DEFAULT '',
                hash TEXT DEFAULT '',
                scan_duration_ms INTEGER DEFAULT 0
            )
        )");
        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS watch_events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                watch_group TEXT NOT NULL,
                event_type TEXT DEFAULT '',
                file_path TEXT DEFAULT '',
                rule_name TEXT DEFAULT '',
                details_json TEXT DEFAULT '{}',
                action_taken TEXT DEFAULT '',
                created_at TEXT DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
            )
        )");
        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS metrics_snapshots (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                metric_name TEXT NOT NULL,
                metric_type TEXT DEFAULT 'gauge',
                value REAL DEFAULT 0.0,
                labels_json TEXT DEFAULT '{}',
                created_at TEXT DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
            )
        )");
    }

    std::unique_ptr<SQLite::Database> db_;
};

// ── Test: stop() allows clean DB close ─────────────────────────────────

TEST_F(DBWriterShutdownTest, StopReleasesStatementsBeforeDBClose) {
    // This test reproduces the exact bug scenario:
    // 1. Create DBWriter (which prepares 11 statements)
    // 2. Start the writer thread
    // 3. Enqueue some work
    // 4. Call stop() to join thread + release statements
    // 5. Destroy the Database (db_.reset())
    //
    // Before the fix, step 5 would trigger:
    //   Assertion `0 == ret && "database is locked"' failed.

    std::stop_source stop_src;

    {
        DBWriterConfig cfg;
        cfg.queue_capacity = 64;
        DBWriter writer(*db_, cfg);
        writer.start(stop_src.get_token());

        // Enqueue a few writes.
        writer.enqueue(InsertRun{
            .run_id = "r1",
            .target_type = "job",
            .target_id = "tid1",
            .target_name = "test_job",
            .trigger_type = "manual",
            .trigger_id = "",
            .correlation_id = "cid1",
            .status = "RUNNING",
            .start_ts = iso_now(),
        });

        writer.enqueue(InsertLogChunk{
            .run_id = "r1",
            .job_id = "j1",
            .step_id = "s1",
            .chunk_index = 0,
            .stream = "stdout",
            .content = "hello world",
        });

        // Signal stop and call stop().
        stop_src.request_stop();
        writer.stop();

        // Verify writes were processed.
        EXPECT_GE(writer.total_writes(), 2);
    }

    // Now destroy the database. This MUST NOT assert.
    // Before the fix, ~Database would see SQLITE_BUSY.
    EXPECT_NO_THROW(db_.reset());
}

// ── Test: stop() is idempotent ─────────────────────────────────────────

TEST_F(DBWriterShutdownTest, StopIsIdempotent) {
    std::stop_source stop_src;

    DBWriterConfig cfg;
    DBWriter writer(*db_, cfg);
    writer.start(stop_src.get_token());

    stop_src.request_stop();

    // Call stop() multiple times — must not crash or hang.
    writer.stop();
    writer.stop();
    writer.stop();

    EXPECT_NO_THROW(db_.reset());
}

// ── Test: stop() without start() ───────────────────────────────────────

TEST_F(DBWriterShutdownTest, StopWithoutStart) {
    // DBWriter created but never started — stop() should be safe.
    DBWriterConfig cfg;
    DBWriter writer(*db_, cfg);
    writer.stop();

    EXPECT_EQ(writer.total_writes(), 0);
    EXPECT_NO_THROW(db_.reset());
}

// ── Test: destructor calls stop() ──────────────────────────────────────

TEST_F(DBWriterShutdownTest, DestructorCallsStop) {
    std::stop_source stop_src;

    {
        DBWriterConfig cfg;
        DBWriter writer(*db_, cfg);
        writer.start(stop_src.get_token());

        writer.enqueue(InsertRun{
            .run_id = "r2",
            .target_type = "workflow",
            .target_id = "tid2",
            .target_name = "wf",
            .trigger_type = "schedule",
            .trigger_id = "t1",
            .correlation_id = "",
            .status = "RUNNING",
            .start_ts = iso_now(),
        });

        stop_src.request_stop();
        // Don't call stop() explicitly — let destructor handle it.
    }

    // If ~DBWriter properly releases statements, this should work.
    EXPECT_NO_THROW(db_.reset());
}

// ── Test: stop() flushes pending writes ────────────────────────────────

TEST_F(DBWriterShutdownTest, StopFlushesPendingWrites) {
    std::stop_source stop_src;

    DBWriterConfig cfg;
    cfg.batch_timeout = std::chrono::milliseconds(5000);  // Long timeout.
    DBWriter writer(*db_, cfg);
    writer.start(stop_src.get_token());

    // Enqueue several writes.
    for (int i = 0; i < 10; ++i) {
        writer.enqueue(InsertRun{
            .run_id = "r" + std::to_string(i),
            .target_type = "job",
            .target_id = "tid",
            .target_name = "job" + std::to_string(i),
            .trigger_type = "manual",
            .trigger_id = "",
            .correlation_id = "",
            .status = "RUNNING",
            .start_ts = iso_now(),
        });
    }

    stop_src.request_stop();
    writer.stop();

    // All 10 writes should have been flushed.
    EXPECT_EQ(writer.total_writes(), 10);

    // Verify in the database.
    SQLite::Statement count(*db_, "SELECT COUNT(*) FROM runs");
    count.executeStep();
    EXPECT_EQ(count.getColumn(0).getInt(), 10);
}

// ── Test: enqueue after stop() returns false ───────────────────────────

TEST_F(DBWriterShutdownTest, EnqueueAfterStopReturnsFalse) {
    std::stop_source stop_src;

    DBWriterConfig cfg;
    DBWriter writer(*db_, cfg);
    writer.start(stop_src.get_token());

    stop_src.request_stop();
    writer.stop();

    // Queue is closed — enqueue should fail.
    bool ok = writer.enqueue(InsertRun{
        .run_id = "late",
        .target_type = "job",
        .target_id = "x",
        .target_name = "x",
        .trigger_type = "manual",
        .trigger_id = "",
        .correlation_id = "",
        .status = "RUNNING",
        .start_ts = iso_now(),
    }, std::chrono::milliseconds(10));

    EXPECT_FALSE(ok);
}

// ── Test: stop() under concurrent enqueue pressure ────────────────────

TEST_F(DBWriterShutdownTest, StopUnderConcurrentEnqueue) {
    std::stop_source stop_src;

    DBWriterConfig cfg;
    cfg.queue_capacity = 128;
    DBWriter writer(*db_, cfg);
    writer.start(stop_src.get_token());

    // Spawn a thread that rapidly enqueues.
    std::jthread enqueuer([&](std::stop_token st) {
        int i = 0;
        while (!st.stop_requested()) {
            writer.enqueue(InsertMetricsSnapshot{
                .metric_name = "m" + std::to_string(i++),
                .metric_type = "gauge",
                .value = static_cast<double>(i),
                .labels_json = "{}",
            }, std::chrono::milliseconds(5));
        }
    });

    // Let it run briefly.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Stop everything.
    stop_src.request_stop();
    enqueuer.request_stop();
    enqueuer.join();
    writer.stop();

    EXPECT_GT(writer.total_writes(), 0);
    EXPECT_NO_THROW(db_.reset());
}

// ── Test: daemon shutdown sequence simulation ──────────────────────────

TEST_F(DBWriterShutdownTest, DaemonShutdownSequence) {
    // Simulates the exact daemon shutdown path that was crashing:
    //   stop_source.request_stop() → ... → db_writer.stop() → db.reset()

    std::stop_source stop_source;

    DBWriterConfig cfg;
    DBWriter writer(*db_, cfg);
    writer.start(stop_source.get_token());

    // Simulate some daemon activity.
    writer.enqueue(InsertRun{
        .run_id = "daemon_run_1",
        .target_type = "workflow",
        .target_id = "wf_hash",
        .target_name = "build_all",
        .trigger_type = "schedule",
        .trigger_id = "cron_1",
        .correlation_id = "corr_1",
        .status = "RUNNING",
        .start_ts = iso_now(),
    });

    writer.enqueue(UpdateRunComplete{
        .run_id = "daemon_run_1",
        .status = "SUCCESS",
        .end_ts = iso_now(),
        .exit_code = 0,
        .duration_ms = 1234,
    });

    writer.enqueue(BatchInsertMetricsSnapshots{
        .entries = {
            {.metric_name = "uptime", .metric_type = "gauge",
             .value = 42.0, .labels_json = "{}"},
        },
    });

    // Signal shutdown.
    stop_source.request_stop();

    // Daemon shutdown: stop DB writer then destroy DB.
    writer.stop();
    EXPECT_NO_THROW(db_.reset());
}

}  // namespace

}  // namespace kairos::persist::test
