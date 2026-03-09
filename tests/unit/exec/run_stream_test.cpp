/// tests/unit/exec/run_stream_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  RunStream tests — per-run pub-sub for live log following               ║
// ║  Spec reference: §14.7                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/exec/output_sink.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace kairos::exec {
namespace {

TEST(RunStreamTest, SubscribeAndPublish) {
    RunStream stream;

    std::string captured_run, captured_job, captured_step, captured_chunk;
    bool captured_stderr = false;

    stream.subscribe("run-1", [&](const std::string& run_id,
                                   const std::string& job_id,
                                   const std::string& step_id,
                                   std::string_view chunk,
                                   bool is_stderr) {
        captured_run = run_id;
        captured_job = job_id;
        captured_step = step_id;
        captured_chunk = std::string(chunk);
        captured_stderr = is_stderr;
    });

    stream.publish("run-1", "build", "step-1", "compiling...", false);

    EXPECT_EQ(captured_run, "run-1");
    EXPECT_EQ(captured_job, "build");
    EXPECT_EQ(captured_step, "step-1");
    EXPECT_EQ(captured_chunk, "compiling...");
    EXPECT_FALSE(captured_stderr);
}

TEST(RunStreamTest, PublishToWrongRunNoDelivery) {
    RunStream stream;

    int count = 0;
    stream.subscribe("run-1", [&](auto&&, auto&&, auto&&, auto&&, bool) {
        ++count;
    });

    stream.publish("run-2", "build", "step-1", "data", false);

    EXPECT_EQ(count, 0);
}

TEST(RunStreamTest, MultipleSubscribers) {
    RunStream stream;

    int count = 0;
    stream.subscribe("run-1", [&](auto&&...) { ++count; });
    stream.subscribe("run-1", [&](auto&&...) { ++count; });

    stream.publish("run-1", "j", "s", "data", false);

    EXPECT_EQ(count, 2);
    EXPECT_EQ(stream.subscriber_count("run-1"), 2u);
}

TEST(RunStreamTest, Unsubscribe) {
    RunStream stream;

    int count = 0;
    auto id = stream.subscribe("run-1", [&](auto&&...) { ++count; });

    stream.publish("run-1", "j", "s", "first", false);
    EXPECT_EQ(count, 1);

    stream.unsubscribe(id);
    stream.publish("run-1", "j", "s", "second", false);
    EXPECT_EQ(count, 1);  // No more.
}

TEST(RunStreamTest, CloseRunRemovesAllSubscribers) {
    RunStream stream;

    int count = 0;
    stream.subscribe("run-1", [&](auto&&...) { ++count; });
    stream.subscribe("run-1", [&](auto&&...) { ++count; });

    EXPECT_EQ(stream.active_run_count(), 1u);
    EXPECT_EQ(stream.subscriber_count("run-1"), 2u);

    stream.close_run("run-1");

    EXPECT_EQ(stream.active_run_count(), 0u);
    EXPECT_EQ(stream.subscriber_count("run-1"), 0u);

    // Publishing after close_run does nothing.
    stream.publish("run-1", "j", "s", "late", false);
    EXPECT_EQ(count, 0);
}

TEST(RunStreamTest, EmptyChunkIgnored) {
    RunStream stream;

    int count = 0;
    stream.subscribe("run-1", [&](auto&&...) { ++count; });

    stream.publish("run-1", "j", "s", "", false);
    EXPECT_EQ(count, 0);
}

TEST(RunStreamTest, TotalSubscriberCount) {
    RunStream stream;

    stream.subscribe("run-1", [](auto&&...) {});
    stream.subscribe("run-1", [](auto&&...) {});
    stream.subscribe("run-2", [](auto&&...) {});

    EXPECT_EQ(stream.total_subscriber_count(), 3u);
    EXPECT_EQ(stream.active_run_count(), 2u);
}

TEST(RunStreamTest, ConcurrentPublish) {
    RunStream stream;

    std::atomic<int> total{0};
    stream.subscribe("run-1", [&](auto&&...) {
        total.fetch_add(1);
    });

    constexpr int N = 100;
    std::vector<std::jthread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < N; ++j) {
                stream.publish("run-1", "j", "s", "data", false);
            }
        });
    }
    threads.clear();  // Join.

    EXPECT_EQ(total.load(), 4 * N);
}

}  // namespace
}  // namespace kairos::exec
