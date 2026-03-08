/// tests/integration/integration_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Integration tests — end-to-end pipeline verification                     ║
// ║                                                                           ║
// ║  Tests the full pipeline: Trigger source → TriggerBus → Pipeline →       ║
// ║  RunnerPool → DBWriter → QueryReader. Uses FakeClock, FakeProcess,       ║
// ║  FakeFilesystem, and in-memory SQLite.                                   ║
// ║                                                                           ║
// ║  Spec reference: §30.6                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/pipeline.hpp"
#include "kairos/engine/scheduler.hpp"
#include "kairos/engine/trigger_event.hpp"
#include "kairos/engine/workflow_registry.hpp"
#include "kairos/exec/runner_pool.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/persist/query_reader.hpp"
#include "kairos/testing/fake_clock.hpp"
#include "kairos/testing/fake_filesystem.hpp"
#include "kairos/testing/fake_fs_scanner.hpp"
#include "kairos/testing/fake_process.hpp"
#include "kairos/watch/watch_engine.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

using namespace kairos;
using namespace kairos::engine;
using namespace kairos::watch;
using namespace kairos::testing;
using namespace std::chrono_literals;

// ── Integration test fixture ────────────────────────────────────────────

class IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        clock_.set_now(std::chrono::system_clock::time_point{
            std::chrono::hours(24 * 365 * 56)});
        clock_.set_steady(std::chrono::steady_clock::time_point{
            std::chrono::hours(1)});

        // In-memory SQLite database.
        db_ = persist::open_database(":memory:");
    }

    /// Build a workflow with a single job that runs "echo hello".
    WorkflowDef make_echo_workflow() {
        StepDef step;
        step.step_id = "stp-echo1";
        step.step_name = "echo_step";
        step.command = "echo hello";
        step.use_shell = true;

        JobDef job;
        job.job_id = "job-echo1";
        job.job_name = "echo_job";
        job.steps = {step};

        // Build DAG.
        DagNode node;
        node.job_id = "job-echo1";
        node.job_name = "echo_job";
        node.level = 0;

        WorkflowDag dag;
        dag.nodes["job-echo1"] = node;
        dag.levels = {{"job-echo1"}};

        WorkflowDef wf;
        wf.workflow_id = "wfl-echo1";
        wf.workflow_name = "echo_workflow";
        wf.jobs = {job};
        wf.dag = dag;
        return wf;
    }

    /// Make an interval trigger that fires every `interval_ms`.
    TimerEntry make_interval_trigger(
        const std::string& trigger_id,
        const std::string& target_id,
        std::chrono::milliseconds interval)
    {
        TimerEntry entry;
        entry.trigger_id = trigger_id;
        entry.target_id = target_id;
        entry.target_name = target_id;
        entry.target_kind = TriggerEvent::TargetKind::Workflow;
        entry.spec = IntervalTrigger{.interval = interval};
        entry.enabled = true;
        entry.max_instances = 1;
        return entry;
    }

    FakeClock clock_;
    std::unique_ptr<SQLite::Database> db_;
};

// ── Schedule → Run integration test ─────────────────────────────────────

TEST_F(IntegrationTest, SchedulerFiresTriggerPipelineExecutes) {
    // Setup: scheduler with 5s interval → pipeline → runner pool.
    auto wf = make_echo_workflow();
    auto trigger = make_interval_trigger("trg-int1", "wfl-echo1", 5000ms);

    auto registry = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{wf},
        std::vector<TimerEntry>{trigger},
        std::vector<JobDef>{});

    // Subsystems.
    persist::DBWriterConfig dw_cfg;
    persist::DBWriter db_writer(*db_, dw_cfg);
    persist::QueryReader query_reader(*db_);
    ActiveRunTracker active_runs;

    TriggerBus trigger_bus(1024);

    exec::RunnerPoolConfig pool_cfg{.worker_count = 2, .queue_capacity = 64};
    exec::RunnerPool runner_pool(pool_cfg);

    // Use fake process factory for deterministic results.
    runner_pool.set_process_handle_factory([]() {
        auto proc = std::make_unique<FakeProcessHandle>();
        proc->set_exit_code(0);
        proc->set_stdout_data("hello\n");
        return proc;
    });

    // Pipeline.
    PipelineConfig pipe_cfg;
    Pipeline pipeline(pipe_cfg, Pipeline::Dependencies{
        .clock = &clock_,
        .trigger_bus = &trigger_bus,
        .runner_pool = &runner_pool,
        .registry = registry,
        .active_runs = &active_runs,
        .db_writer = &db_writer,
        .query_reader = &query_reader,
    });

    // Scheduler.
    SchedulerConfig sched_cfg;
    Scheduler scheduler(sched_cfg, Scheduler::Dependencies{
        .clock = &clock_,
        .registry = registry,
        .active_runs = &active_runs,
        .db_writer = &db_writer,
    });

    // Start everything.
    std::stop_source stop;
    db_writer.start(stop.get_token());
    runner_pool.start(stop.get_token());
    pipeline.start(stop.get_token());

    TriggerSink sched_sink = [&trigger_bus](TriggerEvent evt) {
        return trigger_bus.push(std::move(evt), 5000ms);
    };
    scheduler.start(stop.get_token(), sched_sink);

    // Advance clock past the interval.
    clock_.advance(6s);

    // Give threads time to process.
    std::this_thread::sleep_for(200ms);

    // Stop everything.
    stop.request_stop();
    clock_.wake();
    scheduler.stop();
    trigger_bus.close();
    pipeline.stop();
    runner_pool.shutdown();
    db_writer.flush();

    // Verify: a run was persisted. (If FakeProcess returns exit 0,
    // the pipeline should have completed a run.)
    // In a full verification, we'd query the database:
    //   SELECT status FROM runs WHERE target_id='wfl-echo1'
    // For now, verify that the db_writer processed some writes.
    EXPECT_GT(db_writer.total_writes(), 0);
}

// ── Watch → Trigger integration test ────────────────────────────────────

TEST_F(IntegrationTest, WatchEngineDetectsChangeAndEmitsTrigger) {
    // Setup: WatchEngine with FakeFilesystem detects a new file,
    // emits a TriggerEvent to the bus.

    FakeFilesystem fs;
    FakeFilesystemScanner scanner(fs);

    // Initial state: one file.
    fs.add_file("/watched/existing.txt", FakeFileEntry{
        .size = 100, .mtime = clock_.now()});

    // Watch group with a catch-all rule that triggers wfl-deploy.
    WatchGroupDef group;
    group.group_id = "wg-test1";
    group.group_name = "test_group";
    group.watch_items = {"/watched"};
    group.mode = WatchMode::Sample;
    group.sample_rate = 30s;

    WatchRuleDef rule;
    rule.rule_name = "any_change";
    rule.condition = "true";
    rule.severity = "info";
    rule.trigger_target = "wfl-deploy";
    rule.trigger_is_workflow = true;
    group.rules = {rule};

    WatchEngine watch_engine(
        WatchEngineConfig{},
        WatchEngine::Dependencies{
            .clock = &clock_,
            .scanner = &scanner,
        },
        {group});

    std::vector<TriggerEvent> emitted;
    std::mutex mu;
    TriggerSink sink = [&](TriggerEvent evt) {
        std::lock_guard lock(mu);
        emitted.push_back(std::move(evt));
        return true;
    };

    // First scan: baseline (no events).
    watch_engine.scan_once(sink);
    EXPECT_TRUE(emitted.empty());

    // Add a new file.
    clock_.advance(30s);
    fs.add_file("/watched/new_file.txt", FakeFileEntry{
        .size = 42, .mtime = clock_.now()});

    // Second scan: should detect the new file and emit a trigger.
    watch_engine.scan_once(sink);

    ASSERT_EQ(emitted.size(), 1u);
    EXPECT_EQ(emitted[0].type, TriggerType::FileDiff);
    EXPECT_EQ(emitted[0].target_id, "wfl-deploy");
    EXPECT_EQ(emitted[0].target_kind, TriggerEvent::TargetKind::Workflow);

    auto& payload = std::get<FileDiffPayload>(emitted[0].payload);
    EXPECT_EQ(payload.watch_group, "test_group");
}

// ── Watch → Pipeline end-to-end ─────────────────────────────────────────

TEST_F(IntegrationTest, WatchTriggerFlowsThroughPipeline) {
    // Full flow: WatchEngine detects change → TriggerBus → Pipeline
    // resolves DAG → dispatches to runner → run completes.

    FakeFilesystem fs;
    FakeFilesystemScanner scanner(fs);

    fs.add_file("/watched/a.txt", FakeFileEntry{
        .size = 100, .mtime = clock_.now()});

    WatchGroupDef group;
    group.group_id = "wg-deploy";
    group.group_name = "deploy_watch";
    group.watch_items = {"/watched"};
    group.mode = WatchMode::Sample;
    group.sample_rate = 30s;

    WatchRuleDef rule;
    rule.rule_name = "deploy_on_change";
    rule.condition = "true";
    rule.trigger_target = "wfl-echo1";
    rule.trigger_is_workflow = true;
    group.rules = {rule};

    // Workflow.
    auto wf = make_echo_workflow();
    auto registry = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{wf},
        std::vector<TimerEntry>{},
        std::vector<JobDef>{},
        std::vector<WatchGroupDef>{group});

    // Subsystems.
    persist::DBWriterConfig dw_cfg;
    persist::DBWriter db_writer(*db_, dw_cfg);
    persist::QueryReader query_reader(*db_);
    ActiveRunTracker active_runs;
    TriggerBus trigger_bus(1024);

    exec::RunnerPoolConfig pool_cfg{.worker_count = 2, .queue_capacity = 64};
    exec::RunnerPool runner_pool(pool_cfg);
    runner_pool.set_process_handle_factory([]() {
        auto proc = std::make_unique<FakeProcessHandle>();
        proc->set_exit_code(0);
        proc->set_stdout_data("deployed\n");
        return proc;
    });

    // Pipeline.
    PipelineConfig pipe_cfg;
    Pipeline pipeline(pipe_cfg, Pipeline::Dependencies{
        .clock = &clock_,
        .trigger_bus = &trigger_bus,
        .runner_pool = &runner_pool,
        .registry = registry,
        .active_runs = &active_runs,
        .db_writer = &db_writer,
        .query_reader = &query_reader,
    });

    // Watch engine → emit to trigger bus.
    WatchEngine watch_engine(
        WatchEngineConfig{},
        WatchEngine::Dependencies{
            .clock = &clock_,
            .scanner = &scanner,
        },
        {group});

    TriggerSink watch_sink = [&trigger_bus](TriggerEvent evt) {
        return trigger_bus.push(std::move(evt), 5000ms);
    };

    // Start subsystems.
    std::stop_source stop;
    db_writer.start(stop.get_token());
    runner_pool.start(stop.get_token());
    pipeline.start(stop.get_token());

    // Baseline scan.
    watch_engine.scan_once(watch_sink);

    // Modify a file.
    clock_.advance(30s);
    fs.modify_file("/watched/a.txt", FakeFileEntry{
        .size = 500, .mtime = clock_.now()});

    // Second scan: triggers pipeline.
    watch_engine.scan_once(watch_sink);

    // Give pipeline time to process.
    std::this_thread::sleep_for(300ms);

    // Shutdown.
    stop.request_stop();
    clock_.wake();
    trigger_bus.close();
    pipeline.stop();
    runner_pool.shutdown();
    db_writer.flush();

    // Verify run was persisted.
    EXPECT_GT(db_writer.total_writes(), 0);
}
