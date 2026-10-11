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
#include "kairos/persist/database.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/persist/query_reader.hpp"
#include "kairos/testing/fake_process.hpp"
#include "test_helpers.hpp"

#include <filesystem>

#include <gtest/gtest.h>

#include <chrono>
#include <stop_token>
#include <thread>

// Platform-conditional output-append command for tests: POSIX shells
// use "$VAR"; cmd.exe uses %VAR% with no space before >>.
#ifdef _WIN32
// cmd.exe /C breaks nested quotes in redirect targets — the temp
// paths these tests use have no spaces, so no inner quotes needed.
// %VAR% is cmd's sigil; POSIX shells use $VAR.
#define KAIROS_TEST_OUT_REF(name) "%" name "%"
#define KAIROS_TEST_OUT_APPEND(name, kv) \
    "echo " kv ">>%KAIROS_OUTPUT%"
#else
#define KAIROS_TEST_OUT_REF(name) "$" name
#define KAIROS_TEST_OUT_APPEND(name, kv) \
    "echo " kv " >> $KAIROS_OUTPUT"
#endif

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
        pool_->set_process_handle_factory([](const exec::ProcessSpec&) {
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
        pool_->set_process_handle_factory([](const exec::ProcessSpec&) {
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

// ── §15.1 dry-run mode ──────────────────────────────────────────────────

TEST_F(PipelineTest, DryRunExecutesWithoutSpawningProcesses) {
    // The factory produces processes that FAIL loudly (exit 77) and
    // print a marker — if a dry-run ever spawned one, the run would
    // fail and the test catches it.
    pool_ = std::make_unique<exec::RunnerPool>(
        exec::RunnerPoolConfig{.worker_count = 1, .queue_capacity = 8});
    pool_->set_process_handle_factory([](const exec::ProcessSpec&) {
        auto p = std::make_unique<kairos::testing::FakeProcessHandle>();
        p->set_exit_code(77);
        p->set_stdout_data("SPAWNED!\n");
        return p;
    });
    pool_->start(stop_source_.get_token());

    auto wf = make_single_job_workflow(
        "wfl-dry", "Dry", "job-1", "one", "dangerous-command --rm-everything");

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

    auto event = make_manual_trigger("wfl-dry");
    auto* manual = std::get_if<engine::ManualRunPayload>(
        &event.payload);
    ASSERT_NE(manual, nullptr);
    manual->dry_run = true;

    auto status = pipeline.process_event(event, stop_source_.get_token());
    // No process ran (exit-77 trap) → the dry-run path succeeded.
    EXPECT_EQ(status, RunStatus::Success);
}

// ── §15.2 step retries ──────────────────────────────────────────────────

TEST_F(PipelineTest, StepRetriesUntilSuccess) {
    // Fails the first two spawns, succeeds on the third.
    auto counter = std::make_shared<int>(0);
    pool_ = std::make_unique<exec::RunnerPool>(
        exec::RunnerPoolConfig{.worker_count = 1, .queue_capacity = 8});
    pool_->set_process_handle_factory(
        [counter](const exec::ProcessSpec&) {
            auto p =
                std::make_unique<kairos::testing::FakeProcessHandle>();
            if (++(*counter) < 3) {
                p->set_exit_code(1);
                p->set_stderr_data("flaky\n");
            } else {
                p->set_exit_code(0);
                p->set_stdout_data("finally\n");
            }
            return p;
        });
    pool_->start(stop_source_.get_token());

    StepDef step;
    step.step_id = "stp-r";
    step.step_name = "flaky-call";
    step.command = "curl api";
    RetryPolicy policy;
    policy.max_attempts = 3;  // delay 0 → fast test
    step.retry = policy;

    JobDef job;
    job.job_id = "job-r";
    job.job_name = "flaky";
    job.steps = {step};
    DagNode node;
    node.job_id = "job-r";
    node.job_name = "flaky";
    auto wf = build_workflow_def(
        "wfl-retry", "Retry", {job}, {node});

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

    auto status = pipeline.process_event(
        make_manual_trigger("wfl-retry"), stop_source_.get_token());
    EXPECT_EQ(status, RunStatus::Success);
    EXPECT_EQ(*counter, 3);
}

TEST_F(PipelineTest, StepFailsAfterExhaustedRetries) {
    auto counter = std::make_shared<int>(0);
    pool_ = std::make_unique<exec::RunnerPool>(
        exec::RunnerPoolConfig{.worker_count = 1, .queue_capacity = 8});
    pool_->set_process_handle_factory(
        [counter](const exec::ProcessSpec&) {
            auto p =
                std::make_unique<kairos::testing::FakeProcessHandle>();
            ++(*counter);
            p->set_exit_code(9);
            return p;
        });
    pool_->start(stop_source_.get_token());

    StepDef step;
    step.step_id = "stp-x";
    step.step_name = "always-fails";
    step.command = "false";
    RetryPolicy policy;
    policy.max_attempts = 3;
    policy.retry_on = {7};  // 9 not retryable → exactly 1 attempt
    step.retry = policy;

    JobDef job;
    job.job_id = "job-x";
    job.job_name = "xfail";
    job.steps = {step};
    DagNode node;
    node.job_id = "job-x";
    node.job_name = "xfail";
    auto wf = build_workflow_def(
        "wfl-noretry", "NoRetry", {job}, {node});

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

    auto status = pipeline.process_event(
        make_manual_trigger("wfl-noretry"), stop_source_.get_token());
    EXPECT_EQ(status, RunStatus::Failure);
    EXPECT_EQ(*counter, 1);  // retry_on filtered the 9
}

// ── §8 shared context: KAIROS_OUTPUT capture ────────────────────────────

TEST_F(PipelineTest, StepOutputsCapturedAndPersisted) {
    kairos::testing::TempDir data_dir;
    kairos::testing::TempDir scratch;

    // REAL processes (shell) — the capture contract involves the
    // filesystem, so fakes can't exercise it.
    pool_ = std::make_unique<exec::RunnerPool>(
        exec::RunnerPoolConfig{.worker_count = 1, .queue_capacity = 8});
    pool_->set_process_handle_factory([](const exec::ProcessSpec&) {
        return exec::create_process_handle();
    });
    pool_->start(stop_source_.get_token());

    StepDef produce;
    produce.step_id = "stp-produce";
    produce.step_name = "produce";
#ifdef _WIN32
    // cmd.exe: %VAR% expansion; no-space && chains (spaces around &
    // leak into the echoed value; multi-line is not supported by
    // cmd /C); `echo x>>file` without space → no trailing space.
    produce.command =
        "echo VERSION=9.9.9>>%KAIROS_OUTPUT%"
        "&&echo COMMIT_SHA=abc123>>%KAIROS_OUTPUT%";
    StepDef consume;
    consume.step_id = "stp-consume";
    consume.step_name = "consume";
    consume.command = "echo got-%KAIROS_OUTPUT%";
#else
    produce.command =
        "echo VERSION=9.9.9 >> \"$KAIROS_OUTPUT\"\n"
        "echo COMMIT_SHA=abc123 >> \"$KAIROS_OUTPUT\"";

    StepDef consume;
    consume.step_id = "stp-consume";
    consume.step_name = "consume";
    consume.command = "echo got-$KAIROS_OUTPUT";
#endif

    JobDef job;
    job.job_id = "job-out";
    job.job_name = "producer";
    job.steps = {produce, consume};
    DagNode node;
    node.job_id = "job-out";
    node.job_name = "producer";
    auto wf = build_workflow_def(
        "wfl-out", "Outputs", {job}, {node});

    auto registry = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{std::move(wf)},
        std::vector<TimerEntry>{});

    // In-memory DB to observe persistence.
    auto db = kairos::persist::open_database(":memory:");
    kairos::persist::apply_migrations(
        *db, kairos::persist::get_migrations());
    kairos::persist::DBWriterConfig wcfg;
    kairos::persist::DBWriter writer(*db, wcfg);
    std::stop_source writer_stop;
    writer.start(writer_stop.get_token());

    TriggerBus bus(64);
    Pipeline pipeline(PipelineConfig{}, Pipeline::Dependencies{
        .clock = &clock_,
        .trigger_bus = &bus,
        .runner_pool = pool_.get(),
        .registry = registry,
        .active_runs = &active_runs_,
        .db_writer = &writer,
        .data_dir = data_dir.path(),
    });

    auto status = pipeline.process_event(
        make_manual_trigger("wfl-out"), stop_source_.get_token());
    EXPECT_EQ(status, RunStatus::Success);

    writer_stop.request_stop();
    writer.flush();

    // Find the run id from the DB.
    std::string run_id;
    {
        SQLite::Statement q(*db, "SELECT run_id FROM runs LIMIT 1");
        ASSERT_TRUE(q.executeStep());
        run_id = q.getColumn(0).getString();
    }

    // Outputs persisted.
    kairos::persist::QueryReader reader(*db);
    auto outs = reader.get_run_outputs(run_id);
    ASSERT_EQ(outs.size(), 2u);
    EXPECT_EQ(outs[0].key, "COMMIT_SHA");
    EXPECT_EQ(outs[0].value, "abc123");
    EXPECT_EQ(outs[1].key, "VERSION");
    EXPECT_EQ(outs[1].value, "9.9.9");

    // The output file itself exists on disk.
    EXPECT_TRUE(std::filesystem::exists(
        data_dir.path() / "runs" / run_id / "outputs" / "job-out" /
        "stp-produce.env"));
}

TEST_F(PipelineTest, InvalidOutputKeysAreDropped) {
    kairos::testing::TempDir data_dir;
    pool_ = std::make_unique<exec::RunnerPool>(
        exec::RunnerPoolConfig{.worker_count = 1, .queue_capacity = 8});
    pool_->set_process_handle_factory([](const exec::ProcessSpec&) {
        return exec::create_process_handle();
    });
    pool_->start(stop_source_.get_token());

    StepDef produce;
    produce.step_id = "stp-bad";
    produce.step_name = "bad";
#ifdef _WIN32
    produce.command =
        "echo GOOD=1>>%KAIROS_OUTPUT%"
        "&&echo bad-key!!=2>>%KAIROS_OUTPUT%";
#else
    produce.command =
        "echo GOOD=1 >> \"$KAIROS_OUTPUT\"; "
        "echo bad-key!!=2 >> \"$KAIROS_OUTPUT\"";
#endif
    // (bad-key!! isn't KEY=VALUE — the .env parser drops the line, so
    // this also covers parse-level leniency; run stays green.)

    JobDef job;
    job.job_id = "job-bad";
    job.job_name = "badjob";
    job.steps = {produce};
    DagNode node;
    node.job_id = "job-bad";
    node.job_name = "badjob";
    auto wf = build_workflow_def(
        "wfl-bad", "BadOut", {job}, {node});
    auto registry = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{std::move(wf)},
        std::vector<TimerEntry>{});

    auto db = kairos::persist::open_database(":memory:");
    kairos::persist::apply_migrations(
        *db, kairos::persist::get_migrations());
    kairos::persist::DBWriter writer(*db, {});
    std::stop_source writer_stop;
    writer.start(writer_stop.get_token());

    TriggerBus bus(64);
    Pipeline pipeline(PipelineConfig{}, Pipeline::Dependencies{
        .clock = &clock_,
        .trigger_bus = &bus,
        .runner_pool = pool_.get(),
        .registry = registry,
        .active_runs = &active_runs_,
        .db_writer = &writer,
        .data_dir = data_dir.path(),
    });

    auto status = pipeline.process_event(
        make_manual_trigger("wfl-bad"), stop_source_.get_token());
    EXPECT_EQ(status, RunStatus::Success);

    writer_stop.request_stop();
    writer.flush();
    std::string run_id;
    {
        SQLite::Statement q(*db, "SELECT run_id FROM runs LIMIT 1");
        ASSERT_TRUE(q.executeStep());
        run_id = q.getColumn(0).getString();
    }
    kairos::persist::QueryReader reader(*db);
    EXPECT_TRUE(reader.get_run_outputs(run_id).empty());
}

// ── §8 substitution + output() conditions ───────────────────────────────

class RunContextTest : public PipelineTest {
protected:
    void SetUp() override {
        PipelineTest::SetUp();
        data_dir_ = std::make_unique<kairos::testing::TempDir>();
        pool_ = std::make_unique<exec::RunnerPool>(
            exec::RunnerPoolConfig{.worker_count = 1,
                                   .queue_capacity = 8});
        pool_->set_process_handle_factory([](const exec::ProcessSpec&) {
            return exec::create_process_handle();
        });
        pool_->start(stop_source_.get_token());

        db_ = kairos::persist::open_database(":memory:");
        kairos::persist::apply_migrations(
            *db_, kairos::persist::get_migrations());
        writer_ = std::make_unique<kairos::persist::DBWriter>(
            *db_, kairos::persist::DBWriterConfig{});
        writer_stop_ = std::stop_source{};
        writer_->start(writer_stop_.get_token());
    }
    void TearDown() override {
        if (writer_) {
            writer_stop_.request_stop();
            writer_->flush();
            writer_.reset();
        }
        PipelineTest::TearDown();
    }

    /// Run a workflow and return the concatenated log chunks.
    std::string run_and_collect(WorkflowDef wf,
                                const std::string& target_id,
                                RunStatus& status_out) {
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
            .db_writer = writer_.get(),
            .data_dir = data_dir_->path(),
        });
        status_out = pipeline.process_event(
            make_manual_trigger(target_id), stop_source_.get_token());
        writer_stop_.request_stop();
        writer_->flush();

        std::string run_id;
        SQLite::Statement q(*db_, "SELECT run_id FROM runs LIMIT 1");
        if (q.executeStep()) run_id = q.getColumn(0).getString();
        std::string logs;
        SQLite::Statement l(*db_,
            "SELECT content FROM log_chunks WHERE run_id = ? "
            "ORDER BY id");
        l.bind(1, run_id);
        while (l.executeStep()) logs += l.getColumn(0).getString();
        return logs;
    }

    std::unique_ptr<kairos::testing::TempDir> data_dir_;
    std::unique_ptr<SQLite::Database> db_;
    std::unique_ptr<kairos::persist::DBWriter> writer_;
    std::stop_source writer_stop_;
};

TEST_F(RunContextTest, StepsScopeSubstitutionWithinJob) {
    StepDef produce;
    produce.step_id = "stp-p";
    produce.step_name = "produce";
    produce.command = KAIROS_TEST_OUT_APPEND("$KAIROS_OUTPUT", "VERSION=7.7.7");

    StepDef consume;
    consume.step_id = "stp-c";
    consume.step_name = "consume";
    consume.command = "echo deploying-${{ steps.produce.VERSION }}";

    JobDef job;
    job.job_id = "job-1";
    job.job_name = "one";
    job.steps = {produce, consume};
    DagNode node{.job_id = "job-1", .job_name = "one"};
    auto wf = build_workflow_def("wfl-s", "Sub", {job}, {node});

    RunStatus st;
    auto logs = run_and_collect(std::move(wf), "wfl-s", st);
    EXPECT_EQ(st, RunStatus::Success);
    EXPECT_NE(logs.find("deploying-7.7.7"), std::string::npos)
        << "logs: " << logs;
}

TEST_F(RunContextTest, JobsScopeSubstitutionAcrossJobs) {
    StepDef produce;
    produce.step_id = "stp-p";
    produce.step_name = "produce";
    produce.command = KAIROS_TEST_OUT_APPEND("$KAIROS_OUTPUT", "SHA=deadbeef");

    JobDef j1;
    j1.job_id = "job-a";
    j1.job_name = "build";
    j1.steps = {produce};

    StepDef deploy_step;
    deploy_step.step_id = "stp-d";
    deploy_step.step_name = "deploy";
    deploy_step.command = "echo at-${{ jobs.build.produce.SHA }}";

    JobDef j2;
    j2.job_id = "job-b";
    j2.job_name = "deploy";
    j2.steps = {deploy_step};

    DagNode n1{.job_id = "job-a", .job_name = "build"};
    DagNode n2{.job_id = "job-b", .job_name = "deploy"};
    n2.needs = {"job-a"};
    auto wf = build_workflow_def(
        "wfl-x", "Cross", {j1, j2}, {n1, n2});

    RunStatus st;
    auto logs = run_and_collect(std::move(wf), "wfl-x", st);
    EXPECT_EQ(st, RunStatus::Success);
    EXPECT_NE(logs.find("at-deadbeef"), std::string::npos)
        << "logs: " << logs;
}

TEST_F(RunContextTest, OutputGatedCondition) {
    StepDef produce;
    produce.step_id = "stp-p";
    produce.step_name = "produce";
    produce.command = KAIROS_TEST_OUT_APPEND("$KAIROS_OUTPUT", "VERSION=42");
    JobDef j1;
    j1.job_id = "job-a";
    j1.job_name = "producer";
    j1.steps = {produce};

    StepDef check;
    check.step_id = "stp-c";
    check.step_name = "check";
    check.command = "echo condition-passed";
    JobDef j2;
    j2.job_id = "job-b";
    j2.job_name = "checker";
    j2.steps = {check};
    j2.condition_expr =
        R"(output("producer", "produce", "VERSION") == "42")";

    DagNode n1{.job_id = "job-a", .job_name = "producer"};
    DagNode n2{.job_id = "job-b", .job_name = "checker", .needs = {"job-a"}};
    n2.condition_expr = j2.condition_expr;
    auto wf = build_workflow_def(
        "wfl-c", "Cond", {j1, j2}, {n1, n2});

    RunStatus st;
    auto logs = run_and_collect(std::move(wf), "wfl-c", st);
    EXPECT_EQ(st, RunStatus::Success);
    EXPECT_NE(logs.find("condition-passed"), std::string::npos)
        << "logs: " << logs;
}

// ── §15.4 notification hooks ─────────────────────────────────────────────

TEST_F(RunContextTest, CommandNotificationFiresOnSuccess) {
    kairos::testing::TempDir dir;
    auto marker = dir.path() / "fired.txt";

    StepDef step;
    step.step_id = "stp-n";
    step.step_name = "work";
    step.command = "echo doing";

    JobDef job;
    job.job_id = "job-n";
    job.job_name = "work";
    job.steps = {step};
    DagNode node{.job_id = "job-n", .job_name = "work"};

    auto wf = build_workflow_def("wfl-notify", "NotifyFlow",
                                 {job}, {node});
    NotificationDef hook;
    hook.type = NotificationDef::Type::Command;
    hook.command = "echo {{ workflow }}/{{ status }} >> \"" +
        marker.string() + "\"";
    hook.message_template = "{{ workflow }} {{ status }}";
    wf.notifications.on_success.push_back(hook);

    pool_ = std::make_unique<exec::RunnerPool>(
        exec::RunnerPoolConfig{.worker_count = 1, .queue_capacity = 8});
    pool_->set_process_handle_factory([](const exec::ProcessSpec&) {
        return exec::create_process_handle();
    });
    pool_->start(stop_source_.get_token());

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
        .data_dir = std::filesystem::temp_directory_path(),
        .notification_process_factory = []() {
            return exec::create_process_handle();
        },
    });

    auto status = pipeline.process_event(
        make_manual_trigger("wfl-notify"), stop_source_.get_token());
    EXPECT_EQ(status, RunStatus::Success);

    // Async dispatch — poll briefly for the marker file.
    bool fired = false;
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::error_code ec;
        if (std::filesystem::exists(marker, ec)) { fired = true; break; }
    }
    ASSERT_TRUE(fired) << "notification command never ran";
    std::ifstream in(marker);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("NotifyFlow/SUCCESS"), std::string::npos)
        << "content: " << content;
}

TEST_F(RunContextTest, WebhookNotificationDelivered) {
    StepDef step;
    step.step_id = "stp-w";
    step.step_name = "work";
    step.command = "true";
    JobDef job;
    job.job_id = "job-w";
    job.job_name = "work";
    job.steps = {step};
    DagNode node{.job_id = "job-w", .job_name = "work"};

    auto wf = build_workflow_def("wfl-wh", "WebhookFlow",
                                 {job}, {node});
    NotificationDef hook;
    hook.type = NotificationDef::Type::Webhook;
    hook.url = "http://127.0.0.1:1/nothing";  // captured, never dialed
    hook.message_template = "{{ workflow }} {{ status }}";
    wf.notifications.on_success.push_back(hook);

    pool_ = std::make_unique<exec::RunnerPool>(
        exec::RunnerPoolConfig{.worker_count = 1, .queue_capacity = 8});
    pool_->set_process_handle_factory([](const exec::ProcessSpec&) {
        auto p = std::make_unique<kairos::testing::FakeProcessHandle>();
        p->set_exit_code(0);
        return p;
    });
    pool_->start(stop_source_.get_token());

    auto registry = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{std::move(wf)},
        std::vector<TimerEntry>{});

    std::mutex mu;
    std::vector<std::pair<std::string, std::string>> delivered;
    TriggerBus bus(64);
    Pipeline pipeline(PipelineConfig{}, Pipeline::Dependencies{
        .clock = &clock_,
        .trigger_bus = &bus,
        .runner_pool = pool_.get(),
        .registry = registry,
        .active_runs = &active_runs_,
        .webhook_post = [&mu, &delivered](const std::string& url,
                                          const std::string& body) {
            std::lock_guard lock(mu);
            delivered.emplace_back(url, body);
        },
    });

    auto status = pipeline.process_event(
        make_manual_trigger("wfl-wh"), stop_source_.get_token());
    EXPECT_EQ(status, RunStatus::Success);

    for (int i = 0; i < 30; ++i) {
        {
            std::lock_guard lock(mu);
            if (!delivered.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::lock_guard lock(mu);
    ASSERT_EQ(delivered.size(), 1u);
    EXPECT_EQ(delivered[0].first, "http://127.0.0.1:1/nothing");
    EXPECT_NE(delivered[0].second.find("WebhookFlow SUCCESS"),
              std::string::npos)
        << "body: " << delivered[0].second;
}

// ── §9.3 in-step query env vars ──────────────────────────────────────────

TEST_F(RunContextTest, QueryEnvVarsInjectedIntoSteps) {
    kairos::testing::TempDir data_dir;

    // Seed a previous run for the same target.
    {
        SQLite::Statement q(*db_,
            "INSERT INTO runs (run_id, target_type, target_id, "
            "target_name, trigger_type, trigger_id, correlation_id, "
            "status, start_ts, end_ts, duration_ms) VALUES "
            "('run-prev-0001','workflow','wfl-q','queried','manual',"
            "'t','c','SUCCESS','2020-01-01T00:00:00Z',"
            "'2020-01-01T00:00:01Z',10)");
        q.exec();
    }

    StepDef step;
    step.step_id = "stp-q";
    step.step_name = "probe";
#ifdef _WIN32
    step.command =
        "echo num=%KAIROS_RUN_NUMBER% "
        "prev=%KAIROS_PREV_RUN_STATUS% id=%KAIROS_PREV_RUN_ID%";
#else
    step.command =
        "echo num=$KAIROS_RUN_NUMBER "
        "prev=$KAIROS_PREV_RUN_STATUS id=$KAIROS_PREV_RUN_ID";
#endif
    JobDef job;
    job.job_id = "job-q";
    job.job_name = "queried";
    job.steps = {step};
    DagNode node{.job_id = "job-q", .job_name = "queried"};
    auto wf = build_workflow_def("wfl-q", "queried", {job}, {node});

    pool_ = std::make_unique<exec::RunnerPool>(
        exec::RunnerPoolConfig{.worker_count = 1, .queue_capacity = 8});
    pool_->set_process_handle_factory([](const exec::ProcessSpec&) {
        return exec::create_process_handle();
    });
    pool_->start(stop_source_.get_token());

    auto registry = std::make_shared<WorkflowRegistry>(
        std::vector<WorkflowDef>{std::move(wf)},
        std::vector<TimerEntry>{});
    kairos::persist::QueryReader reader(*db_);
    TriggerBus bus(64);
    Pipeline pipeline(PipelineConfig{}, Pipeline::Dependencies{
        .clock = &clock_,
        .trigger_bus = &bus,
        .runner_pool = pool_.get(),
        .registry = registry,
        .active_runs = &active_runs_,
        .db_writer = writer_.get(),
        .query_reader = &reader,
        .data_dir = data_dir.path(),
        .notification_process_factory = []() {
            return exec::create_process_handle();
        },
    });

    auto status = pipeline.process_event(
        make_manual_trigger("wfl-q"), stop_source_.get_token());
    EXPECT_EQ(status, RunStatus::Success);

    writer_stop_.request_stop();
    writer_->flush();
    std::string run_id;
    {
        SQLite::Statement q(*db_,
            "SELECT run_id FROM runs ORDER BY start_ts DESC LIMIT 1");
        ASSERT_TRUE(q.executeStep());
        run_id = q.getColumn(0).getString();
    }
    std::string logs;
    {
        SQLite::Statement q(*db_,
            "SELECT content FROM log_chunks WHERE run_id = ? "
            "ORDER BY id");
        q.bind(1, run_id);
        while (q.executeStep()) logs += q.getColumn(0).getString();
    }
    EXPECT_NE(logs.find("num=2 "), std::string::npos)
        << "logs: " << logs;   // 1 prior run → this is #2
    EXPECT_NE(logs.find("prev=SUCCESS"), std::string::npos)
        << "logs: " << logs;
    EXPECT_NE(logs.find("id=run-prev-0001"), std::string::npos)
        << "logs: " << logs;
}
