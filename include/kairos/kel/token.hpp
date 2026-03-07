/// include/kairos/kel/token.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/kel/token.hpp — Token types and lexer interface                   ║
// ║                                                                           ║
// ║  Hand-written single-pass lexer with one character of lookahead.          ║
// ║  Supports duration literals (2h, 30m, 90s, 1d, 500ms).                   ║
// ║                                                                           ║
// ║  Spec reference: §7.3                                                     ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kairos::kel {

enum class TokenType {
    // ─── Literals ─────────────────────────────────────
    IntLiteral,         // 42, 0xFF, 0b1010
    FloatLiteral,       // 3.14, 1e10
    StringLiteral,      // "hello", 'world'
    DurationLiteral,    // 2h, 30m, 90s, 1d, 500ms
    BoolTrue,           // true
    BoolFalse,          // false

    // ─── Identifiers & keywords ───────────────────────
    Identifier,         // job, event, my_var
    And,                // and
    Or,                 // or
    Not,                // not
    In,                 // in

    // ─── Operators ────────────────────────────────────
    Plus,               // +
    Minus,              // -
    Star,               // *
    Slash,              // /
    Percent,            // %
    EqualEqual,         // ==
    BangEqual,          // !=
    Less,               // <
    LessEqual,          // <=
    Greater,            // >
    GreaterEqual,       // >=

    // ─── Delimiters ───────────────────────────────────
    LeftParen,          // (
    RightParen,         // )
    LeftBracket,        // [
    RightBracket,       // ]
    Comma,              // ,
    Dot,                // .

    // ─── Special ──────────────────────────────────────
    Eof,                // End of input
    Error,              // Lexer error (carries message in lexeme)
};

/// Human-readable name for a token type.
std::string_view token_type_name(TokenType type);

struct Token {
    TokenType    type;
    std::string  lexeme;      // Original source text of the token
    uint32_t     offset;      // Byte offset in source string
    uint32_t     length;      // Length in bytes

    // For numeric literals, pre-parsed values:
    int64_t      int_value    = 0;
    double       float_value  = 0.0;
    int64_t      duration_ms  = 0;  // For DurationLiteral
};

/// Tokenize a KEL expression.  Returns all tokens including a final Eof.
/// On lexer errors, inserts Token{Error} with the error message in lexeme.
std::vector<Token> tokenize(std::string_view source);

}  // namespace kairos::kel
