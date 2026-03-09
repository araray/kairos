/// tests/unit/cli/table_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for kairos/cli/table.hpp — Table renderer, status icons,          ║
// ║  ANSI helpers, format_duration, truncate                                 ║
// ║  Spec reference: §23.7                                                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/cli/table.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace kairos::cli {
namespace {

// ── format_duration tests ──────────────────────────────────────────────

TEST(FormatDuration, Zero) {
    EXPECT_EQ(format_duration(0), "--");
    EXPECT_EQ(format_duration(-1), "--");
}

TEST(FormatDuration, Milliseconds) {
    EXPECT_EQ(format_duration(1), "1ms");
    EXPECT_EQ(format_duration(999), "999ms");
}

TEST(FormatDuration, Seconds) {
    EXPECT_EQ(format_duration(1000), "1.0s");
    EXPECT_EQ(format_duration(1500), "1.5s");
    EXPECT_EQ(format_duration(59999), "60.0s");
}

TEST(FormatDuration, Minutes) {
    EXPECT_EQ(format_duration(60000), "1m 0s");
    EXPECT_EQ(format_duration(90000), "1m 30s");
    EXPECT_EQ(format_duration(3540000), "59m 0s");
}

TEST(FormatDuration, Hours) {
    EXPECT_EQ(format_duration(3600000), "1h 0m");
    EXPECT_EQ(format_duration(5400000), "1h 30m");
}

// ── truncate tests ─────────────────────────────────────────────────────

TEST(Truncate, ShortString) {
    EXPECT_EQ(truncate("abc", 10), "abc");
}

TEST(Truncate, ExactLength) {
    EXPECT_EQ(truncate("abcde", 5), "abcde");
}

TEST(Truncate, TruncatesWithDots) {
    EXPECT_EQ(truncate("abcdefghij", 7), "abcde..");
}

TEST(Truncate, VeryShortMax) {
    EXPECT_EQ(truncate("abcde", 2), "ab");
}

// ── status_icon tests ──────────────────────────────────────────────────

TEST(StatusIcon, SuccessNoColor) {
    EXPECT_EQ(status_icon("SUCCESS", false), "[ok]");
    EXPECT_EQ(status_icon("success", false), "[ok]");
}

TEST(StatusIcon, FailureNoColor) {
    EXPECT_EQ(status_icon("FAILURE", false), "[!!]");
}

TEST(StatusIcon, RunningNoColor) {
    EXPECT_EQ(status_icon("RUNNING", false), "[>>]");
}

TEST(StatusIcon, SkippedNoColor) {
    EXPECT_EQ(status_icon("SKIPPED", false), "[--]");
}

TEST(StatusIcon, CancelledNoColor) {
    EXPECT_EQ(status_icon("CANCELLED", false), "[--]");
}

TEST(StatusIcon, UnknownNoColor) {
    EXPECT_EQ(status_icon("BOGUS", false), "[??]");
}

TEST(StatusIcon, SuccessWithColor) {
    auto result = status_icon("SUCCESS", true);
    // Should contain ANSI codes + the ✓ character.
    EXPECT_NE(result.find("\033["), std::string::npos);
    EXPECT_NE(result.find("\xe2\x9c\x93"), std::string::npos);  // ✓
}

// ── colorize_status tests ──────────────────────────────────────────────

TEST(ColorizeStatus, NoColor) {
    EXPECT_EQ(colorize_status("SUCCESS", false), "SUCCESS");
    EXPECT_EQ(colorize_status("FAILURE", false), "FAILURE");
}

TEST(ColorizeStatus, WithColor) {
    auto result = colorize_status("SUCCESS", true);
    // Should contain green ANSI code.
    EXPECT_NE(result.find("\033[32m"), std::string::npos);
    EXPECT_NE(result.find("SUCCESS"), std::string::npos);
}

// ── Table tests ────────────────────────────────────────────────────────

TEST(Table, EmptyTable) {
    Table t({"A", "B", "C"});
    std::ostringstream out;
    t.render(out, false);

    std::string s = out.str();
    // Should have headers and separator but no data rows.
    EXPECT_NE(s.find("A"), std::string::npos);
    EXPECT_NE(s.find("B"), std::string::npos);
    EXPECT_NE(s.find("C"), std::string::npos);
    EXPECT_EQ(t.row_count(), 0u);
}

TEST(Table, SingleRow) {
    Table t({"NAME", "VALUE"});
    t.add_row({"foo", "42"});

    std::ostringstream out;
    t.render(out, false);

    std::string s = out.str();
    EXPECT_NE(s.find("NAME"), std::string::npos);
    EXPECT_NE(s.find("VALUE"), std::string::npos);
    EXPECT_NE(s.find("foo"), std::string::npos);
    EXPECT_NE(s.find("42"), std::string::npos);
    EXPECT_EQ(t.row_count(), 1u);
}

TEST(Table, ColumnWidthsExpand) {
    Table t({"N", "V"});
    t.add_row({"longername", "x"});

    std::ostringstream out;
    t.render(out, false);

    std::string s = out.str();
    // "longername" is 10 chars, wider than header "N" (1 char).
    // The column should be at least 10 wide.
    EXPECT_NE(s.find("longername"), std::string::npos);
}

TEST(Table, MultipleRows) {
    Table t({"ID", "STATUS"});
    t.add_row({"1", "ok"});
    t.add_row({"2", "fail"});
    t.add_row({"3", "ok"});

    EXPECT_EQ(t.row_count(), 3u);

    std::ostringstream out;
    t.render(out, false);

    std::string s = out.str();
    EXPECT_NE(s.find("ok"), std::string::npos);
    EXPECT_NE(s.find("fail"), std::string::npos);
}

TEST(Table, SeparatorUsesUnicodeDash) {
    Table t({"AB"});
    t.add_row({"xy"});

    std::ostringstream out;
    t.render(out, false);

    std::string s = out.str();
    // Should contain ─ (U+2500, 3-byte UTF-8: 0xe2 0x94 0x80).
    EXPECT_NE(s.find("\xe2\x94\x80"), std::string::npos);
}

TEST(Table, BoldHeadersWithColor) {
    Table t({"HDR"});
    t.add_row({"val"});

    std::ostringstream out;
    t.render(out, true);  // use_color = true

    std::string s = out.str();
    // Should contain bold ANSI code.
    EXPECT_NE(s.find("\033[1m"), std::string::npos);
}

TEST(Table, AnsiCodesIgnoredInWidthCalc) {
    // If a cell contains ANSI codes, the column width should be
    // based on visible characters only.
    Table t({"STATUS"});
    std::string colored = std::string("\033[32m") + "OK" + "\033[0m";
    t.add_row({colored});

    std::ostringstream out;
    t.render(out, false);

    // The "STATUS" header is 6 chars, "OK" visible is 2 chars.
    // Column width should be 6 (from the header), not inflated
    // by ANSI escape bytes.
    std::string s = out.str();
    EXPECT_NE(s.find("STATUS"), std::string::npos);
}

// ── colorize tests ────────────────────────────────────────────────────

TEST(Colorize, WithColorEnabled) {
    auto result = colorize("hello", ansi::red, true);
    EXPECT_NE(result.find("\033[31m"), std::string::npos);
    EXPECT_NE(result.find("hello"), std::string::npos);
    EXPECT_NE(result.find("\033[0m"), std::string::npos);
}

TEST(Colorize, WithColorDisabled) {
    auto result = colorize("hello", ansi::red, false);
    EXPECT_EQ(result, "hello");
}

}  // namespace
}  // namespace kairos::cli
