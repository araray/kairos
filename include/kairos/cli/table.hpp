/// include/kairos/cli/table.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/cli/table.hpp — Table renderer + formatting helpers for CLI      ║
// ║                                                                          ║
// ║  Computes column widths from content, renders aligned text with          ║
// ║  optional ANSI color codes. Auto-detects TTY for color support.         ║
// ║                                                                          ║
// ║  Spec reference: §23.7                                                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/platform/platform.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace kairos::cli {

// ── ANSI color helpers ──────────────────────────────────────────────────

/// Returns true if the terminal supports ANSI color codes.
/// Respects NO_COLOR env var (https://no-color.org/).
inline bool supports_color() {
    static const bool result = []() {
        // Respect NO_COLOR env var.
        if (const char* nc = std::getenv("NO_COLOR"); nc != nullptr)
            return false;
        // Check if stdout is a TTY.
        return platform::is_tty();
    }();
    return result;
}

namespace ansi {

// Reset
inline constexpr const char* reset   = "\033[0m";
inline constexpr const char* bold    = "\033[1m";
inline constexpr const char* dim     = "\033[2m";

// Foreground colors
inline constexpr const char* red     = "\033[31m";
inline constexpr const char* green   = "\033[32m";
inline constexpr const char* yellow  = "\033[33m";
inline constexpr const char* blue    = "\033[34m";
inline constexpr const char* magenta = "\033[35m";
inline constexpr const char* cyan    = "\033[36m";
inline constexpr const char* white   = "\033[37m";
inline constexpr const char* gray    = "\033[90m";

}  // namespace ansi

/// Wrap a string in ANSI color codes (no-op if color is disabled).
inline std::string colorize(const std::string& s,
                            const char* color,
                            bool color_enabled) {
    if (!color_enabled || !color) return s;
    return std::string(color) + s + ansi::reset;
}

// ── Status indicators ───────────────────────────────────────────────────

/// Return a status icon string, optionally colored.
/// Maps common status strings to visual indicators (§23.7).
inline std::string status_icon(const std::string& status,
                               bool color = false) {
    if (status == "SUCCESS" || status == "success" || status == "ok") {
        return color ? colorize("\xe2\x9c\x93", ansi::green, true)  // ✓
                     : "[ok]";
    }
    if (status == "FAILURE" || status == "failure" || status == "fail") {
        return color ? colorize("\xe2\x9c\x97", ansi::red, true)    // ✗
                     : "[!!]";
    }
    if (status == "RUNNING" || status == "running") {
        return color ? colorize("\xe2\x97\x8f", ansi::cyan, true)   // ●
                     : "[>>]";
    }
    if (status == "PENDING" || status == "pending") {
        return color ? colorize("\xe2\x97\x8b", ansi::gray, true)   // ○
                     : "[..]";
    }
    if (status == "SKIPPED" || status == "skipped") {
        return color ? colorize("\xe2\x97\x8b", ansi::yellow, true) // ○
                     : "[--]";
    }
    if (status == "CANCELLED" || status == "cancelled") {
        return color ? colorize("\xe2\x8a\x98", ansi::yellow, true) // ⊘
                     : "[--]";
    }
    if (status == "TIMED_OUT" || status == "timed_out") {
        return color ? colorize("\xe2\x8f\xb0", ansi::red, true)    // ⏰
                     : "[TO]";
    }
    return "[??]";
}

/// Colorize a status string itself (e.g., "SUCCESS" in green).
inline std::string colorize_status(const std::string& status,
                                   bool color = false) {
    if (!color) return status;
    if (status == "SUCCESS") return colorize(status, ansi::green, true);
    if (status == "FAILURE") return colorize(status, ansi::red, true);
    if (status == "RUNNING") return colorize(status, ansi::cyan, true);
    if (status == "CANCELLED") return colorize(status, ansi::yellow, true);
    if (status == "SKIPPED") return colorize(status, ansi::yellow, true);
    if (status == "TIMED_OUT") return colorize(status, ansi::red, true);
    return status;
}

// ── Duration formatting ─────────────────────────────────────────────────

/// Format a duration in milliseconds for human display.
inline std::string format_duration(int64_t ms) {
    if (ms <= 0) return "--";
    if (ms < 1000) return fmt::format("{}ms", ms);
    if (ms < 60000) return fmt::format("{:.1f}s", ms / 1000.0);
    int minutes = static_cast<int>(ms / 60000);
    int seconds = static_cast<int>((ms % 60000) / 1000);
    if (minutes < 60) return fmt::format("{}m {}s", minutes, seconds);
    int hours = minutes / 60;
    minutes %= 60;
    return fmt::format("{}h {}m", hours, minutes);
}

/// Truncate a string to max_len, appending ".." if truncated.
inline std::string truncate(const std::string& s, size_t max_len) {
    if (s.size() <= max_len) return s;
    if (max_len <= 2) return s.substr(0, max_len);
    return s.substr(0, max_len - 2) + "..";
}

// ── Table renderer ──────────────────────────────────────────────────────

/// Simple table renderer for CLI output.
/// Computes column widths from content and renders aligned text
/// with an optional Unicode separator line.
///
/// Usage:
///   Table t({"NAME", "STATUS", "DURATION"});
///   t.add_row({"build", "[ok] SUCCESS", "1.2s"});
///   t.add_row({"test",  "[!!] FAILURE", "3.5s"});
///   t.render(std::cout, use_color);
class Table {
public:
    using Row = std::vector<std::string>;

    /// Construct with column headers.
    explicit Table(std::vector<std::string> headers)
        : headers_(std::move(headers))
    {
        widths_.resize(headers_.size(), 0);
        for (std::size_t i = 0; i < headers_.size(); ++i) {
            widths_[i] = visible_length(headers_[i]);
        }
    }

    /// Add a data row. Each element is rendered left-aligned.
    void add_row(Row row) {
        for (std::size_t i = 0; i < row.size() && i < widths_.size(); ++i) {
            widths_[i] = std::max(widths_[i], visible_length(row[i]));
        }
        rows_.push_back(std::move(row));
    }

    /// Render the table to an output stream.
    /// @param out        Output stream (default: std::cout).
    /// @param use_color  If true, render headers in bold.
    void render(std::ostream& out = std::cout,
                bool use_color = false) const {
        // Header
        for (std::size_t i = 0; i < headers_.size(); ++i) {
            if (i > 0) out << "  ";
            std::string h = headers_[i];
            if (use_color) {
                h = std::string(ansi::bold) + h + ansi::reset;
            }
            // Pad based on visible length (headers have no ANSI).
            size_t pad = widths_[i] > headers_[i].size()
                ? widths_[i] - headers_[i].size() : 0;
            out << h << std::string(pad, ' ');
        }
        out << '\n';

        // Separator line (Unicode ─ character, U+2500).
        for (std::size_t i = 0; i < headers_.size(); ++i) {
            if (i > 0) out << "  ";
            for (std::size_t j = 0; j < widths_[i]; ++j) {
                out << "\xe2\x94\x80";  // ─ (3-byte UTF-8)
            }
        }
        out << '\n';

        // Rows
        for (const auto& row : rows_) {
            for (std::size_t i = 0; i < widths_.size(); ++i) {
                if (i > 0) out << "  ";
                std::string cell = i < row.size() ? row[i] : "";
                size_t vis = visible_length(cell);
                size_t pad = widths_[i] > vis ? widths_[i] - vis : 0;
                out << cell << std::string(pad, ' ');
            }
            out << '\n';
        }
    }

    /// @return Number of data rows.
    [[nodiscard]] std::size_t row_count() const { return rows_.size(); }

private:
    /// Compute the visible length of a string, ignoring ANSI escape codes.
    /// Escape codes are \033[...m sequences.
    static std::size_t visible_length(const std::string& s) {
        std::size_t len = 0;
        bool in_escape = false;
        for (size_t i = 0; i < s.size(); ++i) {
            if (in_escape) {
                if (s[i] == 'm') in_escape = false;
                continue;
            }
            if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
                in_escape = true;
                ++i;  // Skip the '[' on next iteration
                continue;
            }
            ++len;
        }
        return len;
    }

    std::vector<std::string> headers_;
    std::vector<Row> rows_;
    std::vector<std::size_t> widths_;
};

}  // namespace kairos::cli
