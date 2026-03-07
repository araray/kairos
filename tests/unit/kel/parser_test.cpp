/// tests/unit/kel/parser_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  KEL Parser unit tests (~80 tests)                                        ║
// ║  Spec reference: §7.4–§7.5                                               ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/kel/ast.hpp"
#include "kairos/kel/errors.hpp"

#include <gtest/gtest.h>

using namespace kairos::kel;

// ─── Helpers ──────────────────────────────────────────────────────────────

/// Check that a node is a literal with expected int value.
void expect_int_literal(const AstNode& node, int64_t expected) {
    auto* lit = std::get_if<LiteralNode>(&node);
    ASSERT_NE(lit, nullptr) << "Expected LiteralNode";
    ASSERT_TRUE(lit->value.is_int()) << "Expected int literal";
    EXPECT_EQ(lit->value.as_int(), expected);
}

void expect_bool_literal(const AstNode& node, bool expected) {
    auto* lit = std::get_if<LiteralNode>(&node);
    ASSERT_NE(lit, nullptr) << "Expected LiteralNode";
    ASSERT_TRUE(lit->value.is_bool()) << "Expected bool literal";
    EXPECT_EQ(lit->value.as_bool(), expected);
}

void expect_string_literal(const AstNode& node, const std::string& expected) {
    auto* lit = std::get_if<LiteralNode>(&node);
    ASSERT_NE(lit, nullptr) << "Expected LiteralNode";
    ASSERT_TRUE(lit->value.is_string()) << "Expected string literal";
    EXPECT_EQ(lit->value.as_string(), expected);
}

void expect_identifier(const AstNode& node, const std::string& expected) {
    auto* id = std::get_if<IdentifierNode>(&node);
    ASSERT_NE(id, nullptr) << "Expected IdentifierNode";
    EXPECT_EQ(id->name, expected);
}

void expect_binary(const AstNode& node, TokenType op) {
    auto* bin = std::get_if<BinaryOpNode>(&node);
    ASSERT_NE(bin, nullptr) << "Expected BinaryOpNode";
    EXPECT_EQ(bin->op, op);
}

void expect_unary(const AstNode& node, TokenType op) {
    auto* un = std::get_if<UnaryOpNode>(&node);
    ASSERT_NE(un, nullptr) << "Expected UnaryOpNode";
    EXPECT_EQ(un->op, op);
}

// ═══════════════════════════════════════════════════════════════════════════
// Literal parsing
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelParserTest, IntLiteral) {
    auto ast = parse("42");
    expect_int_literal(*ast, 42);
}

TEST(KelParserTest, FloatLiteral) {
    auto ast = parse("3.14");
    auto* lit = std::get_if<LiteralNode>(ast.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_TRUE(lit->value.is_float());
    EXPECT_DOUBLE_EQ(lit->value.as_float(), 3.14);
}

TEST(KelParserTest, StringLiteral) {
    auto ast = parse("\"hello\"");
    expect_string_literal(*ast, "hello");
}

TEST(KelParserTest, BoolTrueLiteral) {
    auto ast = parse("true");
    expect_bool_literal(*ast, true);
}

TEST(KelParserTest, BoolFalseLiteral) {
    auto ast = parse("false");
    expect_bool_literal(*ast, false);
}

TEST(KelParserTest, DurationLiteral) {
    auto ast = parse("30m");
    auto* lit = std::get_if<LiteralNode>(ast.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_TRUE(lit->value.is_duration());
    EXPECT_EQ(lit->value.as_duration().count(), 1800000);
}

TEST(KelParserTest, NegativeInt) {
    auto ast = parse("-5");
    expect_unary(*ast, TokenType::Minus);
}

// ═══════════════════════════════════════════════════════════════════════════
// Identifier parsing
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelParserTest, SimpleIdentifier) {
    auto ast = parse("foo");
    expect_identifier(*ast, "foo");
}

TEST(KelParserTest, UnderscoreIdentifier) {
    auto ast = parse("my_var");
    expect_identifier(*ast, "my_var");
}

// ═══════════════════════════════════════════════════════════════════════════
// Binary operations
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelParserTest, Addition) {
    auto ast = parse("1 + 2");
    expect_binary(*ast, TokenType::Plus);
}

TEST(KelParserTest, Subtraction) {
    auto ast = parse("5 - 3");
    expect_binary(*ast, TokenType::Minus);
}

TEST(KelParserTest, Multiplication) {
    auto ast = parse("2 * 3");
    expect_binary(*ast, TokenType::Star);
}

TEST(KelParserTest, Division) {
    auto ast = parse("10 / 2");
    expect_binary(*ast, TokenType::Slash);
}

TEST(KelParserTest, Modulo) {
    auto ast = parse("10 % 3");
    expect_binary(*ast, TokenType::Percent);
}

TEST(KelParserTest, Comparison) {
    auto ast = parse("a < b");
    expect_binary(*ast, TokenType::Less);
}

TEST(KelParserTest, ComparisonLessEqual) {
    auto ast = parse("a <= b");
    expect_binary(*ast, TokenType::LessEqual);
}

TEST(KelParserTest, ComparisonGreater) {
    auto ast = parse("a > b");
    expect_binary(*ast, TokenType::Greater);
}

TEST(KelParserTest, ComparisonGreaterEqual) {
    auto ast = parse("a >= b");
    expect_binary(*ast, TokenType::GreaterEqual);
}

TEST(KelParserTest, Equality) {
    auto ast = parse("x == y");
    expect_binary(*ast, TokenType::EqualEqual);
}

TEST(KelParserTest, Inequality) {
    auto ast = parse("x != y");
    expect_binary(*ast, TokenType::BangEqual);
}

TEST(KelParserTest, LogicalAnd) {
    auto ast = parse("true and false");
    expect_binary(*ast, TokenType::And);
}

TEST(KelParserTest, LogicalOr) {
    auto ast = parse("true or false");
    expect_binary(*ast, TokenType::Or);
}

TEST(KelParserTest, InOperator) {
    auto ast = parse("x in [1, 2, 3]");
    expect_binary(*ast, TokenType::In);
}

// ═══════════════════════════════════════════════════════════════════════════
// Operator precedence
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelParserTest, PrecedenceMulOverAdd) {
    // 2 + 3 * 4 should parse as 2 + (3 * 4)
    auto ast = parse("2 + 3 * 4");
    auto* bin = std::get_if<BinaryOpNode>(ast.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, TokenType::Plus);
    expect_int_literal(*bin->left, 2);
    // Right should be (3 * 4)
    auto* right = std::get_if<BinaryOpNode>(bin->right.get());
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(right->op, TokenType::Star);
}

TEST(KelParserTest, PrecedenceAndOverOr) {
    // a or b and c should parse as a or (b and c)
    auto ast = parse("a or b and c");
    auto* bin = std::get_if<BinaryOpNode>(ast.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, TokenType::Or);
    expect_identifier(*bin->left, "a");
    auto* right = std::get_if<BinaryOpNode>(bin->right.get());
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(right->op, TokenType::And);
}

TEST(KelParserTest, PrecedenceNotOverAnd) {
    // not a and b should parse as (not a) and b
    auto ast = parse("not a and b");
    auto* bin = std::get_if<BinaryOpNode>(ast.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, TokenType::And);
    expect_unary(*bin->left, TokenType::Not);
}

TEST(KelParserTest, PrecedenceComparisonOverLogical) {
    // a < b and c > d
    auto ast = parse("a < b and c > d");
    auto* bin = std::get_if<BinaryOpNode>(ast.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, TokenType::And);
    expect_binary(*bin->left, TokenType::Less);
    expect_binary(*bin->right, TokenType::Greater);
}

TEST(KelParserTest, ParenthesesOverridePrecedence) {
    // (2 + 3) * 4
    auto ast = parse("(2 + 3) * 4");
    auto* bin = std::get_if<BinaryOpNode>(ast.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, TokenType::Star);
    expect_binary(*bin->left, TokenType::Plus);
    expect_int_literal(*bin->right, 4);
}

TEST(KelParserTest, LeftAssociativity) {
    // 1 - 2 - 3 should parse as (1 - 2) - 3
    auto ast = parse("1 - 2 - 3");
    auto* bin = std::get_if<BinaryOpNode>(ast.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, TokenType::Minus);
    expect_int_literal(*bin->right, 3);
    // Left should be (1 - 2)
    expect_binary(*bin->left, TokenType::Minus);
}

// ═══════════════════════════════════════════════════════════════════════════
// Function calls
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelParserTest, FunctionCallNoArgs) {
    auto ast = parse("now()");
    auto* call = std::get_if<FunctionCallNode>(ast.get());
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->name, "now");
    EXPECT_EQ(call->args.size(), 0);
}

TEST(KelParserTest, FunctionCallOneArg) {
    auto ast = parse("abs(42)");
    auto* call = std::get_if<FunctionCallNode>(ast.get());
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->name, "abs");
    ASSERT_EQ(call->args.size(), 1);
    expect_int_literal(*call->args[0], 42);
}

TEST(KelParserTest, FunctionCallMultipleArgs) {
    auto ast = parse("min(1, 2)");
    auto* call = std::get_if<FunctionCallNode>(ast.get());
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->name, "min");
    EXPECT_EQ(call->args.size(), 2);
}

TEST(KelParserTest, FunctionCallTrailingComma) {
    auto ast = parse("min(1, 2,)");
    auto* call = std::get_if<FunctionCallNode>(ast.get());
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->args.size(), 2);
}

// ═══════════════════════════════════════════════════════════════════════════
// Member access and method calls
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelParserTest, MemberAccess) {
    auto ast = parse("event.type");
    auto* mem = std::get_if<MemberAccessNode>(ast.get());
    ASSERT_NE(mem, nullptr);
    EXPECT_EQ(mem->member, "type");
    expect_identifier(*mem->object, "event");
}

TEST(KelParserTest, ChainedMemberAccess) {
    auto ast = parse("a.b.c");
    auto* mem = std::get_if<MemberAccessNode>(ast.get());
    ASSERT_NE(mem, nullptr);
    EXPECT_EQ(mem->member, "c");
    auto* inner = std::get_if<MemberAccessNode>(mem->object.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->member, "b");
}

TEST(KelParserTest, MethodCall) {
    auto ast = parse("job(\"build\").finished_within(30m)");
    auto* method = std::get_if<MethodCallNode>(ast.get());
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->method, "finished_within");
    ASSERT_EQ(method->args.size(), 1);
}

TEST(KelParserTest, MethodCallNoArgs) {
    // Member access (property), not method call: job("build").last_success
    auto ast = parse("job(\"build\").last_success");
    auto* mem = std::get_if<MemberAccessNode>(ast.get());
    ASSERT_NE(mem, nullptr);
    EXPECT_EQ(mem->member, "last_success");
}

TEST(KelParserTest, FunctionThenMemberAccess) {
    auto ast = parse("job(\"build\").last_success");
    auto* mem = std::get_if<MemberAccessNode>(ast.get());
    ASSERT_NE(mem, nullptr);
    auto* call = std::get_if<FunctionCallNode>(mem->object.get());
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->name, "job");
}

// ═══════════════════════════════════════════════════════════════════════════
// List literals
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelParserTest, EmptyList) {
    auto ast = parse("[]");
    auto* lst = std::get_if<ListLiteralNode>(ast.get());
    ASSERT_NE(lst, nullptr);
    EXPECT_EQ(lst->elements.size(), 0);
}

TEST(KelParserTest, SingleElementList) {
    auto ast = parse("[42]");
    auto* lst = std::get_if<ListLiteralNode>(ast.get());
    ASSERT_NE(lst, nullptr);
    ASSERT_EQ(lst->elements.size(), 1);
    expect_int_literal(*lst->elements[0], 42);
}

TEST(KelParserTest, MultiElementList) {
    auto ast = parse("[1, 2, 3]");
    auto* lst = std::get_if<ListLiteralNode>(ast.get());
    ASSERT_NE(lst, nullptr);
    EXPECT_EQ(lst->elements.size(), 3);
}

TEST(KelParserTest, ListTrailingComma) {
    auto ast = parse("[1, 2,]");
    auto* lst = std::get_if<ListLiteralNode>(ast.get());
    ASSERT_NE(lst, nullptr);
    EXPECT_EQ(lst->elements.size(), 2);
}

TEST(KelParserTest, MixedTypeList) {
    auto ast = parse("[1, \"hello\", true]");
    auto* lst = std::get_if<ListLiteralNode>(ast.get());
    ASSERT_NE(lst, nullptr);
    EXPECT_EQ(lst->elements.size(), 3);
}

// ═══════════════════════════════════════════════════════════════════════════
// Error handling
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelParserTest, UnexpectedEof) {
    EXPECT_THROW(parse("1 +"), KelParseError);
}

TEST(KelParserTest, MismatchedParen) {
    EXPECT_THROW(parse("(1 + 2"), KelParseError);
}

TEST(KelParserTest, MismatchedBracket) {
    EXPECT_THROW(parse("[1, 2"), KelParseError);
}

TEST(KelParserTest, ChainedComparison) {
    // a < b < c is explicitly rejected.
    EXPECT_THROW(parse("1 < 2 < 3"), KelParseError);
}

TEST(KelParserTest, TrailingJunk) {
    EXPECT_THROW(parse("42 foo"), KelParseError);
}

TEST(KelParserTest, EmptyExpression) {
    EXPECT_THROW(parse(""), KelParseError);
}

TEST(KelParserTest, MissingDotMember) {
    EXPECT_THROW(parse("foo."), KelParseError);
}

TEST(KelParserTest, ErrorHasSourceLocation) {
    try {
        parse("1 +");
        FAIL() << "Expected KelParseError";
    } catch (const KelParseError& e) {
        // Should have a meaningful offset.
        EXPECT_GE(e.offset(), 0);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// AST node count
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelParserTest, NodeCountLiteral) {
    auto ast = parse("42");
    EXPECT_EQ(ast_node_count(*ast), 1);
}

TEST(KelParserTest, NodeCountBinaryOp) {
    auto ast = parse("1 + 2");
    EXPECT_EQ(ast_node_count(*ast), 3);  // binary + two literals
}

TEST(KelParserTest, NodeCountFunctionCall) {
    auto ast = parse("min(1, 2)");
    EXPECT_EQ(ast_node_count(*ast), 3);  // call + two literal args
}

TEST(KelParserTest, NodeCountComplex) {
    auto ast = parse("1 + 2 * 3");
    EXPECT_EQ(ast_node_count(*ast), 5);  // plus(1, mult(2, 3))
}

// ═══════════════════════════════════════════════════════════════════════════
// Complex real-world expressions
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelParserTest, AvSchedulerCondition) {
    // Legacy: job_A.last_run_successful and job_B.finished_within(2h)
    auto ast = parse(
        "job(\"job_A\").last_success and job(\"job_B\").finished_within(2h)");
    expect_binary(*ast, TokenType::And);
}

TEST(KelParserTest, EventWatcherCondition) {
    auto ast = parse(
        "aggregate(data, \"*.log\", \"size\", \"sum\") > 10 * 1024 * 1024");
    expect_binary(*ast, TokenType::Greater);
}

TEST(KelParserTest, ComplexBooleanChain) {
    auto ast = parse(
        "a > 0 and b > 0 or c == \"done\"");
    // Should parse as (a > 0 and b > 0) or (c == "done")
    expect_binary(*ast, TokenType::Or);
}

TEST(KelParserTest, NestedFunctionCalls) {
    auto ast = parse("max(min(1, 2), abs(-3))");
    auto* call = std::get_if<FunctionCallNode>(ast.get());
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->name, "max");
    EXPECT_EQ(call->args.size(), 2);
}

TEST(KelParserTest, DurationArithmetic) {
    auto ast = parse("1h + 30m");
    expect_binary(*ast, TokenType::Plus);
}

TEST(KelParserTest, InWithListExpression) {
    auto ast = parse("event.type in [\"created\", \"modified\", \"deleted\"]");
    expect_binary(*ast, TokenType::In);
}
