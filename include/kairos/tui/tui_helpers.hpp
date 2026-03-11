/// include/kairos/tui/tui_helpers.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/tui/tui_helpers.hpp — Testable helper functions for the TUI     ║
// ║                                                                          ║
// ║  These functions are used by the FTXUI dashboard but have no FTXUI      ║
// ║  dependency themselves. They are exposed here so that unit tests can     ║
// ║  exercise format_duration, format_relative, status_icon, etc. without   ║
// ║  pulling in the FTXUI library.                                          ║
// ║                                                                          ║
// ║  Spec reference: §23 (CLI/TUI), §30 (testing strategy)                 ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <string>

namespace kairos::tui::helpers {

/// Format a duration in seconds to a human-readable string.
///
/// Examples:
///   format_duration(0.5)   → "0s"
///   format_duration(45)    → "45s"
///   format_duration(125)   → "2m 5s"
///   format_duration(7384)  → "2h 3m"
///   format_duration(-1)    → "—"
std::string format_duration(double seconds);

/// Format an ISO-8601 timestamp to a relative time string.
///
/// Accepts "YYYY-MM-DD HH:MM:SS" or "YYYY-MM-DDTHH:MM:SS" format.
///
/// Examples:
///   format_relative("")                    → "—"
///   format_relative("2026-03-10 14:30:00") → "2h ago"  (approx)
///   format_relative("short")               → "short"
std::string format_relative(const std::string& iso_ts);

/// Get the Unicode status icon for a run status string.
///
/// Mapping:
///   "SUCCESS"   → "✓"
///   "FAILED"    → "✗"
///   "RUNNING"   → "●"
///   "CANCELLED" → "⊘"
///   "SKIPPED"   → "○"
///   "TIMED_OUT" → "⏱"
///   (other)     → "?"
std::string status_icon(const std::string& status);

/// Get the current local time as "HH:MM:SS".
///
/// Cross-platform: uses localtime_r (POSIX) or localtime_s (Windows).
std::string format_local_time_now();

}  // namespace kairos::tui::helpers
