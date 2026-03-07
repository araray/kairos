/// include/kairos/kel/ast.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/kel/ast.hpp — AST node types for the KEL expression language      ║
// ║                                                                           ║
// ║  Nodes: Literal, Identifier, UnaryOp, BinaryOp, FunctionCall,            ║
// ║         MemberAccess, MethodCall, ListLiteral                             ║
// ║                                                                           ║
// ║  Spec reference: §7.5                                                     ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/kel/token.hpp"
#include "kairos/kel/value.hpp"

#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace kairos::kel {

/// Source location for error messages.
struct SourceLoc {
    uint32_t offset = 0;
    uint32_t length = 0;
};

// Forward declarations.
struct LiteralNode;
struct IdentifierNode;
struct UnaryOpNode;
struct BinaryOpNode;
struct FunctionCallNode;
struct MemberAccessNode;
struct MethodCallNode;
struct ListLiteralNode;

/// An AST node is one of the concrete node types.
using AstNode = std::variant<
    LiteralNode,
    IdentifierNode,
    UnaryOpNode,
    BinaryOpNode,
    FunctionCallNode,
    MemberAccessNode,
    MethodCallNode,
    ListLiteralNode
>;

/// Heap-allocated AST node (for recursive structures).
using AstPtr = std::unique_ptr<AstNode>;

// ─── Concrete node types ──────────────────────────────────────────────

/// Literal value: true, false, 42, 3.14, "hello", 2h
struct LiteralNode {
    KelValue   value;
    SourceLoc  loc;
};

/// Variable reference: event, data, my_var
struct IdentifierNode {
    std::string name;
    SourceLoc   loc;
};

/// Unary operation: not x, -x
struct UnaryOpNode {
    TokenType  op;       // Not, Minus
    AstPtr     operand;
    SourceLoc  loc;
};

/// Binary operation: a + b, x and y, s == "hello"
struct BinaryOpNode {
    TokenType  op;
    AstPtr     left;
    AstPtr     right;
    SourceLoc  loc;
};

/// Free function call: min(a, b), contains(s, "sub")
struct FunctionCallNode {
    std::string           name;
    std::vector<AstPtr>   args;
    SourceLoc             loc;
};

/// Member access: event.type, event.path
struct MemberAccessNode {
    AstPtr       object;
    std::string  member;
    SourceLoc    loc;
};

/// Method call: job("build").finished_within(30m)
struct MethodCallNode {
    AstPtr                object;
    std::string           method;
    std::vector<AstPtr>   args;
    SourceLoc             loc;
};

/// List literal: [1, 2, 3]
struct ListLiteralNode {
    std::vector<AstPtr>  elements;
    SourceLoc            loc;
};

// ─── Parse interface ──────────────────────────────────────────────────

/// Parse a KEL expression string into an AST.
/// Throws KelParseError on syntax errors.
AstPtr parse(std::string_view source);

/// Compute the total node count of an AST tree (for limit checks).
uint32_t ast_node_count(const AstNode& node);

/// Extract the SourceLoc from any AstNode variant.
SourceLoc ast_loc(const AstNode& node);

}  // namespace kairos::kel
