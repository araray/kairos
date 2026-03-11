/// tests/unit/tui/tui_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Unit tests for TUI dashboard helpers + DB refresh queries              ║
// ║                                                                          ║
// ║  Tests the helper functions (format_duration, format_relative,          ║
// ║  status_icon) and validates that the TUI's SQL queries match the        ║
// ║  actual Kairos schema (runs, log_chunks, watch_events,                  ║
// ║  trigger_history).                                                       ║
// ║                                                                          ║
// ║  The DB refresh tests create an in-memory SQLite database with the      ║
// ║  real schema from migration.cpp and verify queries produce correct      ║
// ║  results — this catches column/table name mismatches.                   ║
// ║                                                                          ║
// ║  Spec reference: §23 (TUI), §30 (testing strategy)                     ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/tui/tui_helpers.hpp"

#include <gtest/gtest.h>
#include <SQLiteCpp/SQLiteCpp.h>

#include <chrono>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

namespace helpers = kairos::tui::helpers;

// ══════════════════════════════════════════════════════════════════════════
// Helper function tests
// ══════════════════════════════════════════════════════════════════════════

class TuiHelpersTest : public ::testing::Test {};

// ── format_duration ─────────────────────────────────────────────────────

TEST_F(TuiHelpersTest, FormatDuration_Negative) {
    EXPECT_EQ(helpers::format_duration(-1.0), "—");
    EXPECT_EQ(helpers::format_duration(-100.0), "—");
}

TEST_F(TuiHelpersTest, FormatDuration_Zero) {
    EXPECT_EQ(helpers::format_duration(0.0), "0s");
}

TEST_F(TuiHelpersTest, FormatDuration_Seconds) {
    EXPECT_EQ(helpers::format_duration(1.0), "1s");
    EXPECT_EQ(helpers::format_duration(45.0), "45s");
    EXPECT_EQ(helpers::format_duration(59.9), "59s");
}

TEST_F(TuiHelpersTest, FormatDuration_Minutes) {
    EXPECT_EQ(helpers::format_duration(60.0), "1m 0s");
    EXPECT_EQ(helpers::format_duration(125.0), "2m 5s");
    EXPECT_EQ(helpers::format_duration(3599.0), "59m 59s");
}

TEST_F(TuiHelpersTest, FormatDuration_Hours) {
    EXPECT_EQ(helpers::format_duration(3600.0), "1h 0m");
    EXPECT_EQ(helpers::format_duration(7384.0), "2h 3m");
    EXPECT_EQ(helpers::format_duration(86400.0), "24h 0m");
}

// ── format_relative ─────────────────────────────────────────────────────

TEST_F(TuiHelpersTest, FormatRelative_Empty) {
    EXPECT_EQ(helpers::format_relative(""), "—");
}

TEST_F(TuiHelpersTest, FormatRelative_ShortString) {
    // Strings too short to parse return the raw string.
    EXPECT_EQ(helpers::format_relative("abc"), "abc");
}

TEST_F(TuiHelpersTest, FormatRelative_RecentTimestamp) {
    // Create a timestamp "10 seconds ago" in local time.
    auto now = std::time(nullptr);
    auto ten_ago = now - 10;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &ten_ago);
#else
    localtime_r(&ten_ago, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    std::string ts(buf);

    auto result = helpers::format_relative(ts);
    // Should be something like "10s ago" or "11s ago" (timing-tolerant).
    EXPECT_TRUE(result.find("s ago") != std::string::npos)
        << "Expected '...s ago', got: " << result;
}

TEST_F(TuiHelpersTest, FormatRelative_FutureTimestamp) {
    auto now = std::time(nullptr);
    auto future = now + 3600;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &future);
#else
    localtime_r(&future, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    EXPECT_EQ(helpers::format_relative(std::string(buf)), "just now");
}

TEST_F(TuiHelpersTest, FormatRelative_ISOWithT) {
    // Also accepts "T" separator.
    auto now = std::time(nullptr);
    auto min_ago = now - 300;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &min_ago);
#else
    localtime_r(&min_ago, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    auto result = helpers::format_relative(std::string(buf));
    EXPECT_TRUE(result.find("m ago") != std::string::npos)
        << "Expected '...m ago', got: " << result;
}

// ── status_icon ─────────────────────────────────────────────────────────

TEST_F(TuiHelpersTest, StatusIcon_AllVariants) {
    EXPECT_EQ(helpers::status_icon("SUCCESS"),   "✓");
    EXPECT_EQ(helpers::status_icon("FAILED"),    "✗");
    EXPECT_EQ(helpers::status_icon("RUNNING"),   "●");
    EXPECT_EQ(helpers::status_icon("CANCELLED"), "⊘");
    EXPECT_EQ(helpers::status_icon("SKIPPED"),   "○");
    EXPECT_EQ(helpers::status_icon("TIMED_OUT"), "⏱");
    EXPECT_EQ(helpers::status_icon("UNKNOWN"),   "?");
    EXPECT_EQ(helpers::status_icon(""),          "?");
}

// ── format_local_time_now ───────────────────────────────────────────────

TEST_F(TuiHelpersTest, FormatLocalTimeNow_ValidFormat) {
    auto result = helpers::format_local_time_now();
    // Should be "HH:MM:SS" — 8 characters.
    ASSERT_EQ(result.size(), 8u);
    EXPECT_EQ(result[2], ':');
    EXPECT_EQ(result[5], ':');
}

// ══════════════════════════════════════════════════════════════════════════
// DB schema validation tests
//
// These tests create an in-memory SQLite database with the exact schema
// from Kairos's migration.cpp, insert test data, and then execute the
// exact SQL queries used by the TUI's refresh_state(). This catches
// column/table name mismatches at test time rather than at runtime.
// ══════════════════════════════════════════════════════════════════════════

class TuiDbQueryTest : public ::testing::Test {
protected:
    std::unique_ptr<SQLite::Database> db_;

    void SetUp() override {
        db_ = std::make_unique<SQLite::Database>(":memory:",
            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);

        // Create the real schema (subset relevant to TUI queries).
        db_->exec(R"SQL(
            CREATE TABLE IF NOT EXISTS runs (
                id              INTEGER PRIMARY KEY AUTOINCREMENT,
                run_id          TEXT    NOT NULL UNIQUE,
                target_type     TEXT    NOT NULL,
                target_id       TEXT    NOT NULL,
                target_name     TEXT    NOT NULL,
                trigger_type    TEXT    NOT NULL,
                trigger_id      TEXT    NOT NULL,
                correlation_id  TEXT    NOT NULL,
                status          TEXT    NOT NULL DEFAULT 'PENDING',
                exit_code       INTEGER,
                start_ts        TEXT    NOT NULL,
                end_ts          TEXT,
                duration_ms     INTEGER,
                plan_json       TEXT,
                error_message   TEXT,
                created_at      TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
            );
        )SQL");

        db_->exec(R"SQL(
            CREATE TABLE IF NOT EXISTS log_chunks (
                id              INTEGER PRIMARY KEY AUTOINCREMENT,
                run_id          TEXT    NOT NULL,
                job_id          TEXT    NOT NULL,
                step_id         TEXT    NOT NULL DEFAULT '',
                stream          TEXT    NOT NULL DEFAULT 'stdout',
                chunk_index     INTEGER NOT NULL DEFAULT 0,
                content         TEXT    NOT NULL,
                created_at      TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
            );
        )SQL");

        db_->exec(R"SQL(
            CREATE TABLE IF NOT EXISTS watch_events (
                id              INTEGER PRIMARY KEY AUTOINCREMENT,
                watch_group     TEXT    NOT NULL,
                event_type      TEXT    NOT NULL,
                file_path       TEXT    NOT NULL,
                old_hash        TEXT,
                new_hash        TEXT,
                old_size        INTEGER,
                new_size        INTEGER,
                rule_name       TEXT,
                action_taken    TEXT,
                details_json    TEXT,
                created_at      TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
            );
        )SQL");

        db_->exec(R"SQL(
            CREATE TABLE IF NOT EXISTS trigger_history (
                id              INTEGER PRIMARY KEY AUTOINCREMENT,
                trigger_id      TEXT    NOT NULL,
                trigger_type    TEXT    NOT NULL,
                target_id       TEXT    NOT NULL,
                fired_at        TEXT    NOT NULL,
                run_id          TEXT,
                status          TEXT    NOT NULL DEFAULT 'fired',
                details_json    TEXT,
                created_at      TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
            );
        )SQL");
    }
};

// ── Active runs query ───────────────────────────────────────────────────

TEST_F(TuiDbQueryTest, ActiveRunsQuery_ReturnsRunning) {
    db_->exec(
        "INSERT INTO runs (run_id, target_type, target_id, target_name, "
        "  trigger_type, trigger_id, correlation_id, status, start_ts) "
        "VALUES ('run-001', 'workflow', 'wfl-abc', 'Deploy', "
        "  'schedule', 'trg-1', 'corr-1', 'RUNNING', "
        "  datetime('now', '-2 minutes'))");

    db_->exec(
        "INSERT INTO runs (run_id, target_type, target_id, target_name, "
        "  trigger_type, trigger_id, correlation_id, status, start_ts, "
        "  duration_ms) "
        "VALUES ('run-002', 'workflow', 'wfl-def', 'Backup', "
        "  'manual', 'trg-2', 'corr-2', 'SUCCESS', "
        "  datetime('now', '-1 hour'), 5000)");

    // Execute the exact query from tui_dashboard.cpp.
    SQLite::Statement q(*db_,
        "SELECT run_id, target_name, status, start_ts, "
        "       CAST((julianday('now') - julianday(start_ts))"
        "            * 86400 AS INTEGER) AS elapsed_s "
        "FROM runs WHERE status = 'RUNNING' "
        "ORDER BY start_ts DESC LIMIT 20");

    std::vector<std::string> run_ids;
    while (q.executeStep()) {
        run_ids.push_back(q.getColumn(0).getString());
        // Verify target_name column is accessible.
        std::string name = q.getColumn(1).getString();
        EXPECT_EQ(name, "Deploy");
        // elapsed_s should be positive.
        double elapsed = q.getColumn(4).getDouble();
        EXPECT_GT(elapsed, 0.0);
    }
    ASSERT_EQ(run_ids.size(), 1u);
    EXPECT_EQ(run_ids[0], "run-001");
}

// ── Recent completed runs query ─────────────────────────────────────────

TEST_F(TuiDbQueryTest, RecentRunsQuery_ExcludesRunning) {
    db_->exec(
        "INSERT INTO runs (run_id, target_type, target_id, target_name, "
        "  trigger_type, trigger_id, correlation_id, status, start_ts, "
        "  duration_ms, exit_code) "
        "VALUES ('run-100', 'workflow', 'wfl-x', 'Build', "
        "  'schedule', 'trg-a', 'c-a', 'SUCCESS', "
        "  datetime('now', '-30 minutes'), 4500, 0)");

    db_->exec(
        "INSERT INTO runs (run_id, target_type, target_id, target_name, "
        "  trigger_type, trigger_id, correlation_id, status, start_ts) "
        "VALUES ('run-101', 'workflow', 'wfl-y', 'Test', "
        "  'manual', 'trg-b', 'c-b', 'RUNNING', "
        "  datetime('now', '-1 minute'))");

    SQLite::Statement q(*db_,
        "SELECT run_id, target_name, status, start_ts, "
        "       duration_ms, exit_code "
        "FROM runs WHERE status != 'RUNNING' "
        "ORDER BY start_ts DESC LIMIT 20");

    int count = 0;
    while (q.executeStep()) {
        EXPECT_EQ(q.getColumn(0).getString(), "run-100");
        EXPECT_EQ(q.getColumn(1).getString(), "Build");
        EXPECT_EQ(q.getColumn(2).getString(), "SUCCESS");
        EXPECT_EQ(q.getColumn(4).getInt(), 4500);
        EXPECT_EQ(q.getColumn(5).getInt(), 0);
        count++;
    }
    EXPECT_EQ(count, 1);
}

// ── Runs today / failures today ─────────────────────────────────────────

TEST_F(TuiDbQueryTest, RunsTodayQuery_CountsCorrectly) {
    // Insert a success run "today".
    db_->exec(
        "INSERT INTO runs (run_id, target_type, target_id, target_name, "
        "  trigger_type, trigger_id, correlation_id, status, start_ts) "
        "VALUES ('r-t1', 'job', 'j-1', 'A', "
        "  'schedule', 't-1', 'c-1', 'SUCCESS', "
        "  datetime('now', '-1 hour'))");

    // Insert a failed run "today".
    db_->exec(
        "INSERT INTO runs (run_id, target_type, target_id, target_name, "
        "  trigger_type, trigger_id, correlation_id, status, start_ts) "
        "VALUES ('r-t2', 'job', 'j-2', 'B', "
        "  'schedule', 't-2', 'c-2', 'FAILED', "
        "  datetime('now', '-30 minutes'))");

    SQLite::Statement q(*db_,
        "SELECT COUNT(*), "
        "  SUM(CASE WHEN status='FAILED' THEN 1 ELSE 0 END) "
        "FROM runs WHERE start_ts >= date('now')");

    ASSERT_TRUE(q.executeStep());
    EXPECT_EQ(q.getColumn(0).getInt64(), 2);
    EXPECT_EQ(q.getColumn(1).getInt64(), 1);
}

// ── Watch events query ──────────────────────────────────────────────────

TEST_F(TuiDbQueryTest, WatchEventsQuery_GroupsCorrectly) {
    // Note: column is created_at, not timestamp.
    db_->exec(
        "INSERT INTO watch_events (watch_group, event_type, file_path, "
        "  created_at) "
        "VALUES ('log_monitor', 'modified', '/var/log/app.log', "
        "  datetime('now', '-5 minutes'))");
    db_->exec(
        "INSERT INTO watch_events (watch_group, event_type, file_path, "
        "  created_at) "
        "VALUES ('log_monitor', 'modified', '/var/log/err.log', "
        "  datetime('now', '-2 minutes'))");
    db_->exec(
        "INSERT INTO watch_events (watch_group, event_type, file_path, "
        "  created_at) "
        "VALUES ('git_repo', 'created', '/repo/README.md', "
        "  datetime('now', '-10 minutes'))");

    // Execute the exact query from tui_dashboard.cpp.
    SQLite::Statement q(*db_,
        "SELECT watch_group, COUNT(*), MAX(created_at) "
        "FROM watch_events "
        "GROUP BY watch_group "
        "ORDER BY MAX(created_at) DESC LIMIT 20");

    std::vector<std::pair<std::string, int>> groups;
    while (q.executeStep()) {
        groups.emplace_back(
            q.getColumn(0).getString(),
            q.getColumn(1).getInt());
    }
    ASSERT_EQ(groups.size(), 2u);
    // log_monitor is more recent → first.
    EXPECT_EQ(groups[0].first, "log_monitor");
    EXPECT_EQ(groups[0].second, 2);
    EXPECT_EQ(groups[1].first, "git_repo");
    EXPECT_EQ(groups[1].second, 1);
}

// ── Trigger history query ───────────────────────────────────────────────

TEST_F(TuiDbQueryTest, TriggerHistoryQuery_ReturnsRecentFires) {
    db_->exec(
        "INSERT INTO trigger_history (trigger_id, trigger_type, "
        "  target_id, fired_at, status) "
        "VALUES ('trg-cron-deploy', 'schedule', 'wfl-deploy', "
        "  datetime('now', '-10 minutes'), 'fired')");
    db_->exec(
        "INSERT INTO trigger_history (trigger_id, trigger_type, "
        "  target_id, fired_at, status) "
        "VALUES ('trg-manual-001', 'manual', 'wfl-test', "
        "  datetime('now', '-2 minutes'), 'fired')");

    // Execute the exact query from tui_dashboard.cpp.
    SQLite::Statement q(*db_,
        "SELECT trigger_id, trigger_type, target_id, "
        "       fired_at, status "
        "FROM trigger_history "
        "ORDER BY fired_at DESC LIMIT 20");

    std::vector<std::string> ids;
    while (q.executeStep()) {
        ids.push_back(q.getColumn(0).getString());
        // Verify all columns are accessible.
        std::string tt = q.getColumn(1).getString();
        std::string ti = q.getColumn(2).getString();
        std::string fa = q.getColumn(3).getString();
        std::string st = q.getColumn(4).getString();
        EXPECT_FALSE(tt.empty());
        EXPECT_FALSE(ti.empty());
        EXPECT_FALSE(fa.empty());
        EXPECT_EQ(st, "fired");
    }
    ASSERT_EQ(ids.size(), 2u);
    // Most recent first.
    EXPECT_EQ(ids[0], "trg-manual-001");
    EXPECT_EQ(ids[1], "trg-cron-deploy");
}

// ── Log chunks query ────────────────────────────────────────────────────

TEST_F(TuiDbQueryTest, LogChunksQuery_ReturnsContent) {
    // Note: column is `content`, not `data`.
    db_->exec(
        "INSERT INTO log_chunks (run_id, job_id, step_id, stream, "
        "  chunk_index, content) "
        "VALUES ('run-x', 'job-a', 'step-1', 'stdout', 0, "
        "  'Compiling main.cpp...')");
    db_->exec(
        "INSERT INTO log_chunks (run_id, job_id, step_id, stream, "
        "  chunk_index, content) "
        "VALUES ('run-x', 'job-a', 'step-1', 'stderr', 1, "
        "  'warning: unused variable')");

    // Execute the exact query from tui_dashboard.cpp.
    SQLite::Statement q(*db_,
        "SELECT lc.content, lc.created_at, lc.stream "
        "FROM log_chunks lc "
        "ORDER BY lc.id DESC LIMIT 50");

    std::vector<std::pair<std::string, std::string>> entries;
    while (q.executeStep()) {
        entries.emplace_back(
            q.getColumn(0).getString(),  // content
            q.getColumn(2).getString()); // stream
    }
    ASSERT_EQ(entries.size(), 2u);
    // DESC order → most recent first.
    EXPECT_EQ(entries[0].first, "warning: unused variable");
    EXPECT_EQ(entries[0].second, "stderr");
    EXPECT_EQ(entries[1].first, "Compiling main.cpp...");
    EXPECT_EQ(entries[1].second, "stdout");
}

// ── Empty database graceful degradation ─────────────────────────────────

TEST_F(TuiDbQueryTest, EmptyDatabase_AllQueriesReturnEmpty) {
    // All queries should return 0 rows on a freshly created schema.
    {
        SQLite::Statement q(*db_,
            "SELECT run_id, target_name, status, start_ts, "
            "       CAST((julianday('now') - julianday(start_ts))"
            "            * 86400 AS INTEGER) AS elapsed_s "
            "FROM runs WHERE status = 'RUNNING' "
            "ORDER BY start_ts DESC LIMIT 20");
        EXPECT_FALSE(q.executeStep());
    }
    {
        SQLite::Statement q(*db_,
            "SELECT watch_group, COUNT(*), MAX(created_at) "
            "FROM watch_events "
            "GROUP BY watch_group "
            "ORDER BY MAX(created_at) DESC LIMIT 20");
        EXPECT_FALSE(q.executeStep());
    }
    {
        SQLite::Statement q(*db_,
            "SELECT trigger_id, trigger_type, target_id, "
            "       fired_at, status "
            "FROM trigger_history "
            "ORDER BY fired_at DESC LIMIT 20");
        EXPECT_FALSE(q.executeStep());
    }
    {
        SQLite::Statement q(*db_,
            "SELECT lc.content, lc.created_at, lc.stream "
            "FROM log_chunks lc "
            "ORDER BY lc.id DESC LIMIT 50");
        EXPECT_FALSE(q.executeStep());
    }
}

// ── Daemon-running heuristic ────────────────────────────────────────────

TEST_F(TuiDbQueryTest, DaemonRunningHeuristic_RecentActivity) {
    // No active runs, but a recent run → daemon_running = true.
    db_->exec(
        "INSERT INTO runs (run_id, target_type, target_id, target_name, "
        "  trigger_type, trigger_id, correlation_id, status, start_ts, "
        "  duration_ms) "
        "VALUES ('r-recent', 'job', 'j-1', 'Ping', "
        "  'schedule', 't-1', 'c-1', 'SUCCESS', "
        "  datetime('now', '-1 minute'), 100)");

    SQLite::Statement q(*db_,
        "SELECT COUNT(*) FROM runs "
        "WHERE start_ts > datetime('now', '-5 minutes')");
    ASSERT_TRUE(q.executeStep());
    EXPECT_GT(q.getColumn(0).getInt(), 0);
}

TEST_F(TuiDbQueryTest, DaemonRunningHeuristic_NoRecentActivity) {
    // Old run only → daemon_running = false.
    db_->exec(
        "INSERT INTO runs (run_id, target_type, target_id, target_name, "
        "  trigger_type, trigger_id, correlation_id, status, start_ts, "
        "  duration_ms) "
        "VALUES ('r-old', 'job', 'j-1', 'Ping', "
        "  'schedule', 't-1', 'c-1', 'SUCCESS', "
        "  datetime('now', '-2 hours'), 100)");

    SQLite::Statement q(*db_,
        "SELECT COUNT(*) FROM runs "
        "WHERE start_ts > datetime('now', '-5 minutes')");
    ASSERT_TRUE(q.executeStep());
    EXPECT_EQ(q.getColumn(0).getInt(), 0);
}
