/// tests/unit/persist/watch_events_query_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Watch events query tests — QueryReader::query_watch_events             ║
// ║                                                                          ║
// ║  Tests the SQLite-backed event query used by CLI `events list` and      ║
// ║  the MCP getEvents tool.                                                ║
// ║                                                                          ║
// ║  Spec reference: §16.6, §23.2                                          ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/database.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/persist/query_reader.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stop_token>
#include <string>
#include <thread>

using namespace kairos::persist;

namespace {

std::unique_ptr<SQLite::Database> create_test_db() {
    auto db = std::make_unique<SQLite::Database>(
        ":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db->exec("PRAGMA journal_mode = WAL");
    db->exec("PRAGMA foreign_keys = ON");
    apply_migrations(*db, get_migrations());
    return db;
}

/// Helper: insert a watch event directly for testing.
/// Maps InsertWatchEvent fields to actual v1 schema columns:
///   file_path → affected_files_json
///   action_taken → severity
void insert_event(SQLite::Database& db,
                  const std::string& group,
                  const std::string& rule,
                  const std::string& type,
                  const std::string& severity) {
    SQLite::Statement stmt(db,
        "INSERT INTO watch_events "
        "(watch_group, rule_name, event_type, action_taken, "
        "file_path, details_json) "
        "VALUES (?, ?, ?, ?, '[]', '{}')");
    stmt.bind(1, group);
    stmt.bind(2, rule);
    stmt.bind(3, type);
    stmt.bind(4, severity);
    stmt.exec();
}

}  // anonymous namespace

// ── Basic query tests ───────────────────────────────────────────────────

TEST(WatchEventsQueryTest, EmptyDatabaseReturnsEmpty) {
    auto db = create_test_db();
    QueryReader reader(*db);

    auto events = reader.query_watch_events(10);
    EXPECT_TRUE(events.empty());
}

TEST(WatchEventsQueryTest, QueryReturnsInsertedEvents) {
    auto db = create_test_db();

    insert_event(*db, "logs", "size-alert", "threshold_exceeded",
                 "warning");
    insert_event(*db, "logs", "new-files", "files_created",
                 "info");

    QueryReader reader(*db);
    auto events = reader.query_watch_events(10);

    ASSERT_EQ(events.size(), 2u);
    // Most recent first (ORDER BY created_at DESC).
    // Both have the same created_at (sub-second), so order may vary.
    // Check that both events are present.
    EXPECT_EQ(events[0].watch_group, "logs");
    EXPECT_EQ(events[1].watch_group, "logs");
}

TEST(WatchEventsQueryTest, LimitRespectsMaxCount) {
    auto db = create_test_db();

    for (int i = 0; i < 10; ++i) {
        insert_event(*db, "group", "rule", "type", "info");
    }

    QueryReader reader(*db);
    auto events = reader.query_watch_events(3);

    EXPECT_EQ(events.size(), 3u);
}

// ── Group filter tests ──────────────────────────────────────────────────

TEST(WatchEventsQueryTest, FilterByWatchGroup) {
    auto db = create_test_db();

    insert_event(*db, "logs", "rule-a", "type", "info");
    insert_event(*db, "data", "rule-b", "type", "info");
    insert_event(*db, "logs", "rule-c", "type", "warning");
    insert_event(*db, "data", "rule-d", "type", "critical");

    QueryReader reader(*db);

    auto logs_events = reader.query_watch_events(10, "logs");
    ASSERT_EQ(logs_events.size(), 2u);
    EXPECT_EQ(logs_events[0].watch_group, "logs");
    EXPECT_EQ(logs_events[1].watch_group, "logs");

    auto data_events = reader.query_watch_events(10, "data");
    ASSERT_EQ(data_events.size(), 2u);
    EXPECT_EQ(data_events[0].watch_group, "data");
}

TEST(WatchEventsQueryTest, FilterByNonexistentGroupReturnsEmpty) {
    auto db = create_test_db();

    insert_event(*db, "logs", "rule", "type", "info");

    QueryReader reader(*db);
    auto events = reader.query_watch_events(10, "nonexistent");
    EXPECT_TRUE(events.empty());
}

// ── Field mapping tests ─────────────────────────────────────────────────

TEST(WatchEventsQueryTest, AllFieldsAreMapped) {
    auto db = create_test_db();

    // Insert directly with all columns to test field mapping.
    SQLite::Statement stmt(*db,
        "INSERT INTO watch_events "
        "(watch_group, rule_name, event_type, action_taken, "
        "file_path, details_json) "
        "VALUES (?, ?, ?, ?, ?, ?)");
    stmt.bind(1, "my-group");
    stmt.bind(2, "my-rule");
    stmt.bind(3, "files_modified");
    stmt.bind(4, "critical");
    stmt.bind(5, "[\"a.txt\",\"b.txt\"]");
    stmt.bind(6, "{\"count\":5}");
    stmt.exec();

    QueryReader reader(*db);
    auto events = reader.query_watch_events(1);

    ASSERT_EQ(events.size(), 1u);
    const auto& e = events[0];
    EXPECT_FALSE(e.event_uid.empty());  // Auto-generated ID.
    EXPECT_EQ(e.watch_group, "my-group");
    EXPECT_EQ(e.rule_name, "my-rule");
    EXPECT_EQ(e.event_type, "files_modified");
    EXPECT_EQ(e.severity, "critical");
    EXPECT_EQ(e.affected_files_json, "[\"a.txt\",\"b.txt\"]");
    EXPECT_EQ(e.details_json, "{\"count\":5}");
    EXPECT_FALSE(e.created_at.empty());  // Should have a timestamp.
}
