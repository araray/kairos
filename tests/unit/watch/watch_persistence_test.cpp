/// tests/unit/watch/watch_persistence_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Watch persistence tests                                                  ║
// ║                                                                           ║
// ║  Verifies that InsertWatchSample and InsertWatchEvent request types       ║
// ║  are correctly processed by DBWriter and queryable from SQLite.           ║
// ║  Also tests WatchEngine persistence integration with a real DBWriter.     ║
// ║                                                                           ║
// ║  Spec reference: §16.5, §16.2                                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/watch/watch_engine.hpp"
#include "kairos/testing/fake_clock.hpp"
#include "kairos/testing/fake_filesystem.hpp"
#include "kairos/testing/fake_fs_scanner.hpp"

#include <gtest/gtest.h>

#include <SQLiteCpp/SQLiteCpp.h>

#include <chrono>
#include <stop_token>
#include <string>
#include <thread>

namespace kairos::watch {
namespace {

using namespace std::chrono_literals;

// ── Test fixture ────────────────────────────────────────────────────────

class WatchPersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create in-memory database with full schema.
        db_ = std::make_unique<SQLite::Database>(
            ":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        persist::apply_migrations(*db_, persist::get_migrations());

        // Create and start the DB writer.
        writer_ = std::make_unique<persist::DBWriter>(*db_);
        stop_ = std::make_unique<std::stop_source>();
        writer_->start(stop_->get_token());

        // Set up fake clock.
        clock_.set_now(std::chrono::system_clock::time_point{
            std::chrono::hours(24 * 365 * 56)});
        clock_.set_steady(std::chrono::steady_clock::time_point{
            std::chrono::hours(1)});
    }

    void TearDown() override {
        stop_->request_stop();
        writer_->flush();
        writer_.reset();
    }

    /// Wait for the writer to process all pending requests.
    void flush_writer() {
        std::this_thread::sleep_for(100ms);
        writer_->flush();
    }

    std::unique_ptr<SQLite::Database> db_;
    std::unique_ptr<persist::DBWriter> writer_;
    std::unique_ptr<std::stop_source> stop_;
    testing::FakeClock clock_;
};

// ── InsertWatchSample tests ─────────────────────────────────────────────

TEST_F(WatchPersistenceTest, InsertWatchSampleBasic) {
    persist::InsertWatchSample req;
    req.watch_group = "system_logs";
    req.sample_epoch = 1;
    req.file_path = "/var/log/syslog";
    req.is_dir = false;
    req.size = 1024;
    req.mtime = "2026-03-01T10:00:00Z";
    req.hash = "abc123";
    req.scan_duration_ms = 42;

    ASSERT_TRUE(writer_->enqueue(persist::DBWriteRequest{req}));
    flush_writer();

    SQLite::Statement query(*db_,
        "SELECT watch_group, sample_epoch, file_path, file_size, mtime, hash "
        "FROM watch_samples WHERE watch_group = ?");
    query.bind(1, "system_logs");
    ASSERT_TRUE(query.executeStep());

    EXPECT_EQ(query.getColumn(0).getString(), "system_logs");
    EXPECT_EQ(query.getColumn(1).getInt64(), 1);
    EXPECT_EQ(query.getColumn(2).getString(), "/var/log/syslog");
    EXPECT_EQ(query.getColumn(3).getInt64(), 1024);
    EXPECT_EQ(query.getColumn(4).getString(), "2026-03-01T10:00:00Z");
    EXPECT_EQ(query.getColumn(5).getString(), "abc123");
}

TEST_F(WatchPersistenceTest, InsertMultipleSamplesInOneEpoch) {
    for (int i = 0; i < 3; ++i) {
        persist::InsertWatchSample req;
        req.watch_group = "logs";
        req.sample_epoch = 5;
        req.file_path = "/var/log/file" + std::to_string(i) + ".log";
        req.size = 100 * (i + 1);
        req.mtime = "2026-03-01T10:00:00Z";
        ASSERT_TRUE(writer_->enqueue(persist::DBWriteRequest{req}));
    }
    flush_writer();

    SQLite::Statement count(*db_,
        "SELECT COUNT(*) FROM watch_samples WHERE watch_group = 'logs' "
        "AND sample_epoch = 5");
    ASSERT_TRUE(count.executeStep());
    EXPECT_EQ(count.getColumn(0).getInt(), 3);
}

TEST_F(WatchPersistenceTest, SampleEpochOrdering) {
    for (int epoch = 1; epoch <= 2; ++epoch) {
        persist::InsertWatchSample req;
        req.watch_group = "config";
        req.sample_epoch = epoch;
        req.file_path = "/etc/config.yaml";
        req.size = 512 * epoch;
        req.mtime = "2026-03-01T10:00:00Z";
        ASSERT_TRUE(writer_->enqueue(persist::DBWriteRequest{req}));
    }
    flush_writer();

    SQLite::Statement query(*db_,
        "SELECT file_size FROM watch_samples "
        "WHERE watch_group = 'config' "
        "ORDER BY sample_epoch DESC LIMIT 1");
    ASSERT_TRUE(query.executeStep());
    EXPECT_EQ(query.getColumn(0).getInt64(), 1024);
}

// ── InsertWatchEvent tests ──────────────────────────────────────────────

TEST_F(WatchPersistenceTest, InsertWatchEventBasic) {
    persist::InsertWatchEvent req;
    req.event_uid = "wru-abc123";
    req.watch_group = "system_logs";
    req.rule_name = "large_file_alert";
    req.event_type = "size_changed";
    req.severity = "warning";
    req.affected_files_json = "[\"/var/log/syslog\"]";
    req.sample_epoch = 5;
    req.details_json = "{\"old_size\": 100, \"new_size\": 10000}";

    ASSERT_TRUE(writer_->enqueue(persist::DBWriteRequest{req}));
    flush_writer();

    SQLite::Statement query(*db_,
        "SELECT watch_group, event_type, rule_name "
        "FROM watch_events WHERE watch_group = ?");
    query.bind(1, "system_logs");
    ASSERT_TRUE(query.executeStep());

    EXPECT_EQ(query.getColumn(0).getString(), "system_logs");
    EXPECT_EQ(query.getColumn(1).getString(), "size_changed");
    EXPECT_EQ(query.getColumn(2).getString(), "large_file_alert");
}

TEST_F(WatchPersistenceTest, MultipleEventsForSameGroup) {
    for (int i = 0; i < 5; ++i) {
        persist::InsertWatchEvent req;
        req.event_uid = "wru-evt" + std::to_string(i);
        req.watch_group = "logs";
        req.rule_name = "rule_" + std::to_string(i);
        req.event_type = "content_modified";
        req.severity = "info";
        req.affected_files_json = "[\"/var/log/app.log\"]";
        req.sample_epoch = 10;
        req.details_json = "{}";
        ASSERT_TRUE(writer_->enqueue(persist::DBWriteRequest{req}));
    }
    flush_writer();

    SQLite::Statement count(*db_,
        "SELECT COUNT(*) FROM watch_events WHERE watch_group = 'logs'");
    ASSERT_TRUE(count.executeStep());
    EXPECT_EQ(count.getColumn(0).getInt(), 5);
}

// ── WatchEngine persistence integration ─────────────────────────────────

TEST_F(WatchPersistenceTest, WatchEnginePersistsSamples) {
    testing::FakeFilesystem fs;
    testing::FakeFilesystemScanner scanner(fs);

    testing::FakeFileEntry fe;
    fe.size = 256;
    fe.mtime = std::chrono::system_clock::now();
    fs.add_file("/data/test.txt", fe);

    WatchGroupDef group;
    group.group_id = "wgr-test123";
    group.group_name = "test_group";
    group.watch_items = {"/data"};
    group.mode = WatchMode::Sample;
    group.sample_rate = std::chrono::seconds(60);
    group.max_depth = 5;

    WatchEngineConfig config;
    WatchEngine::Dependencies deps;
    deps.clock = &clock_;
    deps.scanner = &scanner;
    deps.db_writer = writer_.get();

    WatchEngine engine(config, deps, {group});

    engine::TriggerSink sink = [](engine::TriggerEvent) {};
    engine.scan_once(sink);

    flush_writer();

    SQLite::Statement count(*db_,
        "SELECT COUNT(*) FROM watch_samples WHERE watch_group = 'test_group'");
    ASSERT_TRUE(count.executeStep());
    EXPECT_GE(count.getColumn(0).getInt(), 1);
}

TEST_F(WatchPersistenceTest, WatchEnginePersistsEvents) {
    testing::FakeFilesystem fs;
    testing::FakeFilesystemScanner scanner(fs);

    testing::FakeFileEntry fe;
    fe.size = 100;
    fe.mtime = std::chrono::system_clock::now();
    fs.add_file("/data/test.txt", fe);

    WatchGroupDef group;
    group.group_id = "wgr-evtest";
    group.group_name = "event_test_group";
    group.watch_items = {"/data"};
    group.mode = WatchMode::Sample;
    group.sample_rate = std::chrono::seconds(60);
    group.max_depth = 5;

    WatchRuleDef rule;
    rule.rule_name = "catch_all";
    rule.condition = "true";
    rule.severity = "info";
    group.rules = {rule};

    WatchEngineConfig config;
    WatchEngine::Dependencies deps;
    deps.clock = &clock_;
    deps.scanner = &scanner;
    deps.db_writer = writer_.get();

    WatchEngine engine(config, deps, {group});

    engine::TriggerSink sink = [](engine::TriggerEvent) {};

    // First scan: baseline.
    engine.scan_once(sink);

    // Modify the file for the second scan.
    testing::FakeFileEntry fe2;
    fe2.size = 200;
    fe2.mtime = std::chrono::system_clock::now();
    fs.modify_file("/data/test.txt", fe2);

    // Second scan: should detect and persist events.
    engine.scan_once(sink);

    flush_writer();

    SQLite::Statement count(*db_,
        "SELECT COUNT(*) FROM watch_events "
        "WHERE watch_group = 'event_test_group'");
    ASSERT_TRUE(count.executeStep());
    EXPECT_GE(count.getColumn(0).getInt(), 1);
}

TEST_F(WatchPersistenceTest, NullDBWriterIsHandledGracefully) {
    testing::FakeFilesystem fs;
    testing::FakeFilesystemScanner scanner(fs);

    testing::FakeFileEntry fe;
    fe.size = 64;
    fe.mtime = std::chrono::system_clock::now();
    fs.add_file("/data/file.txt", fe);

    WatchGroupDef group;
    group.group_id = "wgr-null";
    group.group_name = "null_writer_group";
    group.watch_items = {"/data"};
    group.mode = WatchMode::Sample;
    group.sample_rate = std::chrono::seconds(60);

    WatchEngineConfig config;
    WatchEngine::Dependencies deps;
    deps.clock = &clock_;
    deps.scanner = &scanner;
    deps.db_writer = nullptr;

    WatchEngine engine(config, deps, {group});

    engine::TriggerSink sink = [](engine::TriggerEvent) {};
    EXPECT_NO_THROW(engine.scan_once(sink));
}

}  // namespace
}  // namespace kairos::watch
