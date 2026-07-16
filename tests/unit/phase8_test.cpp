/// tests/unit/phase8_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  phase8_test.cpp — Phase 8 integration tests                             ║
// ║  Verifies end-to-end: migration → tag sync → querying → filtering       ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/tag_store.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/cli/dag_graph.hpp"

#include <gtest/gtest.h>

namespace kairos {
namespace {

class Phase8IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = std::make_unique<SQLite::Database>(":memory:",
            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        persist::apply_migrations(*db_, persist::get_migrations());
        tag_store_ = std::make_unique<persist::TagStore>(*db_);
    }

    /// Insert a run with tags_json for testing run-level tag filtering.
    void insert_run(const std::string& run_id,
                    const std::string& target_name,
                    const std::string& status,
                    const std::vector<std::string>& tags) {
        SQLite::Statement ins(*db_,
            "INSERT INTO runs (run_id, target_type, target_id, "
            "target_name, trigger_type, trigger_id, correlation_id, "
            "status, start_ts, tags_json) "
            "VALUES (?, 'workflow', ?, ?, 'manual_run', 'manual', "
            "'corr-1', ?, datetime('now'), ?)");
        ins.bind(1, run_id);
        ins.bind(2, "wf_" + target_name);
        ins.bind(3, target_name);
        ins.bind(4, status);
        ins.bind(5, persist::TagStore::tags_to_json(tags));
        ins.exec();
    }

    std::unique_ptr<SQLite::Database> db_;
    std::unique_ptr<persist::TagStore> tag_store_;
};

// ─── Migration v2 is applied correctly ────────────────────────────────────

TEST_F(Phase8IntegrationTest, MigrationV2Applied) {
    SQLite::Statement query(*db_,
        "SELECT MAX(version) FROM schema_version");
    ASSERT_TRUE(query.executeStep());
    EXPECT_EQ(query.getColumn(0).getInt(), 2);
}

// ─── Tag lifecycle: sync → query → has_tag ────────────────────────────────

TEST_F(Phase8IntegrationTest, FullTagLifecycle) {
    // Simulate config load.
    std::vector<persist::EntityTagSet> wf_entities = {
        {persist::TagEntityType::Workflow, "wf_deploy",
         {"deploy", "production", "critical"}},
        {persist::TagEntityType::Workflow, "wf_backup",
         {"backup", "nightly"}},
    };
    tag_store_->sync_all_tags(persist::TagEntityType::Workflow,
                              wf_entities);

    std::vector<persist::EntityTagSet> job_entities = {
        {persist::TagEntityType::Job, "job_build", {"build", "ci"}},
        {persist::TagEntityType::Job, "job_test", {"test", "ci"}},
        {persist::TagEntityType::Job, "job_deploy",
         {"deploy", "production"}},
    };
    tag_store_->sync_all_tags(persist::TagEntityType::Job,
                              job_entities);

    // Verify queries.
    EXPECT_TRUE(tag_store_->has_tag(persist::TagEntityType::Workflow,
                                     "wf_deploy", "critical"));
    EXPECT_FALSE(tag_store_->has_tag(persist::TagEntityType::Workflow,
                                      "wf_deploy", "nightly"));

    // AND filter.
    auto result = tag_store_->find_entities_with_tags(
        persist::TagEntityType::Workflow, {"deploy", "critical"});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], "wf_deploy");

    // Jobs with "ci" tag.
    result = tag_store_->find_entities_with_tags(
        persist::TagEntityType::Job, {"ci"});
    ASSERT_EQ(result.size(), 2u);

    // All distinct tags across all types.
    auto all = tag_store_->all_tags();
    EXPECT_GE(all.size(), 7u);
}

// ─── Tag propagation to runs ──────────────────────────────────────────────

TEST_F(Phase8IntegrationTest, RunTagPropagation) {
    insert_run("run-001", "deploy_pipeline", "SUCCESS",
               {"deploy", "production"});
    insert_run("run-002", "backup_job", "SUCCESS",
               {"backup", "nightly"});
    insert_run("run-003", "deploy_pipeline", "FAILED",
               {"deploy", "production"});

    // Query runs and parse tags.
    SQLite::Statement query(*db_,
        "SELECT run_id, tags_json FROM runs ORDER BY run_id");

    int deploy_count = 0;
    while (query.executeStep()) {
        auto tags = persist::TagStore::tags_from_json(
            query.getColumn(1).getString());
        if (std::find(tags.begin(), tags.end(), "deploy")
            != tags.end()) {
            ++deploy_count;
        }
    }
    EXPECT_EQ(deploy_count, 2);  // run-001 and run-003.
}

// ─── Config reload replaces tags ──────────────────────────────────────────

TEST_F(Phase8IntegrationTest, ConfigReloadReplacesAllTags) {
    tag_store_->sync_all_tags(persist::TagEntityType::Workflow, {
        {persist::TagEntityType::Workflow, "wf_1", {"old_tag"}},
    });

    EXPECT_TRUE(tag_store_->has_tag(persist::TagEntityType::Workflow,
                                     "wf_1", "old_tag"));

    // Simulate reload with different tags.
    tag_store_->sync_all_tags(persist::TagEntityType::Workflow, {
        {persist::TagEntityType::Workflow, "wf_1", {"new_tag"}},
        {persist::TagEntityType::Workflow, "wf_2", {"another_tag"}},
    });

    EXPECT_FALSE(tag_store_->has_tag(persist::TagEntityType::Workflow,
                                      "wf_1", "old_tag"));
    EXPECT_TRUE(tag_store_->has_tag(persist::TagEntityType::Workflow,
                                     "wf_1", "new_tag"));
    EXPECT_TRUE(tag_store_->has_tag(persist::TagEntityType::Workflow,
                                     "wf_2", "another_tag"));
}

// ─── DAG graph rendering integration ──────────────────────────────────────

TEST_F(Phase8IntegrationTest, DagGraphWithTags) {
    std::vector<cli::GraphNode> nodes = {
        {"job_setup", "Setup", {}, {}},
        {"job_build", "Build", {"job_setup"}, {"build", "ci"}},
        {"job_test", "Test", {"job_setup"}, {"test", "ci"}},
        {"job_deploy", "Deploy", {"job_build", "job_test"},
         {"deploy", "production"}},
    };

    auto text = cli::render_dag_graph("deploy_pipeline", nodes, false);
    EXPECT_NE(text.find("deploy_pipeline"), std::string::npos);
    EXPECT_NE(text.find("Setup"), std::string::npos);
    EXPECT_NE(text.find("Deploy"), std::string::npos);
    EXPECT_NE(text.find("[tags: deploy, production]"),
              std::string::npos);

    // JSON output should parse.
    auto json_str = cli::render_dag_graph_json(
        "deploy_pipeline", nodes);
    auto j = nlohmann::json::parse(json_str);
    EXPECT_EQ(j["workflow"], "deploy_pipeline");
    EXPECT_EQ(j["jobs"].size(), 4u);
}

} // anonymous namespace
} // namespace kairos
