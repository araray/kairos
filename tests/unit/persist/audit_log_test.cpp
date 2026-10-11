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

// ═══════════════════════════════════════════════════════════════════════════
//  §15.6 run pinning
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(AuditLogTest, PinnedRunsSurvivePrune) {
    // Two old runs: one pinned, one not.
    {
        SQLite::Statement a(*db_,
            "INSERT INTO runs (run_id, target_type, target_id, "
            "target_name, trigger_type, trigger_id, correlation_id, "
            "status, start_ts, pinned) VALUES "
            "('run-pin-me','workflow','w','w','manual','t','c',"
            "'SUCCESS','2020-01-01T00:00:00Z',1)");
        a.exec();
        SQLite::Statement b(*db_,
            "INSERT INTO runs (run_id, target_type, target_id, "
            "target_name, trigger_type, trigger_id, correlation_id, "
            "status, start_ts, pinned) VALUES "
            "('run-plain','workflow','w','w','manual','t','c',"
            "'SUCCESS','2020-01-01T00:00:00Z',0)");
        b.exec();
    }

    DBWriter writer(*db_);
    std::stop_source stop;
    writer.start(stop.get_token());
    writer.enqueue(PruneOlderThan{.cutoff_date = "2021-01-01T00:00:00Z"});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.request_stop();
    writer.flush();

    QueryReader reader(*db_);
    EXPECT_FALSE(reader.get_run_summary("run-plain").has_value());
    EXPECT_TRUE(reader.get_run_summary("run-pin-me").has_value());

    auto pinned = reader.get_run_summary("run-pin-me");
    ASSERT_TRUE(pinned.has_value());
    EXPECT_TRUE(pinned->pinned);
}

TEST_F(AuditLogTest, PinUnpinRoundTrip) {
    {
        SQLite::Statement a(*db_,
            "INSERT INTO runs (run_id, target_type, target_id, "
            "target_name, trigger_type, trigger_id, correlation_id, "
            "status, start_ts) VALUES "
            "('run-rt','workflow','w','w','manual','t','c',"
            "'SUCCESS','2026-01-01T00:00:00Z')");
        a.exec();
    }

    DBWriter writer(*db_);
    std::stop_source stop;
    writer.start(stop.get_token());
    writer.enqueue(UpdateRunPinned{
        .run_id = "run-rt", .pinned = true});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.request_stop();
    writer.flush();

    QueryReader reader(*db_);
    auto r = reader.get_run_summary("run-rt");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->pinned);

    // Unpin via a second writer.
    DBWriter writer2(*db_);
    std::stop_source stop2;
    writer2.start(stop2.get_token());
    writer2.enqueue(UpdateRunPinned{
        .run_id = "run-rt", .pinned = false});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop2.request_stop();
    writer2.flush();

    auto r2 = reader.get_run_summary("run-rt");
    ASSERT_TRUE(r2.has_value());
    EXPECT_FALSE(r2->pinned);
}  // close PinUnpinRoundTrip

// ═══════════════════════════════════════════════════════════════════════════
//  §15.11 per-workflow retention
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(AuditLogTest, WorkflowRetentionMaxRuns) {
    // 5 runs for "chatty", retention keeps 2.
    for (int i = 0; i < 5; ++i) {
        SQLite::Statement q(*db_,
            "INSERT INTO runs (run_id, target_type, target_id, "
            "target_name, trigger_type, trigger_id, correlation_id, "
            "status, start_ts) VALUES (?, 'workflow', 'w', 'chatty', "
            "'schedule', 't', 'c', 'SUCCESS', ?)");
        q.bind(1, "run-chatty-" + std::to_string(i));
        // Spread over days, ascending.
        q.bind(2, std::string("2026-01-0") +
                    std::to_string(i + 1) + "T00:00:00Z");
        q.exec();
    }

    DBWriter writer(*db_);
    std::stop_source stop;
    writer.start(stop.get_token());
    writer.enqueue(PruneWorkflowRetention{
        .target_name = "chatty", .max_runs = 2,
        .max_age_days = 0, .keep_failures = true});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.request_stop();
    writer.flush();

    QueryReader reader(*db_);
    // Newest two survive.
    EXPECT_TRUE(reader.get_run_summary("run-chatty-4").has_value());
    EXPECT_TRUE(reader.get_run_summary("run-chatty-3").has_value());
    EXPECT_FALSE(reader.get_run_summary("run-chatty-2").has_value());
    EXPECT_FALSE(reader.get_run_summary("run-chatty-0").has_value());
}

TEST_F(AuditLogTest, WorkflowRetentionKeepFailures) {
    // One old SUCCESS and one old FAILURE for "guarded".
    for (auto& [id, status] :
         std::vector<std::pair<std::string, std::string>>{
             {"run-g-ok", "SUCCESS"}, {"run-g-bad", "FAILURE"}}) {
        SQLite::Statement q(*db_,
            "INSERT INTO runs (run_id, target_type, target_id, "
            "target_name, trigger_type, trigger_id, correlation_id, "
            "status, start_ts) VALUES (?, 'workflow', 'w', 'guarded', "
            "'schedule', 't', 'c', ?, '2020-01-01T00:00:00Z')");
        q.bind(1, id);
        q.bind(2, status);
        q.exec();
    }

    DBWriter writer(*db_);
    std::stop_source stop;
    writer.start(stop.get_token());
    // Age rule with keep_failures=true.
    writer.enqueue(PruneWorkflowRetention{
        .target_name = "guarded", .max_runs = 0,
        .max_age_days = 30, .keep_failures = true});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.request_stop();
    writer.flush();

    QueryReader reader(*db_);
    EXPECT_FALSE(reader.get_run_summary("run-g-ok").has_value());
    EXPECT_TRUE(reader.get_run_summary("run-g-bad").has_value());

    // And with keep_failures=false, the failure goes too.
    DBWriter writer2(*db_);
    std::stop_source stop2;
    writer2.start(stop2.get_token());
    writer2.enqueue(PruneWorkflowRetention{
        .target_name = "guarded", .max_runs = 0,
        .max_age_days = 30, .keep_failures = false});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop2.request_stop();
    writer2.flush();
    EXPECT_FALSE(reader.get_run_summary("run-g-bad").has_value());
}
