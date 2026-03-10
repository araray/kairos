/// tests/unit/persist/retention_auto_prune_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Retention auto-prune tests                                               ║
// ║                                                                           ║
// ║  Validates the PruneOlderThan and PruneWatchSamples handlers in         ║
// ║  DBWriter, and verifies that pruning correctly retains recent records.   ║
// ║                                                                           ║
// ║  NOTE: cutoff_date must be an actual ISO 8601 timestamp, NOT a SQL       ║
// ║  expression — SQLite parameters are literal values, not evaluated SQL.   ║
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
#include <iomanip>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>

namespace kairos::persist {
namespace {

/// Format a time_point as ISO 8601 UTC string for SQLite comparison.
std::string to_iso8601(std::chrono::system_clock::time_point tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

/// Compute an ISO 8601 timestamp for N days ago.
std::string iso_days_ago(int days) {
    auto now = std::chrono::system_clock::now();
    auto ago = now - std::chrono::hours(days * 24);
    return to_iso8601(ago);
}

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

    /// Insert a run with a specific ISO 8601 timestamp.
    void insert_run(const std::string& run_id, const std::string& ts) {
        // Use raw SQL for test setup (bypass DBWriter for immediacy).
        std::string sql =
            "INSERT INTO runs (run_id, target_type, target_id, "
            "target_name, trigger_type, trigger_id, correlation_id, "
            "status, start_ts, created_at) VALUES ("
            "'" + run_id + "', 'workflow', 'wf-1', 'test-wf', "
            "'manual', 'trg-1', 'corr-1', 'SUCCESS', "
            "'" + ts + "', '" + ts + "')";
        db_->exec(sql);
    }

    /// Insert a watch sample with a specific epoch.
    /// Schema: id (auto), watch_group, sample_epoch, file_path,
    ///         file_size, mtime, hash, scan_duration_ms, created_at
    void insert_watch_sample(const std::string& watch_group,
                              int64_t epoch,
                              const std::string& file_path) {
        std::string sql =
            "INSERT INTO watch_samples (watch_group, "
            "sample_epoch, file_path, file_size, mtime, hash) VALUES ("
            "'" + watch_group + "', " +
            std::to_string(epoch) + ", '" + file_path + "', 100, "
            "'2024-01-01', 'abc')";
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
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
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
    insert_run("run-1d",  iso_days_ago(1));
    insert_run("run-10d", iso_days_ago(10));
    insert_run("run-29d", iso_days_ago(29));
    insert_run("run-60d", iso_days_ago(60));
    insert_run("run-90d", iso_days_ago(90));
    EXPECT_EQ(count_rows("runs"), 5);

    // Prune runs older than 30 days.
    // cutoff_date is an actual ISO timestamp, NOT a SQL expression.
    writer_->enqueue(PruneOlderThan{
        .cutoff_date = iso_days_ago(30),
    });
    flush_writer();

    // run-1d, run-10d, run-29d should survive (within 30 days).
    // run-60d, run-90d should be deleted.
    EXPECT_EQ(count_rows("runs"), 3);
}

TEST_F(RetentionPruneTest, PruneNoRunsWhenAllRecent) {
    insert_run("run-1", iso_days_ago(1));
    insert_run("run-2", iso_days_ago(2));
    EXPECT_EQ(count_rows("runs"), 2);

    writer_->enqueue(PruneOlderThan{
        .cutoff_date = iso_days_ago(30),
    });
    flush_writer();

    EXPECT_EQ(count_rows("runs"), 2);
}

TEST_F(RetentionPruneTest, PruneAllRunsWhenAllOld) {
    insert_run("run-old1", iso_days_ago(100));
    insert_run("run-old2", iso_days_ago(200));
    EXPECT_EQ(count_rows("runs"), 2);

    writer_->enqueue(PruneOlderThan{
        .cutoff_date = iso_days_ago(7),
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
    insert_run("recent", iso_days_ago(5));
    insert_run("old",    iso_days_ago(100));

    // Set up watch samples.
    for (int e = 1; e <= 10; ++e) {
        insert_watch_sample("data", e, "/data/file.csv");
    }

    EXPECT_EQ(count_rows("runs"), 2);
    EXPECT_EQ(count_rows("watch_samples"), 10);

    // Simulate daemon auto-prune cycle:
    //   1. Prune runs older than 30 days.
    writer_->enqueue(PruneOlderThan{
        .cutoff_date = iso_days_ago(30),
    });
    flush_writer();

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
