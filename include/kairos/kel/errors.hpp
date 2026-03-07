/// include/kairos/kel/errors.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/kel/errors.hpp — KEL error hierarchy                              ║
// ║                                                                           ║
// ║  Three levels:                                                            ║
// ║    KelError         (base)                                                ║
// ║    ├─ KelParseError  (lexer + parser errors with source location)         ║
// ║    ├─ KelEvalError   (runtime type mismatches, unknown vars, div-by-zero) ║
// ║    └─ KelLimitError  (depth, time, string/list length)                    ║
// ║                                                                           ║
// ║  Spec reference: §7.9                                                     ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace kairos::kel {

/// Base class for all KEL errors.
class KelError : public std::runtime_error {
public:
    explicit KelError(const std::string& message)
        : std::runtime_error(message) {}
};

/// Lexer or parser error with source location.
class KelParseError : public KelError {
public:
    KelParseError(const std::string& message, uint32_t offset, uint32_t length)
        : KelError(message), offset_(offset), length_(length) {}

    /// Byte offset in the source expression.
    [[nodiscard]] uint32_t offset() const noexcept { return offset_; }

    /// Length of the erroneous token/span (0 if unknown).
    [[nodiscard]] uint32_t length() const noexcept { return length_; }

    /// Format with a caret pointing at the error location.
    ///
    /// Example:
    ///   KEL parse error at offset 23: expected ')' after arguments
    ///     Expression: job('build').finished_within(30m, 2h)
    ///                                                 ^
    [[nodiscard]] std::string format_with_source(std::string_view source) const {
        std::string result = "KEL parse error at offset ";
        result += std::to_string(offset_);
        result += ": ";
        result += what();
        result += "\n  Expression: ";
        result += source;
        result += "\n              ";

        // Advance to the caret position.
        for (uint32_t i = 0; i < offset_ && i < source.size(); ++i) {
            result += ' ';
        }
        result += '^';
        return result;
    }

private:
    uint32_t offset_;
    uint32_t length_;
};

/// Evaluation-time error: type mismatch, division by zero, unknown variable.
class KelEvalError : public KelError {
public:
    using KelError::KelError;
};

/// Limit violation: depth, time, string length, list length.
class KelLimitError : public KelError {
public:
    using KelError::KelError;
};

}  // namespace kairos::kel
