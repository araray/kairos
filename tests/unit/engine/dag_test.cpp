/// tests/unit/engine/dag_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  dag_test.cpp — Tests for WorkflowDag construction and topological sort  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/dag.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using namespace kairos::engine;

// ── Helper: create DagNode ──────────────────────────────────────────────

DagNode make_node(std::string id, std::string name,
                  std::vector<std::string> needs = {},
                  std::optional<std::string> condition = std::nullopt) {
    DagNode n;
    n.job_id = std::move(id);
    n.job_name = std::move(name);
    n.needs = std::move(needs);
    n.condition_expr = std::move(condition);
    return n;
}

// ── Single node DAG ─────────────────────────────────────────────────────

TEST(DagTest, SingleNode) {
    auto dag = WorkflowDag::build({make_node("job-a", "alpha")});

    EXPECT_EQ(dag.size(), 1u);
    EXPECT_TRUE(dag.is_standalone());
    EXPECT_EQ(dag.max_level(), 0);
    EXPECT_EQ(dag.level_count(), 1);

    auto& order = dag.topological_order();
    ASSERT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], "job-a");

    auto& level0 = dag.jobs_at_level(0);
    ASSERT_EQ(level0.size(), 1u);
    EXPECT_EQ(level0[0], "job-a");
}

// ── Linear chain: A → B → C ────────────────────────────────────────────

TEST(DagTest, LinearChain) {
    auto dag = WorkflowDag::build({
        make_node("job-a", "alpha"),
        make_node("job-b", "beta", {"job-a"}),
        make_node("job-c", "gamma", {"job-b"}),
    });

    EXPECT_EQ(dag.size(), 3u);
    EXPECT_FALSE(dag.is_standalone());
    EXPECT_EQ(dag.max_level(), 2);

    auto& order = dag.topological_order();
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], "job-a");
    EXPECT_EQ(order[1], "job-b");
    EXPECT_EQ(order[2], "job-c");

    // Level verification.
    auto& l0 = dag.jobs_at_level(0);
    ASSERT_EQ(l0.size(), 1u);
    EXPECT_EQ(l0[0], "job-a");

    auto& l1 = dag.jobs_at_level(1);
    ASSERT_EQ(l1.size(), 1u);
    EXPECT_EQ(l1[0], "job-b");

    auto& l2 = dag.jobs_at_level(2);
    ASSERT_EQ(l2.size(), 1u);
    EXPECT_EQ(l2[0], "job-c");
}

// ── Diamond DAG: setup → {build, test} → deploy ────────────────────────

TEST(DagTest, DiamondDag) {
    auto dag = WorkflowDag::build({
        make_node("job-setup",  "setup"),
        make_node("job-build",  "build", {"job-setup"}),
        make_node("job-test",   "test",  {"job-setup"}),
        make_node("job-deploy", "deploy", {"job-build", "job-test"}),
    });

    EXPECT_EQ(dag.size(), 4u);
    EXPECT_EQ(dag.max_level(), 2);

    // Level 0: setup.
    auto& l0 = dag.jobs_at_level(0);
    ASSERT_EQ(l0.size(), 1u);
    EXPECT_EQ(l0[0], "job-setup");

    // Level 1: build and test (parallel).
    auto& l1 = dag.jobs_at_level(1);
    ASSERT_EQ(l1.size(), 2u);
    // Deterministic: sorted lexicographically by job_id.
    EXPECT_EQ(l1[0], "job-build");
    EXPECT_EQ(l1[1], "job-test");

    // Level 2: deploy.
    auto& l2 = dag.jobs_at_level(2);
    ASSERT_EQ(l2.size(), 1u);
    EXPECT_EQ(l2[0], "job-deploy");
}

// ── Wide DAG: multiple independent roots ────────────────────────────────

TEST(DagTest, MultipleRoots) {
    auto dag = WorkflowDag::build({
        make_node("job-c", "charlie"),
        make_node("job-a", "alpha"),
        make_node("job-b", "beta"),
    });

    EXPECT_EQ(dag.size(), 3u);
    EXPECT_EQ(dag.max_level(), 0);

    // All at level 0, sorted by job_id.
    auto& l0 = dag.jobs_at_level(0);
    ASSERT_EQ(l0.size(), 3u);
    EXPECT_EQ(l0[0], "job-a");
    EXPECT_EQ(l0[1], "job-b");
    EXPECT_EQ(l0[2], "job-c");
}

// ── Deterministic ordering ──────────────────────────────────────────────

TEST(DagTest, DeterministicOrdering) {
    // Build the same DAG twice with different insertion orders.
    auto dag1 = WorkflowDag::build({
        make_node("job-x", "x"),
        make_node("job-y", "y"),
        make_node("job-z", "z", {"job-x", "job-y"}),
    });

    auto dag2 = WorkflowDag::build({
        make_node("job-z", "z", {"job-y", "job-x"}),
        make_node("job-y", "y"),
        make_node("job-x", "x"),
    });

    EXPECT_EQ(dag1.topological_order(), dag2.topological_order());
}

// ── Cycle detection ─────────────────────────────────────────────────────

TEST(DagTest, CycleDetection) {
    EXPECT_THROW(
        WorkflowDag::build({
            make_node("job-a", "alpha", {"job-b"}),
            make_node("job-b", "beta",  {"job-a"}),
        }),
        std::runtime_error);
}

TEST(DagTest, ThreeNodeCycle) {
    EXPECT_THROW(
        WorkflowDag::build({
            make_node("job-a", "alpha", {"job-c"}),
            make_node("job-b", "beta",  {"job-a"}),
            make_node("job-c", "gamma", {"job-b"}),
        }),
        std::runtime_error);
}

TEST(DagTest, SelfCycle) {
    EXPECT_THROW(
        WorkflowDag::build({
            make_node("job-a", "alpha", {"job-a"}),
        }),
        std::runtime_error);
}

// ── Validation errors ───────────────────────────────────────────────────

TEST(DagTest, DuplicateJobId) {
    EXPECT_THROW(
        WorkflowDag::build({
            make_node("job-a", "alpha"),
            make_node("job-a", "alpha-copy"),
        }),
        std::runtime_error);
}

TEST(DagTest, UnresolvedDependency) {
    EXPECT_THROW(
        WorkflowDag::build({
            make_node("job-a", "alpha", {"job-nonexistent"}),
        }),
        std::runtime_error);
}

// ── Successor/Predecessor queries ───────────────────────────────────────

TEST(DagTest, SuccessorsAndPredecessors) {
    auto dag = WorkflowDag::build({
        make_node("job-a", "alpha"),
        make_node("job-b", "beta",  {"job-a"}),
        make_node("job-c", "gamma", {"job-a"}),
        make_node("job-d", "delta", {"job-b", "job-c"}),
    });

    // job-a has successors: b, c.
    auto& a_succ = dag.successors("job-a");
    EXPECT_EQ(a_succ.size(), 2u);

    // job-d has predecessors: b, c.
    auto& d_pred = dag.predecessors("job-d");
    EXPECT_EQ(d_pred.size(), 2u);

    // job-a has no predecessors.
    auto& a_pred = dag.predecessors("job-a");
    EXPECT_TRUE(a_pred.empty());

    // job-d has no successors.
    auto& d_succ = dag.successors("job-d");
    EXPECT_TRUE(d_succ.empty());
}

// ── Node access ─────────────────────────────────────────────────────────

TEST(DagTest, NodeAccess) {
    auto dag = WorkflowDag::build({
        make_node("job-a", "alpha", {}, "true"),
    });

    auto& node = dag.node("job-a");
    EXPECT_EQ(node.job_name, "alpha");
    EXPECT_TRUE(node.condition_expr.has_value());
    EXPECT_EQ(*node.condition_expr, "true");
    EXPECT_EQ(node.topo_level, 0);
}

TEST(DagTest, HasNode) {
    auto dag = WorkflowDag::build({make_node("job-a", "alpha")});
    EXPECT_TRUE(dag.has_node("job-a"));
    EXPECT_FALSE(dag.has_node("job-b"));
}

TEST(DagTest, NodeAccessThrowsOnMissing) {
    auto dag = WorkflowDag::build({make_node("job-a", "alpha")});
    EXPECT_THROW(dag.node("job-nonexistent"), std::out_of_range);
}

// ── Standalone DAG helper ───────────────────────────────────────────────

TEST(DagTest, MakeStandaloneDag) {
    auto dag = make_standalone_dag("job-x", "standalone-job",
                                    "job(\"prev\").last_success");
    EXPECT_EQ(dag.size(), 1u);
    EXPECT_TRUE(dag.is_standalone());

    auto& node = dag.node("job-x");
    EXPECT_EQ(node.job_name, "standalone-job");
    EXPECT_TRUE(node.condition_expr.has_value());
    EXPECT_EQ(*node.condition_expr, "job(\"prev\").last_success");
}

// ── Out-of-range level access ───────────────────────────────────────────

TEST(DagTest, OutOfRangeLevelReturnsEmpty) {
    auto dag = WorkflowDag::build({make_node("job-a", "alpha")});
    EXPECT_TRUE(dag.jobs_at_level(-1).empty());
    EXPECT_TRUE(dag.jobs_at_level(1).empty());
    EXPECT_TRUE(dag.jobs_at_level(100).empty());
}

// ── Complex DAG with multiple levels ────────────────────────────────────

TEST(DagTest, ComplexMultiLevel) {
    //   a ──┬── c ──── e
    //   b ──┘         ╱
    //   d ───────────╱
    auto dag = WorkflowDag::build({
        make_node("job-a", "A"),
        make_node("job-b", "B"),
        make_node("job-c", "C", {"job-a", "job-b"}),
        make_node("job-d", "D"),
        make_node("job-e", "E", {"job-c", "job-d"}),
    });

    EXPECT_EQ(dag.size(), 5u);
    EXPECT_EQ(dag.max_level(), 2);

    // Level 0: a, b, d (sorted).
    auto& l0 = dag.jobs_at_level(0);
    ASSERT_EQ(l0.size(), 3u);
    EXPECT_EQ(l0[0], "job-a");
    EXPECT_EQ(l0[1], "job-b");
    EXPECT_EQ(l0[2], "job-d");

    // Level 1: c.
    auto& l1 = dag.jobs_at_level(1);
    ASSERT_EQ(l1.size(), 1u);
    EXPECT_EQ(l1[0], "job-c");

    // Level 2: e.
    auto& l2 = dag.jobs_at_level(2);
    ASSERT_EQ(l2.size(), 1u);
    EXPECT_EQ(l2[0], "job-e");
}

// ── find_cycle on acyclic graph returns nullopt ─────────────────────────

TEST(DagTest, FindCycleOnAcyclicReturnsNullopt) {
    auto dag = WorkflowDag::build({
        make_node("job-a", "alpha"),
        make_node("job-b", "beta", {"job-a"}),
    });
    auto cycle = dag.find_cycle();
    EXPECT_FALSE(cycle.has_value());
}
