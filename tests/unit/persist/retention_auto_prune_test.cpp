/// tests/unit/persist/retention_auto_prune_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Retention auto-prune tests                                               ║
// ║                                                                           ║
// ║  Validates the PruneOlderThan and PruneWatchSamples handlers in         ║
// ║  DBWriter, and verifies that the auto-prune timer logic correctly        ║
// ║  calculates cutoff dates and enqueues prune requests.                    ║
// ║                                                                           ║
// ║  Spec reference: §16.8                                                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/database.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/persist/query_reader.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <stop_token>
#include <string>
#include <thread>

namespace kairos::persist {
namespace {

/// Helper to create a temporary database for testing.
class RetentionPruneTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = std::filesystem::temp_directory_path() /
                   ("kairos_retention_test_" +
                    std::to_string(std::chrono::steady_clock::now()
                        .time_since_epoch().count()) +
                    ".db");
        db_ = open_database(db_path_);

        // Start DBWriter.
        writer_ = std::make_unique<DBWriter>(*db_, DBWriterConfig{});
        writer_->start(stop_source_.get_token());
    }

    void TearDown() override {
        stop_source_.request_stop();
        if (writer_) writer_->stop();
        writer_.reset();
        db_.reset();
        std::error_code ec;
        std::filesystem::remove(db_path_, ec);
    }

    /// Insert a run with a specific timestamp (days ago).
    void insert_run_days_ago(const std::string& run_id, int days_ago) {
        std::string ts_sql =
            "datetime('now', '-" + std::to_string(days_ago) + " days')";

        // Use raw SQL for test setup (bypass DBWriter for immediacy).
        std::string sql =
            "INSERT INTO runs (run_id, target_type, target_id, "
            "target_name, trigger_type, trigger_id, correlation_id, "
            "status, start_ts, created_at) VALUES ("
            "'" + run_id + "', 'workflow', 'wf-1', 'test-wf', "
            "'manual', 'trg-1', 'corr-1', 'SUCCESS', " +
            ts_sql + ", " + ts_sql + ")";
        db_->exec(sql);
    }

    /// Insert a watch event with a specific timestamp (days ago).
    void insert_watch_event_days_ago(const std::string& event_id,
                                      int days_ago) {
        std::string ts_sql =
            "datetime('now', '-" + std::to_string(days_ago) + " days')";

        std::string sql =
            "INSERT INTO watch_events (event_id, watch_group, "
            "event_type, file_path, created_at) VALUES ("
            "'" + event_id + "', 'logs', 'modified', '/tmp/test.log', " +
            ts_sql + ")";
        db_->exec(sql);
    }

    /// Insert a watch sample with a specific epoch.
    void insert_watch_sample(const std::string& watch_group,
                              int64_t epoch,
                              const std::string& file_path) {
        std::string sql =
            "INSERT INTO watch_samples (sample_id, watch_group, "
            "sample_epoch, file_path, file_size, mtime, hash_value, "
            "created_at) VALUES ("
            "'s-" + std::to_string(epoch) + "-" + file_path + "', "
            "'" + watch_group + "', " +
            std::to_string(epoch) + ", '" + file_path + "', 100, "
            "'2024-01-01', 'abc', datetime('now'))";
        db_->exec(sql);
    }

    /// Count rows in a table.
    int count_rows(const std::string& table) {
        SQLite::Statement q(*db_,
            "SELECT COUNT(*) FROM " + table);
        if (q.executeStep()) {
            return q.getColumn(0).getInt();
        }
        return -1;
    }

    /// Flush the DBWriter and wait for it to process.
    void flush_writer() {
        writer_->flush();
        // Give writer thread time to process.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::filesystem::path db_path_;
    std::unique_ptr<SQLite::Database> db_;
    std::unique_ptr<DBWriter> writer_;
    std::stop_source stop_source_;
};

// ═══════════════════════════════════════════════════════════════════════
// PruneOlderThan tests
// ═══════════════════════════════════════════════════════════════════════

TEST_F(RetentionPruneTest, PruneOldRuns) {
    // Insert runs at various ages.
    insert_run_days_ago("run-10d", 10);
    insert_run_days_ago("run-30d", 30);
    insert_run_days_ago("run-60d", 60);
    insert_run_days_ago("run-90d", 90);
    insert_run_days_ago("run-1d", 1);
    EXPECT_EQ(count_rows("runs"), 5);

    // Prune runs older than 30 days.
    writer_->enqueue(PruneOlderThan{
        .cutoff_date = "datetime('now', '-30 days')",
    });
    flush_writer();

    // Only runs within 30 days should remain.
    int remaining = count_rows("runs");
    // run-10d and run-1d should survive.
    // run-30d is borderline — depends on exact timing.
    // run-60d and run-90d should be deleted.
    EXPECT_LE(remaining, 3);
    EXPECT_GE(remaining, 2);
}

TEST_F(RetentionPruneTest, PruneNoRunsWhenAllRecent) {
    insert_run_days_ago("run-1", 1);
    insert_run_days_ago("run-2", 2);
    EXPECT_EQ(count_rows("runs"), 2);

    writer_->enqueue(PruneOlderThan{
        .cutoff_date = "datetime('now', '-30 days')",
    });
    flush_writer();

    EXPECT_EQ(count_rows("runs"), 2);
}

TEST_F(RetentionPruneTest, PruneAllRunsWhenAllOld) {
    insert_run_days_ago("run-old1", 100);
    insert_run_days_ago("run-old2", 200);
    EXPECT_EQ(count_rows("runs"), 2);

    writer_->enqueue(PruneOlderThan{
        .cutoff_date = "datetime('now', '-7 days')",
    });
    flush_writer();

    EXPECT_EQ(count_rows("runs"), 0);
}

// ═══════════════════════════════════════════════════════════════════════
// PruneWatchSamples tests
// ═══════════════════════════════════════════════════════════════════════

TEST_F(RetentionPruneTest, PruneWatchSamplesKeepsMaxEpochs) {
    // Insert samples across 5 epochs for one group.
    for (int epoch = 1; epoch <= 5; ++epoch) {
        insert_watch_sample("logs", epoch, "/tmp/a.log");
        insert_watch_sample("logs", epoch, "/tmp/b.log");
    }
    // 5 epochs × 2 files = 10 rows.
    EXPECT_EQ(count_rows("watch_samples"), 10);

    // Keep only 3 most recent epochs.
    writer_->enqueue(PruneWatchSamples{
        .watch_group = "logs",
        .max_epochs = 3,
    });
    flush_writer();

    // Epochs 3, 4, 5 survive = 6 rows.
    int remaining = count_rows("watch_samples");
    EXPECT_EQ(remaining, 6);
}

TEST_F(RetentionPruneTest, PruneWatchSamplesNothingToDelete) {
    insert_watch_sample("logs", 1, "/tmp/a.log");
    insert_watch_sample("logs", 2, "/tmp/a.log");
    EXPECT_EQ(count_rows("watch_samples"), 2);

    // Keep up to 10 — nothing to prune.
    writer_->enqueue(PruneWatchSamples{
        .watch_group = "logs",
        .max_epochs = 10,
    });
    flush_writer();

    EXPECT_EQ(count_rows("watch_samples"), 2);
}

TEST_F(RetentionPruneTest, PruneWatchSamplesMultipleGroups) {
    // Group A: 5 epochs.
    for (int e = 1; e <= 5; ++e) {
        insert_watch_sample("group_a", e, "/tmp/a.log");
    }
    // Group B: 3 epochs.
    for (int e = 1; e <= 3; ++e) {
        insert_watch_sample("group_b", e, "/tmp/b.log");
    }
    EXPECT_EQ(count_rows("watch_samples"), 8);

    // Prune group_a to keep 2.
    writer_->enqueue(PruneWatchSamples{
        .watch_group = "group_a",
        .max_epochs = 2,
    });
    flush_writer();

    // Group A: 2 remaining, Group B: 3 unchanged = 5 total.
    EXPECT_EQ(count_rows("watch_samples"), 5);
}

// ═══════════════════════════════════════════════════════════════════════
// Combined prune (simulates daemon auto-prune cycle)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(RetentionPruneTest, FullRetentionCycle) {
    // Set up old and recent runs.
    insert_run_days_ago("recent", 5);
    insert_run_days_ago("old", 100);

    // Set up watch samples.
    for (int e = 1; e <= 10; ++e) {
        insert_watch_sample("data", e, "/data/file.csv");
    }

    EXPECT_EQ(count_rows("runs"), 2);
    EXPECT_EQ(count_rows("watch_samples"), 10);

    // Simulate daemon auto-prune cycle:
    //   1. Prune runs older than 30 days.
    writer_->enqueue(PruneOlderThan{
        .cutoff_date = "datetime('now', '-30 days')",
    });
    //   2. Prune watch samples to keep 5 most recent.
    writer_->enqueue(PruneWatchSamples{
        .watch_group = "data",
        .max_epochs = 5,
    });
    flush_writer();

    // recent run survives, old run deleted.
    EXPECT_EQ(count_rows("runs"), 1);
    // 5 most recent sample epochs survive.
    EXPECT_EQ(count_rows("watch_samples"), 5);
}

}  // anonymous namespace
}  // namespace kairos::persist
