/// tests/unit/persist/run_stats_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  QueryReader::query_run_stats unit tests                                 ║
// ║                                                                          ║
// ║  Tests:                                                                  ║
// ║    - Empty database returns zero stats                                   ║
// ║    - Stats with runs inserted                                           ║
// ║    - Runs today count (24h window)                                      ║
// ║    - Failure count                                                       ║
// ║    - Active (running) count                                              ║
// ║    - Database size query                                                 ║
// ║    - ActiveRunTracker::total_active()                                    ║
// ║                                                                          ║
// ║  Spec reference: §16.6, §23.4                                           ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/database.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/persist/query_reader.hpp"
#include "kairos/engine/trigger_types.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

// ── Test fixture ─────────────────────────────────────────────────────────

class RunStatsTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = fs::temp_directory_path() / "kairos_run_stats_test.db";
        fs::remove(db_path_);
        db_ = kairos::persist::open_database(db_path_);
    }

    void TearDown() override {
        db_.reset();
        fs::remove(db_path_);
    }

    /// Insert a run row directly for testing.
    void insert_run(const std::string& run_id,
                    const std::string& status,
                    const std::string& created_at = "") {
        std::string ts = created_at;
        if (ts.empty()) {
            ts = "datetime('now')";
        } else {
            ts = "'" + ts + "'";
        }

        std::string sql =
            "INSERT INTO runs (run_id, target_type, target_id, "
            "target_name, trigger_type, trigger_id, correlation_id, "
            "status, start_ts, created_at) "
            "VALUES ('" + run_id + "', 'workflow', 'wfl-test', "
            "'test', 'manual', 'trg-test', 'corr-test', "
            "'" + status + "', " + ts + ", " + ts + ")";
        db_->exec(sql);
    }

    fs::path db_path_;
    std::unique_ptr<SQLite::Database> db_;
};

// ── Tests ────────────────────────────────────────────────────────────────

TEST_F(RunStatsTest, EmptyDatabaseReturnsZeros) {
    kairos::persist::QueryReader reader(*db_);
    auto stats = reader.query_run_stats();

    EXPECT_EQ(stats.total_runs, 0);
    EXPECT_EQ(stats.runs_today, 0);
    EXPECT_EQ(stats.failures_today, 0);
    EXPECT_EQ(stats.active_runs, 0);
}

TEST_F(RunStatsTest, TotalRunsCount) {
    kairos::persist::QueryReader reader(*db_);

    insert_run("run-1", "SUCCESS");
    insert_run("run-2", "FAILURE");
    insert_run("run-3", "SUCCESS");

    auto stats = reader.query_run_stats();
    EXPECT_EQ(stats.total_runs, 3);
}

TEST_F(RunStatsTest, RunsTodayCount) {
    kairos::persist::QueryReader reader(*db_);

    // Recent runs (within 24h).
    insert_run("run-1", "SUCCESS");
    insert_run("run-2", "SUCCESS");

    // Old run (2 days ago).
    insert_run("run-old", "SUCCESS",
               "2020-01-01T00:00:00Z");

    auto stats = reader.query_run_stats();
    EXPECT_EQ(stats.total_runs, 3);
    EXPECT_EQ(stats.runs_today, 2);  // Only recent ones.
}

TEST_F(RunStatsTest, FailuresTodayCount) {
    kairos::persist::QueryReader reader(*db_);

    insert_run("run-1", "SUCCESS");
    insert_run("run-2", "FAILURE");
    insert_run("run-3", "FAILURE");
    insert_run("run-4", "SUCCESS");

    auto stats = reader.query_run_stats();
    EXPECT_EQ(stats.failures_today, 2);
}

TEST_F(RunStatsTest, ActiveRunsCount) {
    kairos::persist::QueryReader reader(*db_);

    insert_run("run-1", "RUNNING");
    insert_run("run-2", "SUCCESS");
    insert_run("run-3", "RUNNING");

    auto stats = reader.query_run_stats();
    EXPECT_EQ(stats.active_runs, 2);
}

TEST_F(RunStatsTest, MixedScenario) {
    kairos::persist::QueryReader reader(*db_);

    insert_run("r1", "SUCCESS");
    insert_run("r2", "FAILURE");
    insert_run("r3", "RUNNING");
    insert_run("r4", "CANCELLED");
    insert_run("r-old", "FAILURE", "2020-01-01T00:00:00Z");

    auto stats = reader.query_run_stats();
    EXPECT_EQ(stats.total_runs, 5);
    EXPECT_EQ(stats.runs_today, 4);      // 4 recent.
    EXPECT_EQ(stats.failures_today, 1);  // 1 recent failure.
    EXPECT_EQ(stats.active_runs, 1);     // 1 running.
}

// ── Database size query ──────────────────────────────────────────────────

TEST_F(RunStatsTest, QueryDbSizeReturnsPositive) {
    auto size = kairos::persist::QueryReader::query_db_size(db_path_);
    EXPECT_GT(size, 0);
}

TEST_F(RunStatsTest, QueryDbSizeNonexistentReturnsZero) {
    auto size = kairos::persist::QueryReader::query_db_size(
        "/tmp/nonexistent_kairos_test.db");
    EXPECT_EQ(size, 0);
}

// ── ActiveRunTracker::total_active() ─────────────────────────────────────

TEST_F(RunStatsTest, ActiveRunTrackerTotalActiveEmpty) {
    kairos::engine::ActiveRunTracker tracker;
    EXPECT_EQ(tracker.total_active(), 0);
}

TEST_F(RunStatsTest, ActiveRunTrackerTotalActive) {
    kairos::engine::ActiveRunTracker tracker;

    tracker.increment("wfl-a");
    tracker.increment("wfl-a");
    tracker.increment("wfl-b");

    EXPECT_EQ(tracker.total_active(), 3);

    tracker.decrement("wfl-a");
    EXPECT_EQ(tracker.total_active(), 2);

    tracker.decrement("wfl-a");
    tracker.decrement("wfl-b");
    EXPECT_EQ(tracker.total_active(), 0);
}

TEST_F(RunStatsTest, ActiveRunTrackerTotalActiveAfterReset) {
    kairos::engine::ActiveRunTracker tracker;

    tracker.increment("wfl-a");
    tracker.increment("wfl-b");
    EXPECT_EQ(tracker.total_active(), 2);

    tracker.reset();
    EXPECT_EQ(tracker.total_active(), 0);
}

}  // anonymous namespace
