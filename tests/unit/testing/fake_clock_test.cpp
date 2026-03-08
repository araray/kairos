/// tests/unit/testing/fake_clock_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  fake_clock_test.cpp — Tests for ClockSource, FakeClock,                 ║
// ║  and SystemClockSource.                                                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/testing/fake_clock.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

using namespace kairos;
using namespace kairos::testing;

// ── FakeClock basic operations ──────────────────────────────────────────

TEST(FakeClockTest, InitialTimeIsEpoch) {
    FakeClock clock;
    auto wall = clock.now();
    auto steady = clock.steady_now();
    EXPECT_EQ(wall.time_since_epoch().count(), 0);
    EXPECT_EQ(steady.time_since_epoch().count(), 0);
}

TEST(FakeClockTest, SetNow) {
    FakeClock clock;
    auto t = std::chrono::system_clock::time_point{} +
             std::chrono::hours(24);
    clock.set_now(t);
    EXPECT_EQ(clock.now(), t);
}

TEST(FakeClockTest, SetSteady) {
    FakeClock clock;
    auto t = std::chrono::steady_clock::time_point{} +
             std::chrono::seconds(100);
    clock.set_steady(t);
    EXPECT_EQ(clock.steady_now(), t);
}

TEST(FakeClockTest, AdvancesBothClocks) {
    FakeClock clock;
    auto wall_0 = clock.now();
    auto steady_0 = clock.steady_now();

    clock.advance(std::chrono::seconds(30));

    auto wall_30 = clock.now();
    auto steady_30 = clock.steady_now();

    auto wall_diff = std::chrono::duration_cast<std::chrono::seconds>(
        wall_30 - wall_0);
    auto steady_diff = std::chrono::duration_cast<std::chrono::seconds>(
        steady_30 - steady_0);

    EXPECT_EQ(wall_diff.count(), 30);
    EXPECT_EQ(steady_diff.count(), 30);
}

TEST(FakeClockTest, MultipleAdvances) {
    FakeClock clock;
    clock.advance(std::chrono::seconds(10));
    clock.advance(std::chrono::seconds(20));
    clock.advance(std::chrono::seconds(30));

    auto wall = clock.now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        wall.time_since_epoch());
    EXPECT_EQ(elapsed.count(), 60);
}

// ── FakeClock sleep_until ───────────────────────────────────────────────

TEST(FakeClockTest, SleepUntilReturnsWhenAdvancedPastTarget) {
    FakeClock clock;
    std::atomic<bool> done{false};

    auto target = clock.steady_now() + std::chrono::seconds(10);

    std::jthread t([&](std::stop_token) {
        bool reached = clock.sleep_until(target);
        EXPECT_TRUE(reached);
        done.store(true);
    });

    // Give the thread time to start sleeping.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(done.load());

    // Advance past target.
    clock.advance(std::chrono::seconds(15));

    // Wait for thread to finish.
    t.join();
    EXPECT_TRUE(done.load());
}

TEST(FakeClockTest, SleepUntilReturnsOnWake) {
    FakeClock clock;
    std::atomic<bool> done{false};

    auto target = clock.steady_now() + std::chrono::hours(1);

    std::jthread t([&](std::stop_token) {
        bool reached = clock.sleep_until(target);
        EXPECT_FALSE(reached);  // Woken, not reached.
        done.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(done.load());

    clock.wake();
    t.join();
    EXPECT_TRUE(done.load());
}

TEST(FakeClockTest, SleepUntilAlreadyPastReturnImmediately) {
    FakeClock clock;
    clock.advance(std::chrono::seconds(100));

    auto target = clock.steady_now() - std::chrono::seconds(10);
    bool reached = clock.sleep_until(target);
    EXPECT_TRUE(reached);
}

// ── SystemClockSource basic ─────────────────────────────────────────────

TEST(SystemClockSourceTest, NowReturnsReasonableTime) {
    SystemClockSource clock;
    auto now = clock.now();
    // Should be after 2020-01-01.
    auto epoch_2020 = std::chrono::system_clock::from_time_t(1577836800);
    EXPECT_GT(now, epoch_2020);
}

TEST(SystemClockSourceTest, SteadyNowIsMonotonic) {
    SystemClockSource clock;
    auto t1 = clock.steady_now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto t2 = clock.steady_now();
    EXPECT_GT(t2, t1);
}

TEST(SystemClockSourceTest, WakeUnblocksSleepUntil) {
    SystemClockSource clock;
    std::atomic<bool> done{false};

    auto target = clock.steady_now() + std::chrono::seconds(60);

    std::jthread t([&](std::stop_token) {
        bool reached = clock.sleep_until(target);
        EXPECT_FALSE(reached);  // Woken early.
        done.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(done.load());

    clock.wake();
    t.join();
    EXPECT_TRUE(done.load());
}

// ── ClockSource polymorphism ────────────────────────────────────────────

TEST(ClockSourceTest, PolymorphicUsage) {
    FakeClock fake;
    SystemClockSource sys;

    // Both satisfy ClockSource interface.
    ClockSource* clock = &fake;
    auto _ = clock->now();      (void)_;
    auto __ = clock->steady_now(); (void)__;

    clock = &sys;
    auto ___ = clock->now(); (void)___;
    auto ____ = clock->steady_now(); (void)____;
}
