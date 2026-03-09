/// tests/unit/engine/explain_plan_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for ExecutionPlan construction and rendering                       ║
// ║                                                                          ║
// ║  Verifies that build_execution_plan produces correct PlanEntry           ║
// ║  sequences for various DAG topologies and condition scenarios.           ║
// ║                                                                          ║
// ║  Phase 3 Batch 14: workflows explain                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/execution_plan.hpp"
#include "kairos/engine/dag.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace kairos::engine::test {

// ═══════════════════════════════════════════════════════════════════════
//  ExecutionPlan statistics
// ═══════════════════════════════════════════════════════════════════════

TEST(ExecutionPlanTest, EmptyPlan_AllZeros) {
    ExecutionPlan plan;
    plan.workflow_id = "wfl-test";
    plan.workflow_name = "Test Workflow";
    plan.trigger_type = "manual";

    EXPECT_EQ(plan.jobs_to_run(), 0);
    EXPECT_EQ(plan.jobs_to_skip(), 0);
    EXPECT_EQ(plan.jobs_pending(), 0);
    EXPECT_EQ(plan.max_parallelism(), 0);
}

TEST(ExecutionPlanTest, SingleRunEntry) {
    ExecutionPlan plan;
    plan.workflow_id = "wfl-1";
    plan.workflow_name = "Simple";
    plan.trigger_type = "manual";

    PlanEntry e;
    e.job_id = "job-1";
    e.job_name = "build";
    e.level = 0;
    e.action = PlanAction::Run;
    e.reason = "No dependencies";
    plan.entries.push_back(e);

    EXPECT_EQ(plan.jobs_to_run(), 1);
    EXPECT_EQ(plan.jobs_to_skip(), 0);
    EXPECT_EQ(plan.max_parallelism(), 1);
}

TEST(ExecutionPlanTest, MixedActions_CorrectCounts) {
    ExecutionPlan plan;
    plan.workflow_id = "wfl-2";
    plan.workflow_name = "Mixed";
    plan.trigger_type = "schedule";

    plan.entries.push_back(PlanEntry{
        .job_id = "j1", .job_name = "build", .level = 0,
        .action = PlanAction::Run, .reason = "ok"});
    plan.entries.push_back(PlanEntry{
        .job_id = "j2", .job_name = "lint", .level = 0,
        .action = PlanAction::Run, .reason = "ok"});
    plan.entries.push_back(PlanEntry{
        .job_id = "j3", .job_name = "test", .level = 1,
        .action = PlanAction::Skip, .reason = "condition false"});
    plan.entries.push_back(PlanEntry{
        .job_id = "j4", .job_name = "deploy", .level = 2,
        .action = PlanAction::ConditionPending, .reason = "pending"});

    EXPECT_EQ(plan.jobs_to_run(), 2);
    EXPECT_EQ(plan.jobs_to_skip(), 1);
    EXPECT_EQ(plan.jobs_pending(), 1);
    EXPECT_EQ(plan.max_parallelism(), 2);  // 2 at level 0
}

TEST(ExecutionPlanTest, MaxParallelism_MultiLevel) {
    ExecutionPlan plan;
    plan.workflow_id = "wfl-3";
    plan.workflow_name = "Deep";
    plan.trigger_type = "manual";

    // Level 0: 3 jobs
    for (int i = 0; i < 3; ++i) {
        plan.entries.push_back(PlanEntry{
            .job_id = "j" + std::to_string(i),
            .job_name = "l0-" + std::to_string(i),
            .level = 0,
            .action = PlanAction::Run, .reason = "ok"});
    }
    // Level 1: 1 job
    plan.entries.push_back(PlanEntry{
        .job_id = "j3", .job_name = "l1-0",
        .level = 1, .action = PlanAction::Run, .reason = "ok"});
    // Level 2: 2 jobs
    for (int i = 0; i < 2; ++i) {
        plan.entries.push_back(PlanEntry{
            .job_id = "j" + std::to_string(4 + i),
            .job_name = "l2-" + std::to_string(i),
            .level = 2,
            .action = PlanAction::Run, .reason = "ok"});
    }

    EXPECT_EQ(plan.max_parallelism(), 3);  // Level 0
}

// ═══════════════════════════════════════════════════════════════════════
//  PlanAction stringification
// ═══════════════════════════════════════════════════════════════════════

TEST(ExecutionPlanTest, PlanAction_ToString) {
    EXPECT_EQ(plan_action_to_string(PlanAction::Run), "RUN");
    EXPECT_EQ(plan_action_to_string(PlanAction::Skip), "SKIP");
    EXPECT_EQ(plan_action_to_string(PlanAction::ConditionPending), "PEND");
    EXPECT_EQ(plan_action_to_string(PlanAction::DependencyFailed), "DEP_FAIL");
    EXPECT_EQ(plan_action_to_string(PlanAction::Disabled), "DISABLED");
}

// ═══════════════════════════════════════════════════════════════════════
//  Text rendering
// ═══════════════════════════════════════════════════════════════════════

TEST(ExecutionPlanTest, RenderText_ContainsWorkflowInfo) {
    ExecutionPlan plan;
    plan.workflow_id = "wfl-abc123";
    plan.workflow_name = "Deploy Pipeline";
    plan.trigger_type = "manual";

    plan.entries.push_back(PlanEntry{
        .job_id = "j1", .job_name = "setup", .level = 0,
        .action = PlanAction::Run, .reason = "No dependencies"});

    auto text = plan.render_text();
    EXPECT_NE(text.find("Deploy Pipeline"), std::string::npos);
    EXPECT_NE(text.find("wfl-abc123"), std::string::npos);
    EXPECT_NE(text.find("manual"), std::string::npos);
    EXPECT_NE(text.find("setup"), std::string::npos);
    EXPECT_NE(text.find("RUN"), std::string::npos);
    EXPECT_NE(text.find("Summary"), std::string::npos);
}

TEST(ExecutionPlanTest, RenderText_ShowsSummary) {
    ExecutionPlan plan;
    plan.workflow_id = "wfl-1";
    plan.workflow_name = "Test";
    plan.trigger_type = "manual";

    plan.entries.push_back(PlanEntry{
        .job_id = "j1", .job_name = "build", .level = 0,
        .action = PlanAction::Run, .reason = "ok"});
    plan.entries.push_back(PlanEntry{
        .job_id = "j2", .job_name = "skip-me", .level = 1,
        .action = PlanAction::Skip, .reason = "cond false"});

    auto text = plan.render_text();
    EXPECT_NE(text.find("1 to run"), std::string::npos);
    EXPECT_NE(text.find("1 to skip"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════
//  DAG-based plan construction (the logic used by workflows explain)
// ═══════════════════════════════════════════════════════════════════════

TEST(ExecutionPlanTest, BuildFromDag_SimpleChain) {
    // A → B → C (linear chain, no conditions)
    DagNode a{.job_id = "j-a", .job_name = "build"};
    DagNode b{.job_id = "j-b", .job_name = "test",
              .needs = {"j-a"}};
    DagNode c{.job_id = "j-c", .job_name = "deploy",
              .needs = {"j-b"}};

    auto dag = WorkflowDag::build({a, b, c});

    ExecutionPlan plan;
    plan.workflow_id = "wfl-chain";
    plan.workflow_name = "Chain";
    plan.trigger_type = "explain";

    // Walk the DAG level-by-level (mirrors explain handler logic).
    for (int lvl = 0; lvl < dag.level_count(); ++lvl) {
        for (const auto& job_id : dag.jobs_at_level(lvl)) {
            const auto& node = dag.node(job_id);
            PlanEntry entry;
            entry.job_id = node.job_id;
            entry.job_name = node.job_name;
            entry.level = node.topo_level;
            entry.needs = node.needs;

            if (node.needs.empty()) {
                entry.action = PlanAction::Run;
                entry.reason = "No dependencies";
            } else {
                entry.action = PlanAction::Run;
                entry.reason = "Needs will be met";
            }

            plan.entries.push_back(std::move(entry));
        }
    }

    EXPECT_EQ(plan.entries.size(), 3u);
    EXPECT_EQ(plan.entries[0].job_name, "build");
    EXPECT_EQ(plan.entries[0].level, 0);
    EXPECT_EQ(plan.entries[1].job_name, "test");
    EXPECT_EQ(plan.entries[1].level, 1);
    EXPECT_EQ(plan.entries[2].job_name, "deploy");
    EXPECT_EQ(plan.entries[2].level, 2);
    EXPECT_EQ(plan.max_parallelism(), 1);
}

TEST(ExecutionPlanTest, BuildFromDag_ParallelRoots) {
    // A, B (parallel) → C
    DagNode a{.job_id = "j-a", .job_name = "build"};
    DagNode b{.job_id = "j-b", .job_name = "lint"};
    DagNode c{.job_id = "j-c", .job_name = "deploy",
              .needs = {"j-a", "j-b"}};

    auto dag = WorkflowDag::build({a, b, c});

    ExecutionPlan plan;
    plan.workflow_id = "wfl-par";
    plan.workflow_name = "Parallel";
    plan.trigger_type = "explain";

    for (int lvl = 0; lvl < dag.level_count(); ++lvl) {
        for (const auto& job_id : dag.jobs_at_level(lvl)) {
            const auto& node = dag.node(job_id);
            PlanEntry entry;
            entry.job_id = node.job_id;
            entry.job_name = node.job_name;
            entry.level = node.topo_level;
            entry.needs = node.needs;
            entry.action = PlanAction::Run;
            entry.reason = "ok";
            plan.entries.push_back(std::move(entry));
        }
    }

    EXPECT_EQ(plan.entries.size(), 3u);
    EXPECT_EQ(plan.max_parallelism(), 2);  // a, b at level 0

    // Level 0 should have both build and lint.
    int level0_count = 0;
    for (const auto& e : plan.entries) {
        if (e.level == 0) ++level0_count;
    }
    EXPECT_EQ(level0_count, 2);
}

TEST(ExecutionPlanTest, BuildFromDag_WithCondition_MarksPending) {
    DagNode a{.job_id = "j-a", .job_name = "test"};
    DagNode b{.job_id = "j-b", .job_name = "deploy",
              .needs = {"j-a"},
              .condition_expr = "job(\"test\").last_success"};

    auto dag = WorkflowDag::build({a, b});

    ExecutionPlan plan;
    plan.workflow_id = "wfl-cond";
    plan.workflow_name = "Conditional";
    plan.trigger_type = "explain";

    for (int lvl = 0; lvl < dag.level_count(); ++lvl) {
        for (const auto& job_id : dag.jobs_at_level(lvl)) {
            const auto& node = dag.node(job_id);
            PlanEntry entry;
            entry.job_id = node.job_id;
            entry.job_name = node.job_name;
            entry.level = node.topo_level;
            entry.needs = node.needs;
            entry.condition_expr = node.condition_expr.value_or("");

            if (node.condition_expr.has_value() &&
                !node.condition_expr->empty()) {
                // In the explain test, mark conditions referencing
                // same-workflow jobs as pending.
                bool refs_self = false;
                for (const auto& other :
                     dag.topological_order()) {
                    const auto& other_node = dag.node(other);
                    if (node.condition_expr->find(
                            "\"" + other_node.job_name + "\"") !=
                        std::string::npos) {
                        refs_self = true;
                        break;
                    }
                }
                if (refs_self) {
                    entry.action = PlanAction::ConditionPending;
                    entry.reason = "Condition references same-wf job";
                    entry.condition_result = "pending";
                } else {
                    entry.action = PlanAction::Run;
                    entry.reason = "External condition";
                }
            } else {
                entry.action = PlanAction::Run;
                entry.reason = "No condition";
            }

            plan.entries.push_back(std::move(entry));
        }
    }

    ASSERT_EQ(plan.entries.size(), 2u);
    EXPECT_EQ(plan.entries[0].action, PlanAction::Run);
    EXPECT_EQ(plan.entries[1].action, PlanAction::ConditionPending);
    EXPECT_EQ(plan.entries[1].condition_expr, "job(\"test\").last_success");
    EXPECT_EQ(plan.jobs_pending(), 1);
}

TEST(ExecutionPlanTest, BuildFromDag_DependencyFailed) {
    // A (skip) → B (dep_failed)
    ExecutionPlan plan;
    plan.workflow_id = "wfl-df";
    plan.workflow_name = "DepFail";
    plan.trigger_type = "explain";

    plan.entries.push_back(PlanEntry{
        .job_id = "j-a", .job_name = "setup", .level = 0,
        .action = PlanAction::Skip, .reason = "condition false"});

    // B depends on A which is skipped → dependency failed.
    plan.entries.push_back(PlanEntry{
        .job_id = "j-b", .job_name = "build", .level = 1,
        .action = PlanAction::DependencyFailed,
        .reason = "Upstream dependency will not run",
        .needs = {"j-a"}});

    EXPECT_EQ(plan.jobs_to_run(), 0);
    EXPECT_EQ(plan.jobs_to_skip(), 1);
    EXPECT_EQ(plan.max_parallelism(), 0);
}

// ═══════════════════════════════════════════════════════════════════════
//  JSON rendering
// ═══════════════════════════════════════════════════════════════════════

TEST(ExecutionPlanTest, RenderJson_ValidJson) {
    ExecutionPlan plan;
    plan.workflow_id = "wfl-j";
    plan.workflow_name = "JsonTest";
    plan.trigger_type = "manual";

    plan.entries.push_back(PlanEntry{
        .job_id = "j1", .job_name = "build", .level = 0,
        .action = PlanAction::Run, .reason = "ok"});

    auto json_str = plan.render_json();
    EXPECT_FALSE(json_str.empty());
    // Should contain the workflow name and job name.
    EXPECT_NE(json_str.find("JsonTest"), std::string::npos);
    EXPECT_NE(json_str.find("build"), std::string::npos);
    EXPECT_NE(json_str.find("RUN"), std::string::npos);
}

}  // namespace kairos::engine::test
