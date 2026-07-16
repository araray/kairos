/// tests/unit/persist/tags_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  tags_test.cpp — Unit tests for TagStore                                 ║
// ║  Spec reference: Roadmap §6 Tags System                                 ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/tag_store.hpp"
#include "kairos/persist/migration.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace kairos::persist {
namespace {

/// Test fixture: in-memory DB with all migrations applied.
class TagStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = std::make_unique<SQLite::Database>(":memory:",
            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        apply_migrations(*db_, get_migrations());
        store_ = std::make_unique<TagStore>(*db_);
    }

    std::unique_ptr<SQLite::Database> db_;
    std::unique_ptr<TagStore> store_;
};

// ─── Basic CRUD ───────────────────────────────────────────────────────────

TEST_F(TagStoreTest, SyncAndGetTags) {
    store_->sync_tags(TagEntityType::Workflow, "wf_deploy",
                      {"deploy", "production", "critical"});

    auto tags = store_->get_tags(TagEntityType::Workflow, "wf_deploy");
    ASSERT_EQ(tags.size(), 3u);
    EXPECT_EQ(tags[0], "critical");
    EXPECT_EQ(tags[1], "deploy");
    EXPECT_EQ(tags[2], "production");
}

TEST_F(TagStoreTest, GetTagsEmpty) {
    auto tags = store_->get_tags(TagEntityType::Workflow, "nonexistent");
    EXPECT_TRUE(tags.empty());
}

TEST_F(TagStoreTest, SyncReplacesExistingTags) {
    store_->sync_tags(TagEntityType::Job, "job_build", {"build", "ci"});
    store_->sync_tags(TagEntityType::Job, "job_build", {"build", "release"});

    auto tags = store_->get_tags(TagEntityType::Job, "job_build");
    ASSERT_EQ(tags.size(), 2u);
    EXPECT_EQ(tags[0], "build");
    EXPECT_EQ(tags[1], "release");
}

TEST_F(TagStoreTest, SyncEmptyTagsClearsAll) {
    store_->sync_tags(TagEntityType::Job, "job_build", {"build", "ci"});
    store_->sync_tags(TagEntityType::Job, "job_build", {});

    auto tags = store_->get_tags(TagEntityType::Job, "job_build");
    EXPECT_TRUE(tags.empty());
}

// ─── has_tag ──────────────────────────────────────────────────────────────

TEST_F(TagStoreTest, HasTagTrue) {
    store_->sync_tags(TagEntityType::Workflow, "wf_deploy",
                      {"deploy", "production"});
    EXPECT_TRUE(store_->has_tag(TagEntityType::Workflow,
                                "wf_deploy", "deploy"));
    EXPECT_TRUE(store_->has_tag(TagEntityType::Workflow,
                                "wf_deploy", "production"));
}

TEST_F(TagStoreTest, HasTagFalse) {
    store_->sync_tags(TagEntityType::Workflow, "wf_deploy",
                      {"deploy", "production"});
    EXPECT_FALSE(store_->has_tag(TagEntityType::Workflow,
                                 "wf_deploy", "staging"));
}

TEST_F(TagStoreTest, HasTagNonexistentEntity) {
    EXPECT_FALSE(store_->has_tag(TagEntityType::Workflow,
                                 "no_such", "any"));
}

TEST_F(TagStoreTest, HasTagStringOverload) {
    store_->sync_tags(TagEntityType::WatchGroup, "log_monitor",
                      {"monitoring"});
    EXPECT_TRUE(store_->has_tag("watch_group", "log_monitor",
                                "monitoring"));
    EXPECT_FALSE(store_->has_tag("watch_group", "log_monitor",
                                 "other"));
}

// ─── find_entities_with_tags (AND semantics) ──────────────────────────────

TEST_F(TagStoreTest, FindEntitiesAndSemantics) {
    store_->sync_tags(TagEntityType::Workflow, "wf_1",
                      {"deploy", "production"});
    store_->sync_tags(TagEntityType::Workflow, "wf_2",
                      {"deploy", "staging"});
    store_->sync_tags(TagEntityType::Workflow, "wf_3", {"build"});

    // Both tags required.
    auto result = store_->find_entities_with_tags(
        TagEntityType::Workflow, {"deploy", "production"});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], "wf_1");

    // Single tag.
    result = store_->find_entities_with_tags(
        TagEntityType::Workflow, {"deploy"});
    ASSERT_EQ(result.size(), 2u);
    std::sort(result.begin(), result.end());
    EXPECT_EQ(result[0], "wf_1");
    EXPECT_EQ(result[1], "wf_2");
}

TEST_F(TagStoreTest, FindEntitiesNoMatch) {
    store_->sync_tags(TagEntityType::Workflow, "wf_1", {"deploy"});
    auto result = store_->find_entities_with_tags(
        TagEntityType::Workflow, {"nonexistent"});
    EXPECT_TRUE(result.empty());
}

TEST_F(TagStoreTest, FindEntitiesEmptyInput) {
    auto result = store_->find_entities_with_tags(
        TagEntityType::Workflow, {});
    EXPECT_TRUE(result.empty());
}

// ─── find_entities_with_any_tag (OR semantics) ───────────────────────────

TEST_F(TagStoreTest, FindEntitiesAnyTag) {
    store_->sync_tags(TagEntityType::Job, "job_1", {"build"});
    store_->sync_tags(TagEntityType::Job, "job_2", {"test"});
    store_->sync_tags(TagEntityType::Job, "job_3", {"deploy"});

    auto result = store_->find_entities_with_any_tag(
        TagEntityType::Job, {"build", "test"});
    ASSERT_EQ(result.size(), 2u);
    std::sort(result.begin(), result.end());
    EXPECT_EQ(result[0], "job_1");
    EXPECT_EQ(result[1], "job_2");
}

// ─── Bulk sync ────────────────────────────────────────────────────────────

TEST_F(TagStoreTest, SyncAllTagsBulk) {
    store_->sync_tags(TagEntityType::Workflow, "wf_old", {"legacy"});

    std::vector<EntityTagSet> entities = {
        {TagEntityType::Workflow, "wf_1", {"deploy", "production"}},
        {TagEntityType::Workflow, "wf_2", {"build"}},
    };

    store_->sync_all_tags(TagEntityType::Workflow, entities);

    // wf_old tags should be removed (sync_all clears first).
    EXPECT_TRUE(store_->get_tags(TagEntityType::Workflow,
                                 "wf_old").empty());

    auto tags1 = store_->get_tags(TagEntityType::Workflow, "wf_1");
    ASSERT_EQ(tags1.size(), 2u);

    auto tags2 = store_->get_tags(TagEntityType::Workflow, "wf_2");
    ASSERT_EQ(tags2.size(), 1u);
    EXPECT_EQ(tags2[0], "build");
}

// ─── all_tags ─────────────────────────────────────────────────────────────

TEST_F(TagStoreTest, AllTagsGlobal) {
    store_->sync_tags(TagEntityType::Workflow, "wf_1",
                      {"deploy", "production"});
    store_->sync_tags(TagEntityType::Job, "job_1",
                      {"build", "deploy"});

    auto all = store_->all_tags();
    // build, deploy, production (deduplicated, sorted).
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0], "build");
    EXPECT_EQ(all[1], "deploy");
    EXPECT_EQ(all[2], "production");
}

TEST_F(TagStoreTest, AllTagsFiltered) {
    store_->sync_tags(TagEntityType::Workflow, "wf_1", {"deploy"});
    store_->sync_tags(TagEntityType::Job, "job_1", {"build"});

    auto wf_tags = store_->all_tags(TagEntityType::Workflow);
    ASSERT_EQ(wf_tags.size(), 1u);
    EXPECT_EQ(wf_tags[0], "deploy");
}

// ─── JSON helpers ─────────────────────────────────────────────────────────

TEST_F(TagStoreTest, TagsToJson) {
    auto json = TagStore::tags_to_json({"deploy", "production"});
    EXPECT_EQ(json, R"(["deploy","production"])");
}

TEST_F(TagStoreTest, TagsToJsonEmpty) {
    auto json = TagStore::tags_to_json({});
    EXPECT_EQ(json, "[]");
}

TEST_F(TagStoreTest, TagsFromJson) {
    auto tags = TagStore::tags_from_json(
        R"(["deploy","production","ci"])");
    ASSERT_EQ(tags.size(), 3u);
    EXPECT_EQ(tags[0], "deploy");
    EXPECT_EQ(tags[1], "production");
    EXPECT_EQ(tags[2], "ci");
}

TEST_F(TagStoreTest, TagsFromJsonEmpty) {
    EXPECT_TRUE(TagStore::tags_from_json("").empty());
    EXPECT_TRUE(TagStore::tags_from_json("[]").empty());
}

TEST_F(TagStoreTest, TagsFromJsonMalformed) {
    EXPECT_TRUE(TagStore::tags_from_json("not json").empty());
    EXPECT_TRUE(TagStore::tags_from_json("{\"k\":1}").empty());
}

// ─── Merge tags ───────────────────────────────────────────────────────────

TEST_F(TagStoreTest, MergeTagsDeduplicatesAndSorts) {
    auto merged = TagStore::merge_tags(
        {"deploy", "critical"}, {"deploy", "build"});
    ASSERT_EQ(merged.size(), 3u);
    EXPECT_EQ(merged[0], "build");
    EXPECT_EQ(merged[1], "critical");
    EXPECT_EQ(merged[2], "deploy");
}

// ─── Entity type parsing ─────────────────────────────────────────────────

TEST_F(TagStoreTest, EntityTypeRoundTrip) {
    EXPECT_EQ(to_string(TagEntityType::Workflow), "workflow");
    EXPECT_EQ(to_string(TagEntityType::Job), "job");
    EXPECT_EQ(to_string(TagEntityType::WatchGroup), "watch_group");
    EXPECT_EQ(to_string(TagEntityType::Trigger), "trigger");

    EXPECT_EQ(*parse_tag_entity_type("workflow"),
              TagEntityType::Workflow);
    EXPECT_EQ(*parse_tag_entity_type("job"),
              TagEntityType::Job);
    EXPECT_FALSE(parse_tag_entity_type("invalid").has_value());
}

// ─── Cross-entity-type isolation ──────────────────────────────────────────

TEST_F(TagStoreTest, TagsIsolatedByEntityType) {
    store_->sync_tags(TagEntityType::Workflow, "shared_id",
                      {"workflow_tag"});
    store_->sync_tags(TagEntityType::Job, "shared_id",
                      {"job_tag"});

    auto wf_tags = store_->get_tags(TagEntityType::Workflow,
                                     "shared_id");
    ASSERT_EQ(wf_tags.size(), 1u);
    EXPECT_EQ(wf_tags[0], "workflow_tag");

    auto job_tags = store_->get_tags(TagEntityType::Job,
                                      "shared_id");
    ASSERT_EQ(job_tags.size(), 1u);
    EXPECT_EQ(job_tags[0], "job_tag");
}

// ─── Migration compatibility ──────────────────────────────────────────────

TEST_F(TagStoreTest, MigrationV2CreatesTable) {
    SQLite::Statement query(*db_,
        "SELECT COUNT(*) FROM entity_tags");
    ASSERT_TRUE(query.executeStep());
    EXPECT_EQ(query.getColumn(0).getInt(), 0);
}

TEST_F(TagStoreTest, MigrationV2AddsTagsJsonColumn) {
    // Verify runs.tags_json column exists. If it doesn't, this throws.
    SQLite::Statement query(*db_,
        "SELECT tags_json FROM runs LIMIT 0");
    SUCCEED();
}

TEST_F(TagStoreTest, SchemaVersionIs2) {
    SQLite::Statement query(*db_,
        "SELECT MAX(version) FROM schema_version");
    ASSERT_TRUE(query.executeStep());
    EXPECT_EQ(query.getColumn(0).getInt(), 2);
}

} // anonymous namespace
} // namespace kairos::persist
