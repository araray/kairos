/// tests/unit/persist/prune_preview_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for QueryReader::query_prune_preview() and                       ║
// ║  QueryReader::query_watch_events_since() / query_max_event_id()         ║
// ║                                                                          ║
// ║  Phase 3 Batch 14: prune --dry-run and events tail                      ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/query_reader.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/migration.hpp"

#include <gtest/gtest.h>
#include <SQLiteCpp/SQLiteCpp.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace kairos::persist::test {

namespace {

/// Helper: format a time_point as ISO-8601 string.
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

/// Helper: format a time point N days ago.
static std::string iso_days_ago(int days) {
    auto tp = std::chrono::system_clock::now() -
              std::chrono::hours(24 * days);
    auto time_t = std::chrono::system_clock::to_time_t(tp);
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

/// Test fixture: in-memory SQLite database with schema.
class PrunePreviewTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = std::make_unique<SQLite::Database>(
            ":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db_->exec("PRAGMA journal_mode = WAL");
        db_->exec("PRAGMA foreign_keys = ON");

        // Create minimal schema for the tables we need.
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
                duration_ms INTEGER DEFAULT 0,
                plan_json TEXT DEFAULT '',
                error_message TEXT DEFAULT '',
                created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
                tags_json TEXT
            )
        )");

        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS job_runs (
                run_id TEXT NOT NULL,
                job_id TEXT NOT NULL,
                job_name TEXT NOT NULL DEFAULT '',
                status TEXT NOT NULL DEFAULT 'PENDING',
                exit_code INTEGER DEFAULT 0,
                start_ts TEXT DEFAULT '',
                end_ts TEXT DEFAULT '',
                duration_ms INTEGER DEFAULT 0,
                topo_level INTEGER DEFAULT 0,
                condition_result TEXT DEFAULT '',
                skip_reason TEXT DEFAULT '',
                error_message TEXT DEFAULT '',
                PRIMARY KEY (run_id, job_id)
            )
        )");

        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS step_runs (
                run_id TEXT NOT NULL,
                job_id TEXT NOT NULL,
                step_id TEXT NOT NULL,
                step_name TEXT NOT NULL DEFAULT '',
                command TEXT DEFAULT '',
                status TEXT NOT NULL DEFAULT 'PENDING',
                exit_code INTEGER DEFAULT 0,
                start_ts TEXT DEFAULT '',
                end_ts TEXT DEFAULT '',
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
                stream TEXT NOT NULL DEFAULT 'stdout',
                data TEXT NOT NULL DEFAULT '',
                created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
            )
        )");

        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS watch_events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                watch_group TEXT NOT NULL,
                rule_name TEXT NOT NULL,
                event_type TEXT NOT NULL,
                action_taken TEXT NOT NULL DEFAULT 'info',
                file_path TEXT DEFAULT '',
                details_json TEXT DEFAULT '',
                created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
            )
        )");

        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS watch_samples (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                watch_group TEXT NOT NULL,
                sample_epoch INTEGER NOT NULL,
                file_path TEXT NOT NULL,
                is_dir INTEGER DEFAULT 0,
                size INTEGER DEFAULT 0,
                mtime TEXT DEFAULT '',
                hash TEXT DEFAULT '',
                scan_duration_ms INTEGER DEFAULT 0,
                collected_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
            )
        )");

        db_->exec(R"(
            CREATE TABLE IF NOT EXISTS metrics_snapshots (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                metric_name TEXT NOT NULL,
                metric_type TEXT NOT NULL,
                value REAL NOT NULL,
                labels_json TEXT DEFAULT '',
                recorded_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
            )
        )");

        reader_ = std::make_unique<QueryReader>(*db_);
    }

    /// Insert a run with a specific start timestamp.
    void insert_run(const std::string& run_id,
                    const std::string& start_ts,
                    const std::string& status = "SUCCESS") {
        SQLite::Statement s(*db_,
            "INSERT INTO runs (run_id, target_type, target_id, "
            "target_name, trigger_type, status, start_ts) "
            "VALUES (?, 'job', 'jid', 'jname', 'manual', ?, ?)");
        s.bind(1, run_id);
        s.bind(2, status);
        s.bind(3, start_ts);
        s.exec();
    }

    /// Insert a run_job record.
    void insert_run_job(const std::string& run_id,
                        const std::string& job_id) {
        SQLite::Statement s(*db_,
            "INSERT INTO job_runs (run_id, job_id, status) "
            "VALUES (?, ?, 'SUCCESS')");
        s.bind(1, run_id);
        s.bind(2, job_id);
        s.exec();
    }

    /// Insert a run_step record.
    void insert_run_step(const std::string& run_id,
                         const std::string& job_id,
                         const std::string& step_id) {
        SQLite::Statement s(*db_,
            "INSERT INTO step_runs (run_id, job_id, step_id, status) "
            "VALUES (?, ?, ?, 'SUCCESS')");
        s.bind(1, run_id);
        s.bind(2, job_id);
        s.bind(3, step_id);
        s.exec();
    }

    /// Insert a log chunk.
    void insert_log_chunk(const std::string& run_id,
                          const std::string& content) {
        SQLite::Statement s(*db_,
            "INSERT INTO log_chunks (run_id, stream, data) "
            "VALUES (?, 'stdout', ?)");
        s.bind(1, run_id);
        s.bind(2, content);
        s.exec();
    }

    /// Insert a watch event with explicit created_at.
    void insert_watch_event(const std::string& group,
                            const std::string& rule,
                            const std::string& created_at) {
        SQLite::Statement s(*db_,
            "INSERT INTO watch_events (watch_group, rule_name, "
            "event_type, action_taken, created_at) "
            "VALUES (?, ?, 'file_changed', 'info', ?)");
        s.bind(1, group);
        s.bind(2, rule);
        s.bind(3, created_at);
        s.exec();
    }

    /// Insert a watch event using default timestamp (now).
    int64_t insert_watch_event_now(const std::string& group,
                                    const std::string& rule) {
        SQLite::Statement s(*db_,
            "INSERT INTO watch_events (watch_group, rule_name, "
            "event_type, action_taken) "
            "VALUES (?, ?, 'file_changed', 'info')");
        s.bind(1, group);
        s.bind(2, rule);
        s.exec();
        return db_->getLastInsertRowid();
    }

    /// Insert a metrics snapshot with explicit recorded_at.
    void insert_metrics_snapshot(const std::string& name,
                                 double value,
                                 const std::string& recorded_at) {
        SQLite::Statement s(*db_,
            "INSERT INTO metrics_snapshots "
            "(metric_name, metric_type, value, recorded_at) "
            "VALUES (?, 'gauge', ?, ?)");
        s.bind(1, name);
        s.bind(2, value);
        s.bind(3, recorded_at);
        s.exec();
    }

    std::unique_ptr<SQLite::Database> db_;
    std::unique_ptr<QueryReader> reader_;
};

}  // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════
//  PrunePreview tests
// ═══════════════════════════════════════════════════════════════════════

TEST_F(PrunePreviewTest, EmptyDatabase_ReturnsZeros) {
    auto preview = reader_->query_prune_preview(30);
    EXPECT_EQ(preview.runs_to_delete, 0);
    EXPECT_EQ(preview.run_jobs_to_delete, 0);
    EXPECT_EQ(preview.run_steps_to_delete, 0);
    EXPECT_EQ(preview.log_chunks_to_delete, 0);
    EXPECT_EQ(preview.watch_events_to_delete, 0);
    EXPECT_EQ(preview.watch_samples_to_delete, 0);
    EXPECT_EQ(preview.metrics_snapshots_to_delete, 0);
}

TEST_F(PrunePreviewTest, RecentRecords_NothingToPrune) {
    insert_run("run-1", iso_now());
    insert_run_job("run-1", "job-1");
    insert_run_step("run-1", "job-1", "step-1");
    insert_log_chunk("run-1", "output");

    auto preview = reader_->query_prune_preview(30);
    EXPECT_EQ(preview.runs_to_delete, 0);
    EXPECT_EQ(preview.run_jobs_to_delete, 0);
    EXPECT_EQ(preview.run_steps_to_delete, 0);
    EXPECT_EQ(preview.log_chunks_to_delete, 0);
}

TEST_F(PrunePreviewTest, OldRuns_CountedForPruning) {
    // Insert runs from 60 days ago.
    insert_run("run-old-1", iso_days_ago(60));
    insert_run("run-old-2", iso_days_ago(45));
    insert_run_job("run-old-1", "job-1");
    insert_run_job("run-old-2", "job-2");
    insert_run_step("run-old-1", "job-1", "step-1");
    insert_log_chunk("run-old-1", "old output");
    insert_log_chunk("run-old-2", "old output 2");

    // Insert a recent run (should not be pruned).
    insert_run("run-new", iso_now());
    insert_run_job("run-new", "job-3");

    auto preview = reader_->query_prune_preview(30);
    EXPECT_EQ(preview.runs_to_delete, 2);
    EXPECT_EQ(preview.run_jobs_to_delete, 2);
    EXPECT_EQ(preview.run_steps_to_delete, 1);
    EXPECT_EQ(preview.log_chunks_to_delete, 2);
}

TEST_F(PrunePreviewTest, CustomRetentionDays) {
    insert_run("run-10d", iso_days_ago(10));
    insert_run("run-3d", iso_days_ago(3));
    insert_run("run-now", iso_now());

    // 7-day retention: only 10-day-old run should be pruned.
    auto p7 = reader_->query_prune_preview(7);
    EXPECT_EQ(p7.runs_to_delete, 1);

    // 1-day retention: both old runs should be pruned.
    auto p1 = reader_->query_prune_preview(1);
    EXPECT_EQ(p1.runs_to_delete, 2);
}

TEST_F(PrunePreviewTest, WatchEvents_CountedForPruning) {
    insert_watch_event("group1", "rule1", iso_days_ago(60));
    insert_watch_event("group1", "rule2", iso_days_ago(45));
    insert_watch_event("group1", "rule3", iso_now());

    auto preview = reader_->query_prune_preview(30);
    EXPECT_EQ(preview.watch_events_to_delete, 2);
}

TEST_F(PrunePreviewTest, MetricsSnapshots_CountedForPruning) {
    insert_metrics_snapshot("kairos.runs.total", 100, iso_days_ago(60));
    insert_metrics_snapshot("kairos.runs.total", 200, iso_days_ago(5));
    insert_metrics_snapshot("kairos.runs.total", 300, iso_now());

    auto preview = reader_->query_prune_preview(30);
    EXPECT_EQ(preview.metrics_snapshots_to_delete, 1);
}

// ═══════════════════════════════════════════════════════════════════════
//  Events tail (cursor-based) tests
// ═══════════════════════════════════════════════════════════════════════

TEST_F(PrunePreviewTest, MaxEventId_EmptyTable) {
    EXPECT_EQ(reader_->query_max_event_id(), 0);
}

TEST_F(PrunePreviewTest, MaxEventId_WithEvents) {
    auto id1 = insert_watch_event_now("g1", "r1");
    auto id2 = insert_watch_event_now("g1", "r2");
    auto id3 = insert_watch_event_now("g2", "r3");

    auto max_id = reader_->query_max_event_id();
    EXPECT_EQ(max_id, id3);
    EXPECT_GT(max_id, 0);
}

TEST_F(PrunePreviewTest, EventsSince_CursorBasedQuery) {
    auto id1 = insert_watch_event_now("g1", "r1");
    auto id2 = insert_watch_event_now("g1", "r2");
    auto id3 = insert_watch_event_now("g2", "r3");

    // Query all events after id=0 (get all).
    auto all = reader_->query_watch_events_since(0);
    EXPECT_EQ(all.size(), 3u);

    // Query events after id1 (should get id2, id3).
    auto after1 = reader_->query_watch_events_since(id1);
    EXPECT_EQ(after1.size(), 2u);
    EXPECT_EQ(after1[0].rule_name, "r2");
    EXPECT_EQ(after1[1].rule_name, "r3");

    // Query events after id3 (should get nothing).
    auto after3 = reader_->query_watch_events_since(id3);
    EXPECT_TRUE(after3.empty());
}

TEST_F(PrunePreviewTest, EventsSince_FilterByGroup) {
    insert_watch_event_now("g1", "r1");
    insert_watch_event_now("g2", "r2");
    insert_watch_event_now("g1", "r3");

    auto g1_events = reader_->query_watch_events_since(0, 50, "g1");
    EXPECT_EQ(g1_events.size(), 2u);
    EXPECT_EQ(g1_events[0].watch_group, "g1");
    EXPECT_EQ(g1_events[1].watch_group, "g1");

    auto g2_events = reader_->query_watch_events_since(0, 50, "g2");
    EXPECT_EQ(g2_events.size(), 1u);
    EXPECT_EQ(g2_events[0].rule_name, "r2");
}

TEST_F(PrunePreviewTest, EventsSince_Limit) {
    for (int i = 0; i < 10; ++i) {
        insert_watch_event_now("g1", "r" + std::to_string(i));
    }

    auto limited = reader_->query_watch_events_since(0, 3);
    EXPECT_EQ(limited.size(), 3u);
}

TEST_F(PrunePreviewTest, EventsSince_AscendingOrder) {
    auto id1 = insert_watch_event_now("g1", "first");
    auto id2 = insert_watch_event_now("g1", "second");
    auto id3 = insert_watch_event_now("g1", "third");

    auto events = reader_->query_watch_events_since(0);
    ASSERT_GE(events.size(), 3u);
    // Should be in ascending order (oldest first for tail).
    EXPECT_EQ(events[0].rule_name, "first");
    EXPECT_EQ(events[1].rule_name, "second");
    EXPECT_EQ(events[2].rule_name, "third");
}

}  // namespace kairos::persist::test
