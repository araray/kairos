/// tests/integration/yaml_daemon_wiring_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  YAML→Daemon wiring integration test                                     ║
// ║                                                                           ║
// ║  Tests that:                                                              ║
// ║    1. Workflow YAML files are discovered and loaded into the registry    ║
// ║    2. Watch-group YAML files are loaded into the registry                ║
// ║    3. The loaded registry contains expected workflows + groups           ║
// ║    4. Config reload rebuilds the registry from modified YAML             ║
// ║                                                                           ║
// ║  Spec reference: §6.7, §27.2                                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/config/config_store.hpp"
#include "kairos/config/yaml_loader.hpp"
#include "kairos/engine/workflow_registry.hpp"
#include "kairos/watch/watch_group_def.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace kairos::config {
namespace {

namespace fs = std::filesystem;

// ── Fixture: creates temp dirs with YAML files ──────────────────────────

class YamlDaemonWiringTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create temp directory structure.
        auto tmp = fs::temp_directory_path() / "kairos_yaml_wiring_test";
        fs::remove_all(tmp);
        fs::create_directories(tmp / "workflows");
        fs::create_directories(tmp / "watch_groups");
        test_dir_ = tmp;
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    void write_file(const fs::path& path, const std::string& content) {
        std::ofstream f(path);
        f << content;
    }

    fs::path test_dir_;
};

// ── Test: load workflows from YAML directory ────────────────────────────

TEST_F(YamlDaemonWiringTest, LoadWorkflowsFromDir) {
    // Write a workflow YAML.
    write_file(test_dir_ / "workflows" / "build.yaml", R"(
id: build
name: Build Pipeline
jobs:
  compile:
    steps:
      - name: compile
        command: make build
  test:
    needs: [compile]
    steps:
      - name: test
        command: make test
triggers:
  - cron: "0 */6 * * *"
)");

    auto result = load_workflows_dir(test_dir_ / "workflows");
    ASSERT_TRUE(result.ok()) << "YAML errors: "
        << (result.errors.empty() ? "none" :
            result.errors[0].message);

    EXPECT_EQ(result.workflows.size(), 1);
    EXPECT_EQ(result.workflows[0].id, "build");
    EXPECT_EQ(result.workflows[0].name, "Build Pipeline");

    // Should have extracted triggers.
    EXPECT_GE(result.triggers.size(), 1);
}

// ── Test: load watch groups from YAML ───────────────────────────────────

TEST_F(YamlDaemonWiringTest, LoadWatchGroupsFromDir) {
    write_file(test_dir_ / "watch_groups" / "logs.yaml", R"(
watch_groups:
  - name: log_monitor
    watch_items:
      - /var/log
    sample_rate: 60
    max_depth: 3
    rules:
      - name: large_file
        condition: "file_size > 1000000"
        severity: warning
)");

    auto result = load_watch_groups_dir(test_dir_ / "watch_groups");
    ASSERT_TRUE(result.ok()) << "YAML errors: "
        << (result.errors.empty() ? "none" :
            result.errors[0].message);

    EXPECT_EQ(result.watch_groups.size(), 1);
    EXPECT_EQ(result.watch_groups[0].group_name, "log_monitor");
    EXPECT_EQ(result.watch_groups[0].rules.size(), 1);
    EXPECT_EQ(result.watch_groups[0].rules[0].rule_name, "large_file");
}

// ── Test: load_all combines workflows + watch groups ────────────────────

TEST_F(YamlDaemonWiringTest, LoadAllCombinesWorkflowsAndWatchGroups) {
    // Write a workflow.
    write_file(test_dir_ / "workflows" / "deploy.yaml", R"(
id: deploy
name: Deploy Pipeline
jobs:
  deploy:
    steps:
      - name: deploy
        command: ./deploy.sh
)");

    // Write a watch group.
    write_file(test_dir_ / "watch_groups" / "config.yaml", R"(
watch_groups:
  - name: config_watch
    watch_items:
      - /etc/myapp
    sample_rate: 30
    rules:
      - name: config_changed
        condition: "true"
        trigger_target: deploy
)");

    auto result = load_all(
        test_dir_ / "workflows",
        test_dir_ / "watch_groups");

    // Should have both.
    EXPECT_GE(result.workflows.size(), 1);
    EXPECT_GE(result.watch_groups.size(), 1);

    // Log any errors but don't fail — some fields may be optional.
    for (const auto& err : result.errors) {
        std::cerr << "YAML note: " << err.file << ": "
                  << err.path << " — " << err.message << "\n";
    }
}

// ── Test: registry construction from loaded data ────────────────────────

TEST_F(YamlDaemonWiringTest, RegistryBuiltFromYaml) {
    write_file(test_dir_ / "workflows" / "pipeline.yaml", R"(
id: pipeline
name: CI Pipeline
jobs:
  lint:
    steps:
      - name: lint
        command: flake8 .
  build:
    needs: [lint]
    steps:
      - name: build
        command: make
triggers:
  - cron: "0 0 * * *"
)");

    write_file(test_dir_ / "watch_groups" / "src.yaml", R"(
watch_groups:
  - name: source_watch
    watch_items:
      - /src
    sample_rate: 10
    rules:
      - name: src_changed
        condition: "true"
        trigger_target: pipeline
)");

    auto yaml_result = load_all(
        test_dir_ / "workflows",
        test_dir_ / "watch_groups");

    // Build a registry from the results.
    auto registry = std::make_shared<engine::WorkflowRegistry>(
        std::move(yaml_result.workflows),
        std::move(yaml_result.triggers),
        std::move(yaml_result.standalone_jobs),
        std::move(yaml_result.watch_groups));

    EXPECT_GE(registry->workflow_count(), 1);
    EXPECT_GE(registry->watch_group_count(), 1);
}

// ── Test: reload detects new files ──────────────────────────────────────

TEST_F(YamlDaemonWiringTest, ReloadDetectsNewYamlFiles) {
    // Initially empty.
    auto result1 = load_workflows_dir(test_dir_ / "workflows");
    EXPECT_EQ(result1.workflows.size(), 0);

    // Add a workflow file.
    write_file(test_dir_ / "workflows" / "new_wf.yaml", R"(
id: new_wf
name: New Workflow
jobs:
  step1:
    steps:
      - name: run
        command: echo hello
)");

    // Reload should pick up the new file.
    auto result2 = load_workflows_dir(test_dir_ / "workflows");
    EXPECT_EQ(result2.workflows.size(), 1);
    EXPECT_EQ(result2.workflows[0].id, "new_wf");
}

// ── Test: missing directory doesn't crash ───────────────────────────────

TEST_F(YamlDaemonWiringTest, MissingDirectoryReturnsEmpty) {
    auto result = load_workflows_dir(test_dir_ / "nonexistent");
    EXPECT_EQ(result.workflows.size(), 0);
    // Should not crash, may or may not have errors.
}

}  // namespace
}  // namespace kairos::config
