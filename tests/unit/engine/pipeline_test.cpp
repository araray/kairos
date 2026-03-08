/// tests/unit/engine/pipeline_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Pipeline engine component tests                                         ║
// ║  Tests DAG execution, condition evaluation, skip semantics.              ║
// ║  Uses FakeClock + FakeProcess for deterministic testing.                 ║
// ║  Spec reference: §30.3                                                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/pipeline.hpp"
#include "kairos/engine/trigger_event.hpp"
#include "kairos/engine/workflow_registry.hpp"
#include "kairos/exec/runner_pool.hpp"
#include "kairos/testing/fake_clock.hpp"
#include "kairos/testing/fake_process.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stop_token>
#include <thread>

using namespace kairos;
using namespace kairos::engine;
using namespace kairos::exec;
using namespace std::chrono_literals;

// ── Helpers ─────────────────────────────────────────────────────────────

/// Build a WorkflowDef. Cannot default-construct because WorkflowDag
/// has a private default ctor — must build the DAG first, then move it in.
static WorkflowDef build_workflow_def(
    std::string wf_id, std::string wf_name,
    std::vector<JobDef> jobs,
    std::vector<DagNode> dag_nodes)
{
    auto dag = WorkflowDag::build(std::move(dag_nodes));
    return WorkflowDef{
        .workflow_id = std::move(wf_id),
        .workflow_name = std::move(wf_name),
        .jobs = std::move(jobs),
        .dag = std::move(dag),
    };
}

// ── Test fixture ────────────────────────────────────────────────────────

class PipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        clock_.set_now(std::chrono::system_clock::time_point{
            std::chrono::hours(24 * 365 * 56)});
        clock_.set_steady(std::chrono::steady_clock::time_point{
            std::chrono::hours(1)});
    }

    /// Build a single-job workflow definition.
    WorkflowDef make_single_job_workflow(
        const std::string& wf_id, const std::string& wf_name,
        const std::string& job_id, const std::string& job_name,
        const std::string& command,
        std::optional<std::string> condition = std::nullopt)
    {
        StepDef step;
        step.step_id = "stp-001";
        step.step_name = "step1";
        step.command = command;

        JobDef job;
        job.job_id = job_id;
        job.job_name = job_name;
        job.steps = {step};
        job.condition_expr = condition;

        DagNode node;
        node.job_id = job_id;
        node.job_name = job_name;
        node.condition_expr = condition;

        return build_workflow_def(
            wf_id, wf_name, {job}, {node});
    }

    /// Build a 3-job DAG: setup -> [build, test]
    WorkflowDef make_three_job_workflow()
    {
        StepDef step_setup{.step_id = "stp-s", .step_name = "run-setup",
                           .command = "echo setup"};
        StepDef step_build{.step_id = "stp-b", .step_name = "run-build",
                           .command = "echo build"};
        StepDef step_test{.step_id = "stp-t", .step_name = "run-test",
                          .command = "echo test"};

        JobDef j_setup{.job_id = "job-setup", .job_name = "setup",
                       .steps = {step_setup}};
        JobDef j_build{.job_id = "job-build", .job_name = "build",
                       .steps = {step_build}, .needs = {"job-setup"}};
        JobDef j_test{.job_id = "job-test", .job_name = "test",
                      .steps = {step_test}, .needs = {"job-setup"}};

        DagNode n_setup{.job_id = "job-setup", .job_name = "setup"};
        DagNode n_build{.job_id = "job-build", .job_name = "build",
                        .needs = {"job-setup"}};
        DagNode n_test{.job_id = "job-test", .job_name = "test",
                       .needs = {"job-setup"}};

        return build_workflow_def(
            "wfl-three", "Three Job Pipeline",
            {j_setup, j_build, j_test},
            {n_setup, n_build, n_test});
    }

    /// Create a trigger event for a workflow.
    TriggerEvent make_manual_trigger(const std::string& target_id,
                                      TriggerEvent::TargetKind kind =
                                          TriggerEvent::TargetKind::Workflow)
    {
        return TriggerEvent::make_manual_run(
            target_id, kind, "corr-test-001");
    }

    /// Create a runner pool with fake processes that succeed.
    void setup_runner_pool() {
        pool_ = std::make_unique<exec::RunnerPool>(
            exec::RunnerPoolConfig{.worker_count = 2, .queue_capacity = 32});
        pool_->set_process_handle_factory([]() {
            auto p = std::make_unique<kairos::testing::FakeProcessHandle>();
            p->set_exit_code(0);
            p->set_stdout_data("ok\n");
            return p;
        });
        pool_->start(stop_source_.get_token());
    }

    /// Create a runner pool that simulates failures.
    void setup_failing_runner_pool() {
        pool_ = std::make_unique<exec::RunnerPool>(
            exec::RunnerPoolConfig{.worker_count = 2, .queue_capacity = 32});
        pool_->set_process_handle_factory([]() {
            auto p = std::make_unique<kairos::testing::FakeProcessHandle>();
            p->set_exit_code(1);
            p->set_stderr_data("error\n");
            return p;
        });
        pool_->start(stop_source_.get_token());
    }

    void TearDown() override {
        stop_source_.request_stop();
        if (pool_) pool_->shutdown();
    }

    kairos::testing::FakeClock clock_;
    ActiveRunTracker active_runs_;
    std::stop_source stop_source_;
    std::unique_ptr<exec::RunnerPool> pool_;
};

// ── Tests ───────────────────────────────────────────────────────────────

TEST_F(PipelineTest, SingleJobWorkflowExecutesSuccessfully) {
    setup_runner_pool();

    auto wf = make_single_job_workflow(
        "wfl-single", "SingleJob", "job-echo", "echo-job", "echo hello");

    auto registry = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{std::move(wf)},
        std::vector<TimerEntry>{});

    TriggerBus bus(64);

    Pipeline pipeline(PipelineConfig{}, Pipeline::Dependencies{
        .clock = &clock_,
        .trigger_bus = &bus,
        .runner_pool = pool_.get(),
        .registry = registry,
        .active_runs = &active_runs_,
    });

    auto event = make_manual_trigger("wfl-single");
    auto status = pipeline.process_event(event, stop_source_.get_token());

    EXPECT_EQ(status, RunStatus::Success);
}

TEST_F(PipelineTest, ThreeJobDagExecutesInCorrectOrder) {
    setup_runner_pool();

    auto wf = make_three_job_workflow();
    auto registry = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{std::move(wf)},
        std::vector<TimerEntry>{});

    TriggerBus bus(64);

    Pipeline pipeline(PipelineConfig{}, Pipeline::Dependencies{
        .clock = &clock_,
        .trigger_bus = &bus,
        .runner_pool = pool_.get(),
        .registry = registry,
        .active_runs = &active_runs_,
    });

    auto event = make_manual_trigger("wfl-three");
    auto status = pipeline.process_event(event, stop_source_.get_token());

    EXPECT_EQ(status, RunStatus::Success);
}

TEST_F(PipelineTest, FailedStepFailsJob) {
    setup_failing_runner_pool();

    auto wf = make_single_job_workflow(
        "wfl-fail", "FailJob", "job-fail", "fail-job", "exit 1");

    auto registry = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{std::move(wf)},
        std::vector<TimerEntry>{});

    TriggerBus bus(64);

    Pipeline pipeline(PipelineConfig{}, Pipeline::Dependencies{
        .clock = &clock_,
        .trigger_bus = &bus,
        .runner_pool = pool_.get(),
        .registry = registry,
        .active_runs = &active_runs_,
    });

    auto event = make_manual_trigger("wfl-fail");
    auto status = pipeline.process_event(event, stop_source_.get_token());

    EXPECT_EQ(status, RunStatus::Failure);
}

TEST_F(PipelineTest, ConditionFalseSkipsJob) {
    setup_runner_pool();

    auto wf = make_single_job_workflow(
        "wfl-cond", "CondJob", "job-cond", "cond-job", "echo hello",
        "false");

    auto registry = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{std::move(wf)},
        std::vector<TimerEntry>{});

    TriggerBus bus(64);

    Pipeline pipeline(PipelineConfig{}, Pipeline::Dependencies{
        .clock = &clock_,
        .trigger_bus = &bus,
        .runner_pool = pool_.get(),
        .registry = registry,
        .active_runs = &active_runs_,
    });

    auto event = make_manual_trigger("wfl-cond");
    auto status = pipeline.process_event(event, stop_source_.get_token());

    // The only job was skipped — no failures, so run succeeds.
    EXPECT_EQ(status, RunStatus::Success);
}

TEST_F(PipelineTest, ConditionTrueRunsJob) {
    setup_runner_pool();

    auto wf = make_single_job_workflow(
        "wfl-cond-t", "CondTrueJob", "job-cond-t", "cond-true",
        "echo hello", "true");

    auto registry = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{std::move(wf)},
        std::vector<TimerEntry>{});

    TriggerBus bus(64);

    Pipeline pipeline(PipelineConfig{}, Pipeline::Dependencies{
        .clock = &clock_,
        .trigger_bus = &bus,
        .runner_pool = pool_.get(),
        .registry = registry,
        .active_runs = &active_runs_,
    });

    auto event = make_manual_trigger("wfl-cond-t");
    auto status = pipeline.process_event(event, stop_source_.get_token());

    EXPECT_EQ(status, RunStatus::Success);
}

TEST_F(PipelineTest, FailedDependencySkipsDownstream) {
    setup_failing_runner_pool();

    auto wf = make_three_job_workflow();
    auto registry = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{std::move(wf)},
        std::vector<TimerEntry>{});

    TriggerBus bus(64);

    Pipeline pipeline(PipelineConfig{}, Pipeline::Dependencies{
        .clock = &clock_,
        .trigger_bus = &bus,
        .runner_pool = pool_.get(),
        .registry = registry,
        .active_runs = &active_runs_,
    });

    auto event = make_manual_trigger("wfl-three");
    auto status = pipeline.process_event(event, stop_source_.get_token());

    EXPECT_EQ(status, RunStatus::Failure);
}

TEST_F(PipelineTest, UnknownTargetReturnsFailure) {
    setup_runner_pool();

    auto registry = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{},
        std::vector<TimerEntry>{});

    TriggerBus bus(64);

    Pipeline pipeline(PipelineConfig{}, Pipeline::Dependencies{
        .clock = &clock_,
        .trigger_bus = &bus,
        .runner_pool = pool_.get(),
        .registry = registry,
    });

    auto event = make_manual_trigger("wfl-nonexistent");
    auto status = pipeline.process_event(event, stop_source_.get_token());

    EXPECT_EQ(status, RunStatus::Failure);
}

TEST_F(PipelineTest, EmptyJobSucceeds) {
    setup_runner_pool();

    // Job with no steps.
    JobDef empty_job;
    empty_job.job_id = "job-empty";
    empty_job.job_name = "empty";

    DagNode node;
    node.job_id = "job-empty";
    node.job_name = "empty";

    auto wf = build_workflow_def(
        "wfl-empty", "EmptyJob", {empty_job}, {node});

    auto registry = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{std::move(wf)},
        std::vector<TimerEntry>{});

    TriggerBus bus(64);

    Pipeline pipeline(PipelineConfig{}, Pipeline::Dependencies{
        .clock = &clock_,
        .trigger_bus = &bus,
        .runner_pool = pool_.get(),
        .registry = registry,
    });

    auto event = make_manual_trigger("wfl-empty");
    auto status = pipeline.process_event(event, stop_source_.get_token());

    EXPECT_EQ(status, RunStatus::Success);
}

TEST_F(PipelineTest, ActiveRunTrackerIncrementDecrement) {
    setup_runner_pool();

    auto wf = make_single_job_workflow(
        "wfl-track", "TrackJob", "job-track", "track-job", "echo track");

    auto registry = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{std::move(wf)},
        std::vector<TimerEntry>{});

    TriggerBus bus(64);

    Pipeline pipeline(PipelineConfig{}, Pipeline::Dependencies{
        .clock = &clock_,
        .trigger_bus = &bus,
        .runner_pool = pool_.get(),
        .registry = registry,
        .active_runs = &active_runs_,
    });

    EXPECT_EQ(active_runs_.count("wfl-track"), 0);

    auto event = make_manual_trigger("wfl-track");
    pipeline.process_event(event, stop_source_.get_token());

    EXPECT_EQ(active_runs_.count("wfl-track"), 0);
}

TEST_F(PipelineTest, NullClockThrowsOnConstruction) {
    TriggerBus bus(64);

    EXPECT_THROW(
        Pipeline(PipelineConfig{}, Pipeline::Dependencies{
            .clock = nullptr,
            .trigger_bus = &bus,
        }),
        std::invalid_argument);
}

TEST_F(PipelineTest, RunStatusStrings) {
    EXPECT_EQ(run_status_to_string(RunStatus::Evaluating), "EVALUATING");
    EXPECT_EQ(run_status_to_string(RunStatus::Running), "RUNNING");
    EXPECT_EQ(run_status_to_string(RunStatus::Success), "SUCCESS");
    EXPECT_EQ(run_status_to_string(RunStatus::Failure), "FAILURE");
    EXPECT_EQ(run_status_to_string(RunStatus::Skipped), "SKIPPED");
    EXPECT_EQ(run_status_to_string(RunStatus::Cancelled), "CANCELLED");
    EXPECT_EQ(run_status_to_string(RunStatus::TimedOut), "TIMED_OUT");
}
