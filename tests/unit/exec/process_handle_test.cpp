/// tests/unit/exec/process_handle_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for BoundedQueue and ProcessHandle (POSIX)                        ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/core/bounded_queue.hpp"
#include "kairos/exec/process_handle.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════════
// BoundedQueue tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(BoundedQueueTest, PushPopBasic) {
    kairos::core::BoundedQueue<int> q(8);
    EXPECT_TRUE(q.push(42));
    auto val = q.pop();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, 42);
}

TEST(BoundedQueueTest, TryPushTryPop) {
    kairos::core::BoundedQueue<int> q(2);
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_FALSE(q.try_push(3));  // Full.

    EXPECT_EQ(q.try_pop().value(), 1);
    EXPECT_EQ(q.try_pop().value(), 2);
    EXPECT_FALSE(q.try_pop().has_value());  // Empty.
}

TEST(BoundedQueueTest, FIFO) {
    kairos::core::BoundedQueue<int> q(8);
    for (int i = 0; i < 5; ++i) q.push(i);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(q.pop().value(), i);
    }
}

TEST(BoundedQueueTest, Drain) {
    kairos::core::BoundedQueue<int> q(16);
    for (int i = 0; i < 10; ++i) q.push(i);

    auto batch = q.drain(5);
    EXPECT_EQ(batch.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(batch[i], i);
    }
    EXPECT_EQ(q.size(), 5u);
}

TEST(BoundedQueueTest, CloseWakesWaiters) {
    kairos::core::BoundedQueue<int> q(4);

    std::atomic<bool> popped{false};
    std::thread t([&] {
        auto val = q.pop(std::chrono::milliseconds(5000));
        popped = true;
        EXPECT_FALSE(val.has_value());
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.close();
    t.join();
    EXPECT_TRUE(popped);
}

TEST(BoundedQueueTest, BackpressureOnFull) {
    kairos::core::BoundedQueue<int> q(2);
    q.push(1);
    q.push(2);

    // This should timeout because the queue is full.
    auto start = std::chrono::steady_clock::now();
    bool ok = q.push(3, std::chrono::milliseconds(50));
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(ok);
    EXPECT_GE(elapsed, std::chrono::milliseconds(40));
}

TEST(BoundedQueueTest, ConcurrentProducerConsumer) {
    kairos::core::BoundedQueue<int> q(16);
    constexpr int kCount = 1000;

    std::atomic<int64_t> sum_produced{0};
    std::atomic<int64_t> sum_consumed{0};

    // Producer thread.
    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i) {
            q.push(i);
            sum_produced += i;
        }
        q.close();
    });

    // Consumer thread.
    std::thread consumer([&] {
        while (true) {
            auto val = q.pop(std::chrono::milliseconds(100));
            if (!val.has_value()) {
                if (q.is_closed() && q.empty()) break;
                continue;
            }
            sum_consumed += *val;
        }
    });

    producer.join();
    consumer.join();
    EXPECT_EQ(sum_produced, sum_consumed);
}

TEST(BoundedQueueTest, SizeAndEmpty) {
    kairos::core::BoundedQueue<int> q(8);
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);

    q.push(1);
    EXPECT_FALSE(q.empty());
    EXPECT_EQ(q.size(), 1u);

    q.pop();
    EXPECT_TRUE(q.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// Exit code normalization tests
// ═══════════════════════════════════════════════════════════════════════════

#ifndef _WIN32

#include <sys/wait.h>

TEST(ExitCodeNormalize, NormalExit) {
    // Simulate a normal exit with code 0.
    // WIFEXITED(status) && WEXITSTATUS(status) == 0
    int status = 0;
    // On POSIX, the status for normal exit code N is N << 8.
    status = 0 << 8;  // exit(0)
    EXPECT_EQ(kairos::exec::normalize_posix_status(status), 0);

    status = 1 << 8;  // exit(1)
    EXPECT_EQ(kairos::exec::normalize_posix_status(status), 1);

    status = 42 << 8;  // exit(42)
    EXPECT_EQ(kairos::exec::normalize_posix_status(status), 42);
}

TEST(ExitCodeNormalize, SignalKill) {
    // WIFSIGNALED(status) with signal 9 (SIGKILL)
    // On POSIX, the status for a signal death is the signal number in
    // the low 7 bits (no core dump flag).
    int status = 9;  // SIGKILL
    EXPECT_EQ(kairos::exec::normalize_posix_status(status), 137);  // 128+9

    status = 11;  // SIGSEGV
    EXPECT_EQ(kairos::exec::normalize_posix_status(status), 139);  // 128+11

    status = 15;  // SIGTERM
    EXPECT_EQ(kairos::exec::normalize_posix_status(status), 143);  // 128+15
}

#endif  // !_WIN32

// ═══════════════════════════════════════════════════════════════════════════
// ProcessHandle tests (POSIX — real process execution)
// ═══════════════════════════════════════════════════════════════════════════

#ifndef _WIN32

using namespace kairos::exec;

class ProcessHandleTest : public ::testing::Test {
protected:
    std::unique_ptr<ProcessHandle> proc = create_process_handle();
    std::stop_source stop_source;
};

TEST_F(ProcessHandleTest, EchoCommand) {
    ProcessSpec spec;
    spec.command_line = "echo hello world";
    spec.use_shell = true;

    ASSERT_TRUE(proc->spawn(spec));
    auto result = proc->wait(stop_source.get_token());

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.termination,
              ProcessResult::TerminationKind::Normal);
    EXPECT_TRUE(result.success());

    // stdout should contain "hello world\n"
    EXPECT_NE(result.stdout_data.find("hello world"), std::string::npos);
    EXPECT_TRUE(result.stderr_data.empty());
}

TEST_F(ProcessHandleTest, StderrCapture) {
    ProcessSpec spec;
    spec.command_line = "echo error >&2";
    spec.use_shell = true;

    ASSERT_TRUE(proc->spawn(spec));
    auto result = proc->wait(stop_source.get_token());

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.stderr_data.find("error"), std::string::npos);
}

TEST_F(ProcessHandleTest, NonZeroExitCode) {
    ProcessSpec spec;
    spec.command_line = "exit 42";
    spec.use_shell = true;

    ASSERT_TRUE(proc->spawn(spec));
    auto result = proc->wait(stop_source.get_token());

    EXPECT_EQ(result.exit_code, 42);
    EXPECT_FALSE(result.success());
}

TEST_F(ProcessHandleTest, CommandNotFound) {
    ProcessSpec spec;
    spec.command_line = "nonexistent_command_xyz_12345";
    spec.use_shell = true;

    ASSERT_TRUE(proc->spawn(spec));
    auto result = proc->wait(stop_source.get_token());

    EXPECT_EQ(result.exit_code, 127);
}

TEST_F(ProcessHandleTest, Timeout) {
    ProcessSpec spec;
    spec.command_line = "sleep 60";
    spec.use_shell = true;
    spec.timeout = std::chrono::seconds(1);
    spec.kill_timeout = std::chrono::seconds(1);

    ASSERT_TRUE(proc->spawn(spec));
    auto result = proc->wait(stop_source.get_token());

    EXPECT_TRUE(result.timed_out());
    EXPECT_TRUE(result.exit_code == 200 || result.exit_code == 201);
    EXPECT_GE(result.duration.count(), 900);  // At least ~1s.
}

TEST_F(ProcessHandleTest, Cancellation) {
    ProcessSpec spec;
    spec.command_line = "sleep 60";
    spec.use_shell = true;

    ASSERT_TRUE(proc->spawn(spec));

    // Cancel after a brief delay.
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        stop_source.request_stop();
    });

    auto result = proc->wait(stop_source.get_token());
    canceller.join();

    EXPECT_EQ(result.termination,
              ProcessResult::TerminationKind::Cancelled);
}

TEST_F(ProcessHandleTest, WorkingDirectory) {
    ProcessSpec spec;
    spec.command_line = "pwd";
    spec.use_shell = true;
    spec.working_dir = "/tmp";

    ASSERT_TRUE(proc->spawn(spec));
    auto result = proc->wait(stop_source.get_token());

    EXPECT_EQ(result.exit_code, 0);
    // On macOS, /tmp might be a symlink to /private/tmp.
    EXPECT_TRUE(
        result.stdout_data.find("/tmp") != std::string::npos ||
        result.stdout_data.find("/private/tmp") != std::string::npos);
}

TEST_F(ProcessHandleTest, InvalidWorkingDirectory) {
    ProcessSpec spec;
    spec.command_line = "echo test";
    spec.use_shell = true;
    spec.working_dir = "/nonexistent/path/xyz";

    EXPECT_FALSE(proc->spawn(spec));
    EXPECT_EQ(proc->result().exit_code, 202);
    EXPECT_EQ(proc->result().termination,
              ProcessResult::TerminationKind::SpawnFailed);
}

TEST_F(ProcessHandleTest, EnvironmentVariables) {
    ProcessSpec spec;
    spec.command_line = "echo $MY_TEST_VAR";
    spec.use_shell = true;
    spec.environment = {
        {"MY_TEST_VAR", "kairos_test_value"},
        {"PATH", "/usr/bin:/bin"},
    };

    ASSERT_TRUE(proc->spawn(spec));
    auto result = proc->wait(stop_source.get_token());

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.stdout_data.find("kairos_test_value"),
              std::string::npos);
}

TEST_F(ProcessHandleTest, OutputCallback) {
    ProcessSpec spec;
    spec.command_line = R"(echo line1 && echo line2 && echo err >&2)";
    spec.use_shell = true;

    std::vector<std::string> stdout_chunks;
    std::vector<std::string> stderr_chunks;
    std::mutex mtx;

    proc->set_output_callback(
        [&](std::string_view chunk, bool is_stderr) {
            if (chunk.empty()) return;
            std::lock_guard lock(mtx);
            if (is_stderr) {
                stderr_chunks.emplace_back(chunk);
            } else {
                stdout_chunks.emplace_back(chunk);
            }
        });

    ASSERT_TRUE(proc->spawn(spec));
    auto result = proc->wait(stop_source.get_token());

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_FALSE(stdout_chunks.empty());
    EXPECT_FALSE(stderr_chunks.empty());
}

TEST_F(ProcessHandleTest, DurationMeasured) {
    ProcessSpec spec;
    spec.command_line = "sleep 0.1";
    spec.use_shell = true;

    ASSERT_TRUE(proc->spawn(spec));
    auto result = proc->wait(stop_source.get_token());

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_GE(result.duration.count(), 50);  // At least ~100ms (some slack).
}

TEST_F(ProcessHandleTest, PidIsValid) {
    ProcessSpec spec;
    spec.command_line = "sleep 0.1";
    spec.use_shell = true;

    ASSERT_TRUE(proc->spawn(spec));
    EXPECT_GT(proc->pid(), 0);

    proc->wait(stop_source.get_token());
}

TEST_F(ProcessHandleTest, EmptyCommand) {
    ProcessSpec spec;
    spec.command_line = "";
    spec.use_shell = false;
    spec.args = {};

    EXPECT_FALSE(proc->spawn(spec));
    EXPECT_EQ(proc->result().exit_code, 202);
}

TEST_F(ProcessHandleTest, MultiLineOutput) {
    ProcessSpec spec;
    spec.command_line = "for i in 1 2 3 4 5; do echo line$i; done";
    spec.use_shell = true;

    ASSERT_TRUE(proc->spawn(spec));
    auto result = proc->wait(stop_source.get_token());

    EXPECT_EQ(result.exit_code, 0);
    for (int i = 1; i <= 5; ++i) {
        EXPECT_NE(result.stdout_data.find(
            "line" + std::to_string(i)), std::string::npos);
    }
}

#endif  // !_WIN32
