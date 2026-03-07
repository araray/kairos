// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  terminal_posix.cpp — TTY detection, terminal width (POSIX)               ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/platform/terminal.hpp"

#include <sys/ioctl.h>
#include <unistd.h>

namespace kairos::platform {

bool is_tty() {
    return ::isatty(STDOUT_FILENO) != 0;
}

void enable_ansi_escapes() {
    // No-op on POSIX — terminals natively support ANSI escapes.
}

int terminal_width() {
    struct winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return static_cast<int>(ws.ws_col);
    }
    return 80;  // safe default
}

}  // namespace kairos::platform
