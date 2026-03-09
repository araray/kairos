/// tests/unit/persist/metrics_pruning_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for PruneMetricsSnapshots write request + standalone_jobs accessor ║
// ║  Spec reference: §16.8, §23.2                                           ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/workflow_registry.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/query_reader.hpp"

#include <gtest/gtest.h>
#include <SQLiteCpp/SQLiteCpp.h>

#include <chrono>
#include <filesystem>
#include <set>
#include <thread>

namespace kairos {
namespace {

// ════════════════════════════════════════════════════════════════════════
// Helper: create an in-memory DB with schema initialized.
// ════════════════════════════════════════════════════════════════════════

class MetricsPruningTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = std::filesystem::temp_directory_path() /
                   ("kairos_mprune_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this)) +
                    ".db");
        db_ = persist::open_database(db_path_);
        persist::init_database(db_path_);
    }

    void TearDown() override {
        db_.reset();
        std::error_code ec;
        std::filesystem::remove(db_path_, ec);
        // Also remove WAL and SHM files.
        std::filesystem::remove(
            std::filesystem::path(db_path_.string() + "-wal"), ec);
        std::filesystem::remove(
            std::filesystem::path(db_path_.string() + "-shm"), ec);
    }

    SQLite::Database& db() { return *db_; }

private:
    std::filesystem::path db_path_;
    std::unique_ptr<SQLite::Database> db_;
};

// ── PruneMetricsSnapshots tests ────────────────────────────────────────

TEST_F(MetricsPruningTest, PruneDeletesOldSnapshots) {
    // Insert some "old" snapshots with explicit created_at.
    db().exec(
        "INSERT INTO metrics_snapshots (metric_name, metric_type, value, "
        "labels_json, created_at) VALUES "
        "('kairos_runs_total', 'counter', 42.0, '', "
        " datetime('now', '-10 days'))");
    db().exec(
        "INSERT INTO metrics_snapshots (metric_name, metric_type, value, "
        "labels_json, created_at) VALUES "
        "('kairos_uptime_seconds', 'gauge', 3600.0, '', "
        " datetime('now', '-10 days'))");

    // Insert a "recent" snapshot.
    db().exec(
        "INSERT INTO metrics_snapshots (metric_name, metric_type, value, "
        "labels_json, created_at) VALUES "
        "('kairos_runs_total', 'counter', 100.0, '', "
        " datetime('now'))");

    // Verify 3 rows exist.
    {
        SQLite::Statement q(db(), "SELECT COUNT(*) FROM metrics_snapshots");
        q.executeStep();
        EXPECT_EQ(q.getColumn(0).getInt(), 3);
    }

    // Create DBWriter and enqueue a prune request.
    persist::DBWriterConfig cfg;
    persist::DBWriter writer(db(), cfg);

    std::stop_source stop;
    writer.start(stop.get_token());

    writer.enqueue(persist::PruneMetricsSnapshots{.retention_days = 7});

    // Give writer time to process.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer.flush();
    stop.request_stop();

    // Only the recent snapshot should remain.
    {
        SQLite::Statement q(db(), "SELECT COUNT(*) FROM metrics_snapshots");
        q.executeStep();
        EXPECT_EQ(q.getColumn(0).getInt(), 1);
    }

    // Verify it's the recent one.
    {
        SQLite::Statement q(db(),
            "SELECT metric_name, value FROM metrics_snapshots");
        q.executeStep();
        EXPECT_EQ(q.getColumn(0).getString(), "kairos_runs_total");
        EXPECT_DOUBLE_EQ(q.getColumn(1).getDouble(), 100.0);
    }
}

TEST_F(MetricsPruningTest, PruneNoOpsWhenAllRecent) {
    // Insert only recent snapshots.
    db().exec(
        "INSERT INTO metrics_snapshots (metric_name, metric_type, value, "
        "labels_json) VALUES ('m1', 'counter', 1.0, '')");
    db().exec(
        "INSERT INTO metrics_snapshots (metric_name, metric_type, value, "
        "labels_json) VALUES ('m2', 'gauge', 2.0, '')");

    persist::DBWriterConfig cfg;
    persist::DBWriter writer(db(), cfg);

    std::stop_source stop;
    writer.start(stop.get_token());

    writer.enqueue(persist::PruneMetricsSnapshots{.retention_days = 7});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer.flush();
    stop.request_stop();

    // Both should still be present.
    {
        SQLite::Statement q(db(), "SELECT COUNT(*) FROM metrics_snapshots");
        q.executeStep();
        EXPECT_EQ(q.getColumn(0).getInt(), 2);
    }
}

TEST_F(MetricsPruningTest, PruneWithCustomRetentionDays) {
    // Insert a snapshot 3 days ago.
    db().exec(
        "INSERT INTO metrics_snapshots (metric_name, metric_type, value, "
        "labels_json, created_at) VALUES "
        "('m1', 'counter', 1.0, '', datetime('now', '-3 days'))");

    // With retention_days=2, it should be pruned.
    persist::DBWriterConfig cfg;
    persist::DBWriter writer(db(), cfg);

    std::stop_source stop;
    writer.start(stop.get_token());

    writer.enqueue(persist::PruneMetricsSnapshots{.retention_days = 2});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer.flush();
    stop.request_stop();

    {
        SQLite::Statement q(db(), "SELECT COUNT(*) FROM metrics_snapshots");
        q.executeStep();
        EXPECT_EQ(q.getColumn(0).getInt(), 0);
    }
}

TEST_F(MetricsPruningTest, PruneEmptyTable) {
    // Pruning an empty table should not error.
    persist::DBWriterConfig cfg;
    persist::DBWriter writer(db(), cfg);

    std::stop_source stop;
    writer.start(stop.get_token());

    writer.enqueue(persist::PruneMetricsSnapshots{.retention_days = 7});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer.flush();
    stop.request_stop();

    {
        SQLite::Statement q(db(), "SELECT COUNT(*) FROM metrics_snapshots");
        q.executeStep();
        EXPECT_EQ(q.getColumn(0).getInt(), 0);
    }
}

// ════════════════════════════════════════════════════════════════════════
// WorkflowRegistry::standalone_jobs() accessor tests
// ════════════════════════════════════════════════════════════════════════

TEST(WorkflowRegistryJobs, StandaloneJobsAccessor) {
    // Create registry with workflows and standalone jobs.
    // WorkflowDef can't be default-constructed (WorkflowDag's default
    // ctor is private), so we must use designated initializers.
    auto dag = engine::WorkflowDag::build({engine::DagNode{
        .job_id = "job-in-wf",
        .job_name = "in-workflow-job",
    }});

    engine::WorkflowDef wf{
        .workflow_id = "wfl-test",
        .workflow_name = "test-workflow",
        .jobs = {engine::JobDef{
            .job_id = "job-in-wf",
            .job_name = "in-workflow-job",
        }},
        .dag = std::move(dag),
    };

    engine::JobDef sj1{
        .job_id = "job-standalone-1",
        .job_name = "backup",
    };
    engine::JobDef sj2{
        .job_id = "job-standalone-2",
        .job_name = "cleanup",
    };

    std::vector<engine::WorkflowDef> wfs;
    wfs.push_back(std::move(wf));

    engine::WorkflowRegistry registry(
        std::move(wfs),  // workflows
        {},    // triggers
        {sj1, sj2});  // standalone_jobs

    auto jobs = registry.standalone_jobs();
    EXPECT_EQ(jobs.size(), 2u);

    // Verify both jobs are present (order not guaranteed from unordered_map).
    std::set<std::string> names;
    for (const auto* j : jobs) {
        names.insert(j->job_name);
    }
    EXPECT_TRUE(names.count("backup"));
    EXPECT_TRUE(names.count("cleanup"));
}

TEST(WorkflowRegistryJobs, StandaloneJobCount) {
    engine::JobDef sj{
        .job_id = "job-1",
        .job_name = "only-job",
    };

    engine::WorkflowRegistry registry({}, {}, {sj});

    EXPECT_EQ(registry.standalone_job_count(), 1u);
}

TEST(WorkflowRegistryJobs, StandaloneJobByName) {
    engine::JobDef sj{
        .job_id = "job-1",
        .job_name = "my-job",
    };

    engine::WorkflowRegistry registry({}, {}, {sj});

    auto* found = registry.standalone_job_by_name("my-job");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->job_id, "job-1");

    // Not found.
    EXPECT_EQ(registry.standalone_job_by_name("nonexistent"), nullptr);
}

TEST(WorkflowRegistryJobs, EmptyStandaloneJobs) {
    engine::WorkflowRegistry registry({}, {}, {});

    auto jobs = registry.standalone_jobs();
    EXPECT_TRUE(jobs.empty());
    EXPECT_EQ(registry.standalone_job_count(), 0u);
}

}  // namespace
}  // namespace kairos
