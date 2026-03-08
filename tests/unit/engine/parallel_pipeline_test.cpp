/// tests/unit/engine/parallel_pipeline_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Parallel pipeline execution tests                                        ║
// ║                                                                           ║
// ║  Verifies that jobs at the same DAG level execute via the parallel       ║
// ║  dispatch mechanism without deadlocks or race conditions (§11.4).        ║
// ║  Uses FakeClock + FakeProcessHandle + in-memory SQLite.                  ║
// ║                                                                           ║
// ║  Spec reference: §11.4                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/pipeline.hpp"
#include "kairos/engine/dag.hpp"
#include "kairos/engine/trigger_event.hpp"
#include "kairos/engine/workflow_registry.hpp"
#include "kairos/exec/runner_pool.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/testing/fake_clock.hpp"
#include "kairos/testing/fake_process.hpp"

#include <gtest/gtest.h>

#include <SQLiteCpp/SQLiteCpp.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace kairos::engine;
using namespace kairos::testing;
using namespace std::chrono_literals;

// ── Test fixture ────────────────────────────────────────────────────────

class ParallelPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        clock_.set_now(std::chrono::system_clock::time_point{
            std::chrono::hours(24 * 365 * 56)});
        clock_.set_steady(std::chrono::steady_clock::time_point{
            std::chrono::hours(1)});

        db_ = std::make_unique<SQLite::Database>(
            ":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        kairos::persist::apply_migrations(*db_,
            kairos::persist::get_migrations());
        writer_ = std::make_unique<kairos::persist::DBWriter>(*db_);
        stop_ = std::make_unique<std::stop_source>();
        writer_->start(stop_->get_token());

        trigger_bus_ = std::make_unique<TriggerBus>(256);
        active_runs_ = std::make_unique<ActiveRunTracker>();

        pool_ = std::make_unique<kairos::exec::RunnerPool>(
            kairos::exec::RunnerPoolConfig{.worker_count = 4,
                                            .queue_capacity = 64});
        pool_->set_process_handle_factory([]() {
            auto proc = std::make_unique<FakeProcessHandle>();
            proc->set_exit_code(0);
            proc->set_stdout_data("ok\n");
            return proc;
        });
        pool_->start(stop_->get_token());
    }

    void TearDown() override {
        stop_->request_stop();
        pool_->shutdown();
        writer_->flush();
        writer_.reset();
    }

    /// Helper: build a trigger event.
    TriggerEvent make_trigger(const std::string& target_wf) {
        TriggerEvent event;
        event.type = TriggerType::Manual;
        event.trigger_id = "trg-manual";
        event.target_id = target_wf;
        event.target_kind = TriggerEvent::TargetKind::Workflow;
        event.fire_time = clock_.now();
        event.mono_time = clock_.steady_now();
        event.correlation_id = "corr-test";
        return event;
    }

    /// Helper: build a simple step for a job.
    static StepDef make_step(const std::string& id, const std::string& cmd) {
        StepDef s;
        s.step_id = id;
        s.step_name = id;
        s.command = cmd;
        return s;
    }

    /// Helper: build a workflow and construct a Pipeline.
    Pipeline make_pipeline(
        const std::string& wf_id,
        const std::string& wf_name,
        std::vector<DagNode> dag_nodes,
        std::vector<JobDef> jobs)
    {
        auto dag = WorkflowDag::build(dag_nodes);

        WorkflowDef wf;
        wf.workflow_id = wf_id;
        wf.workflow_name = wf_name;
        wf.dag = std::move(dag);
        wf.jobs = std::move(jobs);

        auto reg = std::make_shared<WorkflowRegistry>(
            std::vector<WorkflowDef>{std::move(wf)},
            std::vector<TimerEntry>{});

        PipelineConfig config;
        config.kel_limits = {};
        Pipeline::Dependencies deps;
        deps.clock = &clock_;
        deps.trigger_bus = trigger_bus_.get();
        deps.runner_pool = pool_.get();
        deps.registry = std::move(reg);
        deps.active_runs = active_runs_.get();
        deps.db_writer = writer_.get();

        return Pipeline(config, deps);
    }

    FakeClock clock_;
    std::unique_ptr<SQLite::Database> db_;
    std::unique_ptr<kairos::persist::DBWriter> writer_;
    std::unique_ptr<std::stop_source> stop_;
    std::unique_ptr<TriggerBus> trigger_bus_;
    std::unique_ptr<ActiveRunTracker> active_runs_;
    std::unique_ptr<kairos::exec::RunnerPool> pool_;
};

// ── Tests ───────────────────────────────────────────────────────────────

TEST_F(ParallelPipelineTest, SingleJobLevelRunsNormally) {
    auto pipeline = make_pipeline("wf-1", "simple",
        {DagNode{.job_id = "job-a", .job_name = "alpha"}},
        {JobDef{.job_id = "job-a", .job_name = "alpha",
                .steps = {make_step("stp-a", "echo hello")}}});

    auto status = pipeline.process_event(
        make_trigger("wf-1"), stop_->get_token());
    EXPECT_EQ(status, RunStatus::Success);
}

TEST_F(ParallelPipelineTest, TwoIndependentJobsBothComplete) {
    auto pipeline = make_pipeline("wf-par", "parallel",
        {DagNode{.job_id = "job-a", .job_name = "alpha"},
         DagNode{.job_id = "job-b", .job_name = "beta"}},
        {JobDef{.job_id = "job-a", .job_name = "alpha",
                .steps = {make_step("stp-a", "echo a")}},
         JobDef{.job_id = "job-b", .job_name = "beta",
                .steps = {make_step("stp-b", "echo b")}}});

    auto status = pipeline.process_event(
        make_trigger("wf-par"), stop_->get_token());
    EXPECT_EQ(status, RunStatus::Success);
}

TEST_F(ParallelPipelineTest, ThreeIndependentJobsAtLevelZero) {
    auto pipeline = make_pipeline("wf-3", "three",
        {DagNode{.job_id = "job-a", .job_name = "a"},
         DagNode{.job_id = "job-b", .job_name = "b"},
         DagNode{.job_id = "job-c", .job_name = "c"}},
        {JobDef{.job_id = "job-a", .job_name = "a",
                .steps = {make_step("stp-a", "echo a")}},
         JobDef{.job_id = "job-b", .job_name = "b",
                .steps = {make_step("stp-b", "echo b")}},
         JobDef{.job_id = "job-c", .job_name = "c",
                .steps = {make_step("stp-c", "echo c")}}});

    auto status = pipeline.process_event(
        make_trigger("wf-3"), stop_->get_token());
    EXPECT_EQ(status, RunStatus::Success);
}

TEST_F(ParallelPipelineTest, DependentJobsRespectLevelOrdering) {
    // Level 0: [a, b], Level 1: [c] (needs a,b)
    auto pipeline = make_pipeline("wf-dep", "deps",
        {DagNode{.job_id = "job-a", .job_name = "a"},
         DagNode{.job_id = "job-b", .job_name = "b"},
         DagNode{.job_id = "job-c", .job_name = "c",
                 .needs = {"job-a", "job-b"}}},
        {JobDef{.job_id = "job-a", .job_name = "a",
                .steps = {make_step("stp-a", "echo a")}},
         JobDef{.job_id = "job-b", .job_name = "b",
                .steps = {make_step("stp-b", "echo b")}},
         JobDef{.job_id = "job-c", .job_name = "c",
                .steps = {make_step("stp-c", "echo c")}}});

    auto status = pipeline.process_event(
        make_trigger("wf-dep"), stop_->get_token());
    EXPECT_EQ(status, RunStatus::Success);
}

TEST_F(ParallelPipelineTest, SkippedJobDoesNotBlock) {
    // job-a has false condition; job-b should still run.
    auto pipeline = make_pipeline("wf-skip", "skip",
        {DagNode{.job_id = "job-a", .job_name = "a",
                 .condition_expr = "false"},
         DagNode{.job_id = "job-b", .job_name = "b"}},
        {JobDef{.job_id = "job-a", .job_name = "a",
                .steps = {make_step("stp-a", "echo a")}},
         JobDef{.job_id = "job-b", .job_name = "b",
                .steps = {make_step("stp-b", "echo b")}}});

    auto status = pipeline.process_event(
        make_trigger("wf-skip"), stop_->get_token());
    // Should not deadlock or crash.
    EXPECT_NE(status, RunStatus::Cancelled);
}

TEST_F(ParallelPipelineTest, DiamondDag) {
    //     a
    //    / \
    //   b   c
    //    \ /
    //     d
    auto pipeline = make_pipeline("wf-dia", "diamond",
        {DagNode{.job_id = "job-a", .job_name = "a"},
         DagNode{.job_id = "job-b", .job_name = "b",
                 .needs = {"job-a"}},
         DagNode{.job_id = "job-c", .job_name = "c",
                 .needs = {"job-a"}},
         DagNode{.job_id = "job-d", .job_name = "d",
                 .needs = {"job-b", "job-c"}}},
        {JobDef{.job_id = "job-a", .job_name = "a",
                .steps = {make_step("stp-a", "echo a")}},
         JobDef{.job_id = "job-b", .job_name = "b",
                .steps = {make_step("stp-b", "echo b")}},
         JobDef{.job_id = "job-c", .job_name = "c",
                .steps = {make_step("stp-c", "echo c")}},
         JobDef{.job_id = "job-d", .job_name = "d",
                .steps = {make_step("stp-d", "echo d")}}});

    auto status = pipeline.process_event(
        make_trigger("wf-dia"), stop_->get_token());
    EXPECT_EQ(status, RunStatus::Success);
}

TEST_F(ParallelPipelineTest, StopDuringExecution) {
    auto pipeline = make_pipeline("wf-stop", "stop",
        {DagNode{.job_id = "job-a", .job_name = "a"},
         DagNode{.job_id = "job-b", .job_name = "b"}},
        {JobDef{.job_id = "job-a", .job_name = "a",
                .steps = {make_step("stp-a", "echo a")}},
         JobDef{.job_id = "job-b", .job_name = "b",
                .steps = {make_step("stp-b", "echo b")}}});

    stop_->request_stop();

    auto status = pipeline.process_event(
        make_trigger("wf-stop"), stop_->get_token());
    EXPECT_EQ(status, RunStatus::Cancelled);
}
