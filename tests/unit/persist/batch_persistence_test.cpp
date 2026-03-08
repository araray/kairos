/// tests/unit/persist/batch_persistence_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Batch persistence tests — BatchInsertWatchSamples, PruneWatchSamples   ║
// ║                                                                          ║
// ║  Tests the optimized batch insert path (all entries in one transaction) ║
// ║  and retention pruning for watch samples.                               ║
// ║                                                                          ║
// ║  Spec reference: §16.5, §16.8                                          ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/database.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/persist/query_reader.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

using namespace kairos::persist;

namespace {

/// Helper: create an in-memory database with schema.
std::unique_ptr<SQLite::Database> create_test_db() {
    auto db = std::make_unique<SQLite::Database>(
        ":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db->exec("PRAGMA journal_mode = WAL");
    db->exec("PRAGMA foreign_keys = ON");

    // Run migrations to create the schema.
    apply_migrations(*db, get_migrations());
    return db;
}

/// Count rows in watch_samples for a given group.
int count_samples(SQLite::Database& db, const std::string& group) {
    SQLite::Statement query(db,
        "SELECT COUNT(*) FROM watch_samples WHERE watch_group = ?");
    query.bind(1, group);
    query.executeStep();
    return query.getColumn(0).getInt();
}

/// Count distinct epochs for a given group.
int count_epochs(SQLite::Database& db, const std::string& group) {
    SQLite::Statement query(db,
        "SELECT COUNT(DISTINCT sample_epoch) FROM watch_samples "
        "WHERE watch_group = ?");
    query.bind(1, group);
    query.executeStep();
    return query.getColumn(0).getInt();
}

}  // anonymous namespace

// ── BatchInsertWatchSamples tests ────────────────────────────────────────

TEST(BatchPersistenceTest, BatchInsertEmpty) {
    auto db = create_test_db();
    DBWriter writer(*db);
    std::stop_source stop;
    writer.start(stop.get_token());

    BatchInsertWatchSamples batch;
    batch.watch_group = "test-group";
    batch.sample_epoch = 1;
    batch.scan_duration_ms = 50;
    // No entries.

    writer.enqueue(std::move(batch));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer.flush();

    EXPECT_EQ(count_samples(*db, "test-group"), 0);

    stop.request_stop();
}

TEST(BatchPersistenceTest, BatchInsertMultipleEntries) {
    auto db = create_test_db();
    DBWriter writer(*db);
    std::stop_source stop;
    writer.start(stop.get_token());

    BatchInsertWatchSamples batch;
    batch.watch_group = "logs";
    batch.sample_epoch = 1;
    batch.scan_duration_ms = 120;
    batch.entries = {
        {"/var/log/app.log", false, 1024, "2026-03-08T10:00:00Z", "abc123"},
        {"/var/log/error.log", false, 512, "2026-03-08T09:30:00Z", "def456"},
        {"/var/log/debug.log", false, 2048, "2026-03-08T10:01:00Z", "ghi789"},
    };

    writer.enqueue(std::move(batch));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer.flush();

    EXPECT_EQ(count_samples(*db, "logs"), 3);
    EXPECT_EQ(count_epochs(*db, "logs"), 1);

    stop.request_stop();
}

TEST(BatchPersistenceTest, BatchInsertMultipleEpochs) {
    auto db = create_test_db();
    DBWriter writer(*db);
    std::stop_source stop;
    writer.start(stop.get_token());

    // Insert epoch 1.
    {
        BatchInsertWatchSamples b;
        b.watch_group = "data";
        b.sample_epoch = 1;
        b.scan_duration_ms = 50;
        b.entries = {
            {"/data/a.csv", false, 100, "2026-03-08T10:00:00Z", "aaa"},
            {"/data/b.csv", false, 200, "2026-03-08T10:00:00Z", "bbb"},
        };
        writer.enqueue(std::move(b));
    }

    // Insert epoch 2.
    {
        BatchInsertWatchSamples b;
        b.watch_group = "data";
        b.sample_epoch = 2;
        b.scan_duration_ms = 45;
        b.entries = {
            {"/data/a.csv", false, 150, "2026-03-08T11:00:00Z", "aaa2"},
            {"/data/b.csv", false, 250, "2026-03-08T11:00:00Z", "bbb2"},
            {"/data/c.csv", false, 300, "2026-03-08T11:00:00Z", "ccc"},
        };
        writer.enqueue(std::move(b));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    writer.flush();

    EXPECT_EQ(count_samples(*db, "data"), 5);
    EXPECT_EQ(count_epochs(*db, "data"), 2);

    stop.request_stop();
}

// ── PruneWatchSamples tests ─────────────────────────────────────────────

TEST(BatchPersistenceTest, PruneKeepsRecentEpochs) {
    auto db = create_test_db();
    DBWriter writer(*db);
    std::stop_source stop;
    writer.start(stop.get_token());

    // Insert 5 epochs with 2 files each.
    for (int epoch = 1; epoch <= 5; ++epoch) {
        BatchInsertWatchSamples b;
        b.watch_group = "prune-test";
        b.sample_epoch = epoch;
        b.scan_duration_ms = 10;
        b.entries = {
            {"/file/a.txt", false, epoch * 100, "2026-03-08T10:00:00Z", "h"},
            {"/file/b.txt", false, epoch * 200, "2026-03-08T10:00:00Z", "h"},
        };
        writer.enqueue(std::move(b));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    writer.flush();

    // Verify: 5 epochs × 2 files = 10 rows.
    EXPECT_EQ(count_samples(*db, "prune-test"), 10);
    EXPECT_EQ(count_epochs(*db, "prune-test"), 5);

    // Prune to keep only 3 most-recent epochs.
    PruneWatchSamples prune;
    prune.watch_group = "prune-test";
    prune.max_epochs = 3;
    writer.enqueue(std::move(prune));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    writer.flush();

    // Should now have 3 epochs × 2 files = 6 rows.
    EXPECT_EQ(count_epochs(*db, "prune-test"), 3);
    EXPECT_EQ(count_samples(*db, "prune-test"), 6);

    // Verify the kept epochs are 3, 4, 5 (the most recent).
    SQLite::Statement query(*db,
        "SELECT DISTINCT sample_epoch FROM watch_samples "
        "WHERE watch_group = ? ORDER BY sample_epoch ASC");
    query.bind(1, "prune-test");
    std::vector<int64_t> remaining_epochs;
    while (query.executeStep()) {
        remaining_epochs.push_back(query.getColumn(0).getInt64());
    }
    ASSERT_EQ(remaining_epochs.size(), 3u);
    EXPECT_EQ(remaining_epochs[0], 3);
    EXPECT_EQ(remaining_epochs[1], 4);
    EXPECT_EQ(remaining_epochs[2], 5);

    stop.request_stop();
}

TEST(BatchPersistenceTest, PruneWithFewerEpochsThanLimit) {
    auto db = create_test_db();
    DBWriter writer(*db);
    std::stop_source stop;
    writer.start(stop.get_token());

    // Insert 2 epochs.
    for (int epoch = 1; epoch <= 2; ++epoch) {
        BatchInsertWatchSamples b;
        b.watch_group = "few-epochs";
        b.sample_epoch = epoch;
        b.scan_duration_ms = 10;
        b.entries = {{"/file.txt", false, 100, "2026-03-08T10:00:00Z", "h"}};
        writer.enqueue(std::move(b));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer.flush();

    // Prune with max_epochs = 5 (more than we have).
    PruneWatchSamples prune;
    prune.watch_group = "few-epochs";
    prune.max_epochs = 5;
    writer.enqueue(std::move(prune));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer.flush();

    // Nothing should be pruned.
    EXPECT_EQ(count_epochs(*db, "few-epochs"), 2);
    EXPECT_EQ(count_samples(*db, "few-epochs"), 2);

    stop.request_stop();
}

TEST(BatchPersistenceTest, PruneDoesNotAffectOtherGroups) {
    auto db = create_test_db();
    DBWriter writer(*db);
    std::stop_source stop;
    writer.start(stop.get_token());

    // Insert epochs for two different groups.
    for (int epoch = 1; epoch <= 4; ++epoch) {
        BatchInsertWatchSamples b1;
        b1.watch_group = "group-A";
        b1.sample_epoch = epoch;
        b1.scan_duration_ms = 10;
        b1.entries = {{"/a.txt", false, 100, "2026-03-08T10:00:00Z", "h"}};
        writer.enqueue(std::move(b1));

        BatchInsertWatchSamples b2;
        b2.watch_group = "group-B";
        b2.sample_epoch = epoch;
        b2.scan_duration_ms = 10;
        b2.entries = {{"/b.txt", false, 200, "2026-03-08T10:00:00Z", "h"}};
        writer.enqueue(std::move(b2));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    writer.flush();

    // Prune group-A to 2 epochs.
    PruneWatchSamples prune;
    prune.watch_group = "group-A";
    prune.max_epochs = 2;
    writer.enqueue(std::move(prune));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer.flush();

    // group-A: 2 epochs remaining.
    EXPECT_EQ(count_epochs(*db, "group-A"), 2);
    // group-B: untouched, still 4 epochs.
    EXPECT_EQ(count_epochs(*db, "group-B"), 4);

    stop.request_stop();
}
