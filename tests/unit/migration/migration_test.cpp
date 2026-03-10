/// tests/unit/migration/migration_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Migration tool unit tests                                               ║
// ║                                                                         ║
// ║  Tests config translation and DB import for AVScheduler, EventWatcher,  ║
// ║  and LocalFlow.                                                        ║
// ║                                                                         ║
// ║  Spec reference: §31, §30                                               ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/migration/migration_tool.hpp"

#include <gtest/gtest.h>
#include <SQLiteCpp/SQLiteCpp.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace kairos::migration;

// ── Helpers ──────────────────────────────────────────────────────────────

class MigrationTest : public ::testing::Test {
protected:
    fs::path test_dir_;

    void SetUp() override {
        test_dir_ = fs::temp_directory_path() /
            ("kairos_migration_test_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(test_dir_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    /// Write a temporary file and return its path.
    fs::path write_temp(const std::string& name, const std::string& content) {
        auto path = test_dir_ / name;
        fs::create_directories(path.parent_path());
        std::ofstream out(path);
        out << content;
        return path;
    }

    /// Create a minimal Kairos target database with the expected schema.
    fs::path create_target_db() {
        auto path = test_dir_ / "kairos.db";
        SQLite::Database db(path.string(),
                            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);

        db.exec(R"SQL(
            CREATE TABLE IF NOT EXISTS runs (
                id TEXT PRIMARY KEY,
                entity_type TEXT NOT NULL,
                entity_id TEXT NOT NULL,
                trigger_type TEXT,
                status TEXT NOT NULL,
                started_at TEXT,
                finished_at TEXT,
                metadata TEXT
            );
            CREATE TABLE IF NOT EXISTS run_steps (
                id TEXT PRIMARY KEY,
                run_id TEXT NOT NULL,
                job_name TEXT NOT NULL,
                step_name TEXT NOT NULL,
                status TEXT NOT NULL,
                started_at TEXT,
                finished_at TEXT,
                exit_code INTEGER
            );
            CREATE TABLE IF NOT EXISTS step_outputs (
                step_id TEXT NOT NULL,
                stream TEXT NOT NULL,
                data TEXT
            );
            CREATE TABLE IF NOT EXISTS watch_events (
                id TEXT PRIMARY KEY,
                group_name TEXT NOT NULL,
                rule_name TEXT,
                event_type TEXT,
                detected_at TEXT,
                details TEXT
            );
            CREATE TABLE IF NOT EXISTS watch_samples (
                id TEXT PRIMARY KEY,
                group_name TEXT NOT NULL,
                sampled_at TEXT,
                snapshot_data TEXT
            );
        )SQL");

        return path;
    }

    /// Create a source AVScheduler database with test data.
    fs::path create_avs_source_db(int num_records = 3) {
        auto path = test_dir_ / "avs.db";
        SQLite::Database db(path.string(),
                            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db.exec(R"SQL(
            CREATE TABLE job_execution_logs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                job_name TEXT NOT NULL,
                started_at TEXT,
                finished_at TEXT,
                exit_code INTEGER,
                stdout TEXT,
                stderr TEXT,
                status INTEGER
            );
        )SQL");

        SQLite::Statement insert(db,
            "INSERT INTO job_execution_logs "
            "(job_name, started_at, finished_at, exit_code, stdout, stderr, status) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)");

        for (int i = 0; i < num_records; ++i) {
            insert.reset();
            insert.bind(1, "test_job_" + std::to_string(i));
            insert.bind(2, "2025-01-01T00:00:0" + std::to_string(i) + "Z");
            insert.bind(3, "2025-01-01T00:01:0" + std::to_string(i) + "Z");
            insert.bind(4, i == 1 ? 1 : 0);          // job 1 fails
            insert.bind(5, "stdout line " + std::to_string(i));
            insert.bind(6, i == 1 ? "error msg" : "");
            insert.bind(7, i == 1 ? 0 : 1);          // job 1 fails
            insert.exec();
        }

        return path;
    }

    /// Create a source EventWatcher database with test data.
    fs::path create_ew_source_db() {
        auto path = test_dir_ / "ew.db";
        SQLite::Database db(path.string(),
                            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db.exec(R"SQL(
            CREATE TABLE events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                watch_group TEXT,
                rule_name TEXT,
                event_type TEXT,
                timestamp TEXT,
                details TEXT
            );
            CREATE TABLE samples_json (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                watch_group TEXT,
                sampled_at TEXT,
                data TEXT
            );
        )SQL");

        db.exec(
            "INSERT INTO events (watch_group, rule_name, event_type, "
            "timestamp, details) VALUES "
            "('logs', 'modified', 'file_changed', "
            "'2025-01-01T00:00:00Z', '{\"file\":\"/var/log/test.log\"}')");

        db.exec(
            "INSERT INTO samples_json (watch_group, sampled_at, data) VALUES "
            "('logs', '2025-01-01T00:00:00Z', "
            "'{\"files\":[{\"path\":\"/var/log/test.log\",\"size\":1024}]}')");

        return path;
    }
};

// ══════════════════════════════════════════════════════════════════════════
// parse_source_tool
// ══════════════════════════════════════════════════════════════════════════

TEST_F(MigrationTest, ParseSourceToolValid) {
    EXPECT_EQ(parse_source_tool("avscheduler"), SourceTool::kAVScheduler);
    EXPECT_EQ(parse_source_tool("AVScheduler"), SourceTool::kAVScheduler);
    EXPECT_EQ(parse_source_tool("eventwatcher"), SourceTool::kEventWatcher);
    EXPECT_EQ(parse_source_tool("localflow"), SourceTool::kLocalFlow);
}

TEST_F(MigrationTest, ParseSourceToolInvalid) {
    EXPECT_THROW(parse_source_tool("unknown"), std::invalid_argument);
    EXPECT_THROW(parse_source_tool(""), std::invalid_argument);
}

TEST_F(MigrationTest, SourceToolName) {
    EXPECT_EQ(source_tool_name(SourceTool::kAVScheduler), "avscheduler");
    EXPECT_EQ(source_tool_name(SourceTool::kEventWatcher), "eventwatcher");
    EXPECT_EQ(source_tool_name(SourceTool::kLocalFlow), "localflow");
}

// ══════════════════════════════════════════════════════════════════════════
// AVScheduler Config Migration
// ══════════════════════════════════════════════════════════════════════════

TEST_F(MigrationTest, AVSchedulerConfigBasic) {
    auto source = write_temp("avs/config.toml", R"(
[settings]
db_path = "/home/user/.avscheduler/jobs.db"

[web_server]
host = "127.0.0.1"
port = 5000

[interpreters]
PYTHON = "/usr/bin/python3"
BASH = "/bin/bash"

[jobs.backup]
type = "BASH"
schedule_type = "cron"
schedule = "0 2 * * *"
command = "/opt/scripts/backup.sh"

[jobs.health]
type = "BASH"
schedule_type = "interval"
interval_seconds = 3600
command = "curl -f http://localhost:8080/health"
)");

    auto out = test_dir_ / "output";

    ConfigMigrationOptions opts;
    opts.source = SourceTool::kAVScheduler;
    opts.source_config = source;
    opts.output_dir = out;

    auto result = migrate_config(opts);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.entities_migrated, 2);
    EXPECT_GE(result.files_created.size(), 2u);  // kairos.toml + yaml

    // Verify kairos.toml was created.
    EXPECT_TRUE(fs::exists(out / "kairos.toml"));

    // Verify YAML was created.
    EXPECT_TRUE(fs::exists(out / "workflows" / "avs_jobs.yaml"));

    // Read kairos.toml and check content.
    std::ifstream toml_in(out / "kairos.toml");
    std::string toml_content((std::istreambuf_iterator<char>(toml_in)),
                              std::istreambuf_iterator<char>());
    EXPECT_NE(toml_content.find("listen_port = 5000"), std::string::npos);
    EXPECT_NE(toml_content.find("127.0.0.1"), std::string::npos);
}

TEST_F(MigrationTest, AVSchedulerConfigDryRun) {
    auto source = write_temp("avs/config.toml", R"(
[jobs.test]
type = "BASH"
schedule_type = "cron"
schedule = "* * * * *"
command = "echo hello"
)");

    auto out = test_dir_ / "dry_output";

    ConfigMigrationOptions opts;
    opts.source = SourceTool::kAVScheduler;
    opts.source_config = source;
    opts.output_dir = out;
    opts.dry_run = true;

    auto result = migrate_config(opts);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.entities_migrated, 1);

    // Dry run: files should NOT exist on disk.
    EXPECT_FALSE(fs::exists(out / "kairos.toml"));
}

TEST_F(MigrationTest, AVSchedulerConfigConditionWarning) {
    auto source = write_temp("avs/config.toml", R"(
[jobs.analysis]
type = "PYTHON"
schedule_type = "cron"
schedule = "0 3 * * *"
command = "python3 analyze.py"
condition = "any(dep.last_run_successful for dep in [build, test])"
)");

    auto out = test_dir_ / "output_warn";

    ConfigMigrationOptions opts;
    opts.source = SourceTool::kAVScheduler;
    opts.source_config = source;
    opts.output_dir = out;

    auto result = migrate_config(opts);
    EXPECT_TRUE(result.success);

    // Should have a warning about KEL incompatibility.
    bool found_warning = false;
    for (const auto& msg : result.messages) {
        if (msg.level == MigrationMessage::Level::kWarning &&
            msg.message.find("KEL") != std::string::npos) {
            found_warning = true;
            break;
        }
    }
    EXPECT_TRUE(found_warning)
        << "Expected a KEL incompatibility warning for 'any(...)' condition";
}

TEST_F(MigrationTest, AVSchedulerConfigMissingFile) {
    ConfigMigrationOptions opts;
    opts.source = SourceTool::kAVScheduler;
    opts.source_config = test_dir_ / "nonexistent.toml";
    opts.output_dir = test_dir_ / "output";

    auto result = migrate_config(opts);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.messages.empty());
    EXPECT_TRUE(result.messages[0].is_error());
}

// ══════════════════════════════════════════════════════════════════════════
// EventWatcher Config Migration
// ══════════════════════════════════════════════════════════════════════════

TEST_F(MigrationTest, EventWatcherConfigBasic) {
    auto source_config = write_temp("ew/config.toml", R"(
[database]
db_name = "eventwatcher.db"

[logging]
level = "INFO"
log_dir = "logs"
)");

    auto source_watches = write_temp("ew/watch_groups.yaml", R"(
watch_groups:
  - name: "logs"
    watch_items:
      - "/var/log/*.log"
    sample_rate: 60
    max_samples: 5
    max_depth: 2
    rules:
      - name: "Modified"
        condition: "file.mtime_age < 600"
        action: "notify"
)");

    auto out = test_dir_ / "ew_output";

    ConfigMigrationOptions opts;
    opts.source = SourceTool::kEventWatcher;
    opts.source_config = source_config;
    opts.source_watches = source_watches;
    opts.output_dir = out;

    auto result = migrate_config(opts);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.entities_migrated, 1);
    EXPECT_TRUE(fs::exists(out / "kairos.toml"));
    EXPECT_TRUE(fs::exists(out / "watch_groups" / "migrated.yaml"));
}

TEST_F(MigrationTest, EventWatcherConfigPythonConditionWarning) {
    auto source_config = write_temp("ew2/config.toml", R"(
[database]
db_name = "ew.db"
)");

    auto source_watches = write_temp("ew2/watch_groups.yaml", R"(
watch_groups:
  - name: "custom"
    watch_items:
      - "/data"
    rules:
      - name: "complex"
        condition: "aggregate(data, '*.log', 'size', sum) > 1048576"
)");

    auto out = test_dir_ / "ew2_output";

    ConfigMigrationOptions opts;
    opts.source = SourceTool::kEventWatcher;
    opts.source_config = source_config;
    opts.source_watches = source_watches;
    opts.output_dir = out;

    auto result = migrate_config(opts);
    EXPECT_TRUE(result.success);

    bool found_warning = false;
    for (const auto& msg : result.messages) {
        if (msg.level == MigrationMessage::Level::kWarning &&
            msg.message.find("auto-translate") != std::string::npos) {
            found_warning = true;
            break;
        }
    }
    EXPECT_TRUE(found_warning)
        << "Expected warning about untranslatable aggregate() condition";
}

// ══════════════════════════════════════════════════════════════════════════
// LocalFlow Config Migration
// ══════════════════════════════════════════════════════════════════════════

TEST_F(MigrationTest, LocalFlowConfigBasic) {
    auto wf_dir = test_dir_ / "lf_workflows";
    fs::create_directories(wf_dir);

    write_temp("lf_workflows/deploy.yaml", R"(
workflows:
  - name: deploy
    steps:
      - name: build
        run: "make build"
      - name: test
        run: "make test"
        needs: [build]
)");

    auto out = test_dir_ / "lf_output";

    ConfigMigrationOptions opts;
    opts.source = SourceTool::kLocalFlow;
    opts.source_workflows = wf_dir;
    opts.output_dir = out;

    auto result = migrate_config(opts);
    EXPECT_TRUE(result.success);
    EXPECT_GE(result.entities_migrated, 1);
    EXPECT_TRUE(fs::exists(out / "workflows" / "deploy.yaml"));
}

TEST_F(MigrationTest, LocalFlowConfigMissingDir) {
    ConfigMigrationOptions opts;
    opts.source = SourceTool::kLocalFlow;
    opts.source_workflows = test_dir_ / "nonexistent";
    opts.output_dir = test_dir_ / "output";

    auto result = migrate_config(opts);
    EXPECT_FALSE(result.success);
}

// ══════════════════════════════════════════════════════════════════════════
// AVScheduler DB Migration
// ══════════════════════════════════════════════════════════════════════════

TEST_F(MigrationTest, AVSchedulerDbMigrate) {
    auto source = create_avs_source_db(3);
    auto target = create_target_db();

    DbMigrationOptions opts;
    opts.source = SourceTool::kAVScheduler;
    opts.source_db = source;
    opts.target_db = target;

    auto result = migrate_db(opts);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.runs_imported, 3);
    EXPECT_EQ(result.steps_imported, 3);

    // Verify target database has the records.
    SQLite::Database db(target.string(), SQLite::OPEN_READONLY);
    SQLite::Statement q(db, "SELECT count(*) FROM runs");
    q.executeStep();
    EXPECT_EQ(q.getColumn(0).getInt(), 3);

    // Check metadata contains migrated_from.
    SQLite::Statement q2(db,
        "SELECT metadata FROM runs WHERE id = 'migrated_avs_1'");
    ASSERT_TRUE(q2.executeStep());
    auto meta = q2.getColumn(0).getString();
    EXPECT_NE(meta.find("avscheduler"), std::string::npos);
}

TEST_F(MigrationTest, AVSchedulerDbDryRun) {
    auto source = create_avs_source_db(5);
    auto target = create_target_db();

    DbMigrationOptions opts;
    opts.source = SourceTool::kAVScheduler;
    opts.source_db = source;
    opts.target_db = target;
    opts.dry_run = true;

    auto result = migrate_db(opts);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.runs_imported, 5);

    // Target should be empty (dry run).
    SQLite::Database db(target.string(), SQLite::OPEN_READONLY);
    SQLite::Statement q(db, "SELECT count(*) FROM runs");
    q.executeStep();
    EXPECT_EQ(q.getColumn(0).getInt(), 0);
}

TEST_F(MigrationTest, AVSchedulerDbDuplicateSkip) {
    auto source = create_avs_source_db(2);
    auto target = create_target_db();

    DbMigrationOptions opts;
    opts.source = SourceTool::kAVScheduler;
    opts.source_db = source;
    opts.target_db = target;

    // First migration.
    auto result1 = migrate_db(opts);
    EXPECT_TRUE(result1.success);
    EXPECT_EQ(result1.runs_imported, 2);

    // Second migration — should skip duplicates.
    auto result2 = migrate_db(opts);
    EXPECT_TRUE(result2.success);
    // INSERT OR IGNORE means no duplicates inserted.
    // The duplicate_skipped count comes from the explicit check.
    EXPECT_EQ(result2.duplicates_skipped, 2);
}

TEST_F(MigrationTest, AVSchedulerDbMissingSource) {
    DbMigrationOptions opts;
    opts.source = SourceTool::kAVScheduler;
    opts.source_db = test_dir_ / "nonexistent.db";
    opts.target_db = test_dir_ / "target.db";

    auto result = migrate_db(opts);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.messages.empty());
}

// ══════════════════════════════════════════════════════════════════════════
// EventWatcher DB Migration
// ══════════════════════════════════════════════════════════════════════════

TEST_F(MigrationTest, EventWatcherDbMigrate) {
    auto source = create_ew_source_db();
    auto target = create_target_db();

    DbMigrationOptions opts;
    opts.source = SourceTool::kEventWatcher;
    opts.source_db = source;
    opts.target_db = target;

    auto result = migrate_db(opts);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.events_imported, 1);
    EXPECT_EQ(result.samples_imported, 1);

    // Verify.
    SQLite::Database db(target.string(), SQLite::OPEN_READONLY);
    {
        SQLite::Statement q(db, "SELECT count(*) FROM watch_events");
        q.executeStep();
        EXPECT_EQ(q.getColumn(0).getInt(), 1);
    }
    {
        SQLite::Statement q(db, "SELECT count(*) FROM watch_samples");
        q.executeStep();
        EXPECT_EQ(q.getColumn(0).getInt(), 1);
    }
}

// ══════════════════════════════════════════════════════════════════════════
// LocalFlow DB Migration (no-op)
// ══════════════════════════════════════════════════════════════════════════

TEST_F(MigrationTest, LocalFlowDbNoOp) {
    auto target = create_target_db();

    DbMigrationOptions opts;
    opts.source = SourceTool::kLocalFlow;
    opts.source_db = test_dir_ / "dummy.db";  // Won't be read.
    opts.target_db = target;

    auto result = migrate_db(opts);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.runs_imported, 0);

    // Should have an info message.
    bool found_info = false;
    for (const auto& msg : result.messages) {
        if (msg.message.find("no database") != std::string::npos) {
            found_info = true;
            break;
        }
    }
    EXPECT_TRUE(found_info);
}
