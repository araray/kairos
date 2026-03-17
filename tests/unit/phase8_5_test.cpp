/// tests/unit/phase85_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Phase 8.5 integration test                                               ║
// ║                                                                           ║
// ║  Tests:                                                                   ║
// ║    1. has_tag() KEL builtin wired through TagStore                       ║
// ║    2. Tag sync from registry definitions into entity_tags table          ║
// ║    3. has_tag() returns false when tag_store is null (graceful degrade)  ║
// ║    4. has_tag() argument validation                                       ║
// ║                                                                           ║
// ║  Spec reference: Roadmap §6 (Tags), §2.3 (completions), §4.1 (TUI)     ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include <gtest/gtest.h>

#include "kairos/kel/evaluator.hpp"
#include "kairos/kel/errors.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/persist/tag_store.hpp"

#include <filesystem>
#include <memory>

namespace kairos::test {

// ── Test fixture ─────────────────────────────────────────────────────────

class Phase85Test : public ::testing::Test {
protected:
    void SetUp() override {
        // Create in-memory database with migrations applied.
        db_ = std::make_unique<SQLite::Database>(
            ":memory:",
            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);

        // Apply all migrations.
        persist::apply_migrations(*db_, persist::get_migrations());

        tag_store_ = std::make_unique<persist::TagStore>(*db_);
    }

    std::unique_ptr<SQLite::Database> db_;
    std::unique_ptr<persist::TagStore> tag_store_;
};

// ── 1. has_tag() KEL builtin via TagStore ────────────────────────────────

TEST_F(Phase85Test, HasTagReturnsTrueWhenTagExists) {
    // Insert a tag for a workflow entity.
    tag_store_->sync_tags(persist::TagEntityType::Workflow,
                          "wfl-deploy", {"production", "critical"});

    // Build a KEL context with has_tag() wired to the TagStore.
    auto ctx = kel::make_default_context();
    auto* ts = tag_store_.get();
    ctx.functions["has_tag"] = [ts](
        const std::vector<kel::KelValue>& args) -> kel::KelValue
    {
        if (args.size() != 2 || !args[0].is_string() || !args[1].is_string())
            throw kel::KelEvalError(
                "has_tag() requires two string arguments (entity_id, tag)");
        const auto& entity_id = args[0].as_string();
        const auto& tag = args[1].as_string();
        static const std::string types[] = {
            "workflow", "job", "watch_group", "trigger"};
        for (const auto& t : types) {
            if (ts->has_tag(t, entity_id, tag))
                return kel::KelValue(true);
        }
        return kel::KelValue(false);
    };

    // Evaluate: has_tag("wfl-deploy", "production") → true
    auto result = kel::eval_expression(
        R"(has_tag("wfl-deploy", "production"))", ctx);
    ASSERT_TRUE(result.is_bool());
    EXPECT_TRUE(result.as_bool());

    // Evaluate: has_tag("wfl-deploy", "critical") → true
    result = kel::eval_expression(
        R"(has_tag("wfl-deploy", "critical"))", ctx);
    EXPECT_TRUE(result.as_bool());
}

TEST_F(Phase85Test, HasTagReturnsFalseWhenTagMissing) {
    tag_store_->sync_tags(persist::TagEntityType::Workflow,
                          "wfl-deploy", {"production"});

    auto ctx = kel::make_default_context();
    auto* ts = tag_store_.get();
    ctx.functions["has_tag"] = [ts](
        const std::vector<kel::KelValue>& args) -> kel::KelValue
    {
        const auto& eid = args[0].as_string();
        const auto& tag = args[1].as_string();
        static const std::string types[] = {
            "workflow", "job", "watch_group", "trigger"};
        for (const auto& t : types) {
            if (ts->has_tag(t, eid, tag))
                return kel::KelValue(true);
        }
        return kel::KelValue(false);
    };

    // Tag "staging" does not exist.
    auto result = kel::eval_expression(
        R"(has_tag("wfl-deploy", "staging"))", ctx);
    ASSERT_TRUE(result.is_bool());
    EXPECT_FALSE(result.as_bool());
}

TEST_F(Phase85Test, HasTagReturnsFalseForUnknownEntity) {
    auto ctx = kel::make_default_context();
    auto* ts = tag_store_.get();
    ctx.functions["has_tag"] = [ts](
        const std::vector<kel::KelValue>& args) -> kel::KelValue
    {
        const auto& eid = args[0].as_string();
        const auto& tag = args[1].as_string();
        static const std::string types[] = {
            "workflow", "job", "watch_group", "trigger"};
        for (const auto& t : types) {
            if (ts->has_tag(t, eid, tag))
                return kel::KelValue(true);
        }
        return kel::KelValue(false);
    };

    auto result = kel::eval_expression(
        R"(has_tag("nonexistent", "any"))", ctx);
    ASSERT_TRUE(result.is_bool());
    EXPECT_FALSE(result.as_bool());
}

TEST_F(Phase85Test, HasTagSearchesAllEntityTypes) {
    // Tag on a job entity, not a workflow.
    tag_store_->sync_tags(persist::TagEntityType::Job,
                          "job-build", {"ci"});

    auto ctx = kel::make_default_context();
    auto* ts = tag_store_.get();
    ctx.functions["has_tag"] = [ts](
        const std::vector<kel::KelValue>& args) -> kel::KelValue
    {
        const auto& eid = args[0].as_string();
        const auto& tag = args[1].as_string();
        static const std::string types[] = {
            "workflow", "job", "watch_group", "trigger"};
        for (const auto& t : types) {
            if (ts->has_tag(t, eid, tag))
                return kel::KelValue(true);
        }
        return kel::KelValue(false);
    };

    // Should find "ci" on the job entity even though we
    // search workflows first.
    auto result = kel::eval_expression(
        R"(has_tag("job-build", "ci"))", ctx);
    EXPECT_TRUE(result.as_bool());
}

TEST_F(Phase85Test, HasTagWorksInConditionExpression) {
    tag_store_->sync_tags(persist::TagEntityType::Workflow,
                          "wfl-deploy", {"production"});

    auto ctx = kel::make_default_context();
    auto* ts = tag_store_.get();
    ctx.functions["has_tag"] = [ts](
        const std::vector<kel::KelValue>& args) -> kel::KelValue
    {
        const auto& eid = args[0].as_string();
        const auto& tag = args[1].as_string();
        static const std::string types[] = {
            "workflow", "job", "watch_group", "trigger"};
        for (const auto& t : types) {
            if (ts->has_tag(t, eid, tag))
                return kel::KelValue(true);
        }
        return kel::KelValue(false);
    };

    // Compound expression: has_tag() and true → true
    auto result = kel::eval_expression(
        R"(has_tag("wfl-deploy", "production") and true)", ctx);
    EXPECT_TRUE(result.as_bool());

    // has_tag() and false → false
    result = kel::eval_expression(
        R"(has_tag("wfl-deploy", "production") and false)", ctx);
    EXPECT_FALSE(result.as_bool());

    // not has_tag("wfl-deploy", "staging") → true
    result = kel::eval_expression(
        R"(not has_tag("wfl-deploy", "staging"))", ctx);
    EXPECT_TRUE(result.as_bool());
}

TEST_F(Phase85Test, HasTagArgumentValidation) {
    auto ctx = kel::make_default_context();
    auto* ts = tag_store_.get();
    ctx.functions["has_tag"] = [ts](
        const std::vector<kel::KelValue>& args) -> kel::KelValue
    {
        if (args.size() != 2 || !args[0].is_string() || !args[1].is_string())
            throw kel::KelEvalError(
                "has_tag() requires two string arguments (entity_id, tag)");
        return kel::KelValue(false);
    };

    // Wrong number of args.
    EXPECT_THROW(
        kel::eval_expression(R"(has_tag("only_one"))", ctx),
        kel::KelEvalError);

    // Wrong type.
    EXPECT_THROW(
        kel::eval_expression(R"(has_tag(42, "tag"))", ctx),
        kel::KelEvalError);

    // Too many args.
    EXPECT_THROW(
        kel::eval_expression(R"(has_tag("a", "b", "c"))", ctx),
        kel::KelEvalError);
}

// ── 2. Tag sync from definitions ─────────────────────────────────────────

TEST_F(Phase85Test, SyncAllTagsForWorkflows) {
    // Simulate what daemon.cpp does after loading registry.
    std::vector<persist::EntityTagSet> wf_tags = {
        {persist::TagEntityType::Workflow, "wfl-deploy",
         {"production", "critical"}},
        {persist::TagEntityType::Workflow, "wfl-test",
         {"ci", "nightly"}},
    };

    tag_store_->sync_all_tags(persist::TagEntityType::Workflow, wf_tags);

    // Verify tags are in the DB.
    auto tags1 = tag_store_->get_tags(persist::TagEntityType::Workflow,
                                       "wfl-deploy");
    ASSERT_EQ(tags1.size(), 2u);
    EXPECT_EQ(tags1[0], "critical");      // Alphabetical order.
    EXPECT_EQ(tags1[1], "production");

    auto tags2 = tag_store_->get_tags(persist::TagEntityType::Workflow,
                                       "wfl-test");
    ASSERT_EQ(tags2.size(), 2u);
    EXPECT_EQ(tags2[0], "ci");
    EXPECT_EQ(tags2[1], "nightly");
}

TEST_F(Phase85Test, SyncAllTagsReplacesOldTags) {
    // First sync.
    std::vector<persist::EntityTagSet> old_tags = {
        {persist::TagEntityType::Job, "job-build", {"old-tag"}},
    };
    tag_store_->sync_all_tags(persist::TagEntityType::Job, old_tags);

    auto tags = tag_store_->get_tags(persist::TagEntityType::Job, "job-build");
    ASSERT_EQ(tags.size(), 1u);
    EXPECT_EQ(tags[0], "old-tag");

    // Second sync replaces all.
    std::vector<persist::EntityTagSet> new_tags = {
        {persist::TagEntityType::Job, "job-build", {"new-tag", "alpha"}},
    };
    tag_store_->sync_all_tags(persist::TagEntityType::Job, new_tags);

    tags = tag_store_->get_tags(persist::TagEntityType::Job, "job-build");
    ASSERT_EQ(tags.size(), 2u);
    EXPECT_EQ(tags[0], "alpha");
    EXPECT_EQ(tags[1], "new-tag");
}

TEST_F(Phase85Test, HasTagViaStringTypeOverload) {
    tag_store_->sync_tags(persist::TagEntityType::WatchGroup,
                          "wg-logs", {"monitoring"});

    // Use the string-type overload (as used in KEL lambda).
    EXPECT_TRUE(tag_store_->has_tag("watch_group", "wg-logs", "monitoring"));
    EXPECT_FALSE(tag_store_->has_tag("watch_group", "wg-logs", "other"));
    EXPECT_FALSE(tag_store_->has_tag("workflow", "wg-logs", "monitoring"));
}

// ── 3. Graceful degradation without TagStore ─────────────────────────────

TEST_F(Phase85Test, HasTagNotRegisteredWithoutTagStore) {
    // If tag_store is null (e.g. in tests), has_tag should not be in
    // the default context.
    auto ctx = kel::make_default_context();
    EXPECT_EQ(ctx.functions.count("has_tag"), 0u);
}

}  // namespace kairos::test
