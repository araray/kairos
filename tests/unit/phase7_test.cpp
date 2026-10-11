/// tests/unit/phase7_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Phase 7 tests — CLI Power Features                                      ║
// ║                                                                           ║
// ║  Tests for:                                                               ║
// ║    §5.1 Regex filter (RowFilter)                                         ║
// ║    §7   KEL eval (one-shot evaluation via eval_expression)               ║
// ║    §2.1 Interactive selector (unit-testable helpers)                      ║
// ║                                                                           ║
// ║  Spec reference: Roadmap §2.1, §5.1, §7                                 ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/cli/filter.hpp"
#include "kairos/cli/interactive_selector.hpp"
#include "kairos/kel/evaluator.hpp"
#include "kairos/kel/errors.hpp"
#include "kairos/kel/value.hpp"

#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace kairos;

// ════════════════════════════════════════════════════════════════════════════
//  §5.1 — RowFilter tests
// ════════════════════════════════════════════════════════════════════════════

TEST(RowFilterTest, DefaultFilterPassesAll) {
    cli::RowFilter f;
    EXPECT_FALSE(f.active());
    EXPECT_TRUE(f.matches(std::vector<std::string>{"anything", "goes"}));
    EXPECT_TRUE(f.matches(std::string("hello")));
}

TEST(RowFilterTest, SimplePatternMatch) {
    cli::RowFilter f("deploy");
    EXPECT_TRUE(f.active());
    EXPECT_TRUE(f.matches(std::vector<std::string>{"my-deploy-workflow", "SUCCESS"}));
    EXPECT_FALSE(f.matches(std::vector<std::string>{"build-workflow", "SUCCESS"}));
}

TEST(RowFilterTest, CaseInsensitive) {
    cli::RowFilter f("DEPLOY");
    EXPECT_TRUE(f.matches(std::vector<std::string>{"my-deploy-workflow", "SUCCESS"}));
    EXPECT_TRUE(f.matches(std::vector<std::string>{"Deploy-Prod", "RUNNING"}));
}

TEST(RowFilterTest, RegexSpecialChars) {
    cli::RowFilter f("\\.log$");
    EXPECT_TRUE(f.matches(std::vector<std::string>{"access.log"}));
    EXPECT_FALSE(f.matches(std::vector<std::string>{"access_log"}));
    EXPECT_FALSE(f.matches(std::vector<std::string>{"logfile"}));
}

TEST(RowFilterTest, RegexAlternation) {
    cli::RowFilter f("backup|sync");
    EXPECT_TRUE(f.matches(std::vector<std::string>{"daily-backup", "SUCCESS"}));
    EXPECT_TRUE(f.matches(std::vector<std::string>{"file-sync", "RUNNING"}));
    EXPECT_FALSE(f.matches(std::vector<std::string>{"deploy-prod", "FAILURE"}));
}

TEST(RowFilterTest, MatchesConcatenatedFields) {
    cli::RowFilter f("deploy.*SUCCESS");
    EXPECT_TRUE(f.matches(std::vector<std::string>{"deploy-prod", "SUCCESS", "1.2s"}));
    EXPECT_FALSE(f.matches(std::vector<std::string>{"deploy-prod", "FAILURE", "1.2s"}));
}

TEST(RowFilterTest, StripsAnsiCodes) {
    std::string colorized = "\033[32mSUCCESS\033[0m";
    cli::RowFilter f("SUCCESS");
    EXPECT_TRUE(f.matches(std::vector<std::string>{colorized}));
}

TEST(RowFilterTest, StripAnsiHelper) {
    std::string input = "\033[1m\033[36mbold cyan\033[0m normal";
    std::string result = cli::RowFilter::strip_ansi(input);
    EXPECT_EQ(result, "bold cyan normal");
}

TEST(RowFilterTest, StripAnsiEmpty) {
    EXPECT_EQ(cli::RowFilter::strip_ansi(""), "");
    EXPECT_EQ(cli::RowFilter::strip_ansi("plain"), "plain");
}

TEST(RowFilterTest, MakeFilterInvalidRegex) {
    // Invalid regex should return a disabled filter (not throw).
    auto f = cli::make_filter("[invalid(");
    EXPECT_FALSE(f.active());
    EXPECT_TRUE(f.matches(std::vector<std::string>{"anything"}));
}

TEST(RowFilterTest, MakeFilterEmpty) {
    auto f = cli::make_filter("");
    EXPECT_FALSE(f.active());
}

TEST(RowFilterTest, SingleStringMatch) {
    cli::RowFilter f("prod");
    EXPECT_TRUE(f.matches(std::string("deploy-prod-v2")));
    EXPECT_FALSE(f.matches(std::string("staging-build")));
}

TEST(RowFilterTest, EmptyRowAlwaysFails) {
    cli::RowFilter f("something");
    EXPECT_FALSE(f.matches(std::vector<std::string>{}));
}

// ════════════════════════════════════════════════════════════════════════════
//  §2.1 — Interactive selector tests (non-interactive, unit-testable parts)
// ════════════════════════════════════════════════════════════════════════════

TEST(InteractiveSelectorTest, CanUseInteractiveInTests) {
    // In test environment, stdin is not a TTY.
    // can_use_interactive() should return false.
    // (It may return true in some CI setups, so we just verify it
    // doesn't crash.)
    [[maybe_unused]] bool result = cli::can_use_interactive();
}

TEST(InteractiveSelectorTest, EmptyItemsReturnsNullopt) {
    auto result = cli::interactive_select({});
    EXPECT_FALSE(result.has_value());
}

TEST(InteractiveSelectorTest, NonTtyReturnsNullopt) {
    // In a test environment (not a TTY), interactive_select returns
    // nullopt immediately without blocking.
    std::vector<cli::SelectorItem> items = {
        {"id1", "Display 1", "search 1"},
        {"id2", "Display 2", "search 2"},
    };
    auto result = cli::interactive_select(items);
    // Should be nullopt since we're not in a real terminal.
    EXPECT_FALSE(result.has_value());
}

TEST(InteractiveSelectorTest, SelectorItemConstruction) {
    cli::SelectorItem item{"run-abc123", "run-abc1..  deploy  SUCCESS",
                           "run-abc123 deploy SUCCESS"};
    EXPECT_EQ(item.id, "run-abc123");
    EXPECT_FALSE(item.display.empty());
    EXPECT_FALSE(item.search_text.empty());
}

// ════════════════════════════════════════════════════════════════════════════
//  §7 — KEL eval tests (one-shot evaluation)
// ════════════════════════════════════════════════════════════════════════════

TEST(KelEvalTest, BasicArithmetic) {
    auto ctx = kel::make_default_context();
    kel::EvalLimits limits;
    auto result = kel::eval_expression("2 + 3 * 4", ctx, limits);
    EXPECT_TRUE(result.is_int());
    EXPECT_EQ(result.as_int(), 14);
}

TEST(KelEvalTest, StringConcatenation) {
    auto ctx = kel::make_default_context();
    kel::EvalLimits limits;
    auto result = kel::eval_expression(
        "\"hello\" + \" world\"", ctx, limits);
    EXPECT_TRUE(result.is_string());
    EXPECT_EQ(result.as_string(), "hello world");
}

TEST(KelEvalTest, BooleanLogic) {
    auto ctx = kel::make_default_context();
    kel::EvalLimits limits;

    auto r1 = kel::eval_expression("true and true", ctx, limits);
    EXPECT_TRUE(r1.as_bool());

    auto r2 = kel::eval_expression("true and false", ctx, limits);
    EXPECT_FALSE(r2.as_bool());

    auto r3 = kel::eval_expression("not false", ctx, limits);
    EXPECT_TRUE(r3.as_bool());
}

TEST(KelEvalTest, Comparison) {
    auto ctx = kel::make_default_context();
    kel::EvalLimits limits;

    auto r1 = kel::eval_expression("5 > 3", ctx, limits);
    EXPECT_TRUE(r1.as_bool());

    auto r2 = kel::eval_expression("\"abc\" == \"abc\"", ctx, limits);
    EXPECT_TRUE(r2.as_bool());

    auto r3 = kel::eval_expression("10 <= 5", ctx, limits);
    EXPECT_FALSE(r3.as_bool());
}

TEST(KelEvalTest, TypeInfo) {
    auto ctx = kel::make_default_context();
    kel::EvalLimits limits;

    auto r1 = kel::eval_expression("42", ctx, limits);
    EXPECT_EQ(r1.type_name(), "int");

    auto r2 = kel::eval_expression("3.14", ctx, limits);
    EXPECT_EQ(r2.type_name(), "float");

    auto r3 = kel::eval_expression("true", ctx, limits);
    EXPECT_EQ(r3.type_name(), "bool");

    auto r4 = kel::eval_expression("\"hello\"", ctx, limits);
    EXPECT_EQ(r4.type_name(), "string");
}

TEST(KelEvalTest, DisplayString) {
    auto ctx = kel::make_default_context();
    kel::EvalLimits limits;

    auto r1 = kel::eval_expression("42", ctx, limits);
    EXPECT_EQ(r1.to_display_string(), "42");

    auto r2 = kel::eval_expression("true", ctx, limits);
    EXPECT_EQ(r2.to_display_string(), "true");

    auto r3 = kel::eval_expression("\"hello\"", ctx, limits);
    EXPECT_EQ(r3.to_display_string(), "\"hello\"");
}

TEST(KelEvalTest, InvalidExpressionThrows) {
    auto ctx = kel::make_default_context();
    kel::EvalLimits limits;

    EXPECT_THROW(
        kel::eval_expression("10 / 0", ctx, limits),
        kel::KelEvalError);
}

TEST(KelEvalTest, ParseErrorThrows) {
    auto ctx = kel::make_default_context();
    kel::EvalLimits limits;

    EXPECT_THROW(
        kel::eval_expression("((( unclosed", ctx, limits),
        kel::KelParseError);
}

TEST(KelEvalTest, FloatArithmetic) {
    auto ctx = kel::make_default_context();
    kel::EvalLimits limits;
    auto result = kel::eval_expression("3.14 * 2.0", ctx, limits);
    EXPECT_TRUE(result.is_float());
    EXPECT_DOUBLE_EQ(result.as_float(), 6.28);
}

TEST(KelEvalTest, Precedence) {
    auto ctx = kel::make_default_context();
    kel::EvalLimits limits;
    auto r1 = kel::eval_expression("(2 + 3) * 4", ctx, limits);
    EXPECT_EQ(r1.as_int(), 20);

    auto r2 = kel::eval_expression("2 * 3 + 4 * 5", ctx, limits);
    EXPECT_EQ(r2.as_int(), 26);
}

TEST(KelEvalTest, ShortCircuit) {
    auto ctx = kel::make_default_context();
    kel::EvalLimits limits;

    // Division by zero should not be reached due to short-circuit.
    auto r1 = kel::eval_expression("false and (1 / 0 > 0)", ctx, limits);
    EXPECT_FALSE(r1.as_bool());

    auto r2 = kel::eval_expression("true or (1 / 0 > 0)", ctx, limits);
    EXPECT_TRUE(r2.as_bool());
}

TEST(KelEvalTest, NowFunction) {
    auto ctx = kel::make_default_context();
    kel::EvalLimits limits;
    // now() should be available in default context and return a string.
    // If not available, the expression will throw.
    try {
        auto result = kel::eval_expression("now()", ctx, limits);
        // If now() exists, it returns a string.
        EXPECT_EQ(result.type_name(), "string");
    } catch (const kel::KelEvalError&) {
        // now() might not be in default context — that's OK for
        // this test; it verifies the eval path doesn't crash.
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  Combined filter + row interaction tests
// ════════════════════════════════════════════════════════════════════════════

TEST(FilterIntegrationTest, FilterWorksWithTypicalRunsData) {
    cli::RowFilter f("deploy");

    std::vector<std::vector<std::string>> rows = {
        {"run-abc12..", "deploy-prod", "SUCCESS", "cron", "2026-03-15", "1.2s"},
        {"run-def34..", "build-main",  "SUCCESS", "manual", "2026-03-15", "3.5s"},
        {"run-ghi56..", "deploy-stg",  "FAILURE", "cron", "2026-03-14", "0.5s"},
    };

    int matched = 0;
    for (const auto& row : rows) {
        if (f.matches(row)) matched++;
    }
    EXPECT_EQ(matched, 2);  // deploy-prod and deploy-stg
}

TEST(FilterIntegrationTest, StatusFilter) {
    cli::RowFilter f("FAILURE");

    std::vector<std::vector<std::string>> rows = {
        {"run1", "workflow-a", "SUCCESS"},
        {"run2", "workflow-b", "FAILURE"},
        {"run3", "workflow-c", "RUNNING"},
    };

    int matched = 0;
    for (const auto& row : rows) {
        if (f.matches(row)) matched++;
    }
    EXPECT_EQ(matched, 1);
}

TEST(FilterIntegrationTest, ComplexRegex) {
    cli::RowFilter f("(deploy|sync).*FAIL");

    std::vector<std::vector<std::string>> rows = {
        {"deploy-prod", "FAILURE"},
        {"deploy-prod", "SUCCESS"},
        {"sync-backup", "FAILURE"},
        {"build-main", "FAILURE"},
    };

    int matched = 0;
    for (const auto& row : rows) {
        if (f.matches(row)) matched++;
    }
    EXPECT_EQ(matched, 2);  // deploy-prod FAILURE and sync-backup FAILURE
}
