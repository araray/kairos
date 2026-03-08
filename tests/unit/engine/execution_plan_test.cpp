/// tests/unit/engine/execution_plan_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  execution_plan_test.cpp — Tests for ExecutionPlan statistics and         ║
// ║  rendering, plus WatchEventType string conversion.                        ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/execution_plan.hpp"
#include "kairos/watch/event_types.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

using namespace kairos::engine;
using namespace kairos::watch;

// ── PlanAction string conversion ────────────────────────────────────────

TEST(PlanActionTest, ToString) {
    EXPECT_EQ(plan_action_to_string(PlanAction::Run), "RUN");
    EXPECT_EQ(plan_action_to_string(PlanAction::Skip), "SKIP");
    EXPECT_EQ(plan_action_to_string(PlanAction::ConditionPending), "PEND");
    EXPECT_EQ(plan_action_to_string(PlanAction::DependencyFailed),
              "DEP_FAIL");
    EXPECT_EQ(plan_action_to_string(PlanAction::Disabled), "DISABLED");
}

// ── ExecutionPlan statistics ────────────────────────────────────────────

TEST(ExecutionPlanTest, EmptyPlanStats) {
    ExecutionPlan plan;
    EXPECT_EQ(plan.jobs_to_run(), 0);
    EXPECT_EQ(plan.jobs_to_skip(), 0);
    EXPECT_EQ(plan.jobs_pending(), 0);
    EXPECT_EQ(plan.max_parallelism(), 0);
}

TEST(ExecutionPlanTest, DiamondPlanStats) {
    ExecutionPlan plan;
    plan.workflow_id = "wfl-abc";
    plan.workflow_name = "Deploy Pipeline";
    plan.trigger_type = "manual";

    plan.entries = {
        {.job_id = "job-setup", .job_name = "setup",
         .level = 0, .action = PlanAction::Run,
         .reason = "No deps, no condition"},
        {.job_id = "job-build", .job_name = "build",
         .level = 1, .action = PlanAction::Run,
         .reason = "Needs [setup] → will be met"},
        {.job_id = "job-test", .job_name = "test",
         .level = 1, .action = PlanAction::Run,
         .reason = "Needs [setup] → will be met"},
        {.job_id = "job-deploy", .job_name = "deploy",
         .level = 2, .action = PlanAction::ConditionPending,
         .reason = "Depends on test result",
         .condition_expr = "job(\"test\").last_success"},
    };

    EXPECT_EQ(plan.jobs_to_run(), 3);
    EXPECT_EQ(plan.jobs_to_skip(), 0);
    EXPECT_EQ(plan.jobs_pending(), 1);
    EXPECT_EQ(plan.max_parallelism(), 2);  // build + test at level 1
}

TEST(ExecutionPlanTest, SkippedJobsCount) {
    ExecutionPlan plan;
    plan.entries = {
        {.job_id = "a", .job_name = "a",
         .level = 0, .action = PlanAction::Run},
        {.job_id = "b", .job_name = "b",
         .level = 1, .action = PlanAction::Skip,
         .reason = "Condition false"},
        {.job_id = "c", .job_name = "c",
         .level = 2, .action = PlanAction::DependencyFailed,
         .reason = "Upstream failed"},
    };

    EXPECT_EQ(plan.jobs_to_run(), 1);
    EXPECT_EQ(plan.jobs_to_skip(), 1);
    EXPECT_EQ(plan.jobs_pending(), 0);
}

// ── Text rendering ──────────────────────────────────────────────────────

TEST(ExecutionPlanTest, RenderTextContainsWorkflowName) {
    ExecutionPlan plan;
    plan.workflow_name = "My Pipeline";
    plan.workflow_id = "wfl-123";
    plan.trigger_type = "manual";
    plan.entries = {
        {.job_id = "a", .job_name = "setup", .level = 0,
         .action = PlanAction::Run, .reason = "Always runs"},
    };

    auto text = plan.render_text();
    EXPECT_TRUE(text.find("My Pipeline") != std::string::npos);
    EXPECT_TRUE(text.find("wfl-123") != std::string::npos);
    EXPECT_TRUE(text.find("manual") != std::string::npos);
    EXPECT_TRUE(text.find("RUN") != std::string::npos);
    EXPECT_TRUE(text.find("1 to run") != std::string::npos);
}

// ── JSON rendering ──────────────────────────────────────────────────────

TEST(ExecutionPlanTest, RenderJsonValid) {
    ExecutionPlan plan;
    plan.workflow_id = "wfl-abc";
    plan.workflow_name = "Test";
    plan.trigger_type = "schedule";
    plan.entries = {
        {.job_id = "a", .job_name = "alpha", .level = 0,
         .action = PlanAction::Run, .reason = "Root",
         .needs = {}},
        {.job_id = "b", .job_name = "beta", .level = 1,
         .action = PlanAction::Skip, .reason = "Condition false",
         .condition_expr = "false",
         .condition_result = "false",
         .needs = {"a"}},
    };

    auto json_str = plan.render_json();
    auto j = nlohmann::json::parse(json_str);

    EXPECT_EQ(j["workflow_id"], "wfl-abc");
    EXPECT_EQ(j["summary"]["jobs_to_run"], 1);
    EXPECT_EQ(j["summary"]["jobs_to_skip"], 1);
    EXPECT_EQ(j["entries"].size(), 2u);
    EXPECT_EQ(j["entries"][0]["action"], "RUN");
    EXPECT_EQ(j["entries"][1]["action"], "SKIP");
    EXPECT_EQ(j["entries"][1]["condition_expr"], "false");
    EXPECT_EQ(j["entries"][1]["needs"][0], "a");
}

// ── WatchEventType flags ────────────────────────────────────────────────

TEST(WatchEventTypeTest, HasFlag) {
    auto combined = WatchEventType::SizeChanged |
                    WatchEventType::ContentChanged;
    EXPECT_TRUE(has_flag(combined, WatchEventType::SizeChanged));
    EXPECT_TRUE(has_flag(combined, WatchEventType::ContentChanged));
    EXPECT_FALSE(has_flag(combined, WatchEventType::FileCreated));
}

TEST(WatchEventTypeTest, ToString) {
    auto type = WatchEventType::FileCreated;
    EXPECT_EQ(event_type_to_string(type), "file_created");

    auto combined = WatchEventType::SizeChanged |
                    WatchEventType::ContentChanged;
    auto str = event_type_to_string(combined);
    EXPECT_TRUE(str.find("size_changed") != std::string::npos);
    EXPECT_TRUE(str.find("content_changed") != std::string::npos);
}

TEST(WatchEventTypeTest, ToStringUnknown) {
    EXPECT_EQ(event_type_to_string(WatchEventType::Unknown), "unknown");
}

TEST(WatchEventTypeTest, StringRoundtrip) {
    auto original = WatchEventType::FileCreated |
                    WatchEventType::SizeChanged |
                    WatchEventType::PatternFound;
    auto str = event_type_to_string(original);
    auto parsed = string_to_event_type(str);
    EXPECT_EQ(static_cast<uint32_t>(parsed),
              static_cast<uint32_t>(original));
}

TEST(WatchEventTypeTest, ParseSingleFlag) {
    auto result = string_to_event_type("file_deleted");
    EXPECT_EQ(result, WatchEventType::FileDeleted);
}

TEST(WatchEventTypeTest, ParseMultipleFlags) {
    auto result = string_to_event_type(
        "size_changed,content_modified,pattern_found");
    EXPECT_TRUE(has_flag(result, WatchEventType::SizeChanged));
    EXPECT_TRUE(has_flag(result, WatchEventType::ContentModified));
    EXPECT_TRUE(has_flag(result, WatchEventType::PatternFound));
    EXPECT_FALSE(has_flag(result, WatchEventType::FileCreated));
}

TEST(WatchEventTypeTest, ParseWithWhitespace) {
    auto result = string_to_event_type("file_created , file_deleted");
    EXPECT_TRUE(has_flag(result, WatchEventType::FileCreated));
    EXPECT_TRUE(has_flag(result, WatchEventType::FileDeleted));
}

TEST(WatchEventTypeTest, AllMetaEvents) {
    auto meta = WatchEventType::ScanTimeout |
                WatchEventType::QueueOverflow |
                WatchEventType::WatchError;
    auto str = event_type_to_string(meta);
    EXPECT_TRUE(str.find("scan_timeout") != std::string::npos);
    EXPECT_TRUE(str.find("queue_overflow") != std::string::npos);
    EXPECT_TRUE(str.find("watch_error") != std::string::npos);
}

// ── FakeFilesystem basic tests ──────────────────────────────────────────

#include "kairos/testing/fake_filesystem.hpp"

using namespace kairos::testing;

TEST(FakeFilesystemTest, AddAndStat) {
    FakeFilesystem fs;
    FakeFileEntry entry;
    entry.size = 100;
    entry.mtime = std::chrono::system_clock::now();

    fs.add_file("/watched/a.txt", entry);

    EXPECT_TRUE(fs.exists("/watched/a.txt"));
    EXPECT_FALSE(fs.exists("/watched/b.txt"));

    auto stat = fs.stat("/watched/a.txt");
    ASSERT_TRUE(stat.has_value());
    EXPECT_EQ(stat->size, 100u);
}

TEST(FakeFilesystemTest, ScanUnderRoot) {
    FakeFilesystem fs;
    fs.add_file("/root/a.txt", {.size = 10});
    fs.add_file("/root/sub/b.txt", {.size = 20});
    fs.add_file("/other/c.txt", {.size = 30});

    auto results = fs.scan("/root");
    EXPECT_EQ(results.size(), 2u);  // a.txt and sub/b.txt
}

TEST(FakeFilesystemTest, ScanWithMaxDepth) {
    FakeFilesystem fs;
    fs.add_file("/root/a.txt", {.size = 10});
    fs.add_file("/root/sub/b.txt", {.size = 20});
    fs.add_file("/root/sub/deep/c.txt", {.size = 30});

    auto results = fs.scan("/root", 1);  // max_depth=1
    EXPECT_EQ(results.size(), 2u);  // a.txt and sub/b.txt
}

TEST(FakeFilesystemTest, RemoveFile) {
    FakeFilesystem fs;
    fs.add_file("/root/a.txt", {.size = 10});
    EXPECT_TRUE(fs.exists("/root/a.txt"));

    fs.remove_file("/root/a.txt");
    EXPECT_FALSE(fs.exists("/root/a.txt"));
}

TEST(FakeFilesystemTest, ModifyFile) {
    FakeFilesystem fs;
    fs.add_file("/root/a.txt", {.size = 10});

    FakeFileEntry modified;
    modified.size = 200;
    fs.modify_file("/root/a.txt", modified);

    auto stat = fs.stat("/root/a.txt");
    EXPECT_EQ(stat->size, 200u);
}
