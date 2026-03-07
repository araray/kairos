/// include/kairos/platform/terminal.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/platform/terminal.hpp — TTY detection and terminal helpers        ║
// ║  Spec reference: §25.4                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

namespace kairos::platform {

/// Returns true if stdout is connected to a terminal (not piped/redirected).
bool is_tty();

/// Enable ANSI escape processing on Windows.  No-op on POSIX.
void enable_ansi_escapes();

/// Get terminal width in columns.  Returns 80 as a safe default
/// if detection fails.
int terminal_width();

}  // namespace kairos::platform
