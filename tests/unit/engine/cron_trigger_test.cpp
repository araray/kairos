/// tests/unit/engine/cron_trigger_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for CronTrigger::next_fire_after() via croncpp integration         ║
// ║                                                                           ║
// ║  Validates cron expression parsing and next-fire-time computation         ║
// ║  for standard cron patterns, edge cases, and error handling.              ║
// ║                                                                           ║
// ║  Spec reference: §10.3.1                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/trigger_types.hpp"

#include <gtest/gtest.h>

#include <ctime>

namespace kairos::engine {
namespace {

/// Helper: make a system_clock time_point from components.
/// Uses local time (croncpp evaluates in local time per spec).
SystemTimePoint make_time(int year, int month, int day,
                           int hour, int min, int sec = 0)
{
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    tm.tm_isdst = -1;  // Let mktime determine DST.
    auto tt = std::mktime(&tm);
    return std::chrono::system_clock::from_time_t(tt);
}

/// Extract local time components from a time_point.
std::tm to_local_tm(SystemTimePoint tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &tt);
#else
    localtime_r(&tt, &local);
#endif
    return local;
}

// ═══════════════════════════════════════════════════════════════════════
//  BASIC CRON EXPRESSIONS
// ═══════════════════════════════════════════════════════════════════════

TEST(CronTriggerTest, EveryMinute) {
    CronTrigger ct{"* * * * *"};

    auto after = make_time(2026, 3, 7, 14, 30, 0);
    auto next = ct.next_fire_after(after);

    auto tm = to_local_tm(next);
    EXPECT_EQ(tm.tm_min, 31);
    EXPECT_EQ(tm.tm_hour, 14);
}

TEST(CronTriggerTest, EveryHour) {
    CronTrigger ct{"0 * * * *"};

    auto after = make_time(2026, 3, 7, 14, 30, 0);
    auto next = ct.next_fire_after(after);

    auto tm = to_local_tm(next);
    EXPECT_EQ(tm.tm_min, 0);
    EXPECT_EQ(tm.tm_hour, 15);
}

TEST(CronTriggerTest, DailyAt2AM) {
    CronTrigger ct{"0 2 * * *"};

    auto after = make_time(2026, 3, 7, 3, 0, 0);  // Already past 2 AM.
    auto next = ct.next_fire_after(after);

    auto tm = to_local_tm(next);
    EXPECT_EQ(tm.tm_hour, 2);
    EXPECT_EQ(tm.tm_min, 0);
    EXPECT_EQ(tm.tm_mday, 8);  // Next day.
}

TEST(CronTriggerTest, WeekdaysOnly) {
    // "0 9 * * 1-5" = 9:00 AM on weekdays.
    CronTrigger ct{"0 9 * * 1-5"};

    // Saturday March 7, 2026 at 10:00.
    auto after = make_time(2026, 3, 7, 10, 0, 0);
    auto next = ct.next_fire_after(after);

    auto tm = to_local_tm(next);
    EXPECT_EQ(tm.tm_hour, 9);
    EXPECT_EQ(tm.tm_min, 0);
    // Should skip to Monday (March 9).
    EXPECT_EQ(tm.tm_wday, 1);  // Monday.
}

TEST(CronTriggerTest, EveryFiveMinutes) {
    CronTrigger ct{"*/5 * * * *"};

    auto after = make_time(2026, 3, 7, 14, 7, 0);
    auto next = ct.next_fire_after(after);

    auto tm = to_local_tm(next);
    EXPECT_EQ(tm.tm_min, 10);
}

TEST(CronTriggerTest, SpecificMinutes) {
    // "10,20,40 * * * *" = at minutes 10, 20, 40.
    CronTrigger ct{"10,20,40 * * * *"};

    auto after = make_time(2026, 3, 7, 14, 15, 0);
    auto next = ct.next_fire_after(after);

    auto tm = to_local_tm(next);
    EXPECT_EQ(tm.tm_min, 20);
}

TEST(CronTriggerTest, MonthlyFirstDay) {
    // "0 0 1 * *" = midnight on the 1st of every month.
    CronTrigger ct{"0 0 1 * *"};

    auto after = make_time(2026, 3, 15, 0, 0, 0);
    auto next = ct.next_fire_after(after);

    auto tm = to_local_tm(next);
    EXPECT_EQ(tm.tm_mday, 1);
    EXPECT_EQ(tm.tm_mon + 1, 4);  // April.
}

// ═══════════════════════════════════════════════════════════════════════
//  DETERMINISM AND ORDERING
// ═══════════════════════════════════════════════════════════════════════

TEST(CronTriggerTest, NextFireIsStrictlyAfter) {
    CronTrigger ct{"* * * * *"};

    // Query "after" exactly at a minute boundary.
    auto after = make_time(2026, 3, 7, 14, 30, 0);
    auto next = ct.next_fire_after(after);

    // next_fire_after should return strictly after the input.
    EXPECT_GT(next, after);
}

TEST(CronTriggerTest, ConsecutiveFiresIncrease) {
    CronTrigger ct{"0 * * * *"};

    auto t1 = make_time(2026, 3, 7, 10, 0, 0);
    auto t2 = ct.next_fire_after(t1);
    auto t3 = ct.next_fire_after(t2);

    EXPECT_GT(t2, t1);
    EXPECT_GT(t3, t2);

    auto tm2 = to_local_tm(t2);
    auto tm3 = to_local_tm(t3);
    EXPECT_EQ(tm2.tm_hour, 11);
    EXPECT_EQ(tm3.tm_hour, 12);
}

// ═══════════════════════════════════════════════════════════════════════
//  ERROR HANDLING
// ═══════════════════════════════════════════════════════════════════════

TEST(CronTriggerTest, InvalidExpressionThrows) {
    CronTrigger ct{"this is not a cron expression"};

    auto after = make_time(2026, 3, 7, 14, 0, 0);
    EXPECT_THROW((void)ct.next_fire_after(after), std::runtime_error);
}

TEST(CronTriggerTest, EmptyExpressionThrows) {
    CronTrigger ct{""};

    auto after = make_time(2026, 3, 7, 14, 0, 0);
    EXPECT_THROW((void)ct.next_fire_after(after), std::runtime_error);
}

// ═══════════════════════════════════════════════════════════════════════
//  SPEC PARITY: AVScheduler cron patterns
// ═══════════════════════════════════════════════════════════════════════

TEST(CronTriggerTest, AVSchedulerPattern_TenPastEveryHour) {
    // From spec §31.2: "10 * * * *"
    CronTrigger ct{"10 * * * *"};

    auto after = make_time(2026, 3, 7, 14, 5, 0);
    auto next = ct.next_fire_after(after);

    auto tm = to_local_tm(next);
    EXPECT_EQ(tm.tm_min, 10);
    EXPECT_EQ(tm.tm_hour, 14);
}

TEST(CronTriggerTest, AVSchedulerPattern_DailyBackup) {
    // From spec §31.2: "0 2 * * *" (daily backup at 2 AM).
    CronTrigger ct{"0 2 * * *"};

    auto after = make_time(2026, 3, 7, 0, 0, 0);
    auto next = ct.next_fire_after(after);

    auto tm = to_local_tm(next);
    EXPECT_EQ(tm.tm_hour, 2);
    EXPECT_EQ(tm.tm_min, 0);
    EXPECT_EQ(tm.tm_mday, 7);  // Same day, since 0:00 < 2:00.
}

// ═══════════════════════════════════════════════════════════════════════
//  INTEGRATION WITH TimerEntry
// ═══════════════════════════════════════════════════════════════════════

TEST(CronTriggerTest, TimerEntryTimeUntilFire) {
    TimerEntry entry;
    entry.trigger_id = "trg-test";
    entry.target_id = "wfl-test";
    entry.target_name = "test";
    entry.spec = CronTrigger{"0 * * * *"};

    // Set next fire to 30 minutes from "now".
    auto wall_now = std::chrono::system_clock::now();
    entry.next_fire_wall = wall_now + std::chrono::minutes(30);
    entry.next_fire_mono = std::chrono::steady_clock::now()
        + std::chrono::minutes(30);

    auto mono_now = std::chrono::steady_clock::now();
    auto ms = entry.time_until_fire(wall_now, mono_now);

    // Should be approximately 30 minutes (within tolerance).
    EXPECT_GT(ms.count(), 29 * 60 * 1000);  // > 29 min.
    EXPECT_LT(ms.count(), 31 * 60 * 1000);  // < 31 min.
}

}  // anonymous namespace
}  // namespace kairos::engine
