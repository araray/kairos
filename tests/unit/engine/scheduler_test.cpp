/// tests/unit/engine/scheduler_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Scheduler engine component tests                                        ║
// ║  Uses FakeClock for deterministic, wall-clock-independent testing.        ║
// ║  Spec reference: §30.3                                                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/scheduler.hpp"
#include "kairos/engine/trigger_event.hpp"
#include "kairos/engine/trigger_types.hpp"
#include "kairos/engine/workflow_registry.hpp"
#include "kairos/testing/fake_clock.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stop_token>
#include <thread>
#include <vector>

using namespace kairos::engine;
using namespace std::chrono_literals;

// ── Test fixture ────────────────────────────────────────────────────────

class SchedulerTest : public ::testing::Test {
protected:
    void SetUp() override {
        clock_.set_now(std::chrono::system_clock::time_point{
            std::chrono::hours(24 * 365 * 56)});  // ~2026
        clock_.set_steady(std::chrono::steady_clock::time_point{
            std::chrono::hours(1)});  // Arbitrary start
    }

    /// Build a registry with the given triggers.
    std::shared_ptr<const WorkflowRegistry> make_registry(
        std::vector<TimerEntry> triggers)
    {
        return std::make_shared<WorkflowRegistry>(
            std::vector<WorkflowDef>{},
            std::move(triggers),
            std::vector<JobDef>{});
    }

    /// Make an interval trigger entry.
    TimerEntry make_interval_trigger(
        const std::string& id, const std::string& target,
        std::chrono::milliseconds interval)
    {
        TimerEntry entry;
        entry.trigger_id = id;
        entry.target_id = target;
        entry.target_name = target;
        entry.target_kind = TriggerEvent::TargetKind::Workflow;
        entry.spec = IntervalTrigger{.interval = interval};
        entry.enabled = true;
        entry.max_instances = 1;
        return entry;
    }

    /// Make a date trigger entry.
    TimerEntry make_date_trigger(
        const std::string& id, const std::string& target,
        std::chrono::system_clock::time_point fire_at)
    {
        TimerEntry entry;
        entry.trigger_id = id;
        entry.target_id = target;
        entry.target_name = target;
        entry.target_kind = TriggerEvent::TargetKind::Workflow;
        entry.spec = DateTrigger{.fire_at = fire_at};
        entry.enabled = true;
        entry.max_instances = 1;
        return entry;
    }

    kairos::testing::FakeClock clock_;
    ActiveRunTracker active_runs_;
};

// ── Tests ───────────────────────────────────────────────────────────────

TEST_F(SchedulerTest, IntervalTriggerFiresAfterInterval) {
    auto registry = make_registry({
        make_interval_trigger("trg-001", "wfl-test", 5000ms),
    });

    std::stop_source stop;
    std::vector<TriggerEvent> fired;
    std::mutex mu;

    Scheduler sched(SchedulerConfig{}, Scheduler::Dependencies{
        .clock = &clock_,
        .registry = registry,
        .active_runs = &active_runs_,
    });

    sched.start(stop.get_token(), [&](TriggerEvent evt) {
        std::lock_guard lock(mu);
        fired.push_back(std::move(evt));
        return true;
    });

    std::this_thread::sleep_for(50ms);
    clock_.advance(6000ms);
    std::this_thread::sleep_for(50ms);

    stop.request_stop();
    clock_.wake();
    sched.stop();

    std::lock_guard lock(mu);
    ASSERT_GE(fired.size(), 1u);
    EXPECT_EQ(fired[0].type, TriggerType::ScheduleTick);
    EXPECT_EQ(fired[0].target_id, "wfl-test");
    EXPECT_EQ(fired[0].trigger_id, "trg-001");
}

TEST_F(SchedulerTest, IntervalTriggerFiresMultipleTimes) {
    auto registry = make_registry({
        make_interval_trigger("trg-001", "wfl-test", 1000ms),
    });

    std::stop_source stop;
    std::vector<TriggerEvent> fired;
    std::mutex mu;

    Scheduler sched(SchedulerConfig{}, Scheduler::Dependencies{
        .clock = &clock_,
        .registry = registry,
        .active_runs = &active_runs_,
    });

    sched.start(stop.get_token(), [&](TriggerEvent evt) {
        std::lock_guard lock(mu);
        fired.push_back(std::move(evt));
        return true;
    });

    std::this_thread::sleep_for(50ms);

    for (int i = 0; i < 3; ++i) {
        clock_.advance(1100ms);
        std::this_thread::sleep_for(50ms);
    }

    stop.request_stop();
    clock_.wake();
    sched.stop();

    std::lock_guard lock(mu);
    EXPECT_GE(fired.size(), 3u);
}

TEST_F(SchedulerTest, DateTriggerFiresOnceAndExhausts) {
    auto fire_at = clock_.now() + std::chrono::milliseconds(2000);
    std::vector<TimerEntry> triggers;
    triggers.push_back(
        make_date_trigger("trg-date-001", "wfl-once", fire_at));
    auto registry = make_registry(std::move(triggers));

    std::stop_source stop;
    std::vector<TriggerEvent> fired;
    std::mutex mu;

    Scheduler sched(SchedulerConfig{}, Scheduler::Dependencies{
        .clock = &clock_,
        .registry = registry,
        .active_runs = &active_runs_,
    });

    sched.start(stop.get_token(), [&](TriggerEvent evt) {
        std::lock_guard lock(mu);
        fired.push_back(std::move(evt));
        return true;
    });

    std::this_thread::sleep_for(50ms);
    clock_.advance(3000ms);
    std::this_thread::sleep_for(50ms);
    clock_.advance(3000ms);
    std::this_thread::sleep_for(50ms);

    stop.request_stop();
    clock_.wake();
    sched.stop();

    std::lock_guard lock(mu);
    EXPECT_EQ(fired.size(), 1u);
    EXPECT_EQ(fired[0].target_id, "wfl-once");
}

TEST_F(SchedulerTest, DisabledTriggerDoesNotFire) {
    auto trigger = make_interval_trigger("trg-disabled", "wfl-disabled", 1000ms);
    trigger.enabled = false;

    std::vector<TimerEntry> triggers;
    triggers.push_back(std::move(trigger));
    auto registry = make_registry(std::move(triggers));

    std::stop_source stop;
    std::vector<TriggerEvent> fired;
    std::mutex mu;

    Scheduler sched(SchedulerConfig{}, Scheduler::Dependencies{
        .clock = &clock_,
        .registry = registry,
    });

    sched.start(stop.get_token(), [&](TriggerEvent evt) {
        std::lock_guard lock(mu);
        fired.push_back(std::move(evt));
        return true;
    });

    std::this_thread::sleep_for(50ms);
    clock_.advance(5000ms);
    std::this_thread::sleep_for(50ms);

    stop.request_stop();
    clock_.wake();
    sched.stop();

    std::lock_guard lock(mu);
    EXPECT_TRUE(fired.empty());
}

TEST_F(SchedulerTest, MaxInstancesBlocksFiring) {
    auto registry = make_registry({
        make_interval_trigger("trg-max", "wfl-busy", 1000ms),
    });

    active_runs_.increment("wfl-busy");

    std::stop_source stop;
    std::vector<TriggerEvent> fired;
    std::mutex mu;

    Scheduler sched(SchedulerConfig{}, Scheduler::Dependencies{
        .clock = &clock_,
        .registry = registry,
        .active_runs = &active_runs_,
    });

    sched.start(stop.get_token(), [&](TriggerEvent evt) {
        std::lock_guard lock(mu);
        fired.push_back(std::move(evt));
        return true;
    });

    std::this_thread::sleep_for(50ms);
    clock_.advance(2000ms);
    std::this_thread::sleep_for(50ms);

    stop.request_stop();
    clock_.wake();
    sched.stop();

    std::lock_guard lock(mu);
    EXPECT_TRUE(fired.empty());
}

TEST_F(SchedulerTest, ConfigReloadRebuildsTriggers) {
    auto registry1 = make_registry({
        make_interval_trigger("trg-A", "wfl-A", 10000ms),
    });

    std::stop_source stop;
    std::vector<TriggerEvent> fired;
    std::mutex mu;

    Scheduler sched(SchedulerConfig{}, Scheduler::Dependencies{
        .clock = &clock_,
        .registry = registry1,
        .active_runs = &active_runs_,
    });

    sched.start(stop.get_token(), [&](TriggerEvent evt) {
        std::lock_guard lock(mu);
        fired.push_back(std::move(evt));
        return true;
    });

    std::this_thread::sleep_for(50ms);

    auto registry2 = make_registry({
        make_interval_trigger("trg-B", "wfl-B", 500ms),
    });
    sched.request_reload(registry2);
    std::this_thread::sleep_for(50ms);

    clock_.advance(600ms);
    std::this_thread::sleep_for(50ms);

    stop.request_stop();
    clock_.wake();
    sched.stop();

    std::lock_guard lock(mu);
    ASSERT_GE(fired.size(), 1u);
    EXPECT_EQ(fired[0].target_id, "wfl-B");
}

TEST_F(SchedulerTest, EmptyRegistryIdlesWithoutCrash) {
    auto registry = make_registry({});

    std::stop_source stop;
    std::vector<TriggerEvent> fired;

    Scheduler sched(SchedulerConfig{}, Scheduler::Dependencies{
        .clock = &clock_,
        .registry = registry,
    });

    sched.start(stop.get_token(), [&](TriggerEvent evt) {
        fired.push_back(std::move(evt));
        return true;
    });

    std::this_thread::sleep_for(100ms);
    clock_.advance(10000ms);
    std::this_thread::sleep_for(50ms);

    stop.request_stop();
    clock_.wake();
    sched.stop();

    EXPECT_TRUE(fired.empty());
    EXPECT_EQ(sched.timer_count(), 0u);
}

TEST_F(SchedulerTest, NullClockThrowsOnConstruction) {
    EXPECT_THROW(
        Scheduler(SchedulerConfig{}, Scheduler::Dependencies{
            .clock = nullptr,
        }),
        std::invalid_argument);
}

TEST_F(SchedulerTest, TriggerEventHasCorrectFields) {
    auto registry = make_registry({
        make_interval_trigger("trg-fields", "wfl-fields", 1000ms),
    });

    std::stop_source stop;
    std::vector<TriggerEvent> fired;
    std::mutex mu;

    Scheduler sched(SchedulerConfig{}, Scheduler::Dependencies{
        .clock = &clock_,
        .registry = registry,
    });

    sched.start(stop.get_token(), [&](TriggerEvent evt) {
        std::lock_guard lock(mu);
        fired.push_back(std::move(evt));
        return true;
    });

    std::this_thread::sleep_for(50ms);
    clock_.advance(1100ms);
    std::this_thread::sleep_for(50ms);

    stop.request_stop();
    clock_.wake();
    sched.stop();

    std::lock_guard lock(mu);
    ASSERT_GE(fired.size(), 1u);

    const auto& evt = fired[0];
    EXPECT_EQ(evt.type, TriggerType::ScheduleTick);
    EXPECT_EQ(evt.trigger_id, "trg-fields");
    EXPECT_EQ(evt.target_id, "wfl-fields");
    EXPECT_FALSE(evt.correlation_id.empty());

    auto* payload = std::get_if<ScheduleTickPayload>(&evt.payload);
    ASSERT_NE(payload, nullptr);
    EXPECT_EQ(payload->trigger_type, "interval");
    EXPECT_FALSE(payload->is_misfire);
}
