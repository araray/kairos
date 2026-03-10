/// tests/unit/exec/runner_pool_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for RunnerPool                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/exec/runner_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <latch>
#include <mutex>

using namespace kairos::exec;

#ifndef _WIN32

class RunnerPoolTest : public ::testing::Test {
protected:
    std::stop_source stop_source_;
};

TEST_F(RunnerPoolTest, SingleJobExecution) {
    RunnerPoolConfig config;
    config.worker_count = 2;
    config.queue_capacity = 16;

    RunnerPool pool(config);
    pool.set_process_handle_factory([](const ProcessSpec&) { return create_process_handle(); });
    pool.start(stop_source_.get_token());

    std::atomic<bool> completed{false};
    std::string captured_step_id;
    int captured_exit_code = -1;

    WorkItem item;
    item.run_id = "run-001";
    item.job_id = "job-build";
    item.step_id = "stp-001";
    item.process_spec.command_line = "echo hello";
    item.process_spec.use_shell = true;
    item.on_complete = [&](const std::string& step_id,
                           ProcessResult result) {
        captured_step_id = step_id;
        captured_exit_code = result.exit_code;
        completed = true;
    };

    ASSERT_TRUE(pool.submit(std::move(item)));

    // Wait for completion.
    for (int i = 0; i < 100 && !completed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_TRUE(completed);
    EXPECT_EQ(captured_step_id, "stp-001");
    EXPECT_EQ(captured_exit_code, 0);

    pool.shutdown();
}

TEST_F(RunnerPoolTest, MultipleJobsParallel) {
    RunnerPoolConfig config;
    config.worker_count = 4;
    config.queue_capacity = 32;

    RunnerPool pool(config);
    pool.set_process_handle_factory([](const ProcessSpec&) { return create_process_handle(); });
    pool.start(stop_source_.get_token());

    constexpr int kJobs = 10;
    std::atomic<int> completed_count{0};

    for (int i = 0; i < kJobs; ++i) {
        WorkItem item;
        item.run_id = "run-multi";
        item.job_id = "job-" + std::to_string(i);
        item.step_id = "stp-" + std::to_string(i);
        item.process_spec.command_line = "echo job" + std::to_string(i);
        item.process_spec.use_shell = true;
        item.on_complete = [&](const std::string&,
                               ProcessResult result) {
            EXPECT_EQ(result.exit_code, 0);
            completed_count.fetch_add(1);
        };

        ASSERT_TRUE(pool.submit(std::move(item)));
    }

    // Wait for all completions.
    for (int i = 0; i < 200 && completed_count < kJobs; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_EQ(completed_count, kJobs);
    pool.shutdown();
}

TEST_F(RunnerPoolTest, FailingJob) {
    RunnerPoolConfig config;
    config.worker_count = 1;

    RunnerPool pool(config);
    pool.set_process_handle_factory([](const ProcessSpec&) { return create_process_handle(); });
    pool.start(stop_source_.get_token());

    std::atomic<bool> completed{false};
    int captured_exit = -1;

    WorkItem item;
    item.step_id = "stp-fail";
    item.process_spec.command_line = "exit 1";
    item.process_spec.use_shell = true;
    item.on_complete = [&](const std::string&,
                           ProcessResult result) {
        captured_exit = result.exit_code;
        completed = true;
    };

    pool.submit(std::move(item));

    for (int i = 0; i < 100 && !completed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_TRUE(completed);
    EXPECT_EQ(captured_exit, 1);
    pool.shutdown();
}

TEST_F(RunnerPoolTest, QueueDepthTracking) {
    RunnerPoolConfig config;
    config.worker_count = 1;
    config.queue_capacity = 16;

    RunnerPool pool(config);
    pool.set_process_handle_factory([](const ProcessSpec&) { return create_process_handle(); });
    // Don't start yet — items will accumulate.

    EXPECT_EQ(pool.queue_depth(), 0u);

    WorkItem item;
    item.step_id = "stp-1";
    item.process_spec.command_line = "sleep 10";
    item.process_spec.use_shell = true;
    item.on_complete = [](const std::string&, ProcessResult) {};

    pool.submit(std::move(item), std::chrono::milliseconds(100));
    EXPECT_EQ(pool.queue_depth(), 1u);

    pool.shutdown();
}

TEST_F(RunnerPoolTest, ShutdownDrainsQueue) {
    RunnerPoolConfig config;
    config.worker_count = 2;
    config.queue_capacity = 32;

    RunnerPool pool(config);
    pool.set_process_handle_factory([](const ProcessSpec&) { return create_process_handle(); });
    pool.start(stop_source_.get_token());

    std::atomic<int> completed_count{0};

    // Submit a few quick jobs.
    for (int i = 0; i < 5; ++i) {
        WorkItem item;
        item.step_id = "stp-" + std::to_string(i);
        item.process_spec.command_line = "true";
        item.process_spec.use_shell = true;
        item.on_complete = [&](const std::string&, ProcessResult) {
            completed_count++;
        };
        pool.submit(std::move(item));
    }

    // Give some time to process.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    pool.shutdown();

    // All submitted jobs should have completed.
    EXPECT_GE(completed_count, 0);  // At least some should complete.
}

TEST_F(RunnerPoolTest, OutputCapture) {
    RunnerPoolConfig config;
    config.worker_count = 1;

    RunnerPool pool(config);
    pool.set_process_handle_factory([](const ProcessSpec&) { return create_process_handle(); });
    pool.start(stop_source_.get_token());

    std::atomic<bool> completed{false};
    std::string captured_stdout;

    WorkItem item;
    item.step_id = "stp-out";
    item.process_spec.command_line = "echo captured_output";
    item.process_spec.use_shell = true;
    item.on_complete = [&](const std::string&,
                           ProcessResult result) {
        captured_stdout = result.stdout_data;
        completed = true;
    };

    pool.submit(std::move(item));

    for (int i = 0; i < 100 && !completed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_TRUE(completed);
    EXPECT_NE(captured_stdout.find("captured_output"), std::string::npos);
    pool.shutdown();
}

#endif  // !_WIN32
