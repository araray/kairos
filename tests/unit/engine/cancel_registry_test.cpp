/// tests/unit/engine/cancel_registry_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  CancelRegistry unit tests (§23.10)                                     ║
// ║                                                                          ║
// ║  Tests:                                                                  ║
// ║    1. register_run returns a stop_token that fires on cancel             ║
// ║    2. cancel returns false for unknown run_id                            ║
// ║    3. unregister_run cleans up                                           ║
// ║    4. cancel idempotent (double cancel is safe)                          ║
// ║    5. active_count tracks registrations                                  ║
// ║    6. CombinedStopToken fires on global stop                            ║
// ║    7. CombinedStopToken fires on per-run cancel                         ║
// ║    8. CombinedStopToken fires immediately if already stopped            ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/cancel_registry.hpp"

#include <gtest/gtest.h>

#include <thread>

namespace {

// ── CancelRegistry tests ─────────────────────────────────────────────────

TEST(CancelRegistryTest, RegisterAndCancel) {
    kairos::engine::CancelRegistry reg;

    auto token = reg.register_run("run-001");
    EXPECT_FALSE(token.stop_requested());

    bool cancelled = reg.cancel("run-001");
    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(token.stop_requested());
}

TEST(CancelRegistryTest, CancelUnknownRunReturnsFalse) {
    kairos::engine::CancelRegistry reg;

    bool cancelled = reg.cancel("nonexistent");
    EXPECT_FALSE(cancelled);
}

TEST(CancelRegistryTest, UnregisterRemovesRun) {
    kairos::engine::CancelRegistry reg;

    (void)reg.register_run("run-002");
    EXPECT_TRUE(reg.is_registered("run-002"));
    EXPECT_EQ(reg.active_count(), 1u);

    reg.unregister_run("run-002");
    EXPECT_FALSE(reg.is_registered("run-002"));
    EXPECT_EQ(reg.active_count(), 0u);

    // Cancel after unregister should return false.
    EXPECT_FALSE(reg.cancel("run-002"));
}

TEST(CancelRegistryTest, DoubleCancelIsIdempotent) {
    kairos::engine::CancelRegistry reg;

    auto token = reg.register_run("run-003");
    EXPECT_TRUE(reg.cancel("run-003"));
    EXPECT_TRUE(token.stop_requested());

    // Second cancel should still return true (it's registered and cancelled).
    EXPECT_TRUE(reg.cancel("run-003"));
}

TEST(CancelRegistryTest, ActiveCountTracksMultiple) {
    kairos::engine::CancelRegistry reg;

    EXPECT_EQ(reg.active_count(), 0u);

    (void)reg.register_run("run-a");
    (void)reg.register_run("run-b");
    (void)reg.register_run("run-c");
    EXPECT_EQ(reg.active_count(), 3u);

    reg.unregister_run("run-b");
    EXPECT_EQ(reg.active_count(), 2u);

    reg.unregister_run("run-a");
    reg.unregister_run("run-c");
    EXPECT_EQ(reg.active_count(), 0u);
}

TEST(CancelRegistryTest, UnregisterIdempotent) {
    kairos::engine::CancelRegistry reg;

    (void)reg.register_run("run-004");
    reg.unregister_run("run-004");
    // Second unregister should be a no-op.
    reg.unregister_run("run-004");
    EXPECT_EQ(reg.active_count(), 0u);
}

TEST(CancelRegistryTest, CancelTokenStopPossible) {
    kairos::engine::CancelRegistry reg;
    auto token = reg.register_run("run-005");
    EXPECT_TRUE(token.stop_possible());
}

TEST(CancelRegistryTest, ConcurrentRegisterCancel) {
    kairos::engine::CancelRegistry reg;

    // Register many runs, cancel them from multiple threads.
    constexpr int N = 100;
    std::vector<std::stop_token> tokens;
    tokens.reserve(N);

    for (int i = 0; i < N; ++i) {
        tokens.push_back(reg.register_run("run-" + std::to_string(i)));
    }

    std::vector<std::jthread> threads;
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&reg, i]() {
            reg.cancel("run-" + std::to_string(i));
        });
    }
    threads.clear();  // Join all.

    for (int i = 0; i < N; ++i) {
        EXPECT_TRUE(tokens[i].stop_requested());
        reg.unregister_run("run-" + std::to_string(i));
    }
    EXPECT_EQ(reg.active_count(), 0u);
}

// ── CombinedStopToken tests ─────────────────────────────────────────────

TEST(CombinedStopTokenTest, FiresOnGlobalStop) {
    std::stop_source global;
    std::stop_source per_run;

    kairos::engine::CombinedStopToken combined;
    combined.arm(global.get_token(), per_run.get_token());

    EXPECT_FALSE(combined.token().stop_requested());

    global.request_stop();
    EXPECT_TRUE(combined.token().stop_requested());
}

TEST(CombinedStopTokenTest, FiresOnPerRunCancel) {
    std::stop_source global;
    std::stop_source per_run;

    kairos::engine::CombinedStopToken combined;
    combined.arm(global.get_token(), per_run.get_token());

    EXPECT_FALSE(combined.token().stop_requested());

    per_run.request_stop();
    EXPECT_TRUE(combined.token().stop_requested());
}

TEST(CombinedStopTokenTest, FiresImmediatelyIfGlobalAlreadyStopped) {
    std::stop_source global;
    global.request_stop();  // Already stopped.

    std::stop_source per_run;

    kairos::engine::CombinedStopToken combined;
    combined.arm(global.get_token(), per_run.get_token());

    EXPECT_TRUE(combined.token().stop_requested());
}

TEST(CombinedStopTokenTest, FiresImmediatelyIfRunAlreadyCancelled) {
    std::stop_source global;
    std::stop_source per_run;
    per_run.request_stop();

    kairos::engine::CombinedStopToken combined;
    combined.arm(global.get_token(), per_run.get_token());

    EXPECT_TRUE(combined.token().stop_requested());
}

TEST(CombinedStopTokenTest, NeitherFiredRemainsNotStopped) {
    std::stop_source global;
    std::stop_source per_run;

    kairos::engine::CombinedStopToken combined;
    combined.arm(global.get_token(), per_run.get_token());

    EXPECT_FALSE(combined.token().stop_requested());
    // Neither source is stopped.
    EXPECT_FALSE(global.get_token().stop_requested());
    EXPECT_FALSE(per_run.get_token().stop_requested());
}

}  // anonymous namespace
