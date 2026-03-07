/// src/kel/lexer.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  KEL Lexer — single-pass scanner with one char lookahead                  ║
// ║                                                                           ║
// ║  Key decisions:                                                           ║
// ║    • Numbers followed by duration suffixes (ms,s,m,h,d) → DurationLiteral║
// ║    • Identifiers matching keywords → keyword token type                   ║
// ║    • Supports hex (0xFF) and binary (0b1010) integer literals             ║
// ║    • String escapes: \", \', \\, \n, \t                                  ║
// ║                                                                           ║
// ║  Spec reference: §7.3                                                     ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/kel/token.hpp"

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace kairos::kel {

// ═══════════════════════════════════════════════════════════════════════════
// Token type names (for error messages and debugging)
// ═══════════════════════════════════════════════════════════════════════════

std::string_view token_type_name(TokenType type) {
    switch (type) {
        case TokenType::IntLiteral:      return "integer";
        case TokenType::FloatLiteral:    return "float";
        case TokenType::StringLiteral:   return "string";
        case TokenType::DurationLiteral: return "duration";
        case TokenType::BoolTrue:        return "true";
        case TokenType::BoolFalse:       return "false";
        case TokenType::Identifier:      return "identifier";
        case TokenType::And:             return "and";
        case TokenType::Or:              return "or";
        case TokenType::Not:             return "not";
        case TokenType::In:              return "in";
        case TokenType::Plus:            return "+";
        case TokenType::Minus:           return "-";
        case TokenType::Star:            return "*";
        case TokenType::Slash:           return "/";
        case TokenType::Percent:         return "%";
        case TokenType::EqualEqual:      return "==";
        case TokenType::BangEqual:       return "!=";
        case TokenType::Less:            return "<";
        case TokenType::LessEqual:       return "<=";
        case TokenType::Greater:         return ">";
        case TokenType::GreaterEqual:    return ">=";
        case TokenType::LeftParen:       return "(";
        case TokenType::RightParen:      return ")";
        case TokenType::LeftBracket:     return "[";
        case TokenType::RightBracket:    return "]";
        case TokenType::Comma:           return ",";
        case TokenType::Dot:             return ".";
        case TokenType::Eof:             return "end of expression";
        case TokenType::Error:           return "error";
    }
    return "unknown";
}

// ═══════════════════════════════════════════════════════════════════════════
// Lexer implementation
// ═══════════════════════════════════════════════════════════════════════════

namespace {

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source), pos_(0) {}

    std::vector<Token> tokenize_all() {
        std::vector<Token> tokens;
        tokens.reserve(32);

        while (pos_ < source_.size()) {
            skip_whitespace();
            if (pos_ >= source_.size()) break;

            char c = peek();

            if (std::isdigit(c)) {
                tokens.push_back(scan_number());
            } else if (c == '"' || c == '\'') {
                tokens.push_back(scan_string());
            } else if (std::isalpha(c) || c == '_') {
                tokens.push_back(scan_identifier());
            } else {
                tokens.push_back(scan_operator());
            }

            // If we got an error token, continue scanning (collect all errors).
        }

        tokens.push_back(Token{TokenType::Eof, "", static_cast<uint32_t>(pos_), 0});
        return tokens;
    }

private:
    std::string_view source_;
    size_t pos_;

    // ─── Character helpers ────────────────────────────────────────────

    [[nodiscard]] char peek() const {
        return pos_ < source_.size() ? source_[pos_] : '\0';
    }

    [[nodiscard]] char peek_next() const {
        return (pos_ + 1) < source_.size() ? source_[pos_ + 1] : '\0';
    }

    char advance() {
        return source_[pos_++];
    }

    bool match(char expected) {
        if (pos_ < source_.size() && source_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    void skip_whitespace() {
        while (pos_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[pos_]))) {
            ++pos_;
        }
    }

    Token make_token(TokenType type, uint32_t start) {
        auto len = static_cast<uint32_t>(pos_ - start);
        return Token{type, std::string(source_.substr(start, len)), start, len};
    }

    Token make_error(const std::string& msg, uint32_t start) {
        return Token{TokenType::Error, msg, start, 1};
    }

    // ─── Number scanning ──────────────────────────────────────────────

    Token scan_number() {
        uint32_t start = static_cast<uint32_t>(pos_);
        bool is_float = false;

        // Check for hex/binary prefix.
        if (peek() == '0' && (peek_next() == 'x' || peek_next() == 'X')) {
            pos_ += 2;
            while (pos_ < source_.size() && std::isxdigit(static_cast<unsigned char>(source_[pos_]))) {
                ++pos_;
            }
            auto lexeme = std::string(source_.substr(start, pos_ - start));
            Token tok = make_token(TokenType::IntLiteral, start);
            tok.int_value = std::stoll(lexeme, nullptr, 16);
            return tok;
        }
        if (peek() == '0' && (peek_next() == 'b' || peek_next() == 'B')) {
            pos_ += 2;
            while (pos_ < source_.size() && (source_[pos_] == '0' || source_[pos_] == '1')) {
                ++pos_;
            }
            auto lexeme = std::string(source_.substr(start, pos_ - start));
            Token tok = make_token(TokenType::IntLiteral, start);
            tok.int_value = std::stoll(lexeme.substr(2), nullptr, 2);
            return tok;
        }

        // Decimal digits.
        while (pos_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[pos_]))) {
            ++pos_;
        }

        // Fractional part.
        if (pos_ < source_.size() && source_[pos_] == '.' &&
            pos_ + 1 < source_.size() && std::isdigit(static_cast<unsigned char>(source_[pos_ + 1]))) {
            is_float = true;
            ++pos_;  // consume '.'
            while (pos_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[pos_]))) {
                ++pos_;
            }
        }

        // Exponent part.
        if (pos_ < source_.size() && (source_[pos_] == 'e' || source_[pos_] == 'E')) {
            is_float = true;
            ++pos_;
            if (pos_ < source_.size() && (source_[pos_] == '+' || source_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= source_.size() || !std::isdigit(static_cast<unsigned char>(source_[pos_]))) {
                return make_error("expected digit after exponent", start);
            }
            while (pos_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[pos_]))) {
                ++pos_;
            }
        }

        // Check for duration suffix BEFORE emitting a numeric token.
        // Only applies to non-float values (30m, 2h, 1d, 500ms, 90s).
        if (!is_float && pos_ < source_.size()) {
            int64_t multiplier = 0;
            size_t suffix_len = 0;

            if (source_[pos_] == 'm' && pos_ + 1 < source_.size() && source_[pos_ + 1] == 's') {
                multiplier = 1;
                suffix_len = 2;
            } else if (source_[pos_] == 's' && (pos_ + 1 >= source_.size() ||
                       !std::isalnum(static_cast<unsigned char>(source_[pos_ + 1])))) {
                multiplier = 1000;
                suffix_len = 1;
            } else if (source_[pos_] == 'm' && (pos_ + 1 >= source_.size() ||
                       !std::isalnum(static_cast<unsigned char>(source_[pos_ + 1])))) {
                multiplier = 60000;
                suffix_len = 1;
            } else if (source_[pos_] == 'h' && (pos_ + 1 >= source_.size() ||
                       !std::isalnum(static_cast<unsigned char>(source_[pos_ + 1])))) {
                multiplier = 3600000;
                suffix_len = 1;
            } else if (source_[pos_] == 'd' && (pos_ + 1 >= source_.size() ||
                       !std::isalnum(static_cast<unsigned char>(source_[pos_ + 1])))) {
                multiplier = 86400000;
                suffix_len = 1;
            }

            if (multiplier > 0) {
                auto num_str = std::string(source_.substr(start, pos_ - start));
                int64_t num_val = std::stoll(num_str);
                pos_ += suffix_len;
                Token tok = make_token(TokenType::DurationLiteral, start);
                tok.duration_ms = num_val * multiplier;
                return tok;
            }
        }

        // Regular numeric literal.
        auto lexeme = std::string(source_.substr(start, pos_ - start));
        if (is_float) {
            Token tok = make_token(TokenType::FloatLiteral, start);
            tok.float_value = std::stod(lexeme);
            return tok;
        } else {
            Token tok = make_token(TokenType::IntLiteral, start);
            tok.int_value = std::stoll(lexeme);
            return tok;
        }
    }

    // ─── String scanning ──────────────────────────────────────────────

    Token scan_string() {
        uint32_t start = static_cast<uint32_t>(pos_);
        char quote = advance();  // consume opening quote
        std::string value;

        while (pos_ < source_.size() && source_[pos_] != quote) {
            if (source_[pos_] == '\\') {
                ++pos_;
                if (pos_ >= source_.size()) {
                    return make_error("unterminated escape sequence in string", start);
                }
                switch (source_[pos_]) {
                    case '"':  value += '"';  break;
                    case '\'': value += '\''; break;
                    case '\\': value += '\\'; break;
                    case 'n':  value += '\n'; break;
                    case 't':  value += '\t'; break;
                    default:
                        return make_error(
                            std::string("unknown escape sequence: \\") + source_[pos_], start);
                }
                ++pos_;
            } else {
                value += source_[pos_++];
            }
        }

        if (pos_ >= source_.size()) {
            return make_error("unterminated string literal", start);
        }
        ++pos_;  // consume closing quote

        Token tok = make_token(TokenType::StringLiteral, start);
        tok.lexeme = value;  // Store the unescaped value.
        return tok;
    }

    // ─── Identifier / keyword scanning ────────────────────────────────

    Token scan_identifier() {
        uint32_t start = static_cast<uint32_t>(pos_);

        while (pos_ < source_.size() &&
               (std::isalnum(static_cast<unsigned char>(source_[pos_])) || source_[pos_] == '_')) {
            ++pos_;
        }

        auto lexeme = source_.substr(start, pos_ - start);
        Token tok = make_token(TokenType::Identifier, start);

        // Check keywords.
        if      (lexeme == "and")   tok.type = TokenType::And;
        else if (lexeme == "or")    tok.type = TokenType::Or;
        else if (lexeme == "not")   tok.type = TokenType::Not;
        else if (lexeme == "in")    tok.type = TokenType::In;
        else if (lexeme == "true")  tok.type = TokenType::BoolTrue;
        else if (lexeme == "false") tok.type = TokenType::BoolFalse;

        return tok;
    }

    // ─── Operator scanning ────────────────────────────────────────────

    Token scan_operator() {
        uint32_t start = static_cast<uint32_t>(pos_);
        char c = advance();

        switch (c) {
            case '+': return make_token(TokenType::Plus, start);
            case '-': return make_token(TokenType::Minus, start);
            case '*': return make_token(TokenType::Star, start);
            case '/': return make_token(TokenType::Slash, start);
            case '%': return make_token(TokenType::Percent, start);
            case '(': return make_token(TokenType::LeftParen, start);
            case ')': return make_token(TokenType::RightParen, start);
            case '[': return make_token(TokenType::LeftBracket, start);
            case ']': return make_token(TokenType::RightBracket, start);
            case ',': return make_token(TokenType::Comma, start);
            case '.': return make_token(TokenType::Dot, start);

            case '=':
                if (match('=')) return make_token(TokenType::EqualEqual, start);
                return make_error("unexpected '='; did you mean '=='?", start);

            case '!':
                if (match('=')) return make_token(TokenType::BangEqual, start);
                return make_error("unexpected '!'; did you mean '!=' or 'not'?", start);

            case '<':
                if (match('=')) return make_token(TokenType::LessEqual, start);
                return make_token(TokenType::Less, start);

            case '>':
                if (match('=')) return make_token(TokenType::GreaterEqual, start);
                return make_token(TokenType::Greater, start);

            default:
                return make_error(
                    std::string("unexpected character: '") + c + "'", start);
        }
    }
};

}  // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════════════════

std::vector<Token> tokenize(std::string_view source) {
    Lexer lexer(source);
    return lexer.tokenize_all();
}

}  // namespace kairos::kel
