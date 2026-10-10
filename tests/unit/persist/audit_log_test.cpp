/// tests/unit/persist/audit_log_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Audit log tests — migration v3, DBWriter round-trip, reader queries      ║
// ║                                                                          ║
// ║  Roadmap §15.14 (audit log), §3.8.2 (write API safety).                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/database.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/persist/query_reader.hpp"

#include <gtest/gtest.h>

#include <thread>

using namespace kairos::persist;

class AuditLogTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = open_database(":memory:");
        apply_migrations(*db_, get_migrations());
    }

    std::unique_ptr<SQLite::Database> db_;
};

TEST_F(AuditLogTest, MigrationCreatesTable) {
    SQLite::Statement q(*db_,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
        "AND name='audit_log'");
    ASSERT_TRUE(q.executeStep());
    EXPECT_EQ(q.getColumn(0).getInt(), 1);
    EXPECT_EQ(get_schema_version(*db_),
              static_cast<int>(get_migrations().size()));
}

TEST_F(AuditLogTest, WriterInsertsAndReaderReturnsNewestFirst) {
    DBWriter writer(*db_);
    std::stop_source stop;
    writer.start(stop.get_token());

    writer.enqueue(InsertAuditEntry{
        .actor = "http",
        .action = "create_workflow",
        .target = "deploy",
        .detail_json = R"({"source":"api"})",
    });
    writer.enqueue(InsertAuditEntry{
        .actor = "cli",
        .action = "cancel_run",
        .target = "run-123",
        .detail_json = "{}",
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.request_stop();
    writer.flush();

    QueryReader reader(*db_);
    auto rows = reader.query_audit_log(10);
    ASSERT_EQ(rows.size(), 2u);
    // Newest first.
    EXPECT_EQ(rows[0].action, "cancel_run");
    EXPECT_EQ(rows[0].actor, "cli");
    EXPECT_EQ(rows[0].target, "run-123");
    EXPECT_EQ(rows[1].action, "create_workflow");
    EXPECT_EQ(rows[1].actor, "http");
    EXPECT_FALSE(rows[0].timestamp.empty());
}

TEST_F(AuditLogTest, EmptyLogReturnsEmpty) {
    QueryReader reader(*db_);
    EXPECT_TRUE(reader.query_audit_log(10).empty());
}

TEST_F(AuditLogTest, LimitIsRespected) {
    DBWriter writer(*db_);
    std::stop_source stop;
    writer.start(stop.get_token());

    for (int i = 0; i < 5; ++i) {
        writer.enqueue(InsertAuditEntry{
            .actor = "http",
            .action = "reload_config",
            .target = "",
            .detail_json = "",
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.request_stop();
    writer.flush();

    QueryReader reader(*db_);
    EXPECT_EQ(reader.query_audit_log(3).size(), 3u);
    EXPECT_EQ(reader.query_audit_log(50).size(), 5u);
}
