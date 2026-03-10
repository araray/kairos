/// tests/unit/config/yaml_loader_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for config/yaml_loader.hpp — YAML workflow and watch-group        ║
// ║  parsing, validation, ID generation, DAG construction, triggers.          ║
// ║                                                                           ║
// ║  Spec reference: §4.5, §4.6, §6.7                                        ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/config/yaml_loader.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace kairos::config {
namespace {

namespace fs = std::filesystem;

// ── Helpers ─────────────────────────────────────────────────────────────

/// Create a temporary directory and clean up after test.
class TmpDir {
public:
    TmpDir() {
        path_ = fs::temp_directory_path() / ("kairos_test_" +
            std::to_string(std::hash<std::thread::id>{}(
                std::this_thread::get_id())) +
            "_" + std::to_string(counter_++));
        fs::create_directories(path_);
    }
    ~TmpDir() { fs::remove_all(path_); }

    const fs::path& path() const { return path_; }

    void write(const std::string& name, const std::string& content) {
        std::ofstream out(path_ / name);
        out << content;
    }

private:
    fs::path path_;
    static inline int counter_ = 0;
};

// ═══════════════════════════════════════════════════════════════════════
//  WORKFLOW PARSING
// ═══════════════════════════════════════════════════════════════════════

TEST(YamlLoaderTest, ParseMinimalWorkflow) {
    auto r = load_workflow_string(R"(
name: "simple"
jobs:
  hello:
    steps:
      - name: "greet"
        run: "echo hello"
)");

    ASSERT_TRUE(r.ok()) << r.errors[0].message;
    ASSERT_EQ(r.workflows.size(), 1u);

    const auto& wf = r.workflows[0];
    EXPECT_EQ(wf.workflow_name, "simple");
    EXPECT_FALSE(wf.workflow_id.empty());
    EXPECT_TRUE(wf.workflow_id.starts_with("wfl-"));

    ASSERT_EQ(wf.jobs.size(), 1u);
    EXPECT_EQ(wf.jobs[0].job_name, "hello");
    EXPECT_TRUE(wf.jobs[0].job_id.starts_with("job-"));

    ASSERT_EQ(wf.jobs[0].steps.size(), 1u);
    EXPECT_EQ(wf.jobs[0].steps[0].step_name, "greet");
    EXPECT_EQ(wf.jobs[0].steps[0].command, "echo hello");
    EXPECT_TRUE(wf.jobs[0].steps[0].step_id.starts_with("stp-"));
}

TEST(YamlLoaderTest, ParseWorkflowWithDag) {
    auto r = load_workflow_string(R"(
name: "pipeline"
jobs:
  build:
    steps:
      - name: compile
        run: "make build"
  test:
    needs: [build]
    steps:
      - name: "run tests"
        run: "make test"
  deploy:
    needs: [test]
    condition: "job('test').last_success"
    steps:
      - name: "deploy"
        run: "./deploy.sh"
)");

    ASSERT_TRUE(r.ok()) << r.errors[0].message;

    const auto& wf = r.workflows[0];
    ASSERT_EQ(wf.jobs.size(), 3u);

    // DAG should have 3 levels: build(0), test(1), deploy(2).
    EXPECT_EQ(wf.dag.max_level(), 2);
    EXPECT_EQ(wf.dag.size(), 3u);

    // Topological sort should produce a valid order.
    auto order = wf.dag.topological_order();
    ASSERT_EQ(order.size(), 3u);

    // Build should come first.
    auto find_job = [&](const std::string& name) -> const engine::JobDef* {
        for (const auto& j : wf.jobs) {
            if (j.job_name == name) return &j;
        }
        return nullptr;
    };

    auto* build = find_job("build");
    auto* test = find_job("test");
    auto* deploy = find_job("deploy");
    ASSERT_NE(build, nullptr);
    ASSERT_NE(test, nullptr);
    ASSERT_NE(deploy, nullptr);

    // test needs build (needs should be resolved to IDs).
    ASSERT_EQ(test->needs.size(), 1u);
    EXPECT_EQ(test->needs[0], build->job_id);

    // deploy has a condition.
    ASSERT_TRUE(deploy->condition_expr.has_value());
    EXPECT_EQ(*deploy->condition_expr, "job('test').last_success");
}

TEST(YamlLoaderTest, ParseWorkflowWithTriggers) {
    auto r = load_workflow_string(R"(
name: "scheduled"
triggers:
  - type: cron
    cron: "0 2 * * *"
  - type: interval
    interval: "5m"
  - type: manual
jobs:
  backup:
    steps:
      - name: "backup"
        run: "/opt/scripts/backup.sh"
)");

    ASSERT_TRUE(r.ok()) << r.errors[0].message;

    // Should have 2 timer entries (cron + interval; manual is skipped).
    ASSERT_EQ(r.triggers.size(), 2u);

    // Cron trigger.
    EXPECT_TRUE(std::holds_alternative<engine::CronTrigger>(
        r.triggers[0].spec));
    auto& cron = std::get<engine::CronTrigger>(r.triggers[0].spec);
    EXPECT_EQ(cron.expression, "0 2 * * *");

    // Interval trigger.
    EXPECT_TRUE(std::holds_alternative<engine::IntervalTrigger>(
        r.triggers[1].spec));
    auto& intv = std::get<engine::IntervalTrigger>(r.triggers[1].spec);
    EXPECT_EQ(intv.interval, std::chrono::milliseconds{300000});  // 5 min

    // Both target the workflow.
    EXPECT_EQ(r.triggers[0].target_id, r.workflows[0].workflow_id);
}

TEST(YamlLoaderTest, ParseWorkflowWithEnvironment) {
    auto r = load_workflow_string(R"(
name: "with_env"
jobs:
  build:
    env:
      CC: "gcc"
      CFLAGS: "-O2"
    working_dir: "/src"
    steps:
      - name: compile
        run: "make build"
        env:
          VERBOSE: "1"
        working_dir: "/src/build"
)");

    ASSERT_TRUE(r.ok());

    const auto& job = r.workflows[0].jobs[0];
    EXPECT_EQ(job.env.at("CC"), "gcc");
    EXPECT_EQ(job.env.at("CFLAGS"), "-O2");
    EXPECT_EQ(job.working_dir, "/src");

    const auto& step = job.steps[0];
    EXPECT_EQ(step.env.at("VERBOSE"), "1");
    EXPECT_EQ(step.working_dir.string(), "/src/build");
}

TEST(YamlLoaderTest, ParseWorkflowStepTimeout) {
    auto r = load_workflow_string(R"(
name: "timeouts"
jobs:
  slow:
    steps:
      - name: "long_task"
        run: "sleep 999"
        timeout_seconds: 300
)");

    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(r.workflows[0].jobs[0].steps[0].timeout.has_value());
    EXPECT_EQ(r.workflows[0].jobs[0].steps[0].timeout->count(), 300);
}

TEST(YamlLoaderTest, ParseWorkflowContinueOnError) {
    auto r = load_workflow_string(R"(
name: "resilient"
jobs:
  lint:
    continue_on_error: true
    steps:
      - name: "lint"
        run: "eslint ."
  build:
    needs: [lint]
    steps:
      - name: "compile"
        run: "make build"
)");

    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.workflows[0].jobs[0].continue_on_error);
    EXPECT_FALSE(r.workflows[0].jobs[1].continue_on_error);
}

TEST(YamlLoaderTest, ParseWorkflowExplicitId) {
    auto r = load_workflow_string(R"(
id: wf_custom_id
name: "custom"
jobs:
  hello:
    steps:
      - name: "greet"
        run: "echo hi"
)");

    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.workflows[0].workflow_id, "wf_custom_id");
}

TEST(YamlLoaderTest, DeterministicIds) {
    const std::string yaml = R"(
name: "deterministic"
jobs:
  hello:
    steps:
      - name: "greet"
        run: "echo hello"
)";

    auto r1 = load_workflow_string(yaml);
    auto r2 = load_workflow_string(yaml);

    ASSERT_TRUE(r1.ok());
    ASSERT_TRUE(r2.ok());

    // Same YAML → same IDs.
    EXPECT_EQ(r1.workflows[0].workflow_id, r2.workflows[0].workflow_id);
    EXPECT_EQ(r1.workflows[0].jobs[0].job_id,
              r2.workflows[0].jobs[0].job_id);
    EXPECT_EQ(r1.workflows[0].jobs[0].steps[0].step_id,
              r2.workflows[0].jobs[0].steps[0].step_id);
}

// ═══════════════════════════════════════════════════════════════════════
//  ERROR HANDLING
// ═══════════════════════════════════════════════════════════════════════

TEST(YamlLoaderTest, ErrorMissingName) {
    auto r = load_workflow_string(R"(
jobs:
  hello:
    steps:
      - run: "echo hi"
)");

    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.errors.size(), 1u);
    EXPECT_NE(r.errors[0].message.find("name"), std::string::npos);
}

TEST(YamlLoaderTest, ErrorMissingJobs) {
    auto r = load_workflow_string(R"(
name: "no_jobs"
)");

    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.errors[0].message.find("jobs"), std::string::npos);
}

TEST(YamlLoaderTest, ErrorMissingStepRun) {
    auto r = load_workflow_string(R"(
name: "bad_step"
jobs:
  hello:
    steps:
      - name: "missing_run"
)");

    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.errors[0].message.find("run"), std::string::npos);
}

TEST(YamlLoaderTest, ErrorCyclicDependency) {
    auto r = load_workflow_string(R"(
name: "cyclic"
jobs:
  a:
    needs: [b]
    steps:
      - run: "echo a"
  b:
    needs: [a]
    steps:
      - run: "echo b"
)");

    EXPECT_FALSE(r.ok());
    bool has_cycle_error = false;
    for (const auto& e : r.errors) {
        if (e.message.find("Cycle") != std::string::npos ||
            e.message.find("cycle") != std::string::npos) {
            has_cycle_error = true;
        }
    }
    EXPECT_TRUE(has_cycle_error);
}

TEST(YamlLoaderTest, ErrorUnknownDependency) {
    auto r = load_workflow_string(R"(
name: "bad_dep"
jobs:
  deploy:
    needs: [nonexistent]
    steps:
      - run: "echo deploy"
)");

    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.errors[0].message.find("nonexistent"), std::string::npos);
}

TEST(YamlLoaderTest, ErrorInvalidYaml) {
    auto r = load_workflow_string("this: is: [not: valid: yaml: :");

    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.errors[0].message.find("YAML parse error"),
              std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════
//  DIRECTORY LOADING
// ═══════════════════════════════════════════════════════════════════════

TEST(YamlLoaderTest, LoadWorkflowsDir) {
    TmpDir tmp;

    tmp.write("wf1.yaml", R"(
name: "alpha"
jobs:
  hello:
    steps:
      - run: "echo alpha"
)");

    tmp.write("wf2.yml", R"(
name: "beta"
triggers:
  - type: interval
    interval: "10m"
jobs:
  world:
    steps:
      - run: "echo beta"
)");

    auto r = load_workflows_dir(tmp.path());
    ASSERT_TRUE(r.ok()) << r.errors[0].message;
    EXPECT_EQ(r.workflows.size(), 2u);
    EXPECT_EQ(r.triggers.size(), 1u);  // Only beta has a trigger.
}

TEST(YamlLoaderTest, LoadWorkflowsDirDuplicateId) {
    TmpDir tmp;

    // Both files define the same workflow (same name + same jobs → same ID).
    tmp.write("wf1.yaml", R"(
name: "dup"
jobs:
  hello:
    steps:
      - run: "echo hello"
)");

    tmp.write("wf2.yaml", R"(
name: "dup"
jobs:
  hello:
    steps:
      - run: "echo hello"
)");

    auto r = load_workflows_dir(tmp.path());
    EXPECT_FALSE(r.ok());
    bool has_dup_error = false;
    for (const auto& e : r.errors) {
        if (e.message.find("Duplicate") != std::string::npos) {
            has_dup_error = true;
        }
    }
    EXPECT_TRUE(has_dup_error);
}

TEST(YamlLoaderTest, LoadWorkflowsDirNonexistent) {
    auto r = load_workflows_dir("/nonexistent/path");
    EXPECT_FALSE(r.ok());
}

// ═══════════════════════════════════════════════════════════════════════
//  WATCH GROUP PARSING
// ═══════════════════════════════════════════════════════════════════════

TEST(YamlLoaderTest, ParseWatchGroup) {
    auto r = load_watch_groups_string(R"(
watch_groups:
  - name: "log_monitor"
    enabled: true
    mode: "hybrid"
    watch_items:
      - "/var/log/app/*.log"
      - "/var/log/app/errors/"
    sample_rate: 120
    max_depth: 5
    max_files: 50000
    hash_policy: "size+mtime"
    pattern: "ERROR|CRITICAL"
    exclude_globs:
      - "*.tmp"
      - ".git/**"
    symlink_policy: "no_follow"
    rules:
      - name: "ErrorDetected"
        condition: "file.pattern_found == true"
        severity: "critical"
        event_types: [content_changed]
        trigger:
          workflow: "alert_pipeline"
      - name: "LargeFile"
        condition: "file.size > 1073741824"
        severity: "warning"
)");

    ASSERT_TRUE(r.ok()) << r.errors[0].message;
    ASSERT_EQ(r.watch_groups.size(), 1u);

    const auto& wg = r.watch_groups[0];
    EXPECT_EQ(wg.group_name, "log_monitor");
    EXPECT_TRUE(wg.enabled);
    EXPECT_EQ(wg.mode, watch::WatchMode::Hybrid);
    EXPECT_EQ(wg.watch_items.size(), 2u);
    EXPECT_EQ(wg.sample_rate.count(), 120);
    EXPECT_EQ(wg.max_depth, 5);
    EXPECT_EQ(wg.max_files, 50000);
    EXPECT_EQ(wg.hash_policy, watch::HashPolicy::SizePlusMtime);
    ASSERT_TRUE(wg.pattern.has_value());
    EXPECT_EQ(*wg.pattern, "ERROR|CRITICAL");
    EXPECT_EQ(wg.exclude_globs.size(), 2u);
    EXPECT_EQ(wg.symlink_policy, watch::SymlinkPolicy::NoFollow);

    // Content-addressable ID.
    EXPECT_TRUE(wg.group_id.starts_with("wgr-"));

    // Rules.
    ASSERT_EQ(wg.rules.size(), 2u);
    EXPECT_EQ(wg.rules[0].rule_name, "ErrorDetected");
    EXPECT_EQ(wg.rules[0].severity, "critical");
    EXPECT_EQ(wg.rules[0].event_types.size(), 1u);
    EXPECT_EQ(wg.rules[0].event_types[0], "content_changed");
    ASSERT_TRUE(wg.rules[0].trigger_target.has_value());
    EXPECT_EQ(*wg.rules[0].trigger_target, "alert_pipeline");
    EXPECT_TRUE(wg.rules[0].trigger_is_workflow);

    EXPECT_EQ(wg.rules[1].rule_name, "LargeFile");
    EXPECT_FALSE(wg.rules[1].trigger_target.has_value());
}

TEST(YamlLoaderTest, ParseWatchGroupMinimal) {
    auto r = load_watch_groups_string(R"(
watch_groups:
  - name: "simple"
    watch_items:
      - "/tmp/test"
)");

    ASSERT_TRUE(r.ok()) << r.errors[0].message;
    ASSERT_EQ(r.watch_groups.size(), 1u);

    const auto& wg = r.watch_groups[0];
    EXPECT_EQ(wg.group_name, "simple");
    EXPECT_EQ(wg.mode, watch::WatchMode::Hybrid);  // Default.
    EXPECT_EQ(wg.max_depth, 10);  // Default.
}

TEST(YamlLoaderTest, ParseWatchGroupComputeHashesFalse) {
    auto r = load_watch_groups_string(R"(
watch_groups:
  - name: "no_hash"
    watch_items: ["/tmp"]
    compute_hashes: false
)");

    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.watch_groups[0].hash_policy,
              watch::HashPolicy::MtimeOnly);
}

TEST(YamlLoaderTest, ParseWatchGroupSampleMode) {
    auto r = load_watch_groups_string(R"(
watch_groups:
  - name: "sample_only"
    watch_items: ["/tmp"]
    mode: "sample"
    sample_rate: 300
)");

    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.watch_groups[0].mode, watch::WatchMode::Sample);
    EXPECT_EQ(r.watch_groups[0].sample_rate.count(), 300);
}

TEST(YamlLoaderTest, ParseMultipleWatchGroups) {
    auto r = load_watch_groups_string(R"(
watch_groups:
  - name: "alpha"
    watch_items: ["/alpha"]
  - name: "beta"
    watch_items: ["/beta"]
    mode: "native"
)");

    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.watch_groups.size(), 2u);
    EXPECT_EQ(r.watch_groups[0].group_name, "alpha");
    EXPECT_EQ(r.watch_groups[1].group_name, "beta");
    EXPECT_EQ(r.watch_groups[1].mode, watch::WatchMode::Native);
}

TEST(YamlLoaderTest, ErrorWatchGroupMissingName) {
    auto r = load_watch_groups_string(R"(
watch_groups:
  - watch_items: ["/tmp"]
)");

    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.errors[0].message.find("name"), std::string::npos);
}

TEST(YamlLoaderTest, ErrorWatchGroupMissingPaths) {
    auto r = load_watch_groups_string(R"(
watch_groups:
  - name: "empty"
)");

    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.errors[0].message.find("watch_item"), std::string::npos);
}

TEST(YamlLoaderTest, WatchGroupDeterministicIds) {
    const std::string yaml = R"(
watch_groups:
  - name: "test"
    watch_items: ["/data"]
)";

    auto r1 = load_watch_groups_string(yaml);
    auto r2 = load_watch_groups_string(yaml);

    ASSERT_TRUE(r1.ok());
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r1.watch_groups[0].group_id, r2.watch_groups[0].group_id);
}

// ═══════════════════════════════════════════════════════════════════════
//  WATCH GROUP DIRECTORY LOADING
// ═══════════════════════════════════════════════════════════════════════

TEST(YamlLoaderTest, LoadWatchGroupsDir) {
    TmpDir tmp;

    tmp.write("wg1.yaml", R"(
watch_groups:
  - name: "logs"
    watch_items: ["/var/log"]
)");

    tmp.write("wg2.yml", R"(
watch_groups:
  - name: "data"
    watch_items: ["/data"]
    mode: "sample"
)");

    auto r = load_watch_groups_dir(tmp.path());
    ASSERT_TRUE(r.ok()) << r.errors[0].message;
    EXPECT_EQ(r.watch_groups.size(), 2u);
}

// ═══════════════════════════════════════════════════════════════════════
//  LOAD ALL (combined)
// ═══════════════════════════════════════════════════════════════════════

TEST(YamlLoaderTest, LoadAll) {
    TmpDir tmp;

    auto wf_dir = tmp.path() / "workflows";
    auto wg_dir = tmp.path() / "watches";
    fs::create_directories(wf_dir);
    fs::create_directories(wg_dir);

    {
        std::ofstream out(wf_dir / "deploy.yaml");
        out << R"(
name: "deploy"
triggers:
  - type: cron
    cron: "0 * * * *"
jobs:
  build:
    steps:
      - run: "make build"
  deploy:
    needs: [build]
    steps:
      - run: "./deploy.sh"
)";
    }

    {
        std::ofstream out(wg_dir / "monitors.yaml");
        out << R"(
watch_groups:
  - name: "files"
    watch_items: ["/data"]
    rules:
      - name: "changed"
        condition: "true"
)";
    }

    auto r = load_all(wf_dir, wg_dir);
    ASSERT_TRUE(r.ok()) << r.errors[0].message;
    EXPECT_EQ(r.workflows.size(), 1u);
    EXPECT_EQ(r.triggers.size(), 1u);
    EXPECT_EQ(r.watch_groups.size(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════
//  TRIGGER PARSING EDGE CASES
// ═══════════════════════════════════════════════════════════════════════

TEST(YamlLoaderTest, TriggerShorthandCron) {
    // Accept "cron: expr" as shorthand (no "type: cron" needed).
    auto r = load_workflow_string(R"(
name: "shorthand"
triggers:
  - cron: "0 2 * * 1-5"
jobs:
  task:
    steps:
      - run: "echo task"
)");

    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.triggers.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<engine::CronTrigger>(
        r.triggers[0].spec));
}

TEST(YamlLoaderTest, TriggerShorthandInterval) {
    auto r = load_workflow_string(R"(
name: "interval_short"
triggers:
  - interval: "1h"
jobs:
  task:
    steps:
      - run: "echo task"
)");

    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.triggers.size(), 1u);
    auto& intv = std::get<engine::IntervalTrigger>(r.triggers[0].spec);
    EXPECT_EQ(intv.interval, std::chrono::milliseconds{3600000});
}

TEST(YamlLoaderTest, TriggerWatchGroupSkipped) {
    auto r = load_workflow_string(R"(
name: "watch_triggered"
triggers:
  - type: watch_group
    name: "log_monitor"
jobs:
  task:
    steps:
      - run: "echo task"
)");

    ASSERT_TRUE(r.ok());
    // Watch-group triggers are handled by watch engine, not scheduler.
    EXPECT_EQ(r.triggers.size(), 0u);
}

TEST(YamlLoaderTest, TriggerUnknownType) {
    auto r = load_workflow_string(R"(
name: "bad_trigger"
triggers:
  - type: webhook
    url: "http://example.com"
jobs:
  task:
    steps:
      - run: "echo task"
)");

    // Unknown trigger type should produce an error.
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.errors[0].message.find("webhook"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════
//  SPEC PARITY: AVScheduler and LocalFlow migration compatibility
// ═══════════════════════════════════════════════════════════════════════

TEST(YamlLoaderTest, AVSchedulerMigratedFormat) {
    // Per spec §31.2: AVScheduler migrated format uses triggers + steps.
    auto r = load_workflow_string(R"(
name: "avs_migrated"
description: "Migrated from AVScheduler config.toml"
jobs:
  health_check:
    steps:
      - name: "check"
        run: "curl -f http://localhost:8080/health"
  daily_backup:
    condition: "job('health_check').last_run_successful"
    steps:
      - name: "backup"
        run: "/opt/scripts/backup.sh"
)");

    ASSERT_TRUE(r.ok()) << r.errors[0].message;
    ASSERT_EQ(r.workflows[0].jobs.size(), 2u);

    // Verify condition is preserved.
    auto find_job = [&](const std::string& name) -> const engine::JobDef* {
        for (const auto& j : r.workflows[0].jobs) {
            if (j.job_name == name) return &j;
        }
        return nullptr;
    };

    auto* backup = find_job("daily_backup");
    ASSERT_NE(backup, nullptr);
    ASSERT_TRUE(backup->condition_expr.has_value());
    EXPECT_EQ(*backup->condition_expr,
              "job('health_check').last_run_successful");
}

// ═══════════════════════════════════════════════════════════════════════
//  STANDALONE JOB PARSING
// ═══════════════════════════════════════════════════════════════════════

TEST(YamlLoaderTest, StandaloneJobWithExplicitFlag) {
    auto r = load_workflow_string(R"(
name: "loterias"
standalone: true
triggers:
  - type: cron
    spec: "55 23 * * *"
steps:
  - name: "update"
    run: "/usr/bin/update-loterias"
    env:
      KAIROS_JOB_NAME: "loterias"
)");

    ASSERT_TRUE(r.ok()) << r.errors[0].message;

    // Standalone produces a job, NOT a workflow.
    EXPECT_EQ(r.workflows.size(), 0u);
    ASSERT_EQ(r.standalone_jobs.size(), 1u);

    const auto& job = r.standalone_jobs[0];
    EXPECT_EQ(job.job_name, "loterias");
    EXPECT_TRUE(job.job_id.starts_with("job-"));

    ASSERT_EQ(job.steps.size(), 1u);
    EXPECT_EQ(job.steps[0].step_name, "update");
    EXPECT_EQ(job.steps[0].command, "/usr/bin/update-loterias");

    // Env should be on the step (from step-level env).
    EXPECT_EQ(job.steps[0].env.at("KAIROS_JOB_NAME"), "loterias");

    // Trigger should target the standalone job.
    ASSERT_EQ(r.triggers.size(), 1u);
}

TEST(YamlLoaderTest, StandaloneJobAutoDetected) {
    // No explicit 'standalone: true' — inferred from steps at top level
    // without a jobs mapping.
    auto r = load_workflow_string(R"(
name: "quick"
steps:
  - name: "do_it"
    run: "echo hello"
)");

    ASSERT_TRUE(r.ok()) << r.errors[0].message;
    EXPECT_EQ(r.workflows.size(), 0u);
    ASSERT_EQ(r.standalone_jobs.size(), 1u);
    EXPECT_EQ(r.standalone_jobs[0].job_name, "quick");
    EXPECT_EQ(r.standalone_jobs[0].steps[0].command, "echo hello");
}

TEST(YamlLoaderTest, StandaloneJobHoistsCondition) {
    auto r = load_workflow_string(R"(
name: "guarded"
standalone: true
condition: 'job("cleanup").last_success'
steps:
  - name: "work"
    run: "do-work"
)");

    ASSERT_TRUE(r.ok()) << r.errors[0].message;
    const auto& job = r.standalone_jobs[0];
    ASSERT_TRUE(job.condition_expr.has_value());
    EXPECT_EQ(*job.condition_expr, "job(\"cleanup\").last_success");
}

TEST(YamlLoaderTest, StandaloneJobHoistsEnvAndWorkingDir) {
    auto r = load_workflow_string(R"(
name: "envtest"
standalone: true
working_dir: "/tmp/work"
env:
  FOO: bar
steps:
  - name: "step1"
    run: "echo $FOO"
)");

    ASSERT_TRUE(r.ok()) << r.errors[0].message;
    const auto& job = r.standalone_jobs[0];
    EXPECT_EQ(job.working_dir, "/tmp/work");
    EXPECT_EQ(job.env.at("FOO"), "bar");
}

TEST(YamlLoaderTest, StandaloneJobInDir) {
    TmpDir tmp;

    // One regular workflow, one standalone job.
    tmp.write("wf.yaml", R"(
name: "normal_wf"
jobs:
  build:
    steps:
      - run: "make"
)");

    tmp.write("cron_job.yaml", R"(
name: "nightly_backup"
standalone: true
triggers:
  - type: cron
    spec: "0 3 * * *"
steps:
  - name: "backup"
    run: "/usr/local/bin/backup.sh"
)");

    auto r = load_workflows_dir(tmp.path());
    ASSERT_TRUE(r.ok()) << r.errors[0].message;
    EXPECT_EQ(r.workflows.size(), 1u);
    EXPECT_EQ(r.standalone_jobs.size(), 1u);
    EXPECT_EQ(r.standalone_jobs[0].job_name, "nightly_backup");
    EXPECT_EQ(r.triggers.size(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════
//  COMMAND ALIAS FOR RUN
// ═══════════════════════════════════════════════════════════════════════

TEST(YamlLoaderTest, StepCommandAlias) {
    auto r = load_workflow_string(R"(
name: "alias_test"
jobs:
  test_job:
    steps:
      - name: "with_run"
        run: "echo run"
      - name: "with_command"
        command: "echo command"
)");

    ASSERT_TRUE(r.ok()) << r.errors[0].message;
    const auto& steps = r.workflows[0].jobs[0].steps;
    ASSERT_EQ(steps.size(), 2u);
    EXPECT_EQ(steps[0].command, "echo run");
    EXPECT_EQ(steps[1].command, "echo command");
}

TEST(YamlLoaderTest, StepRunTakesPrecedenceOverCommand) {
    // If both 'run' and 'command' are present, 'run' wins.
    auto r = load_workflow_string(R"(
name: "precedence_test"
jobs:
  test_job:
    steps:
      - name: "both"
        run: "from_run"
        command: "from_command"
)");

    ASSERT_TRUE(r.ok()) << r.errors[0].message;
    EXPECT_EQ(r.workflows[0].jobs[0].steps[0].command, "from_run");
}

TEST(YamlLoaderTest, StandaloneWithCommandAlias) {
    // The original user YAML that triggered the issue.
    auto r = load_workflow_string(R"(
name: loterias
standalone: true
triggers:
  - type: cron
    spec: "55 23 * * *"
steps:
  - name: update
    command: /av/data/repos/kairos/utils/job_wrapper.sh /av/data/repos/loterias/auto-update.job.sh
    env:
      KAIROS_JOB_NAME: loterias
)");

    ASSERT_TRUE(r.ok()) << r.errors[0].message;

    // Should be a standalone job, not a workflow.
    EXPECT_EQ(r.workflows.size(), 0u);
    ASSERT_EQ(r.standalone_jobs.size(), 1u);

    const auto& job = r.standalone_jobs[0];
    EXPECT_EQ(job.job_name, "loterias");

    const auto& step = job.steps[0];
    EXPECT_EQ(step.step_name, "update");
    EXPECT_TRUE(step.command.find("job_wrapper.sh") != std::string::npos);
    EXPECT_EQ(step.env.at("KAIROS_JOB_NAME"), "loterias");

    ASSERT_EQ(r.triggers.size(), 1u);
}

TEST(YamlLoaderTest, WatchGroupRuleWithJobTrigger) {
    auto r = load_watch_groups_string(R"(
watch_groups:
  - name: "config_watcher"
    watch_items: ["/etc/app"]
    rules:
      - name: "ConfigChanged"
        condition: "true"
        event_types: [content_changed]
        trigger:
          job: "reload_service"
)");

    ASSERT_TRUE(r.ok());
    const auto& rule = r.watch_groups[0].rules[0];
    ASSERT_TRUE(rule.trigger_target.has_value());
    EXPECT_EQ(*rule.trigger_target, "reload_service");
    EXPECT_FALSE(rule.trigger_is_workflow);
}

}  // anonymous namespace
}  // namespace kairos::config
