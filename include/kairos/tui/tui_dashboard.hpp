/// include/kairos/tui/tui_dashboard.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/tui/tui_dashboard.hpp — FTXUI-based real-time dashboard         ║
// ║                                                                          ║
// ║  `kairos dashboard` launches a terminal UI that displays:               ║
// ║    - Daemon status (running, uptime, PID, config path)                  ║
// ║    - Active runs (real-time progress)                                    ║
// ║    - Recent runs (table with status, duration)                          ║
// ║    - Watch groups (active, sample counts)                               ║
// ║    - Scheduler timers (next fire times)                                 ║
// ║    - Live log tail (latest log entries)                                 ║
// ║                                                                          ║
// ║  The dashboard connects to the Kairos SQLite database in read-only     ║
// ║  mode and polls at a configurable interval.                             ║
// ║                                                                          ║
// ║  Spec reference: §23 (optional TUI), §5.12                             ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <filesystem>
#include <string>

namespace kairos::tui {

/// Configuration for the TUI dashboard.
struct DashboardConfig {
    /// Path to the Kairos SQLite database.
    std::filesystem::path db_path;

    /// Path to the kairos.toml config file (for display).
    std::filesystem::path config_path;

    /// Path to the Kairos data directory (contains kairos.lock).
    /// If empty, derived from db_path.parent_path().
    std::filesystem::path data_dir;

    /// Refresh interval in milliseconds.
    int refresh_ms = 1000;

    /// Maximum number of recent runs to display.
    int max_recent_runs = 20;

    /// Maximum number of log lines to display.
    int max_log_lines = 50;

    /// Maximum number of watch groups to display.
    int max_watch_groups = 20;

    /// Maximum number of recent watch events to display.
    int max_events = 20;

    /// Maximum number of scheduler timers to display.
    int max_timers = 20;
};

/// Launch the TUI dashboard.
///
/// This function blocks until the user quits (q, Esc, or Ctrl+C).
/// Returns 0 on success, non-zero on error.
///
/// Requires: KAIROS_TUI=ON build flag (FTXUI linked).
///
/// Usage:
///   kairos dashboard [--db /path/to/kairos.db] [--refresh 1000]
///
/// Keyboard controls:
///   q / Esc    — Quit
///   Tab        — Cycle focus between panels
///   ↑↓         — Scroll within focused panel
///   r          — Force immediate refresh
///   l          — Toggle log panel visibility
int run_dashboard(const DashboardConfig& config);

}  // namespace kairos::tui
