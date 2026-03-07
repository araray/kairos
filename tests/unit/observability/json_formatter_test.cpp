// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  json_formatter_test.cpp — JSON log formatter tests                       ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/observability/json_formatter.hpp"
#include "kairos/observability/logging.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <string>

namespace kairos::observability {

TEST(JsonFormatter, ProducesValidJson) {
    KairosJsonFormatter fmt;
    spdlog::memory_buf_t buf;

    spdlog::details::log_msg msg(spdlog::source_loc{"test.cpp", 42, "test"},
                                  "kairos.test",
                                  spdlog::level::info,
                                  "Hello, world!");
    fmt.format(msg, buf);

    std::string output(buf.data(), buf.size());
    ASSERT_FALSE(output.empty());

    // Remove trailing newline for JSON parse.
    if (!output.empty() && output.back() == '\n') {
        output.pop_back();
    }

    auto j = nlohmann::json::parse(output, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << "Output is not valid JSON: " << output;
}

TEST(JsonFormatter, HasRequiredFields) {
    KairosJsonFormatter fmt;
    spdlog::memory_buf_t buf;

    spdlog::details::log_msg msg(spdlog::source_loc{"test.cpp", 42, "test"},
                                  "kairos",
                                  spdlog::level::warn,
                                  "Test message");
    fmt.format(msg, buf);

    std::string output(buf.data(), buf.size());
    if (!output.empty() && output.back() == '\n') output.pop_back();
    auto j = nlohmann::json::parse(output);

    EXPECT_TRUE(j.contains("ts"));
    EXPECT_TRUE(j.contains("level"));
    EXPECT_TRUE(j.contains("logger"));
    EXPECT_TRUE(j.contains("msg"));
    EXPECT_TRUE(j.contains("thread"));
}

TEST(JsonFormatter, LevelIsCorrect) {
    KairosJsonFormatter fmt;

    auto check_level = [&](spdlog::level::level_enum lvl,
                           const std::string& expected) {
        spdlog::memory_buf_t buf;
        spdlog::details::log_msg msg("kairos", lvl, "test");
        fmt.format(msg, buf);
        std::string output(buf.data(), buf.size());
        if (!output.empty() && output.back() == '\n') output.pop_back();
        auto j = nlohmann::json::parse(output);
        EXPECT_EQ(j["level"], expected) << "For spdlog level " << static_cast<int>(lvl);
    };

    check_level(spdlog::level::trace, "trace");
    check_level(spdlog::level::debug, "debug");
    check_level(spdlog::level::info, "info");
    check_level(spdlog::level::warn, "warn");
    check_level(spdlog::level::err, "error");
    check_level(spdlog::level::critical, "critical");
}

TEST(JsonFormatter, EscapesSpecialChars) {
    KairosJsonFormatter fmt;
    spdlog::memory_buf_t buf;

    spdlog::details::log_msg msg("kairos", spdlog::level::info,
                                  "Line with \"quotes\" and\nnewlines");
    fmt.format(msg, buf);

    std::string output(buf.data(), buf.size());
    if (!output.empty() && output.back() == '\n') output.pop_back();

    // Should be valid JSON despite special chars.
    auto j = nlohmann::json::parse(output, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << "JSON parse failed: " << output;

    // The message should contain the original text when decoded.
    EXPECT_EQ(j["msg"], "Line with \"quotes\" and\nnewlines");
}

TEST(JsonFormatter, IncludesSourceLocation) {
    KairosJsonFormatter fmt;
    spdlog::memory_buf_t buf;

    spdlog::details::log_msg msg(
        spdlog::source_loc{"pipeline.cpp", 345, "process_event"},
        "kairos.pipeline",
        spdlog::level::debug,
        "Processing event");
    fmt.format(msg, buf);

    std::string output(buf.data(), buf.size());
    if (!output.empty() && output.back() == '\n') output.pop_back();
    auto j = nlohmann::json::parse(output);

    EXPECT_TRUE(j.contains("src"));
    EXPECT_EQ(j["src"], "pipeline.cpp:345");
}

TEST(JsonFormatter, TimestampIsIso8601) {
    KairosJsonFormatter fmt;
    spdlog::memory_buf_t buf;

    spdlog::details::log_msg msg("kairos", spdlog::level::info, "test");
    fmt.format(msg, buf);

    std::string output(buf.data(), buf.size());
    if (!output.empty() && output.back() == '\n') output.pop_back();
    auto j = nlohmann::json::parse(output);

    std::string ts = j["ts"];
    // Basic format check: YYYY-MM-DDThh:mm:ss.mmmZ
    EXPECT_GE(ts.size(), 23u);
    EXPECT_EQ(ts[4], '-');
    EXPECT_EQ(ts[7], '-');
    EXPECT_EQ(ts[10], 'T');
    EXPECT_EQ(ts.back(), 'Z');
}

TEST(LogLevel, ParseValidLevels) {
    EXPECT_EQ(parse_log_level("trace"),    spdlog::level::trace);
    EXPECT_EQ(parse_log_level("debug"),    spdlog::level::debug);
    EXPECT_EQ(parse_log_level("info"),     spdlog::level::info);
    EXPECT_EQ(parse_log_level("warn"),     spdlog::level::warn);
    EXPECT_EQ(parse_log_level("warning"),  spdlog::level::warn);
    EXPECT_EQ(parse_log_level("error"),    spdlog::level::err);
    EXPECT_EQ(parse_log_level("critical"), spdlog::level::critical);
    EXPECT_EQ(parse_log_level("off"),      spdlog::level::off);
}

TEST(LogLevel, CaseInsensitive) {
    EXPECT_EQ(parse_log_level("INFO"),     spdlog::level::info);
    EXPECT_EQ(parse_log_level("Debug"),    spdlog::level::debug);
    EXPECT_EQ(parse_log_level("CRITICAL"), spdlog::level::critical);
}

TEST(LogLevel, InvalidReturnsInfo) {
    EXPECT_EQ(parse_log_level("verbose"),  spdlog::level::info);
    EXPECT_EQ(parse_log_level(""),         spdlog::level::info);
    EXPECT_EQ(parse_log_level("nonsense"), spdlog::level::info);
}

}  // namespace kairos::observability
