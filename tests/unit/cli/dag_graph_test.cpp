/// tests/unit/cli/dag_graph_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  dag_graph_test.cpp — DAG graph rendering tests                          ║
// ║  Spec reference: Roadmap §15.10                                         ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/cli/dag_graph.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace kairos::cli {
namespace {

TEST(DagGraphTest, EmptyWorkflow) {
    auto result = render_dag_graph("empty_workflow", {}, false);
    EXPECT_NE(result.find("empty_workflow"), std::string::npos);
    EXPECT_NE(result.find("no jobs"), std::string::npos);
}

TEST(DagGraphTest, SingleJob) {
    std::vector<GraphNode> nodes = {
        {"job_build", "Build", {}, {"build"}},
    };

    auto result = render_dag_graph("simple", nodes, false);
    EXPECT_NE(result.find("simple"), std::string::npos);
    EXPECT_NE(result.find("Build"), std::string::npos);
    EXPECT_NE(result.find("[tags: build]"), std::string::npos);
}

TEST(DagGraphTest, LinearChain) {
    std::vector<GraphNode> nodes = {
        {"job_build", "Build", {}, {}},
        {"job_test", "Test", {"job_build"}, {}},
        {"job_deploy", "Deploy", {"job_test"}, {}},
    };

    auto result = render_dag_graph("pipeline", nodes, false);
    EXPECT_NE(result.find("Build"), std::string::npos);
    EXPECT_NE(result.find("Test"), std::string::npos);
    EXPECT_NE(result.find("Deploy"), std::string::npos);
    // Test depends on Build.
    EXPECT_NE(result.find("Build"), std::string::npos);
}

TEST(DagGraphTest, DiamondDAG) {
    std::vector<GraphNode> nodes = {
        {"job_setup", "Setup", {}, {}},
        {"job_build", "Build", {"job_setup"}, {"build"}},
        {"job_test", "Test", {"job_setup"}, {"test"}},
        {"job_deploy", "Deploy", {"job_build", "job_test"},
         {"deploy", "production"}},
    };

    auto result = render_dag_graph("diamond", nodes, false);
    // Deploy has multiple deps — both Build and Test names appear.
    EXPECT_NE(result.find("Build"), std::string::npos);
    EXPECT_NE(result.find("Test"), std::string::npos);
    EXPECT_NE(result.find("[tags: deploy, production]"),
              std::string::npos);
}

TEST(DagGraphTest, NoColorMode) {
    std::vector<GraphNode> nodes = {
        {"job_build", "Build", {}, {}},
    };

    auto result = render_dag_graph("no_color", nodes, false);
    // Should not contain ANSI escape codes.
    EXPECT_EQ(result.find("\033["), std::string::npos);
}

TEST(DagGraphTest, ColorMode) {
    std::vector<GraphNode> nodes = {
        {"job_build", "Build", {}, {}},
    };

    auto result = render_dag_graph("color_test", nodes, true);
    // Should contain ANSI escape codes.
    EXPECT_NE(result.find("\033["), std::string::npos);
}

TEST(DagGraphTest, JsonOutput) {
    std::vector<GraphNode> nodes = {
        {"job_build", "Build", {}, {"build"}},
        {"job_test", "Test", {"job_build"}, {}},
    };

    auto json_str = render_dag_graph_json("test_wf", nodes);
    auto j = nlohmann::json::parse(json_str);

    EXPECT_EQ(j["workflow"], "test_wf");
    ASSERT_EQ(j["jobs"].size(), 2u);

    auto& build = j["jobs"][0];
    EXPECT_EQ(build["name"], "Build");
    EXPECT_EQ(build["topo_level"], 0);
    EXPECT_EQ(build["tags"].size(), 1u);
    EXPECT_EQ(build["tags"][0], "build");

    auto& test = j["jobs"][1];
    EXPECT_EQ(test["name"], "Test");
    EXPECT_EQ(test["topo_level"], 1);
    EXPECT_EQ(test["needs"].size(), 1u);
    EXPECT_EQ(test["needs"][0], "job_build");
}

TEST(DagGraphTest, AlphabeticalWithinLevel) {
    std::vector<GraphNode> nodes = {
        {"job_zebra", "Zebra", {}, {}},
        {"job_alpha", "Alpha", {}, {}},
        {"job_middle", "Middle", {}, {}},
    };

    auto result = render_dag_graph("sorted", nodes, false);
    // All at level 0, should be alphabetical.
    auto alpha_pos = result.find("Alpha");
    auto middle_pos = result.find("Middle");
    auto zebra_pos = result.find("Zebra");

    EXPECT_LT(alpha_pos, middle_pos);
    EXPECT_LT(middle_pos, zebra_pos);
}

} // anonymous namespace
} // namespace kairos::cli
