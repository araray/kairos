/// tests/unit/exec/output_sink_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  OutputMultiplexer tests — fan-out delivery, secret masking, lifecycle   ║
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

// ── Basic delivery ────────────────────────────────────────────────────────

TEST(OutputMultiplexerTest, DeliverToSingleSink) {
    OutputMultiplexer mux;

    std::string captured;
    bool was_stderr = false;

    mux.add_sink([&](std::string_view chunk, bool is_stderr) {
        captured = std::string(chunk);
        was_stderr = is_stderr;
    });

    mux.deliver("hello world", false);

    EXPECT_EQ(captured, "hello world");
    EXPECT_FALSE(was_stderr);
}

TEST(OutputMultiplexerTest, DeliverToMultipleSinks) {
    OutputMultiplexer mux;

    int call_count = 0;
    std::vector<std::string> chunks;

    mux.add_sink([&](std::string_view chunk, bool) {
        ++call_count;
        chunks.emplace_back(chunk);
    });
    mux.add_sink([&](std::string_view chunk, bool) {
        ++call_count;
        chunks.emplace_back(chunk);
    });

    mux.deliver("test", false);

    EXPECT_EQ(call_count, 2);
    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[0], "test");
    EXPECT_EQ(chunks[1], "test");
}

TEST(OutputMultiplexerTest, RemoveSinkStopsDelivery) {
    OutputMultiplexer mux;

    int count = 0;
    auto handle = mux.add_sink([&](std::string_view, bool) { ++count; });

    mux.deliver("first", false);
    EXPECT_EQ(count, 1);

    mux.remove_sink(handle);
    mux.deliver("second", false);
    EXPECT_EQ(count, 1);  // No more deliveries.
}

TEST(OutputMultiplexerTest, EmptyChunkIgnored) {
    OutputMultiplexer mux;

    int count = 0;
    mux.add_sink([&](std::string_view, bool) { ++count; });

    mux.deliver("", false);
    EXPECT_EQ(count, 0);
}

// ── Secret masking ────────────────────────────────────────────────────────

TEST(OutputMultiplexerTest, SecretMasking) {
    OutputMultiplexer mux;
    mux.set_secret_values({"PASSWORD123", "api_key_xyz"});

    std::string captured;
    mux.add_sink([&](std::string_view chunk, bool) {
        captured = std::string(chunk);
    });

    mux.deliver("Connect with PASSWORD123 and api_key_xyz", false);

    EXPECT_EQ(captured, "Connect with *** and ***");
    EXPECT_EQ(std::string::npos, captured.find("PASSWORD123"));
    EXPECT_EQ(std::string::npos, captured.find("api_key_xyz"));
}

TEST(OutputMultiplexerTest, NoSecretsNoMasking) {
    OutputMultiplexer mux;

    std::string captured;
    mux.add_sink([&](std::string_view chunk, bool) {
        captured = std::string(chunk);
    });

    mux.deliver("plain text output", false);

    EXPECT_EQ(captured, "plain text output");
}

TEST(OutputMultiplexerTest, EmptySecretValuesIgnored) {
    OutputMultiplexer mux;
    mux.set_secret_values({"", "real_secret", ""});

    std::string captured;
    mux.add_sink([&](std::string_view chunk, bool) {
        captured = std::string(chunk);
    });

    mux.deliver("has real_secret in it", false);

    EXPECT_EQ(captured, "has *** in it");
    EXPECT_EQ(mux.sink_count(), 1u);
}

// ── Concurrency ───────────────────────────────────────────────────────────

TEST(OutputMultiplexerTest, ConcurrentDelivery) {
    OutputMultiplexer mux;

    std::atomic<int> total_chunks{0};
    mux.add_sink([&](std::string_view, bool) {
        total_chunks.fetch_add(1);
    });

    constexpr int N = 100;
    std::vector<std::jthread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < N; ++j) {
                mux.deliver("chunk", false);
            }
        });
    }
    threads.clear();  // Join all.

    EXPECT_EQ(total_chunks.load(), 4 * N);
}

}  // namespace
}  // namespace kairos::exec
