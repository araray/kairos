/// tests/unit/observability/json_formatter_test.cpp
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

// ══════════════════════════════════════════════════════════════════════════════
// Log formatter masking (§17.3 defense-in-depth)
// ══════════════════════════════════════════════════════════════════════════════

TEST(JsonFormatterMasking, NoMaskingByDefault) {
    KairosJsonFormatter fmt;
    EXPECT_FALSE(fmt.has_mask_values());
}

TEST(JsonFormatterMasking, SetMaskValuesActivatesMasking) {
    KairosJsonFormatter fmt;
    fmt.set_mask_values({"secret123", "password"});
    EXPECT_TRUE(fmt.has_mask_values());
}

TEST(JsonFormatterMasking, EmptyMaskValuesDeactivatesMasking) {
    KairosJsonFormatter fmt;
    fmt.set_mask_values({"secret123"});
    EXPECT_TRUE(fmt.has_mask_values());
    fmt.set_mask_values({});
    EXPECT_FALSE(fmt.has_mask_values());
}

TEST(JsonFormatterMasking, MasksSecretInLogMessage) {
    KairosJsonFormatter fmt;
    fmt.set_mask_values({"s3cr3t_p4ssw0rd"});

    // Build a log message that contains the secret.
    spdlog::details::log_msg msg("test_logger", spdlog::level::info,
                                  "DB password is s3cr3t_p4ssw0rd for host");

    spdlog::memory_buf_t dest;
    fmt.format(msg, dest);

    std::string output(dest.data(), dest.size());

    // Should contain "***" but NOT the actual secret.
    EXPECT_NE(output.find("***"), std::string::npos);
    EXPECT_EQ(output.find("s3cr3t_p4ssw0rd"), std::string::npos);
}

TEST(JsonFormatterMasking, LongestFirstPreventsPartialMatch) {
    KairosJsonFormatter fmt;
    // "password123" should be masked before "password" (longest first).
    fmt.set_mask_values({"password123", "password"});

    spdlog::details::log_msg msg("test_logger", spdlog::level::info,
                                  "value is password123 here");

    spdlog::memory_buf_t dest;
    fmt.format(msg, dest);

    std::string output(dest.data(), dest.size());

    // "password123" should be fully replaced with "***",
    // not partially matched as "***123".
    EXPECT_EQ(output.find("password123"), std::string::npos);
    EXPECT_EQ(output.find("password"), std::string::npos);
    EXPECT_NE(output.find("***"), std::string::npos);
}

TEST(JsonFormatterMasking, ClonePreservesMaskValues) {
    KairosJsonFormatter fmt;
    fmt.set_mask_values({"secret_val"});

    auto cloned = fmt.clone();
    auto* cloned_fmt = dynamic_cast<KairosJsonFormatter*>(cloned.get());
    ASSERT_NE(cloned_fmt, nullptr);
    EXPECT_TRUE(cloned_fmt->has_mask_values());

    // Verify masking works on the clone.
    spdlog::details::log_msg msg("test", spdlog::level::info,
                                  "key=secret_val");
    spdlog::memory_buf_t dest;
    cloned_fmt->format(msg, dest);

    std::string output(dest.data(), dest.size());
    EXPECT_EQ(output.find("secret_val"), std::string::npos);
}

}  // namespace kairos::observability
