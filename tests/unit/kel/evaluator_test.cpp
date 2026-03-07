/// tests/unit/kel/evaluator_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  KEL Evaluator unit tests (~120 tests)                                    ║
// ║                                                                           ║
// ║  Covers: arithmetic, comparisons, booleans, short-circuit, string ops,    ║
// ║  duration ops, type promotion, functions, member/method dispatch,         ║
// ║  sandboxing limits, error handling.                                        ║
// ║                                                                           ║
// ║  Spec reference: §7.6–§7.9, §30.7                                        ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/kel/evaluator.hpp"
#include "kairos/kel/errors.hpp"

#include <gtest/gtest.h>
#include <chrono>

using namespace kairos::kel;
using namespace std::chrono_literals;

// ─── Test fixture ─────────────────────────────────────────────────────────

class KelEvaluatorTest : public ::testing::Test {
protected:
    EvalContext ctx;
    EvalLimits  limits;

    void SetUp() override {
        ctx = make_default_context();
        limits = EvalLimits{};
    }

    /// Convenience: parse + evaluate.
    KelValue eval(std::string_view expr) {
        return eval_expression(expr, ctx, limits);
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Integer arithmetic
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KelEvaluatorTest, IntAdd)  { EXPECT_EQ(eval("2 + 3").as_int(), 5); }
TEST_F(KelEvaluatorTest, IntSub)  { EXPECT_EQ(eval("10 - 7").as_int(), 3); }
TEST_F(KelEvaluatorTest, IntMul)  { EXPECT_EQ(eval("4 * 5").as_int(), 20); }
TEST_F(KelEvaluatorTest, IntDiv)  { EXPECT_EQ(eval("10 / 3").as_int(), 3); }
TEST_F(KelEvaluatorTest, IntMod)  { EXPECT_EQ(eval("10 % 3").as_int(), 1); }

TEST_F(KelEvaluatorTest, IntNeg)  { EXPECT_EQ(eval("-5").as_int(), -5); }
TEST_F(KelEvaluatorTest, IntNegNeg) { EXPECT_EQ(eval("--5").as_int(), 5); }

TEST_F(KelEvaluatorTest, IntPrecedence) {
    EXPECT_EQ(eval("2 + 3 * 4").as_int(), 14);
}

TEST_F(KelEvaluatorTest, IntParens) {
    EXPECT_EQ(eval("(2 + 3) * 4").as_int(), 20);
}

TEST_F(KelEvaluatorTest, IntComplex) {
    EXPECT_EQ(eval("2 * 3 + 4 * 5").as_int(), 26);
}

TEST_F(KelEvaluatorTest, IntZero) {
    EXPECT_EQ(eval("0").as_int(), 0);
}

TEST_F(KelEvaluatorTest, IntHex) {
    EXPECT_EQ(eval("0xFF").as_int(), 255);
}

TEST_F(KelEvaluatorTest, IntBinary) {
    EXPECT_EQ(eval("0b1010").as_int(), 10);
}

// ═══════════════════════════════════════════════════════════════════════════
// Float arithmetic
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KelEvaluatorTest, FloatAdd) {
    EXPECT_DOUBLE_EQ(eval("1.5 + 2.5").as_float(), 4.0);
}

TEST_F(KelEvaluatorTest, FloatSub) {
    EXPECT_DOUBLE_EQ(eval("3.0 - 1.5").as_float(), 1.5);
}

TEST_F(KelEvaluatorTest, FloatMul) {
    EXPECT_DOUBLE_EQ(eval("2.0 * 3.0").as_float(), 6.0);
}

TEST_F(KelEvaluatorTest, FloatDiv) {
    EXPECT_DOUBLE_EQ(eval("7.0 / 2.0").as_float(), 3.5);
}

TEST_F(KelEvaluatorTest, FloatNeg) {
    EXPECT_DOUBLE_EQ(eval("-3.14").as_float(), -3.14);
}

// ═══════════════════════════════════════════════════════════════════════════
// Mixed int/float promotion
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KelEvaluatorTest, MixedAddIntFloat) {
    auto result = eval("1 + 2.5");
    EXPECT_TRUE(result.is_float());
    EXPECT_DOUBLE_EQ(result.as_float(), 3.5);
}

TEST_F(KelEvaluatorTest, MixedMulIntFloat) {
    auto result = eval("3 * 1.5");
    EXPECT_TRUE(result.is_float());
    EXPECT_DOUBLE_EQ(result.as_float(), 4.5);
}

TEST_F(KelEvaluatorTest, MixedCompareIntFloat) {
    EXPECT_TRUE(eval("1 < 1.5").as_bool());
    EXPECT_TRUE(eval("2.0 == 2").as_bool());
}

// ═══════════════════════════════════════════════════════════════════════════
// Boolean logic
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KelEvaluatorTest, BoolAndTT)  { EXPECT_TRUE(eval("true and true").as_bool()); }
TEST_F(KelEvaluatorTest, BoolAndTF)  { EXPECT_FALSE(eval("true and false").as_bool()); }
TEST_F(KelEvaluatorTest, BoolAndFT)  { EXPECT_FALSE(eval("false and true").as_bool()); }
TEST_F(KelEvaluatorTest, BoolAndFF)  { EXPECT_FALSE(eval("false and false").as_bool()); }
TEST_F(KelEvaluatorTest, BoolOrTT)   { EXPECT_TRUE(eval("true or true").as_bool()); }
TEST_F(KelEvaluatorTest, BoolOrTF)   { EXPECT_TRUE(eval("true or false").as_bool()); }
TEST_F(KelEvaluatorTest, BoolOrFT)   { EXPECT_TRUE(eval("false or true").as_bool()); }
TEST_F(KelEvaluatorTest, BoolOrFF)   { EXPECT_FALSE(eval("false or false").as_bool()); }
TEST_F(KelEvaluatorTest, BoolNotT)   { EXPECT_FALSE(eval("not true").as_bool()); }
TEST_F(KelEvaluatorTest, BoolNotF)   { EXPECT_TRUE(eval("not false").as_bool()); }

TEST_F(KelEvaluatorTest, BoolNotNot) {
    EXPECT_TRUE(eval("not not true").as_bool());
}

TEST_F(KelEvaluatorTest, BoolCompound) {
    EXPECT_TRUE(eval("true and true or false").as_bool());
    EXPECT_TRUE(eval("false or true and true").as_bool());
}

// ═══════════════════════════════════════════════════════════════════════════
// Short-circuit evaluation
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KelEvaluatorTest, ShortCircuitAnd) {
    // Division by zero should NOT be reached.
    EXPECT_FALSE(eval("false and (1 / 0 > 0)").as_bool());
}

TEST_F(KelEvaluatorTest, ShortCircuitOr) {
    EXPECT_TRUE(eval("true or (1 / 0 > 0)").as_bool());
}

// ═══════════════════════════════════════════════════════════════════════════
// Comparisons
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KelEvaluatorTest, IntLess)          { EXPECT_TRUE(eval("3 < 5").as_bool()); }
TEST_F(KelEvaluatorTest, IntLessEqual)     { EXPECT_TRUE(eval("5 <= 5").as_bool()); }
TEST_F(KelEvaluatorTest, IntGreater)       { EXPECT_TRUE(eval("5 > 3").as_bool()); }
TEST_F(KelEvaluatorTest, IntGreaterEqual)  { EXPECT_TRUE(eval("5 >= 5").as_bool()); }
TEST_F(KelEvaluatorTest, IntEqual)         { EXPECT_TRUE(eval("5 == 5").as_bool()); }
TEST_F(KelEvaluatorTest, IntNotEqual)      { EXPECT_TRUE(eval("5 != 3").as_bool()); }

TEST_F(KelEvaluatorTest, IntNotLess)       { EXPECT_FALSE(eval("5 < 3").as_bool()); }
TEST_F(KelEvaluatorTest, IntNotEqual2)     { EXPECT_FALSE(eval("5 == 3").as_bool()); }

TEST_F(KelEvaluatorTest, StringEqual)  { EXPECT_TRUE(eval("\"abc\" == \"abc\"").as_bool()); }
TEST_F(KelEvaluatorTest, StringNotEq)  { EXPECT_TRUE(eval("\"abc\" != \"xyz\"").as_bool()); }
TEST_F(KelEvaluatorTest, StringLess)   { EXPECT_TRUE(eval("\"abc\" < \"abd\"").as_bool()); }
TEST_F(KelEvaluatorTest, StringGreater){ EXPECT_TRUE(eval("\"xyz\" > \"abc\"").as_bool()); }

// Cross-type equality returns false (not error).
TEST_F(KelEvaluatorTest, CrossTypeEqIntString) {
    EXPECT_FALSE(eval("42 == \"42\"").as_bool());
}

TEST_F(KelEvaluatorTest, CrossTypeEqBoolInt) {
    EXPECT_FALSE(eval("true == 1").as_bool());
}

// Cross-type ordered comparison throws error.
TEST_F(KelEvaluatorTest, CrossTypeOrderedThrows) {
    EXPECT_THROW(eval("\"hello\" > 42"), KelEvalError);
}

// ═══════════════════════════════════════════════════════════════════════════
// String operations
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KelEvaluatorTest, StringConcat) {
    EXPECT_EQ(eval("\"hello\" + \" \" + \"world\"").as_string(), "hello world");
}

TEST_F(KelEvaluatorTest, StringStartsWith) {
    EXPECT_TRUE(eval("starts_with(\"hello world\", \"hello\")").as_bool());
    EXPECT_FALSE(eval("starts_with(\"hello world\", \"world\")").as_bool());
}

TEST_F(KelEvaluatorTest, StringEndsWith) {
    EXPECT_TRUE(eval("ends_with(\"hello world\", \"world\")").as_bool());
    EXPECT_FALSE(eval("ends_with(\"hello world\", \"hello\")").as_bool());
}

TEST_F(KelEvaluatorTest, StringContains) {
    EXPECT_TRUE(eval("contains(\"hello world\", \"lo wo\")").as_bool());
    EXPECT_FALSE(eval("contains(\"hello world\", \"xyz\")").as_bool());
}

TEST_F(KelEvaluatorTest, StringLower) {
    EXPECT_EQ(eval("lower(\"HELLO\")").as_string(), "hello");
}

TEST_F(KelEvaluatorTest, StringUpper) {
    EXPECT_EQ(eval("upper(\"hello\")").as_string(), "HELLO");
}

TEST_F(KelEvaluatorTest, StringTrim) {
    EXPECT_EQ(eval("trim(\"  hello  \")").as_string(), "hello");
}

TEST_F(KelEvaluatorTest, StringReplace) {
    EXPECT_EQ(eval("replace(\"hello world\", \"world\", \"earth\")").as_string(),
              "hello earth");
}

TEST_F(KelEvaluatorTest, StringMatches) {
    EXPECT_TRUE(eval("matches(\"hello123\", \"[a-z]+[0-9]+\")").as_bool());
    EXPECT_FALSE(eval("matches(\"hello\", \"^[0-9]+$\")").as_bool());
}

TEST_F(KelEvaluatorTest, StringMatchesInvalidRegex) {
    // Invalid regex should return false (defensive).
    EXPECT_FALSE(eval("matches(\"hello\", \"[invalid\")").as_bool());
}

// ═══════════════════════════════════════════════════════════════════════════
// Duration operations
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KelEvaluatorTest, DurationAdd) {
    auto result = eval("1h + 30m");
    EXPECT_TRUE(result.is_duration());
    EXPECT_EQ(result.as_duration().count(), 5400000);  // 90 minutes
}

TEST_F(KelEvaluatorTest, DurationSub) {
    auto result = eval("2h - 30m");
    EXPECT_EQ(result.as_duration().count(), 5400000);
}

TEST_F(KelEvaluatorTest, DurationCompare) {
    EXPECT_TRUE(eval("1h > 30m").as_bool());
    EXPECT_TRUE(eval("30m < 1h").as_bool());
    EXPECT_TRUE(eval("60m == 1h").as_bool());
}

TEST_F(KelEvaluatorTest, DurationScale) {
    auto result = eval("2 * 1h");
    EXPECT_EQ(result.as_duration().count(), 7200000);
}

TEST_F(KelEvaluatorTest, DurationScaleReverse) {
    auto result = eval("1h * 3");
    EXPECT_EQ(result.as_duration().count(), 10800000);
}

TEST_F(KelEvaluatorTest, DurationSeconds) {
    EXPECT_EQ(eval("duration_seconds(90s)").as_int(), 90);
}

TEST_F(KelEvaluatorTest, DurationMinutes) {
    EXPECT_EQ(eval("duration_minutes(2h)").as_int(), 120);
}

TEST_F(KelEvaluatorTest, DurationHours) {
    EXPECT_EQ(eval("duration_hours(1d)").as_int(), 24);
}

// ═══════════════════════════════════════════════════════════════════════════
// 'in' operator
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KelEvaluatorTest, InListFound) {
    EXPECT_TRUE(eval("3 in [1, 2, 3, 4]").as_bool());
}

TEST_F(KelEvaluatorTest, InListNotFound) {
    EXPECT_FALSE(eval("5 in [1, 2, 3, 4]").as_bool());
}

TEST_F(KelEvaluatorTest, InStringList) {
    EXPECT_TRUE(eval("\"b\" in [\"a\", \"b\", \"c\"]").as_bool());
}

TEST_F(KelEvaluatorTest, InEmptyList) {
    EXPECT_FALSE(eval("1 in []").as_bool());
}

TEST_F(KelEvaluatorTest, InRequiresList) {
    EXPECT_THROW(eval("1 in 42"), KelEvalError);
}

// ═══════════════════════════════════════════════════════════════════════════
// General-purpose built-in functions
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KelEvaluatorTest, FnMin)  { EXPECT_EQ(eval("min(3, 7)").as_int(), 3); }
TEST_F(KelEvaluatorTest, FnMax)  { EXPECT_EQ(eval("max(3, 7)").as_int(), 7); }
TEST_F(KelEvaluatorTest, FnAbs)  { EXPECT_EQ(eval("abs(-5)").as_int(), 5); }
TEST_F(KelEvaluatorTest, FnAbsF) { EXPECT_DOUBLE_EQ(eval("abs(-3.14)").as_float(), 3.14); }

TEST_F(KelEvaluatorTest, FnLenString) {
    EXPECT_EQ(eval("len(\"hello\")").as_int(), 5);
}

TEST_F(KelEvaluatorTest, FnLenList) {
    EXPECT_EQ(eval("len([1, 2, 3])").as_int(), 3);
}

TEST_F(KelEvaluatorTest, FnLenEmpty) {
    EXPECT_EQ(eval("len(\"\")").as_int(), 0);
}

TEST_F(KelEvaluatorTest, FnSum) {
    EXPECT_EQ(eval("sum([1, 2, 3, 4])").as_int(), 10);
}

TEST_F(KelEvaluatorTest, FnSumFloat) {
    EXPECT_DOUBLE_EQ(eval("sum([1.5, 2.5])").as_float(), 4.0);
}

TEST_F(KelEvaluatorTest, FnSumEmpty) {
    EXPECT_EQ(eval("sum([])").as_int(), 0);
}

TEST_F(KelEvaluatorTest, FnAvg) {
    EXPECT_DOUBLE_EQ(eval("avg([2, 4, 6])").as_float(), 4.0);
}

TEST_F(KelEvaluatorTest, FnRound) {
    EXPECT_EQ(eval("round(3.7)").as_int(), 4);
    EXPECT_EQ(eval("round(3.2)").as_int(), 3);
}

TEST_F(KelEvaluatorTest, FnFloor) {
    EXPECT_EQ(eval("floor(3.9)").as_int(), 3);
}

TEST_F(KelEvaluatorTest, FnCeil) {
    EXPECT_EQ(eval("ceil(3.1)").as_int(), 4);
}

// ═══════════════════════════════════════════════════════════════════════════
// Type conversion functions
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KelEvaluatorTest, FnIntFromFloat) {
    EXPECT_EQ(eval("int(3.7)").as_int(), 3);
}

TEST_F(KelEvaluatorTest, FnIntFromString) {
    EXPECT_EQ(eval("int(\"42\")").as_int(), 42);
}

TEST_F(KelEvaluatorTest, FnIntFromBool) {
    EXPECT_EQ(eval("int(true)").as_int(), 1);
    EXPECT_EQ(eval("int(false)").as_int(), 0);
}

TEST_F(KelEvaluatorTest, FnFloatFromInt) {
    EXPECT_DOUBLE_EQ(eval("float(42)").as_float(), 42.0);
}

TEST_F(KelEvaluatorTest, FnFloatFromString) {
    EXPECT_DOUBLE_EQ(eval("float(\"3.14\")").as_float(), 3.14);
}

TEST_F(KelEvaluatorTest, FnStr) {
    EXPECT_EQ(eval("str(42)").as_string(), "42");
}

TEST_F(KelEvaluatorTest, FnBoolTruthy) {
    EXPECT_TRUE(eval("bool(1)").as_bool());
    EXPECT_FALSE(eval("bool(0)").as_bool());
    EXPECT_TRUE(eval("bool(\"hello\")").as_bool());
    EXPECT_FALSE(eval("bool(\"\")").as_bool());
}

// ═══════════════════════════════════════════════════════════════════════════
// Variables
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KelEvaluatorTest, VariableLookup) {
    ctx.variables["x"] = KelValue(42);
    EXPECT_EQ(eval("x").as_int(), 42);
}

TEST_F(KelEvaluatorTest, VariableInExpression) {
    ctx.variables["x"] = KelValue(10);
    ctx.variables["y"] = KelValue(20);
    EXPECT_EQ(eval("x + y").as_int(), 30);
}

TEST_F(KelEvaluatorTest, UnknownVariable) {
    EXPECT_THROW(eval("unknown_var"), KelEvalError);
}

// ═══════════════════════════════════════════════════════════════════════════
// Member access and method calls (with custom resolvers)
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KelEvaluatorTest, MemberAccess) {
    ctx.variables["event"] = KelValue(std::string("test_event"));
    ctx.members["string.type"] = [](const KelValue&) -> KelValue {
        return KelValue(std::string("created"));
    };
    EXPECT_EQ(eval("event.type").as_string(), "created");
}

TEST_F(KelEvaluatorTest, MethodCallWithArgs) {
    // Simulate job("build").finished_within(30m).
    ctx.functions["job"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 1 || !args[0].is_string())
            throw KelEvalError("job() requires a string argument");
        return KelValue(std::string("job_ref:" + args[0].as_string()));
    };
    ctx.methods["finished_within"] = [](const KelValue& obj,
                                        const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 1 || !args[0].is_duration())
            throw KelEvalError("finished_within() requires a duration");
        // Simulate: build finished 20 minutes ago.
        auto threshold = args[0].as_duration();
        auto elapsed = std::chrono::minutes(20);
        return KelValue(elapsed < threshold);
    };
    ctx.members["string.last_success"] = [](const KelValue& obj) -> KelValue {
        auto& ref = obj.as_string();
        return KelValue(ref.find("build") != std::string::npos);
    };

    EXPECT_TRUE(eval("job(\"build\").finished_within(1h)").as_bool());
    EXPECT_FALSE(eval("job(\"build\").finished_within(10m)").as_bool());
    EXPECT_TRUE(eval("job(\"build\").last_success").as_bool());
}

TEST_F(KelEvaluatorTest, CompoundJobCondition) {
    // Simulate full AVScheduler-style condition.
    ctx.functions["job"] = [](const std::vector<KelValue>& args) -> KelValue {
        return KelValue(std::string("job_ref:" + args[0].as_string()));
    };
    ctx.members["string.last_success"] = [](const KelValue& obj) -> KelValue {
        auto& ref = obj.as_string();
        bool success = (ref.find("build") != std::string::npos ||
                        ref.find("test") != std::string::npos);
        return KelValue(success);
    };
    ctx.methods["finished_within"] = [](const KelValue&,
                                        const std::vector<KelValue>& args) -> KelValue {
        return KelValue(true);
    };

    EXPECT_TRUE(eval(
        "job(\"build\").last_success and job(\"test\").finished_within(30m)"
    ).as_bool());
}

// ═══════════════════════════════════════════════════════════════════════════
// Error handling
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KelEvaluatorTest, DivisionByZero) {
    EXPECT_THROW(eval("10 / 0"), KelEvalError);
}

TEST_F(KelEvaluatorTest, ModuloByZero) {
    EXPECT_THROW(eval("10 % 0"), KelEvalError);
}

TEST_F(KelEvaluatorTest, FloatDivisionByZero) {
    EXPECT_THROW(eval("10.0 / 0.0"), KelEvalError);
}

TEST_F(KelEvaluatorTest, NotOnNonBool) {
    EXPECT_THROW(eval("not 42"), KelEvalError);
}

TEST_F(KelEvaluatorTest, AndOnNonBool) {
    EXPECT_THROW(eval("42 and true"), KelEvalError);
}

TEST_F(KelEvaluatorTest, NegateString) {
    EXPECT_THROW(eval("-\"hello\""), KelEvalError);
}

TEST_F(KelEvaluatorTest, UnknownFunction) {
    EXPECT_THROW(eval("nonexistent_func(42)"), KelEvalError);
}

TEST_F(KelEvaluatorTest, WrongArgCount) {
    EXPECT_THROW(eval("abs(1, 2)"), KelEvalError);
}

TEST_F(KelEvaluatorTest, WrongArgType) {
    EXPECT_THROW(eval("abs(\"hello\")"), KelEvalError);
}

TEST_F(KelEvaluatorTest, IntConversionBadString) {
    EXPECT_THROW(eval("int(\"not_a_number\")"), KelEvalError);
}

// ═══════════════════════════════════════════════════════════════════════════
// Sandboxing limits
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KelEvaluatorTest, NodeCountLimit) {
    // Build an expression exceeding the default 1024 node limit.
    std::string huge = "1";
    for (int i = 0; i < 1100; ++i) huge += " + 1";
    EXPECT_THROW(eval_expression(huge, ctx, limits), KelLimitError);
}

TEST_F(KelEvaluatorTest, SmallNodeLimit) {
    limits.max_ast_nodes = 3;
    // "1 + 2" has 3 nodes → should succeed.
    EXPECT_EQ(eval_expression("1 + 2", ctx, limits).as_int(), 3);
    // "1 + 2 + 3" has 5 nodes → should fail.
    EXPECT_THROW(eval_expression("1 + 2 + 3", ctx, limits), KelLimitError);
}

TEST_F(KelEvaluatorTest, StringLengthLimit) {
    limits.max_string_length = 10;
    EXPECT_THROW(eval_expression("\"hello\" + \" \" + \"world!\"", ctx, limits),
                 KelLimitError);
}

TEST_F(KelEvaluatorTest, ListLengthLimit) {
    limits.max_list_length = 3;
    EXPECT_THROW(eval_expression("[1, 2, 3, 4]", ctx, limits), KelLimitError);
}

// ═══════════════════════════════════════════════════════════════════════════
// Edge cases
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KelEvaluatorTest, NestedParens) {
    EXPECT_EQ(eval("((((42))))").as_int(), 42);
}

TEST_F(KelEvaluatorTest, EmptyList) {
    auto result = eval("[]");
    EXPECT_TRUE(result.is_list());
    EXPECT_EQ(result.as_list().size(), 0);
}

TEST_F(KelEvaluatorTest, BoolInList) {
    EXPECT_TRUE(eval("true in [false, true]").as_bool());
}

TEST_F(KelEvaluatorTest, DurationEqual) {
    EXPECT_TRUE(eval("60s == 1m").as_bool());
}

TEST_F(KelEvaluatorTest, DurationEqualMinHour) {
    EXPECT_TRUE(eval("60m == 1h").as_bool());
}

TEST_F(KelEvaluatorTest, ComplexExpression) {
    // EventWatcher-style aggregate check.
    ctx.variables["total_size"] = KelValue(int64_t(11 * 1024 * 1024 * 1024LL));
    EXPECT_TRUE(eval("total_size > 10 * 1024 * 1024 * 1024").as_bool());
}

TEST_F(KelEvaluatorTest, StringInExpression) {
    ctx.variables["event_type"] = KelValue(std::string("content_changed"));
    EXPECT_TRUE(eval("event_type == \"content_changed\"").as_bool());
}

TEST_F(KelEvaluatorTest, NestedFunctionCalls) {
    EXPECT_EQ(eval("max(min(5, 3), abs(-7))").as_int(), 7);
}

TEST_F(KelEvaluatorTest, FunctionAsArg) {
    EXPECT_EQ(eval("min(abs(-5), abs(-3))").as_int(), 3);
}

TEST_F(KelEvaluatorTest, MinFloat) {
    EXPECT_DOUBLE_EQ(eval("min(1.5, 2.5)").as_float(), 1.5);
}

// ═══════════════════════════════════════════════════════════════════════════
// Parse errors rejected at evaluation
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KelEvaluatorTest, ParseErrorRejectsDefKeyword) {
    EXPECT_THROW(eval("def f(x) = x + 1"), KelParseError);
}

TEST_F(KelEvaluatorTest, ParseErrorRejectsAssignment) {
    EXPECT_THROW(eval("x = 5"), KelParseError);
}
