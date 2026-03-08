/// tests/unit/engine/trigger_event_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  trigger_event_test.cpp — Tests for TriggerEvent, TriggerType, and       ║
// ║  ActiveRunTracker.                                                        ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/trigger_event.hpp"
#include "kairos/engine/trigger_types.hpp"

#include <gtest/gtest.h>

using namespace kairos::engine;

// ── TriggerType string conversion ───────────────────────────────────────

TEST(TriggerTypeTest, ToStringSchedule) {
    EXPECT_EQ(trigger_type_to_string(TriggerType::ScheduleTick), "schedule");
}

TEST(TriggerTypeTest, ToStringWatch) {
    EXPECT_EQ(trigger_type_to_string(TriggerType::FileEvent), "watch");
    EXPECT_EQ(trigger_type_to_string(TriggerType::FileDiff), "watch");
}

TEST(TriggerTypeTest, ToStringManual) {
    EXPECT_EQ(trigger_type_to_string(TriggerType::ManualRun), "manual");
}

TEST(TriggerTypeTest, ToStringConfigReload) {
    EXPECT_EQ(trigger_type_to_string(TriggerType::ConfigReload),
              "config_reload");
}

TEST(TriggerTypeTest, ParseSchedule) {
    EXPECT_EQ(string_to_trigger_type("schedule"), TriggerType::ScheduleTick);
}

TEST(TriggerTypeTest, ParseWatch) {
    EXPECT_EQ(string_to_trigger_type("watch"), TriggerType::FileEvent);
}

TEST(TriggerTypeTest, ParseManual) {
    EXPECT_EQ(string_to_trigger_type("manual"), TriggerType::ManualRun);
}

TEST(TriggerTypeTest, ParseUnknownDefaultsToManual) {
    EXPECT_EQ(string_to_trigger_type("nonsense"), TriggerType::ManualRun);
}

// ── TriggerEvent factory methods ────────────────────────────────────────

TEST(TriggerEventTest, MakeScheduleTick) {
    auto evt = TriggerEvent::make_schedule_tick(
        "trg-abc123", "wfl-def456",
        TriggerEvent::TargetKind::Workflow,
        std::chrono::system_clock::now(),
        "corr-123",
        ScheduleTickPayload{.trigger_type = "cron",
                            .expression = "0 2 * * *"});

    EXPECT_EQ(evt.type, TriggerType::ScheduleTick);
    EXPECT_EQ(evt.trigger_id, "trg-abc123");
    EXPECT_EQ(evt.target_id, "wfl-def456");
    EXPECT_EQ(evt.target_kind, TriggerEvent::TargetKind::Workflow);
    EXPECT_EQ(evt.correlation_id, "corr-123");

    auto& payload = std::get<ScheduleTickPayload>(evt.payload);
    EXPECT_EQ(payload.trigger_type, "cron");
    EXPECT_EQ(payload.expression, "0 2 * * *");
    EXPECT_FALSE(payload.is_misfire);
}

TEST(TriggerEventTest, MakeManualRun) {
    auto evt = TriggerEvent::make_manual_run(
        "wfl-abc123", TriggerEvent::TargetKind::Workflow,
        "corr-456", "cli");

    EXPECT_EQ(evt.type, TriggerType::ManualRun);
    EXPECT_EQ(evt.target_id, "wfl-abc123");
    EXPECT_EQ(evt.trigger_id, "cli");
    EXPECT_EQ(evt.correlation_id, "corr-456");

    auto& payload = std::get<ManualRunPayload>(evt.payload);
    EXPECT_EQ(payload.invoked_by, "cli");
}

TEST(TriggerEventTest, MakeFileEvent) {
    auto evt = TriggerEvent::make_file_event(
        "wgr-logs", "wfl-process",
        TriggerEvent::TargetKind::Workflow,
        "corr-789",
        FileEventPayload{
            .watch_group = "logs",
            .changed_paths = {"/var/log/app.log"},
            .event_type = "content_modified"});

    EXPECT_EQ(evt.type, TriggerType::FileEvent);
    auto& payload = std::get<FileEventPayload>(evt.payload);
    EXPECT_EQ(payload.watch_group, "logs");
    EXPECT_EQ(payload.changed_paths.size(), 1u);
}

TEST(TriggerEventTest, MakeConfigReload) {
    auto evt = TriggerEvent::make_config_reload("signal", "corr-reload");
    EXPECT_EQ(evt.type, TriggerType::ConfigReload);

    auto& payload = std::get<ConfigReloadPayload>(evt.payload);
    EXPECT_EQ(payload.source, "signal");
}

// ── TriggerEvent can be moved through BoundedQueue ──────────────────────

TEST(TriggerEventTest, ThroughBoundedQueue) {
    kairos::core::BoundedQueue<TriggerEvent> bus(16);

    auto evt = TriggerEvent::make_manual_run(
        "job-test", TriggerEvent::TargetKind::StandaloneJob,
        "corr-q", "mcp");

    EXPECT_TRUE(bus.try_push(std::move(evt)));
    EXPECT_EQ(bus.size(), 1u);

    auto popped = bus.try_pop();
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(popped->target_id, "job-test");
    EXPECT_EQ(popped->correlation_id, "corr-q");
}

// ── ActiveRunTracker ────────────────────────────────────────────────────

TEST(ActiveRunTrackerTest, IncrementAndDecrement) {
    ActiveRunTracker tracker;
    EXPECT_EQ(tracker.count("job-a"), 0);

    tracker.increment("job-a");
    EXPECT_EQ(tracker.count("job-a"), 1);

    tracker.increment("job-a");
    EXPECT_EQ(tracker.count("job-a"), 2);

    tracker.decrement("job-a");
    EXPECT_EQ(tracker.count("job-a"), 1);

    tracker.decrement("job-a");
    EXPECT_EQ(tracker.count("job-a"), 0);
}

TEST(ActiveRunTrackerTest, DecrementBelowZeroClampsToZero) {
    ActiveRunTracker tracker;
    EXPECT_EQ(tracker.decrement("job-x"), 0);
    EXPECT_EQ(tracker.count("job-x"), 0);
}

TEST(ActiveRunTrackerTest, CanFire) {
    ActiveRunTracker tracker;
    EXPECT_TRUE(tracker.can_fire("job-a", 1));

    tracker.increment("job-a");
    EXPECT_FALSE(tracker.can_fire("job-a", 1));
    EXPECT_TRUE(tracker.can_fire("job-a", 2));
}

TEST(ActiveRunTrackerTest, Reset) {
    ActiveRunTracker tracker;
    tracker.increment("job-a");
    tracker.increment("job-b");
    tracker.reset();
    EXPECT_EQ(tracker.count("job-a"), 0);
    EXPECT_EQ(tracker.count("job-b"), 0);
}

TEST(ActiveRunTrackerTest, IndependentTargets) {
    ActiveRunTracker tracker;
    tracker.increment("job-a");
    tracker.increment("job-b");
    tracker.increment("job-b");

    EXPECT_EQ(tracker.count("job-a"), 1);
    EXPECT_EQ(tracker.count("job-b"), 2);
}

// ── IntervalTrigger ─────────────────────────────────────────────────────

TEST(IntervalTriggerTest, NextFireAfter) {
    IntervalTrigger trigger;
    trigger.interval = std::chrono::seconds(60);

    auto now = std::chrono::steady_clock::time_point{} +
               std::chrono::seconds(100);
    auto next = trigger.next_fire_after(now);

    auto diff = std::chrono::duration_cast<std::chrono::seconds>(next - now);
    EXPECT_EQ(diff.count(), 60);
}

// ── TriggerSpec variant ─────────────────────────────────────────────────

TEST(TriggerSpecTest, TypeName) {
    TriggerSpec cron_spec = CronTrigger{.expression = "0 * * * *"};
    TriggerSpec int_spec  = IntervalTrigger{.interval = std::chrono::seconds(30)};
    TriggerSpec date_spec = DateTrigger{};

    EXPECT_EQ(trigger_spec_type_name(cron_spec), "cron");
    EXPECT_EQ(trigger_spec_type_name(int_spec), "interval");
    EXPECT_EQ(trigger_spec_type_name(date_spec), "date");
}

// ── TimerEntry time_until_fire ──────────────────────────────────────────

TEST(TimerEntryTest, TimeUntilFireInterval) {
    TimerEntry entry;
    entry.spec = IntervalTrigger{.interval = std::chrono::seconds(60)};
    auto now_mono = std::chrono::steady_clock::time_point{} +
                    std::chrono::seconds(100);
    entry.next_fire_mono = now_mono + std::chrono::seconds(30);

    auto until = entry.time_until_fire(
        std::chrono::system_clock::now(), now_mono);
    EXPECT_EQ(until.count(), 30000);  // 30 seconds in ms
}

TEST(TimerEntryTest, TimeUntilFireOverdue) {
    TimerEntry entry;
    entry.spec = IntervalTrigger{.interval = std::chrono::seconds(60)};
    auto now_mono = std::chrono::steady_clock::time_point{} +
                    std::chrono::seconds(200);
    entry.next_fire_mono = now_mono - std::chrono::seconds(10);

    auto until = entry.time_until_fire(
        std::chrono::system_clock::now(), now_mono);
    EXPECT_LT(until.count(), 0);  // Overdue
}

// ── MisfirePolicy parsing ───────────────────────────────────────────────

TEST(MisfirePolicyTest, Parse) {
    EXPECT_EQ(parse_misfire_policy("coalesce"), MisfirePolicy::Coalesce);
    EXPECT_EQ(parse_misfire_policy("skip"), MisfirePolicy::Skip);
    EXPECT_EQ(parse_misfire_policy("run_all"), MisfirePolicy::RunAll);
    EXPECT_EQ(parse_misfire_policy("unknown"), MisfirePolicy::Coalesce);
}
