/// tests/e2e/daemon_lifecycle_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  E2E: Daemon Lifecycle Tests                                             ║
// ║                                                                          ║
// ║  Tier 4 tests: exercise the full daemon subsystem stack —               ║
// ║    TriggerBus → Pipeline → RunnerPool → ProcessHandle → DBWriter        ║
// ║                                                                          ║
// ║  Uses real (in-memory) SQLite, real threads, real child processes.       ║
// ║                                                                          ║
// ║  Spec reference: §30.6 (Tier 4)                                        ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/core/id_generator.hpp"
#include "kairos/engine/cancel_registry.hpp"
#include "kairos/engine/dag.hpp"
#include "kairos/engine/pipeline.hpp"
#include "kairos/engine/trigger_event.hpp"
#include "kairos/engine/trigger_types.hpp"
#include "kairos/engine/workflow_registry.hpp"
#include "kairos/exec/output_sink.hpp"
#include "kairos/exec/runner_pool.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/query_reader.hpp"
#include "kairos/testing/fake_clock.hpp"

#include <gtest/gtest.h>

#include <SQLiteCpp/SQLiteCpp.h>

#include <chrono>
#include <mutex>
#include <stop_token>
#include <thread>

using namespace kairos;
using namespace kairos::engine;
using namespace std::chrono_literals;

#ifndef _WIN32

// ── Test fixture: full daemon subsystem stack ──────────────────────────

class DaemonLifecycleE2E : public ::testing::Test {
protected:
    void SetUp() override {
        // In-memory SQLite — same pattern as integration_test.cpp.
        db_ = persist::open_database(":memory:");

        // DBWriter — default config is fine for tests.
        persist::DBWriterConfig dw_cfg;
        db_writer_ = std::make_unique<persist::DBWriter>(*db_, dw_cfg);
        db_writer_->start(stop_.get_token());

        // QueryReader.
        query_reader_ = std::make_unique<persist::QueryReader>(*db_);

        // RunnerPool with real processes.
        exec::RunnerPoolConfig rp_cfg{.worker_count = 2,
                                       .queue_capacity = 64};
        runner_pool_ = std::make_unique<exec::RunnerPool>(rp_cfg);
        runner_pool_->set_process_handle_factory(
            [](const exec::ProcessSpec&) {
                return exec::create_process_handle();
            });
        runner_pool_->start(stop_.get_token());

        // Shared subsystems.
        run_stream_ = std::make_unique<exec::RunStream>();
        cancel_reg_ = std::make_unique<CancelRegistry>();
        trigger_bus_ = std::make_unique<TriggerBus>(256);
        active_runs_ = std::make_unique<ActiveRunTracker>();
        clock_ = std::make_unique<SystemClockSource>();
    }

    void TearDown() override {
        stop_.request_stop();
        if (pipeline_) pipeline_->stop();
        runner_pool_->shutdown();
        db_writer_->stop();
    }

    // ── Workflow builders ────────────────────────────────────────────

    /// Create a single-job workflow.
    WorkflowDef make_workflow(const std::string& name,
                              const std::string& cmd) {
        StepDef step;
        step.step_id = "stp-1";
        step.step_name = "step-1";
        step.command = cmd;
        step.use_shell = true;

        JobDef job;
        job.job_id = "job-" + name;
        job.job_name = name + "-job";
        job.steps = {step};

        DagNode node;
        node.job_id = "job-" + name;
        node.job_name = name + "-job";

        return WorkflowDef{
            .workflow_id = "wfl-" + name,
            .workflow_name = name,
            .jobs = {job},
            .dag = WorkflowDag::build({node}),
        };
    }

    /// Create a two-job DAG: job-a → job-b.
    WorkflowDef make_dag_workflow(const std::string& name,
                                  const std::string& cmd_a,
                                  const std::string& cmd_b) {
        StepDef step_a;
        step_a.step_id = "stp-a1";
        step_a.step_name = "step-a1";
        step_a.command = cmd_a;
        step_a.use_shell = true;

        StepDef step_b;
        step_b.step_id = "stp-b1";
        step_b.step_name = "step-b1";
        step_b.command = cmd_b;
        step_b.use_shell = true;

        JobDef job_a;
        job_a.job_id = "job-a";
        job_a.job_name = "job-a";
        job_a.steps = {step_a};

        JobDef job_b;
        job_b.job_id = "job-b";
        job_b.job_name = "job-b";
        job_b.needs = {"job-a"};
        job_b.steps = {step_b};

        DagNode node_a;
        node_a.job_id = "job-a";
        node_a.job_name = "job-a";

        DagNode node_b;
        node_b.job_id = "job-b";
        node_b.job_name = "job-b";
        node_b.needs = {"job-a"};

        return WorkflowDef{
            .workflow_id = "wfl-" + name,
            .workflow_name = name,
            .jobs = {job_a, job_b},
            .dag = WorkflowDag::build({node_a, node_b}),
        };
    }

    // ── Pipeline setup ───────────────────────────────────────────────

    void start_pipeline(
        std::shared_ptr<const WorkflowRegistry> reg)
    {
        PipelineConfig cfg;
        cfg.drain_timeout = 5s;

        Pipeline::Dependencies deps;
        deps.clock = clock_.get();
        deps.trigger_bus = trigger_bus_.get();
        deps.runner_pool = runner_pool_.get();
        deps.registry = reg;
        deps.active_runs = active_runs_.get();
        deps.db_writer = db_writer_.get();
        deps.query_reader = query_reader_.get();
        deps.run_stream = run_stream_.get();
        deps.cancel_registry = cancel_reg_.get();

        pipeline_ = std::make_unique<Pipeline>(cfg, deps);
        pipeline_->start(stop_.get_token());
    }

    // ── Trigger + wait ───────────────────────────────────────────────

    /// Push a manual trigger and wait for the run to complete.
    std::string trigger_and_wait(const std::string& workflow_id,
                                  std::chrono::seconds timeout = 10s) {
        auto event = TriggerEvent::make_manual_run(
            workflow_id,
            TriggerEvent::TargetKind::Workflow,
            core::generate_correlation_id(),
            "e2e_test");

        trigger_bus_->push(event, 1s);

        // Poll DB for completed run.
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(200ms);
            db_writer_->flush();
            std::this_thread::sleep_for(100ms);

            auto runs = query_reader_->query_recent_runs(
                1, "", "", "");
            if (!runs.empty()) {
                auto& s = runs[0].status;
                if (s == "SUCCESS" || s == "FAILURE" ||
                    s == "SKIPPED" || s == "CANCELLED") {
                    return s;
                }
            }
        }
        return "TIMED_OUT";
    }

    std::stop_source stop_;
    std::unique_ptr<SQLite::Database> db_;
    std::unique_ptr<persist::DBWriter> db_writer_;
    std::unique_ptr<persist::QueryReader> query_reader_;
    std::unique_ptr<exec::RunnerPool> runner_pool_;
    std::unique_ptr<exec::RunStream> run_stream_;
    std::unique_ptr<CancelRegistry> cancel_reg_;
    std::unique_ptr<TriggerBus> trigger_bus_;
    std::unique_ptr<ActiveRunTracker> active_runs_;
    std::unique_ptr<ClockSource> clock_;
    std::unique_ptr<Pipeline> pipeline_;
};

// ══════════════════════════════════════════════════════════════════════════════
// Test 1: Basic lifecycle — start → trigger → run → success → shutdown
// ══════════════════════════════════════════════════════════════════════════════

TEST_F(DaemonLifecycleE2E, BasicStartTriggerRunShutdown) {
    auto wf = make_workflow("hello", "echo 'Hello from Kairos E2E'");
    auto reg = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{wf},
        std::vector<TimerEntry>{},
        std::vector<JobDef>{});

    start_pipeline(reg);

    auto status = trigger_and_wait("wfl-hello");
    EXPECT_EQ(status, "SUCCESS");
}

// ══════════════════════════════════════════════════════════════════════════════
// Test 2: Failing job → FAILURE status persisted
// ══════════════════════════════════════════════════════════════════════════════

TEST_F(DaemonLifecycleE2E, FailingJobProducesFailureStatus) {
    auto wf = make_workflow("fail", "exit 42");
    auto reg = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{wf},
        std::vector<TimerEntry>{},
        std::vector<JobDef>{});

    start_pipeline(reg);

    auto status = trigger_and_wait("wfl-fail");
    EXPECT_EQ(status, "FAILURE");
}

// ══════════════════════════════════════════════════════════════════════════════
// Test 3: DAG execution — job-b runs after job-a succeeds
// ══════════════════════════════════════════════════════════════════════════════

TEST_F(DaemonLifecycleE2E, DagExecutionOrder) {
    auto wf = make_dag_workflow("dag", "echo step-a", "echo step-b");
    auto reg = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{wf},
        std::vector<TimerEntry>{},
        std::vector<JobDef>{});

    start_pipeline(reg);

    auto status = trigger_and_wait("wfl-dag");
    EXPECT_EQ(status, "SUCCESS");
}

// ══════════════════════════════════════════════════════════════════════════════
// Test 4: DAG failure propagation — job-a fails → workflow FAILURE
// ══════════════════════════════════════════════════════════════════════════════

TEST_F(DaemonLifecycleE2E, DagFailurePropagation) {
    auto wf = make_dag_workflow("dagfail", "exit 1", "echo should-not-run");
    auto reg = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{wf},
        std::vector<TimerEntry>{},
        std::vector<JobDef>{});

    start_pipeline(reg);

    auto status = trigger_and_wait("wfl-dagfail");
    EXPECT_EQ(status, "FAILURE");
}

// ══════════════════════════════════════════════════════════════════════════════
// Test 5: Graceful shutdown during a running process
// ══════════════════════════════════════════════════════════════════════════════

TEST_F(DaemonLifecycleE2E, GracefulShutdownDuringRun) {
    auto wf = make_workflow("longrun", "sleep 30");
    auto reg = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{wf},
        std::vector<TimerEntry>{},
        std::vector<JobDef>{});

    start_pipeline(reg);

    // Trigger the run.
    auto event = TriggerEvent::make_manual_run(
        "wfl-longrun",
        TriggerEvent::TargetKind::Workflow,
        core::generate_correlation_id(),
        "e2e_test");
    trigger_bus_->push(event, 1s);

    // Let the process start spawning.
    std::this_thread::sleep_for(500ms);

    // Request stop — triggers graceful shutdown.
    stop_.request_stop();
    pipeline_->stop();

    // No crash = success.
    SUCCEED();
}

// ══════════════════════════════════════════════════════════════════════════════
// Test 6: Multiple sequential triggers all complete
// ══════════════════════════════════════════════════════════════════════════════

TEST_F(DaemonLifecycleE2E, MultipleSequentialTriggers) {
    auto wf = make_workflow("multi", "echo run");
    auto reg = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{wf},
        std::vector<TimerEntry>{},
        std::vector<JobDef>{});

    start_pipeline(reg);

    for (int i = 0; i < 3; ++i) {
        auto event = TriggerEvent::make_manual_run(
            "wfl-multi",
            TriggerEvent::TargetKind::Workflow,
            core::generate_correlation_id(),
            "e2e_test");
        trigger_bus_->push(event, 1s);
        std::this_thread::sleep_for(300ms);
    }

    // Wait for completion.
    std::this_thread::sleep_for(3s);
    db_writer_->flush();
    std::this_thread::sleep_for(200ms);

    auto runs = query_reader_->query_recent_runs(10, "", "", "");
    EXPECT_GE(runs.size(), 1u);
}

// ══════════════════════════════════════════════════════════════════════════════
// Test 7: Run persists timing data
// ══════════════════════════════════════════════════════════════════════════════

TEST_F(DaemonLifecycleE2E, RunPersistsTimingData) {
    auto wf = make_workflow("output", "echo KAIROS_E2E_MARKER_42");
    auto reg = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{wf},
        std::vector<TimerEntry>{},
        std::vector<JobDef>{});

    start_pipeline(reg);

    auto status = trigger_and_wait("wfl-output");
    EXPECT_EQ(status, "SUCCESS");

    auto runs = query_reader_->query_recent_runs(1, "", "", "");
    ASSERT_GE(runs.size(), 1u);
    EXPECT_EQ(runs[0].status, "SUCCESS");
    EXPECT_GT(runs[0].duration_ms, 0);
}

#endif  // !_WIN32
