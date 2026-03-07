/// src/kel/parser.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  KEL Parser — recursive descent for operator-precedence grammar           ║
// ║                                                                           ║
// ║  Grammar (§7.4):                                                          ║
// ║    expression     → or_expr                                               ║
// ║    or_expr        → and_expr ( "or" and_expr )*                           ║
// ║    and_expr       → not_expr ( "and" not_expr )*                          ║
// ║    not_expr       → "not" not_expr | comparison                           ║
// ║    comparison     → addition ( cmp_op addition )?                         ║
// ║    addition       → multiplication ( ("+"|"-") multiplication )*          ║
// ║    multiplication → unary ( ("*"|"/"|"%") unary )*                        ║
// ║    unary          → "-" unary | postfix                                   ║
// ║    postfix        → primary ( "." IDENT ( "(" args? ")" )? )*            ║
// ║                   | primary ( "(" args? ")" )?                            ║
// ║    primary        → literal | IDENT | "(" expr ")" | "[" list "]"        ║
// ║                                                                           ║
// ║  No chaining comparisons: a < b < c is a parse error.                    ║
// ║                                                                           ║
// ║  Spec reference: §7.4–§7.5                                               ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/kel/ast.hpp"
#include "kairos/kel/errors.hpp"
#include "kairos/kel/token.hpp"

#include <stdexcept>

namespace kairos::kel {

namespace {

// ═══════════════════════════════════════════════════════════════════════════
// Parser class
// ═══════════════════════════════════════════════════════════════════════════

class Parser {
public:
    explicit Parser(std::vector<Token> tokens)
        : tokens_(std::move(tokens)), pos_(0) {}

    AstPtr parse_expression() {
        auto result = parse_or();
        if (!at_end()) {
            auto& tok = current();
            throw KelParseError(
                "unexpected token '" + tok.lexeme + "' after expression",
                tok.offset, tok.length);
        }
        return result;
    }

private:
    std::vector<Token> tokens_;
    size_t pos_;

    // ─── Token access ─────────────────────────────────────────────────

    [[nodiscard]] const Token& current() const {
        return tokens_[pos_];
    }

    [[nodiscard]] bool at_end() const {
        return pos_ >= tokens_.size() || tokens_[pos_].type == TokenType::Eof;
    }

    [[nodiscard]] bool check(TokenType type) const {
        return !at_end() && tokens_[pos_].type == type;
    }

    Token advance() {
        auto tok = tokens_[pos_];
        if (!at_end()) ++pos_;
        return tok;
    }

    bool match(TokenType type) {
        if (check(type)) {
            advance();
            return true;
        }
        return false;
    }

    Token expect(TokenType type, const std::string& context) {
        if (check(type)) return advance();
        auto& tok = current();
        throw KelParseError(
            "expected " + std::string(token_type_name(type)) +
            " " + context + ", got " +
            std::string(token_type_name(tok.type)) +
            (tok.lexeme.empty() ? "" : " '" + tok.lexeme + "'"),
            tok.offset, tok.length);
    }

    SourceLoc loc_from(const Token& tok) const {
        return SourceLoc{tok.offset, tok.length};
    }

    SourceLoc span(const Token& start, size_t end_pos) const {
        auto end_offset = (end_pos > 0 && end_pos <= tokens_.size())
            ? tokens_[end_pos - 1].offset + tokens_[end_pos - 1].length
            : start.offset + start.length;
        return SourceLoc{start.offset,
                         static_cast<uint32_t>(end_offset - start.offset)};
    }

    // Helper: wrap a node into a heap-allocated AstPtr.
    template <typename NodeT>
    AstPtr make_node(NodeT&& node) {
        return std::make_unique<AstNode>(std::forward<NodeT>(node));
    }

    // ─── Grammar rules ────────────────────────────────────────────────

    // or_expr → and_expr ( "or" and_expr )*
    AstPtr parse_or() {
        auto start_tok = current();
        auto left = parse_and();

        while (check(TokenType::Or)) {
            auto op_tok = advance();
            auto right = parse_and();
            left = make_node(BinaryOpNode{
                TokenType::Or,
                std::move(left),
                std::move(right),
                span(start_tok, pos_)
            });
        }
        return left;
    }

    // and_expr → not_expr ( "and" not_expr )*
    AstPtr parse_and() {
        auto start_tok = current();
        auto left = parse_not();

        while (check(TokenType::And)) {
            auto op_tok = advance();
            auto right = parse_not();
            left = make_node(BinaryOpNode{
                TokenType::And,
                std::move(left),
                std::move(right),
                span(start_tok, pos_)
            });
        }
        return left;
    }

    // not_expr → "not" not_expr | comparison
    AstPtr parse_not() {
        if (check(TokenType::Not)) {
            auto op_tok = advance();
            auto operand = parse_not();
            return make_node(UnaryOpNode{
                TokenType::Not,
                std::move(operand),
                span(op_tok, pos_)
            });
        }
        return parse_comparison();
    }

    // comparison → addition ( cmp_op addition )?
    // No chaining: a < b < c is a parse error.
    AstPtr parse_comparison() {
        auto start_tok = current();
        auto left = parse_addition();

        if (is_comparison_op()) {
            auto op_tok = advance();
            auto right = parse_addition();

            // Reject chained comparisons.
            if (is_comparison_op()) {
                auto& bad = current();
                throw KelParseError(
                    "chained comparison operators are not allowed; use 'and' instead",
                    bad.offset, bad.length);
            }

            return make_node(BinaryOpNode{
                op_tok.type,
                std::move(left),
                std::move(right),
                span(start_tok, pos_)
            });
        }
        return left;
    }

    [[nodiscard]] bool is_comparison_op() const {
        if (at_end()) return false;
        switch (current().type) {
            case TokenType::EqualEqual:
            case TokenType::BangEqual:
            case TokenType::Less:
            case TokenType::LessEqual:
            case TokenType::Greater:
            case TokenType::GreaterEqual:
            case TokenType::In:
                return true;
            default:
                return false;
        }
    }

    // addition → multiplication ( ("+"|"-") multiplication )*
    AstPtr parse_addition() {
        auto start_tok = current();
        auto left = parse_multiplication();

        while (check(TokenType::Plus) || check(TokenType::Minus)) {
            auto op_tok = advance();
            auto right = parse_multiplication();
            left = make_node(BinaryOpNode{
                op_tok.type,
                std::move(left),
                std::move(right),
                span(start_tok, pos_)
            });
        }
        return left;
    }

    // multiplication → unary ( ("*"|"/"|"%") unary )*
    AstPtr parse_multiplication() {
        auto start_tok = current();
        auto left = parse_unary();

        while (check(TokenType::Star) || check(TokenType::Slash) || check(TokenType::Percent)) {
            auto op_tok = advance();
            auto right = parse_unary();
            left = make_node(BinaryOpNode{
                op_tok.type,
                std::move(left),
                std::move(right),
                span(start_tok, pos_)
            });
        }
        return left;
    }

    // unary → "-" unary | postfix
    AstPtr parse_unary() {
        if (check(TokenType::Minus)) {
            auto op_tok = advance();
            auto operand = parse_unary();
            return make_node(UnaryOpNode{
                TokenType::Minus,
                std::move(operand),
                span(op_tok, pos_)
            });
        }
        return parse_postfix();
    }

    // postfix → primary ( "." IDENT ( "(" args? ")" )? )*
    //         | primary ( "(" args? ")" )?
    AstPtr parse_postfix() {
        auto start_tok = current();
        auto node = parse_primary();

        // Handle function call on bare identifier: foo(args)
        if (check(TokenType::LeftParen)) {
            // Only if node is an identifier (free function call).
            if (auto* id = std::get_if<IdentifierNode>(node.get())) {
                std::string func_name = id->name;
                advance();  // consume '('
                auto args = parse_arguments();
                expect(TokenType::RightParen, "after function arguments");
                node = make_node(FunctionCallNode{
                    std::move(func_name),
                    std::move(args),
                    span(start_tok, pos_)
                });
            }
        }

        // Handle member access / method calls: obj.member, obj.method(args)
        while (check(TokenType::Dot)) {
            advance();  // consume '.'
            auto member_tok = expect(TokenType::Identifier, "after '.'");

            if (check(TokenType::LeftParen)) {
                // Method call: obj.method(args)
                advance();  // consume '('
                auto args = parse_arguments();
                expect(TokenType::RightParen, "after method arguments");
                node = make_node(MethodCallNode{
                    std::move(node),
                    member_tok.lexeme,
                    std::move(args),
                    span(start_tok, pos_)
                });
            } else {
                // Member access: obj.member
                node = make_node(MemberAccessNode{
                    std::move(node),
                    member_tok.lexeme,
                    span(start_tok, pos_)
                });
            }
        }

        return node;
    }

    // primary → literal | IDENT | "(" expr ")" | "[" list "]"
    AstPtr parse_primary() {
        // Check for lexer errors first.
        if (check(TokenType::Error)) {
            auto tok = advance();
            throw KelParseError(tok.lexeme, tok.offset, tok.length);
        }

        // Boolean literals.
        if (check(TokenType::BoolTrue)) {
            auto tok = advance();
            return make_node(LiteralNode{KelValue(true), loc_from(tok)});
        }
        if (check(TokenType::BoolFalse)) {
            auto tok = advance();
            return make_node(LiteralNode{KelValue(false), loc_from(tok)});
        }

        // Integer literal.
        if (check(TokenType::IntLiteral)) {
            auto tok = advance();
            return make_node(LiteralNode{KelValue(tok.int_value), loc_from(tok)});
        }

        // Float literal.
        if (check(TokenType::FloatLiteral)) {
            auto tok = advance();
            return make_node(LiteralNode{KelValue(tok.float_value), loc_from(tok)});
        }

        // String literal.
        if (check(TokenType::StringLiteral)) {
            auto tok = advance();
            return make_node(LiteralNode{KelValue(tok.lexeme), loc_from(tok)});
        }

        // Duration literal.
        if (check(TokenType::DurationLiteral)) {
            auto tok = advance();
            return make_node(LiteralNode{
                KelValue(KelValue::Duration(tok.duration_ms)), loc_from(tok)});
        }

        // Identifier.
        if (check(TokenType::Identifier)) {
            auto tok = advance();
            return make_node(IdentifierNode{tok.lexeme, loc_from(tok)});
        }

        // Parenthesized expression.
        if (check(TokenType::LeftParen)) {
            auto start = advance();
            auto expr = parse_or();
            expect(TokenType::RightParen, "to close '('");
            return expr;
        }

        // List literal: [ expr, expr, ... ]
        if (check(TokenType::LeftBracket)) {
            auto start = advance();
            std::vector<AstPtr> elements;
            if (!check(TokenType::RightBracket)) {
                elements.push_back(parse_or());
                while (match(TokenType::Comma)) {
                    // Allow trailing comma.
                    if (check(TokenType::RightBracket)) break;
                    elements.push_back(parse_or());
                }
            }
            expect(TokenType::RightBracket, "to close '['");
            return make_node(ListLiteralNode{
                std::move(elements),
                span(start, pos_)
            });
        }

        // Nothing matched — error.
        if (at_end()) {
            throw KelParseError("unexpected end of expression",
                                current().offset, 0);
        }
        auto tok = current();
        throw KelParseError(
            "unexpected token '" + tok.lexeme + "'",
            tok.offset, tok.length);
    }

    // arguments → expression ( "," expression )*
    std::vector<AstPtr> parse_arguments() {
        std::vector<AstPtr> args;
        if (!check(TokenType::RightParen)) {
            args.push_back(parse_or());
            while (match(TokenType::Comma)) {
                if (check(TokenType::RightParen)) break;  // trailing comma
                args.push_back(parse_or());
            }
        }
        return args;
    }
};

}  // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════════════════

AstPtr parse(std::string_view source) {
    auto tokens = tokenize(source);

    // Check for lexer errors up-front (the parser will also handle them,
    // but this gives better error messages for standalone lexer failures).
    for (auto& tok : tokens) {
        if (tok.type == TokenType::Error) {
            throw KelParseError(tok.lexeme, tok.offset, tok.length);
        }
    }

    Parser parser(std::move(tokens));
    return parser.parse_expression();
}

uint32_t ast_node_count(const AstNode& node) {
    return std::visit([](const auto& n) -> uint32_t {
        using T = std::decay_t<decltype(n)>;

        if constexpr (std::is_same_v<T, LiteralNode> ||
                      std::is_same_v<T, IdentifierNode>) {
            return 1;
        }
        if constexpr (std::is_same_v<T, UnaryOpNode>) {
            return 1 + ast_node_count(*n.operand);
        }
        if constexpr (std::is_same_v<T, BinaryOpNode>) {
            return 1 + ast_node_count(*n.left) + ast_node_count(*n.right);
        }
        if constexpr (std::is_same_v<T, FunctionCallNode>) {
            uint32_t count = 1;
            for (auto& arg : n.args) count += ast_node_count(*arg);
            return count;
        }
        if constexpr (std::is_same_v<T, MemberAccessNode>) {
            return 1 + ast_node_count(*n.object);
        }
        if constexpr (std::is_same_v<T, MethodCallNode>) {
            uint32_t count = 1 + ast_node_count(*n.object);
            for (auto& arg : n.args) count += ast_node_count(*arg);
            return count;
        }
        if constexpr (std::is_same_v<T, ListLiteralNode>) {
            uint32_t count = 1;
            for (auto& el : n.elements) count += ast_node_count(*el);
            return count;
        }
        return 1;
    }, node);
}

SourceLoc ast_loc(const AstNode& node) {
    return std::visit([](const auto& n) -> SourceLoc {
        return n.loc;
    }, node);
}

}  // namespace kairos::kel
