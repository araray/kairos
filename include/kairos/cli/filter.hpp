/// include/kairos/cli/filter.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/cli/filter.hpp — Regex-based row filtering for CLI list commands  ║
// ║                                                                           ║
// ║  Provides a compiled-once regex filter that matches against concatenated  ║
// ║  row fields.  Applied client-side after query (§5.1).                     ║
// ║                                                                           ║
// ║  Spec reference: Roadmap §5.1                                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <iostream>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace kairos::cli {

/// A compiled row filter.  Construct once from a pattern string,
/// then call matches() for each row.
///
/// The filter strips ANSI escape codes before matching so that
/// colorized cell values are matched on their visible text only.
class RowFilter {
public:
    /// Construct a disabled (pass-all) filter.
    RowFilter() = default;

    /// Compile a regex pattern.  Case-insensitive by default.
    /// Throws std::regex_error on invalid pattern.
    explicit RowFilter(const std::string& pattern)
        : active_(true)
        , re_(pattern,
              std::regex_constants::ECMAScript |
              std::regex_constants::icase |
              std::regex_constants::optimize)
    {}

    /// @return true if this filter is active (a pattern was compiled).
    [[nodiscard]] bool active() const noexcept { return active_; }

    /// Test if a row (vector of cell strings) matches the filter.
    /// Concatenates all cells with a space separator, strips ANSI
    /// codes, and runs regex_search.
    [[nodiscard]] bool matches(const std::vector<std::string>& row) const {
        if (!active_) return true;
        // Concatenate cells into a single searchable string.
        std::string combined;
        for (size_t i = 0; i < row.size(); ++i) {
            if (i > 0) combined += ' ';
            combined += strip_ansi(row[i]);
        }
        return std::regex_search(combined, re_);
    }

    /// Test if a single string matches the filter.
    [[nodiscard]] bool matches(const std::string& s) const {
        if (!active_) return true;
        return std::regex_search(strip_ansi(s), re_);
    }

    /// Strip ANSI escape sequences from a string.
    /// Public so it can be used by the interactive selector too.
    [[nodiscard]] static std::string strip_ansi(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        bool in_escape = false;
        for (size_t i = 0; i < s.size(); ++i) {
            if (in_escape) {
                if (s[i] == 'm') in_escape = false;
                continue;
            }
            if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
                in_escape = true;
                ++i;  // Skip '['
                continue;
            }
            out += s[i];
        }
        return out;
    }

private:
    bool active_ = false;
    std::regex re_;
};

/// Try to compile a RowFilter.  On invalid regex, prints an error
/// to stderr and returns a disabled filter (so the command still
/// runs without filtering rather than crashing).
inline RowFilter make_filter(const std::string& pattern) {
    if (pattern.empty()) return {};
    try {
        return RowFilter(pattern);
    } catch (const std::regex_error& e) {
        // §5.1: Invalid regex → clear error, continue unfiltered.
        std::cerr << "Invalid --filter pattern: " << e.what()
                  << "\n  Pattern: " << pattern
                  << "\n  (showing unfiltered results)\n\n";
        return {};
    }
}

}  // namespace kairos::cli
