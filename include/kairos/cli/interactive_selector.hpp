/// include/kairos/cli/interactive_selector.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/cli/interactive_selector.hpp — Minimal TUI item selector          ║
// ║                                                                           ║
// ║  A lightweight, ncurses-free fuzzy-filter + arrow-key selector for CLI.  ║
// ║  Works on all platforms (POSIX termios, Windows Console API).             ║
// ║                                                                           ║
// ║  Spec reference: Roadmap §2.1                                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace kairos::cli {

/// An item in the interactive selector.
struct SelectorItem {
    std::string id;           ///< The ID to return on selection.
    std::string display;      ///< The display text (may contain ANSI).
    std::string search_text;  ///< Plain-text for filtering (no ANSI).
};

/// Options for the interactive selector.
struct SelectorOptions {
    std::string prompt = "Select";  ///< Header prompt text.
    int max_visible = 15;           ///< Max items visible at once.
    bool case_sensitive = false;    ///< Filter case sensitivity.
};

/// Show an interactive selector and return the selected item's ID.
///
/// Presents a scrollable list with type-ahead filtering.
/// Navigation: ↑/↓ (or j/k), Enter to select, Esc/Ctrl-C to cancel.
///
/// @return The selected item's ID, or std::nullopt if cancelled.
///         Returns std::nullopt immediately if stdin is not a TTY
///         or if items is empty.
[[nodiscard]] std::optional<std::string> interactive_select(
    const std::vector<SelectorItem>& items,
    const SelectorOptions& opts = {});

/// @return true if the terminal supports interactive selection
///         (stdin is a TTY, not piped).
[[nodiscard]] bool can_use_interactive();

}  // namespace kairos::cli
