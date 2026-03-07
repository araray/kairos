/// tests/unit/persist/schema_migration_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  schema_migration_test.cpp — SQLite schema migration tests                ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/migration.hpp"
#include "kairos/persist/database.hpp"
#include "test_helpers.hpp"

#include <gtest/gtest.h>

#include <SQLiteCpp/SQLiteCpp.h>

namespace kairos::persist {

// ═══════════════════════════════════════════════════════════════════════════
// Migration registry
// ═══════════════════════════════════════════════════════════════════════════

TEST(MigrationRegistry, HasAtLeastOneMigration) {
    const auto& migrations = get_migrations();
    EXPECT_GE(migrations.size(), 1u);
}

TEST(MigrationRegistry, MigrationsAreSequential) {
    const auto& migrations = get_migrations();
    for (size_t i = 0; i < migrations.size(); ++i) {
        EXPECT_EQ(migrations[i].version, static_cast<int>(i + 1))
            << "Migration " << i << " has wrong version";
    }
}

TEST(MigrationRegistry, MigrationsHaveDescriptions) {
    const auto& migrations = get_migrations();
    for (const auto& m : migrations) {
        EXPECT_FALSE(m.description.empty())
            << "Migration " << m.version << " has no description";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Schema version
// ═══════════════════════════════════════════════════════════════════════════

TEST(SchemaVersion, EmptyDbReturnsZero) {
    SQLite::Database db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    EXPECT_EQ(get_schema_version(db), 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Apply migrations
// ═══════════════════════════════════════════════════════════════════════════

TEST(ApplyMigrations, FreshDbGetsAllMigrations) {
    SQLite::Database db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    int version = apply_migrations(db, get_migrations());
    EXPECT_EQ(version, static_cast<int>(get_migrations().size()));
}

TEST(ApplyMigrations, IdempotentOnReRun) {
    SQLite::Database db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    int v1 = apply_migrations(db, get_migrations());
    int v2 = apply_migrations(db, get_migrations());
    EXPECT_EQ(v1, v2) << "Re-running migrations should be idempotent";
}

TEST(ApplyMigrations, SchemaVersionRecorded) {
    SQLite::Database db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    apply_migrations(db, get_migrations());
    int ver = get_schema_version(db);
    EXPECT_GE(ver, 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// Table existence checks
// ═══════════════════════════════════════════════════════════════════════════

class SchemaTest : public ::testing::Test {
protected:
    SQLite::Database db_{":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE};

    void SetUp() override {
        apply_migrations(db_, get_migrations());
    }

    bool table_exists(const std::string& name) {
        SQLite::Statement q(db_,
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?");
        q.bind(1, name);
        q.executeStep();
        return q.getColumn(0).getInt() > 0;
    }

    int count_columns(const std::string& table) {
        SQLite::Statement q(db_,
            "SELECT COUNT(*) FROM pragma_table_info(?)");
        q.bind(1, table);
        q.executeStep();
        return q.getColumn(0).getInt();
    }
};

TEST_F(SchemaTest, AllTablesExist) {
    EXPECT_TRUE(table_exists("schema_version"));
    EXPECT_TRUE(table_exists("runs"));
    EXPECT_TRUE(table_exists("job_runs"));
    EXPECT_TRUE(table_exists("step_runs"));
    EXPECT_TRUE(table_exists("log_chunks"));
    EXPECT_TRUE(table_exists("watch_samples"));
    EXPECT_TRUE(table_exists("watch_events"));
    EXPECT_TRUE(table_exists("trigger_history"));
    EXPECT_TRUE(table_exists("metrics_snapshots"));
    EXPECT_TRUE(table_exists("config_snapshots"));
}

TEST_F(SchemaTest, RunsTableHasExpectedColumns) {
    // runs should have 15 columns per schema definition.
    int cols = count_columns("runs");
    EXPECT_GE(cols, 14) << "runs table should have at least 14 columns";
}

TEST_F(SchemaTest, CanInsertAndQueryRun) {
    SQLite::Statement insert(db_,
        "INSERT INTO runs (run_id, target_type, target_id, target_name, "
        "trigger_type, trigger_id, correlation_id, status, start_ts) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    insert.bind(1, "run-test123");
    insert.bind(2, "workflow");
    insert.bind(3, "wfl-abc123");
    insert.bind(4, "Test Workflow");
    insert.bind(5, "manual_run");
    insert.bind(6, "manual");
    insert.bind(7, "corr-xyz");
    insert.bind(8, "RUNNING");
    insert.bind(9, "2026-03-02T14:30:05.123Z");
    insert.exec();

    SQLite::Statement query(db_, "SELECT run_id, status FROM runs WHERE run_id=?");
    query.bind(1, "run-test123");
    ASSERT_TRUE(query.executeStep());
    EXPECT_EQ(query.getColumn(0).getString(), "run-test123");
    EXPECT_EQ(query.getColumn(1).getString(), "RUNNING");
}

TEST_F(SchemaTest, CanInsertWatchEvent) {
    SQLite::Statement insert(db_,
        "INSERT INTO watch_events (watch_group, event_type, file_path) "
        "VALUES (?, ?, ?)");
    insert.bind(1, "log_monitor");
    insert.bind(2, "file_modified");
    insert.bind(3, "/var/log/app.log");
    insert.exec();

    SQLite::Statement query(db_,
        "SELECT COUNT(*) FROM watch_events WHERE watch_group=?");
    query.bind(1, "log_monitor");
    query.executeStep();
    EXPECT_EQ(query.getColumn(0).getInt(), 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// Database open with file
// ═══════════════════════════════════════════════════════════════════════════

TEST(DatabaseOpen, CreatesFileAndMigrates) {
    testing::TempDir tmp;
    auto db_path = tmp.path() / "test.db";

    auto db = open_database(db_path);
    ASSERT_NE(db, nullptr);

    int ver = get_schema_version(*db);
    EXPECT_GE(ver, 1);

    // Verify WAL mode.
    SQLite::Statement q(*db, "PRAGMA journal_mode");
    q.executeStep();
    EXPECT_EQ(q.getColumn(0).getString(), "wal");
}

TEST(DatabaseOpen, IdempotentOnSecondOpen) {
    testing::TempDir tmp;
    auto db_path = tmp.path() / "test.db";

    {
        auto db1 = open_database(db_path);
        int v1 = get_schema_version(*db1);
        EXPECT_GE(v1, 1);
    }

    {
        auto db2 = open_database(db_path);
        int v2 = get_schema_version(*db2);
        EXPECT_GE(v2, 1);
    }
}

}  // namespace kairos::persist
