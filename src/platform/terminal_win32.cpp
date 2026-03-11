/// src/platform/terminal_win32.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  terminal_win32.cpp — TTY detection, ANSI escape enabling, console width  ║
// ║                                                                           ║
// ║  Spec reference: §25.4                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef _WIN32

#include "kairos/platform/terminal.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <io.h>      // _isatty, _fileno
#include <cstdio>    // stdout

namespace kairos::platform {

bool is_tty() {
    return _isatty(_fileno(stdout)) != 0;
}

void enable_ansi_escapes() {
    // Enable VT100 escape processing on Windows 10 1511+ consoles.
    // This is a no-op on POSIX (terminals natively support ANSI),
    // but Windows consoles require explicit opt-in.
    HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;

    DWORD mode = 0;
    if (!::GetConsoleMode(hOut, &mode)) return;

    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    (void)::SetConsoleMode(hOut, mode);

    // Also enable for stderr (for colored error messages).
    HANDLE hErr = ::GetStdHandle(STD_ERROR_HANDLE);
    if (hErr != INVALID_HANDLE_VALUE) {
        DWORD err_mode = 0;
        if (::GetConsoleMode(hErr, &err_mode)) {
            err_mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            (void)::SetConsoleMode(hErr, err_mode);
        }
    }
}

int terminal_width() {
    HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return 80;

    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (::GetConsoleScreenBufferInfo(hOut, &csbi)) {
        int width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        if (width > 0) return width;
    }
    return 80;  // safe default
}

}  // namespace kairos::platform

#endif  // _WIN32
