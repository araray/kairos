/// tests/unit/kel/lexer_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  KEL Lexer unit tests (~80 tests)                                         ║
// ║  Spec reference: §7.3                                                     ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/kel/token.hpp"

#include <gtest/gtest.h>

using namespace kairos::kel;

// ─── Helpers ──────────────────────────────────────────────────────────────

#define EXPECT_TOKEN(tokens, idx, expected_type, expected_lexeme) \
    do { \
        ASSERT_GT(tokens.size(), static_cast<size_t>(idx)) \
            << "Token index " << idx << " out of range"; \
        EXPECT_EQ(tokens[idx].type, expected_type) \
            << "Token[" << idx << "] type mismatch"; \
        EXPECT_EQ(tokens[idx].lexeme, expected_lexeme) \
            << "Token[" << idx << "] lexeme mismatch"; \
    } while (0)

// ═══════════════════════════════════════════════════════════════════════════
// Integer literals
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelLexerTest, IntegerZero) {
    auto tokens = tokenize("0");
    EXPECT_TOKEN(tokens, 0, TokenType::IntLiteral, "0");
    EXPECT_EQ(tokens[0].int_value, 0);
}

TEST(KelLexerTest, IntegerPositive) {
    auto tokens = tokenize("42");
    EXPECT_TOKEN(tokens, 0, TokenType::IntLiteral, "42");
    EXPECT_EQ(tokens[0].int_value, 42);
}

TEST(KelLexerTest, IntegerLarge) {
    auto tokens = tokenize("1234567890");
    EXPECT_EQ(tokens[0].int_value, 1234567890);
}

TEST(KelLexerTest, HexLiteral) {
    auto tokens = tokenize("0xFF");
    EXPECT_TOKEN(tokens, 0, TokenType::IntLiteral, "0xFF");
    EXPECT_EQ(tokens[0].int_value, 255);
}

TEST(KelLexerTest, HexLiteralUppercase) {
    auto tokens = tokenize("0XAB");
    EXPECT_EQ(tokens[0].int_value, 0xAB);
}

TEST(KelLexerTest, BinaryLiteral) {
    auto tokens = tokenize("0b1010");
    EXPECT_TOKEN(tokens, 0, TokenType::IntLiteral, "0b1010");
    EXPECT_EQ(tokens[0].int_value, 10);
}

TEST(KelLexerTest, BinaryLiteralUppercase) {
    auto tokens = tokenize("0B1100");
    EXPECT_EQ(tokens[0].int_value, 12);
}

// ═══════════════════════════════════════════════════════════════════════════
// Float literals
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelLexerTest, FloatSimple) {
    auto tokens = tokenize("3.14");
    EXPECT_TOKEN(tokens, 0, TokenType::FloatLiteral, "3.14");
    EXPECT_DOUBLE_EQ(tokens[0].float_value, 3.14);
}

TEST(KelLexerTest, FloatLeadingZero) {
    auto tokens = tokenize("0.5");
    EXPECT_DOUBLE_EQ(tokens[0].float_value, 0.5);
}

TEST(KelLexerTest, FloatExponent) {
    auto tokens = tokenize("1e10");
    EXPECT_TOKEN(tokens, 0, TokenType::FloatLiteral, "1e10");
    EXPECT_DOUBLE_EQ(tokens[0].float_value, 1e10);
}

TEST(KelLexerTest, FloatExponentNeg) {
    auto tokens = tokenize("2.5e-3");
    EXPECT_DOUBLE_EQ(tokens[0].float_value, 2.5e-3);
}

TEST(KelLexerTest, FloatExponentPos) {
    auto tokens = tokenize("1E+5");
    EXPECT_DOUBLE_EQ(tokens[0].float_value, 1e5);
}

// ═══════════════════════════════════════════════════════════════════════════
// Duration literals
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelLexerTest, DurationMilliseconds) {
    auto tokens = tokenize("500ms");
    EXPECT_TOKEN(tokens, 0, TokenType::DurationLiteral, "500ms");
    EXPECT_EQ(tokens[0].duration_ms, 500);
}

TEST(KelLexerTest, DurationSeconds) {
    auto tokens = tokenize("90s");
    EXPECT_EQ(tokens[0].duration_ms, 90000);
}

TEST(KelLexerTest, DurationMinutes) {
    auto tokens = tokenize("30m");
    EXPECT_EQ(tokens[0].duration_ms, 1800000);
}

TEST(KelLexerTest, DurationHours) {
    auto tokens = tokenize("2h");
    EXPECT_EQ(tokens[0].duration_ms, 7200000);
}

TEST(KelLexerTest, DurationDays) {
    auto tokens = tokenize("1d");
    EXPECT_EQ(tokens[0].duration_ms, 86400000);
}

TEST(KelLexerTest, DurationZeroSeconds) {
    auto tokens = tokenize("0s");
    EXPECT_EQ(tokens[0].duration_ms, 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// String literals
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelLexerTest, StringDouble) {
    auto tokens = tokenize("\"hello\"");
    EXPECT_TOKEN(tokens, 0, TokenType::StringLiteral, "hello");
}

TEST(KelLexerTest, StringSingle) {
    auto tokens = tokenize("'world'");
    EXPECT_TOKEN(tokens, 0, TokenType::StringLiteral, "world");
}

TEST(KelLexerTest, StringEmpty) {
    auto tokens = tokenize("\"\"");
    EXPECT_TOKEN(tokens, 0, TokenType::StringLiteral, "");
}

TEST(KelLexerTest, StringEscapeNewline) {
    auto tokens = tokenize("\"line1\\nline2\"");
    EXPECT_EQ(tokens[0].lexeme, "line1\nline2");
}

TEST(KelLexerTest, StringEscapeTab) {
    auto tokens = tokenize("\"col1\\tcol2\"");
    EXPECT_EQ(tokens[0].lexeme, "col1\tcol2");
}

TEST(KelLexerTest, StringEscapeBackslash) {
    auto tokens = tokenize("\"path\\\\to\\\\file\"");
    EXPECT_EQ(tokens[0].lexeme, "path\\to\\file");
}

TEST(KelLexerTest, StringEscapeQuote) {
    auto tokens = tokenize("\"she said \\\"hi\\\"\"");
    EXPECT_EQ(tokens[0].lexeme, "she said \"hi\"");
}

TEST(KelLexerTest, StringUnterminated) {
    auto tokens = tokenize("\"unterminated");
    EXPECT_EQ(tokens[0].type, TokenType::Error);
}

TEST(KelLexerTest, StringUnknownEscape) {
    auto tokens = tokenize("\"bad\\x\"");
    EXPECT_EQ(tokens[0].type, TokenType::Error);
}

// ═══════════════════════════════════════════════════════════════════════════
// Boolean keywords
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelLexerTest, BoolTrue) {
    auto tokens = tokenize("true");
    EXPECT_TOKEN(tokens, 0, TokenType::BoolTrue, "true");
}

TEST(KelLexerTest, BoolFalse) {
    auto tokens = tokenize("false");
    EXPECT_TOKEN(tokens, 0, TokenType::BoolFalse, "false");
}

// ═══════════════════════════════════════════════════════════════════════════
// Keywords and identifiers
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelLexerTest, KeywordAnd) {
    auto tokens = tokenize("and");
    EXPECT_TOKEN(tokens, 0, TokenType::And, "and");
}

TEST(KelLexerTest, KeywordOr) {
    auto tokens = tokenize("or");
    EXPECT_TOKEN(tokens, 0, TokenType::Or, "or");
}

TEST(KelLexerTest, KeywordNot) {
    auto tokens = tokenize("not");
    EXPECT_TOKEN(tokens, 0, TokenType::Not, "not");
}

TEST(KelLexerTest, KeywordIn) {
    auto tokens = tokenize("in");
    EXPECT_TOKEN(tokens, 0, TokenType::In, "in");
}

TEST(KelLexerTest, IdentifierSimple) {
    auto tokens = tokenize("foo");
    EXPECT_TOKEN(tokens, 0, TokenType::Identifier, "foo");
}

TEST(KelLexerTest, IdentifierUnderscore) {
    auto tokens = tokenize("my_var");
    EXPECT_TOKEN(tokens, 0, TokenType::Identifier, "my_var");
}

TEST(KelLexerTest, IdentifierStartUnderscore) {
    auto tokens = tokenize("_private");
    EXPECT_TOKEN(tokens, 0, TokenType::Identifier, "_private");
}

TEST(KelLexerTest, IdentifierWithDigits) {
    auto tokens = tokenize("var2");
    EXPECT_TOKEN(tokens, 0, TokenType::Identifier, "var2");
}

TEST(KelLexerTest, IdentifierNotKeyword) {
    // "android" starts with "and" but is not the keyword "and".
    auto tokens = tokenize("android");
    EXPECT_TOKEN(tokens, 0, TokenType::Identifier, "android");
}

TEST(KelLexerTest, IdentifierNotKeyword2) {
    auto tokens = tokenize("orchid");
    EXPECT_TOKEN(tokens, 0, TokenType::Identifier, "orchid");
}

// ═══════════════════════════════════════════════════════════════════════════
// Operators
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelLexerTest, OperatorPlus)         { EXPECT_EQ(tokenize("+")[0].type, TokenType::Plus); }
TEST(KelLexerTest, OperatorMinus)        { EXPECT_EQ(tokenize("-")[0].type, TokenType::Minus); }
TEST(KelLexerTest, OperatorStar)         { EXPECT_EQ(tokenize("*")[0].type, TokenType::Star); }
TEST(KelLexerTest, OperatorSlash)        { EXPECT_EQ(tokenize("/")[0].type, TokenType::Slash); }
TEST(KelLexerTest, OperatorPercent)      { EXPECT_EQ(tokenize("%")[0].type, TokenType::Percent); }
TEST(KelLexerTest, OperatorEqualEqual)   { EXPECT_EQ(tokenize("==")[0].type, TokenType::EqualEqual); }
TEST(KelLexerTest, OperatorBangEqual)    { EXPECT_EQ(tokenize("!=")[0].type, TokenType::BangEqual); }
TEST(KelLexerTest, OperatorLess)         { EXPECT_EQ(tokenize("<")[0].type, TokenType::Less); }
TEST(KelLexerTest, OperatorLessEqual)    { EXPECT_EQ(tokenize("<=")[0].type, TokenType::LessEqual); }
TEST(KelLexerTest, OperatorGreater)      { EXPECT_EQ(tokenize(">")[0].type, TokenType::Greater); }
TEST(KelLexerTest, OperatorGreaterEqual) { EXPECT_EQ(tokenize(">=")[0].type, TokenType::GreaterEqual); }

// ═══════════════════════════════════════════════════════════════════════════
// Delimiters
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelLexerTest, LeftParen)    { EXPECT_EQ(tokenize("(")[0].type, TokenType::LeftParen); }
TEST(KelLexerTest, RightParen)   { EXPECT_EQ(tokenize(")")[0].type, TokenType::RightParen); }
TEST(KelLexerTest, LeftBracket)  { EXPECT_EQ(tokenize("[")[0].type, TokenType::LeftBracket); }
TEST(KelLexerTest, RightBracket) { EXPECT_EQ(tokenize("]")[0].type, TokenType::RightBracket); }
TEST(KelLexerTest, Comma)        { EXPECT_EQ(tokenize(",")[0].type, TokenType::Comma); }
TEST(KelLexerTest, Dot)          { EXPECT_EQ(tokenize(".")[0].type, TokenType::Dot); }

// ═══════════════════════════════════════════════════════════════════════════
// Error cases
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelLexerTest, UnexpectedCharacter) {
    auto tokens = tokenize("@");
    EXPECT_EQ(tokens[0].type, TokenType::Error);
}

TEST(KelLexerTest, SingleEquals) {
    auto tokens = tokenize("=");
    EXPECT_EQ(tokens[0].type, TokenType::Error);
}

TEST(KelLexerTest, SingleBang) {
    auto tokens = tokenize("!");
    EXPECT_EQ(tokens[0].type, TokenType::Error);
}

// ═══════════════════════════════════════════════════════════════════════════
// Whitespace handling
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelLexerTest, WhitespaceSkipped) {
    auto tokens = tokenize("  42  ");
    EXPECT_TOKEN(tokens, 0, TokenType::IntLiteral, "42");
    EXPECT_EQ(tokens[1].type, TokenType::Eof);
}

TEST(KelLexerTest, EmptyInput) {
    auto tokens = tokenize("");
    ASSERT_EQ(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::Eof);
}

TEST(KelLexerTest, WhitespaceOnly) {
    auto tokens = tokenize("   \t\n  ");
    ASSERT_EQ(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::Eof);
}

// ═══════════════════════════════════════════════════════════════════════════
// Multi-token expressions
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelLexerTest, SimpleArithmetic) {
    auto tokens = tokenize("2 + 3");
    EXPECT_TOKEN(tokens, 0, TokenType::IntLiteral, "2");
    EXPECT_TOKEN(tokens, 1, TokenType::Plus, "+");
    EXPECT_TOKEN(tokens, 2, TokenType::IntLiteral, "3");
    EXPECT_EQ(tokens[3].type, TokenType::Eof);
}

TEST(KelLexerTest, FunctionCallTokens) {
    auto tokens = tokenize("min(1, 2)");
    EXPECT_TOKEN(tokens, 0, TokenType::Identifier, "min");
    EXPECT_TOKEN(tokens, 1, TokenType::LeftParen, "(");
    EXPECT_TOKEN(tokens, 2, TokenType::IntLiteral, "1");
    EXPECT_TOKEN(tokens, 3, TokenType::Comma, ",");
    EXPECT_TOKEN(tokens, 4, TokenType::IntLiteral, "2");
    EXPECT_TOKEN(tokens, 5, TokenType::RightParen, ")");
}

TEST(KelLexerTest, DotAccessTokens) {
    auto tokens = tokenize("event.type");
    EXPECT_TOKEN(tokens, 0, TokenType::Identifier, "event");
    EXPECT_TOKEN(tokens, 1, TokenType::Dot, ".");
    EXPECT_TOKEN(tokens, 2, TokenType::Identifier, "type");
}

TEST(KelLexerTest, BooleanExprTokens) {
    auto tokens = tokenize("true and not false");
    EXPECT_TOKEN(tokens, 0, TokenType::BoolTrue, "true");
    EXPECT_TOKEN(tokens, 1, TokenType::And, "and");
    EXPECT_TOKEN(tokens, 2, TokenType::Not, "not");
    EXPECT_TOKEN(tokens, 3, TokenType::BoolFalse, "false");
}

TEST(KelLexerTest, ComparisonWithDuration) {
    auto tokens = tokenize("age < 30m");
    EXPECT_TOKEN(tokens, 0, TokenType::Identifier, "age");
    EXPECT_TOKEN(tokens, 1, TokenType::Less, "<");
    EXPECT_TOKEN(tokens, 2, TokenType::DurationLiteral, "30m");
    EXPECT_EQ(tokens[2].duration_ms, 1800000);
}

TEST(KelLexerTest, JobMethodCallTokens) {
    // job("build").finished_within(30m)
    auto tokens = tokenize("job(\"build\").finished_within(30m)");
    EXPECT_TOKEN(tokens, 0, TokenType::Identifier, "job");
    EXPECT_TOKEN(tokens, 1, TokenType::LeftParen, "(");
    EXPECT_TOKEN(tokens, 2, TokenType::StringLiteral, "build");
    EXPECT_TOKEN(tokens, 3, TokenType::RightParen, ")");
    EXPECT_TOKEN(tokens, 4, TokenType::Dot, ".");
    EXPECT_TOKEN(tokens, 5, TokenType::Identifier, "finished_within");
    EXPECT_TOKEN(tokens, 6, TokenType::LeftParen, "(");
    EXPECT_TOKEN(tokens, 7, TokenType::DurationLiteral, "30m");
    EXPECT_TOKEN(tokens, 8, TokenType::RightParen, ")");
}

TEST(KelLexerTest, ListLiteralTokens) {
    auto tokens = tokenize("[1, 2, 3]");
    EXPECT_TOKEN(tokens, 0, TokenType::LeftBracket, "[");
    EXPECT_TOKEN(tokens, 1, TokenType::IntLiteral, "1");
    EXPECT_TOKEN(tokens, 2, TokenType::Comma, ",");
    EXPECT_TOKEN(tokens, 3, TokenType::IntLiteral, "2");
    EXPECT_TOKEN(tokens, 4, TokenType::Comma, ",");
    EXPECT_TOKEN(tokens, 5, TokenType::IntLiteral, "3");
    EXPECT_TOKEN(tokens, 6, TokenType::RightBracket, "]");
}

TEST(KelLexerTest, ComplexExpression) {
    auto tokens = tokenize(
        "job(\"build\").last_success and job(\"test\").finished_within(30m)");
    EXPECT_GT(tokens.size(), 10);  // Should have many tokens.
    EXPECT_EQ(tokens.back().type, TokenType::Eof);
}

TEST(KelLexerTest, OffsetTracking) {
    auto tokens = tokenize("a + b");
    EXPECT_EQ(tokens[0].offset, 0);
    EXPECT_EQ(tokens[1].offset, 2);
    EXPECT_EQ(tokens[2].offset, 4);
}

TEST(KelLexerTest, InOperatorInExpression) {
    auto tokens = tokenize("x in [1, 2, 3]");
    EXPECT_TOKEN(tokens, 0, TokenType::Identifier, "x");
    EXPECT_TOKEN(tokens, 1, TokenType::In, "in");
    EXPECT_TOKEN(tokens, 2, TokenType::LeftBracket, "[");
}

// Duration disambiguation: 'm' as minutes vs. identifier starting with 'm'
TEST(KelLexerTest, IdentifierStartingWithM) {
    auto tokens = tokenize("my_var");
    EXPECT_TOKEN(tokens, 0, TokenType::Identifier, "my_var");
}

TEST(KelLexerTest, DurationMsNotIdentifier) {
    // "100ms" should be a duration, not "100" + identifier "ms"
    auto tokens = tokenize("100ms");
    ASSERT_EQ(tokens.size(), 2);  // DurationLiteral + Eof
    EXPECT_EQ(tokens[0].type, TokenType::DurationLiteral);
    EXPECT_EQ(tokens[0].duration_ms, 100);
}
