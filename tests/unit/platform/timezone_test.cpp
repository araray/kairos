/// tests/unit/platform/timezone_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for timezone utility                                               ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/platform/timezone.hpp"

#include <gtest/gtest.h>

using namespace kairos::platform;

// ═══════════════════════════════════════════════════════════════════════════
// TimezoneConfig::parse
// ═══════════════════════════════════════════════════════════════════════════

TEST(TimezoneConfig, ParseUTC) {
    auto cfg = TimezoneConfig::parse("UTC");
    EXPECT_EQ(cfg.mode, TimezoneMode::UTC);
    EXPECT_EQ(cfg.offset_minutes, 0);
    EXPECT_EQ(cfg.suffix(), "Z");
}

TEST(TimezoneConfig, ParseUtcLower) {
    auto cfg = TimezoneConfig::parse("utc");
    EXPECT_EQ(cfg.mode, TimezoneMode::UTC);
}

TEST(TimezoneConfig, ParseZ) {
    auto cfg = TimezoneConfig::parse("Z");
    EXPECT_EQ(cfg.mode, TimezoneMode::UTC);
}

TEST(TimezoneConfig, ParseEmpty) {
    auto cfg = TimezoneConfig::parse("");
    EXPECT_EQ(cfg.mode, TimezoneMode::UTC);
}

TEST(TimezoneConfig, ParseLocal) {
    auto cfg = TimezoneConfig::parse("local");
    EXPECT_EQ(cfg.mode, TimezoneMode::Local);
}

TEST(TimezoneConfig, ParseLocalMixed) {
    auto cfg = TimezoneConfig::parse("Local");
    EXPECT_EQ(cfg.mode, TimezoneMode::Local);
}

TEST(TimezoneConfig, ParseSystem) {
    auto cfg = TimezoneConfig::parse("system");
    EXPECT_EQ(cfg.mode, TimezoneMode::Local);
}

TEST(TimezoneConfig, ParsePositiveOffset) {
    auto cfg = TimezoneConfig::parse("+05:30");
    EXPECT_EQ(cfg.mode, TimezoneMode::Offset);
    EXPECT_EQ(cfg.offset_minutes, 330);
    EXPECT_EQ(cfg.suffix(), "+05:30");
}

TEST(TimezoneConfig, ParseNegativeOffset) {
    auto cfg = TimezoneConfig::parse("-08:00");
    EXPECT_EQ(cfg.mode, TimezoneMode::Offset);
    EXPECT_EQ(cfg.offset_minutes, -480);
    EXPECT_EQ(cfg.suffix(), "-08:00");
}

TEST(TimezoneConfig, ParseCompactOffset) {
    auto cfg = TimezoneConfig::parse("+0530");
    EXPECT_EQ(cfg.mode, TimezoneMode::Offset);
    EXPECT_EQ(cfg.offset_minutes, 330);
}

TEST(TimezoneConfig, ParseHoursOnly) {
    auto cfg = TimezoneConfig::parse("+05");
    EXPECT_EQ(cfg.mode, TimezoneMode::Offset);
    EXPECT_EQ(cfg.offset_minutes, 300);
}

TEST(TimezoneConfig, ParseZeroOffset) {
    auto cfg = TimezoneConfig::parse("+00:00");
    EXPECT_EQ(cfg.mode, TimezoneMode::Offset);
    EXPECT_EQ(cfg.offset_minutes, 0);
    EXPECT_EQ(cfg.suffix(), "Z");
}

TEST(TimezoneConfig, ParseUnrecognized) {
    auto cfg = TimezoneConfig::parse("America/New_York");
    EXPECT_EQ(cfg.mode, TimezoneMode::UTC);  // Falls back to UTC.
}

// ═══════════════════════════════════════════════════════════════════════════
// parse_utc_timestamp
// ═══════════════════════════════════════════════════════════════════════════

TEST(ParseUtcTimestamp, ISOWithT) {
    auto tp = parse_utc_timestamp("2026-03-11T14:30:45");
    EXPECT_NE(tp.time_since_epoch().count(), 0);
}

TEST(ParseUtcTimestamp, ISOWithTAndZ) {
    auto tp = parse_utc_timestamp("2026-03-11T14:30:45Z");
    EXPECT_NE(tp.time_since_epoch().count(), 0);
}

TEST(ParseUtcTimestamp, ISOWithFractional) {
    auto tp = parse_utc_timestamp("2026-03-11T14:30:45.123Z");
    EXPECT_NE(tp.time_since_epoch().count(), 0);
}

TEST(ParseUtcTimestamp, SpaceSeparated) {
    auto tp = parse_utc_timestamp("2026-03-11 14:30:45");
    EXPECT_NE(tp.time_since_epoch().count(), 0);
}

TEST(ParseUtcTimestamp, Empty) {
    auto tp = parse_utc_timestamp("");
    EXPECT_EQ(tp.time_since_epoch().count(), 0);
}

TEST(ParseUtcTimestamp, TooShort) {
    auto tp = parse_utc_timestamp("2026-03-11");
    EXPECT_EQ(tp.time_since_epoch().count(), 0);
}

TEST(ParseUtcTimestamp, KnownTimestamp) {
    // 2026-01-01T00:00:00Z is a known epoch.
    auto tp = parse_utc_timestamp("2026-01-01T00:00:00Z");
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    EXPECT_EQ(tm.tm_year, 126);  // 2026 - 1900
    EXPECT_EQ(tm.tm_mon, 0);     // January
    EXPECT_EQ(tm.tm_mday, 1);
    EXPECT_EQ(tm.tm_hour, 0);
    EXPECT_EQ(tm.tm_min, 0);
    EXPECT_EQ(tm.tm_sec, 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// format_display_time
// ═══════════════════════════════════════════════════════════════════════════

TEST(FormatDisplayTime, UTCPassthrough) {
    auto tz = TimezoneConfig::parse("UTC");
    auto result = format_display_time("2026-03-11T14:30:45Z", tz);
    EXPECT_EQ(result, "2026-03-11T14:30:45Z");
}

TEST(FormatDisplayTime, UTCAddsZSuffix) {
    auto tz = TimezoneConfig::parse("UTC");
    auto result = format_display_time("2026-03-11T14:30:45", tz);
    EXPECT_EQ(result, "2026-03-11T14:30:45Z");
}

TEST(FormatDisplayTime, UTCStripsFractional) {
    auto tz = TimezoneConfig::parse("UTC");
    auto result = format_display_time("2026-03-11T14:30:45.123Z", tz);
    EXPECT_EQ(result, "2026-03-11T14:30:45Z");
}

TEST(FormatDisplayTime, OffsetApplied) {
    auto tz = TimezoneConfig::parse("+05:30");
    auto result = format_display_time("2026-03-11T14:30:00Z", tz);
    EXPECT_EQ(result, "2026-03-11T20:00:00+05:30");
}

TEST(FormatDisplayTime, NegativeOffsetApplied) {
    auto tz = TimezoneConfig::parse("-08:00");
    auto result = format_display_time("2026-03-11T14:30:00Z", tz);
    EXPECT_EQ(result, "2026-03-11T06:30:00-08:00");
}

TEST(FormatDisplayTime, EmptyTimestamp) {
    auto tz = TimezoneConfig::parse("UTC");
    auto result = format_display_time("", tz);
    EXPECT_EQ(result, "");
}

// ═══════════════════════════════════════════════════════════════════════════
// format_relative
// ═══════════════════════════════════════════════════════════════════════════

TEST(FormatRelative, Empty) {
    EXPECT_EQ(format_relative(""), "--");
}

TEST(FormatRelative, RecentTimestamp) {
    // Format "now" should be "just now".
    auto now = std::chrono::system_clock::now();
    auto result = format_relative(now);
    EXPECT_EQ(result, "just now");
}

TEST(FormatRelative, OldTimestamp) {
    auto old = std::chrono::system_clock::now() - std::chrono::hours(3);
    auto result = format_relative(old);
    EXPECT_EQ(result, "3h ago");
}

// ═══════════════════════════════════════════════════════════════════════════
// format_now
// ═══════════════════════════════════════════════════════════════════════════

TEST(FormatNow, UTCEndsWithZ) {
    auto tz = TimezoneConfig::parse("UTC");
    auto result = format_now(tz);
    EXPECT_FALSE(result.empty());
    EXPECT_EQ(result.back(), 'Z');
}

TEST(FormatNow, OffsetEndsWithOffset) {
    auto tz = TimezoneConfig::parse("+05:30");
    auto result = format_now(tz);
    EXPECT_FALSE(result.empty());
    EXPECT_NE(result.find("+05:30"), std::string::npos);
}
