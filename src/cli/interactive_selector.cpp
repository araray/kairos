/// src/cli/interactive_selector.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  interactive_selector.cpp — Minimal ncurses-free TUI item selector       ║
// ║                                                                           ║
// ║  Cross-platform implementation using raw terminal mode + ANSI escapes.   ║
// ║  POSIX: termios.  Windows: Console API.                                  ║
// ║                                                                           ║
// ║  Spec reference: Roadmap §2.1                                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/cli/interactive_selector.hpp"
#include "kairos/cli/filter.hpp"
#include "kairos/platform/platform.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  define STDIN_FILENO 0
#else
#  include <termios.h>
#  include <unistd.h>
#  include <sys/ioctl.h>
#endif

namespace kairos::cli {

namespace {

// ── Platform-specific terminal helpers ───────────────────────────────

#ifdef _WIN32

/// RAII guard for Windows console raw mode.
struct RawModeGuard {
    HANDLE h_stdin;
    DWORD original_mode = 0;
    bool valid = false;

    RawModeGuard() {
        h_stdin = GetStdHandle(STD_INPUT_HANDLE);
        if (h_stdin == INVALID_HANDLE_VALUE) return;
        if (!GetConsoleMode(h_stdin, &original_mode)) return;
        // Enable virtual terminal input for escape sequences.
        DWORD raw_mode = ENABLE_VIRTUAL_TERMINAL_INPUT;
        if (SetConsoleMode(h_stdin, raw_mode)) valid = true;

        // Also enable VT output for ANSI codes on stderr.
        HANDLE h_err = GetStdHandle(STD_ERROR_HANDLE);
        DWORD err_mode = 0;
        if (GetConsoleMode(h_err, &err_mode)) {
            SetConsoleMode(h_err,
                err_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }

    ~RawModeGuard() {
        if (valid) SetConsoleMode(h_stdin, original_mode);
    }

    int read_char() {
        if (!valid) return -1;
        INPUT_RECORD rec;
        DWORD count;
        while (true) {
            if (!ReadConsoleInputW(h_stdin, &rec, 1, &count) || count == 0)
                return -1;
            if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
                auto& ke = rec.Event.KeyEvent;
                // Handle arrow keys via virtual key codes.
                switch (ke.wVirtualKeyCode) {
                    case VK_UP:     return 1000;  // sentinel: up
                    case VK_DOWN:   return 1001;  // sentinel: down
                    case VK_RETURN: return '\r';
                    case VK_ESCAPE: return 27;
                    default: break;
                }
                // Regular character.
                if (ke.uChar.UnicodeChar != 0) {
                    return static_cast<int>(ke.uChar.UnicodeChar);
                }
            }
        }
    }
};

#else  // POSIX

/// RAII guard for POSIX termios raw mode.
struct RawModeGuard {
    struct termios original;
    bool valid = false;

    RawModeGuard() {
        if (!isatty(STDIN_FILENO)) return;
        if (tcgetattr(STDIN_FILENO, &original) != 0) return;
        struct termios raw = original;
        // Turn off canonical mode and echo.
        raw.c_lflag &= ~(ICANON | ECHO | ISIG);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) valid = true;
    }

    ~RawModeGuard() {
        if (valid) tcsetattr(STDIN_FILENO, TCSANOW, &original);
    }

    int read_char() {
        if (!valid) return -1;
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1) return -1;

        // Check for escape sequences (arrow keys).
        if (c == 27) {
            unsigned char seq[2];
            // Non-blocking peek for escape sequence.
            if (read(STDIN_FILENO, &seq[0], 1) != 1) return 27;
            if (seq[0] != '[') return 27;
            if (read(STDIN_FILENO, &seq[1], 1) != 1) return 27;
            switch (seq[1]) {
                case 'A': return 1000;  // Up
                case 'B': return 1001;  // Down
                default:  return 27;
            }
        }
        return static_cast<int>(c);
    }
};

#endif  // _WIN32

// ── ANSI escape helpers (written to stderr so stdout stays clean) ────

void write_to_stderr(const std::string& s) {
    // Use write() for immediate output without buffering.
#ifdef _WIN32
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE),
              s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
#else
    [[maybe_unused]] auto r = write(STDERR_FILENO, s.data(), s.size());
#endif
}

void move_cursor_up(int n) {
    if (n > 0) write_to_stderr("\033[" + std::to_string(n) + "A");
}

void clear_line() {
    write_to_stderr("\033[2K\r");
}

void hide_cursor() { write_to_stderr("\033[?25l"); }
void show_cursor() { write_to_stderr("\033[?25h"); }

// ── Filtering ────────────────────────────────────────────────────────

/// Case-insensitive substring match (faster than regex for type-ahead).
bool fuzzy_match(const std::string& haystack,
                 const std::string& needle,
                 bool case_sensitive) {
    if (needle.empty()) return true;
    if (case_sensitive) {
        return haystack.find(needle) != std::string::npos;
    }
    // Case-insensitive find.
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

std::vector<size_t> filter_items(
    const std::vector<SelectorItem>& items,
    const std::string& query,
    bool case_sensitive)
{
    std::vector<size_t> result;
    for (size_t i = 0; i < items.size(); ++i) {
        if (fuzzy_match(items[i].search_text, query, case_sensitive)) {
            result.push_back(i);
        }
    }
    return result;
}

// ── Rendering ────────────────────────────────────────────────────────

/// Render the selector state to stderr.
/// Returns the number of lines written (for cursor repositioning).
int render(const std::vector<SelectorItem>& items,
           const std::vector<size_t>& visible_indices,
           size_t cursor_pos,
           size_t scroll_offset,
           const std::string& query,
           const SelectorOptions& opts,
           int prev_lines)
{
    // Move cursor up to the start of our rendering area.
    if (prev_lines > 0) move_cursor_up(prev_lines);

    int lines = 0;
    std::ostringstream out;

    // Prompt line with query.
    out << "\033[2K\r";  // Clear line
    out << "\033[36m" << opts.prompt << " > \033[0m";  // Cyan prompt
    out << query;
    out << "\033[90m";  // Dim
    if (visible_indices.empty()) {
        out << "  (no matches)";
    } else {
        out << "  (" << visible_indices.size() << "/"
            << items.size() << ")";
    }
    out << "\033[0m\n";
    lines++;

    // Separator line.
    out << "\033[2K\r\033[90m";
    for (int i = 0; i < 40; ++i) out << "\xe2\x94\x80";  // ─
    out << "\033[0m\n";
    lines++;

    // Visible items.
    int max_vis = std::min(opts.max_visible,
                           static_cast<int>(visible_indices.size()));
    for (int i = 0; i < max_vis; ++i) {
        size_t idx = scroll_offset + static_cast<size_t>(i);
        if (idx >= visible_indices.size()) break;

        out << "\033[2K\r";  // Clear line
        bool is_selected = (idx == cursor_pos);
        if (is_selected) {
            out << "\033[1m\033[36m" << "▸ " << "\033[0m";  // Bold cyan arrow
            // Display text in bold.
            out << "\033[1m"
                << RowFilter::strip_ansi(items[visible_indices[idx]].display)
                << "\033[0m";
        } else {
            out << "  " << RowFilter::strip_ansi(
                items[visible_indices[idx]].display);
        }
        out << "\n";
        lines++;
    }

    // Clear any leftover lines from previous render.
    for (int i = max_vis; i < prev_lines - 2; ++i) {
        out << "\033[2K\r\n";
        lines++;
    }

    // Footer hint.
    out << "\033[2K\r\033[90m"
        << "↑↓ navigate  Enter select  Esc cancel  Type to filter"
        << "\033[0m";
    lines++;

    write_to_stderr(out.str());
    return lines;
}

/// Clear the selector area.
void clear_render(int lines) {
    if (lines > 0) move_cursor_up(lines);
    for (int i = 0; i <= lines; ++i) {
        clear_line();
        if (i < lines) write_to_stderr("\n");
    }
    if (lines > 0) move_cursor_up(lines);
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────

bool can_use_interactive() {
    return platform::is_tty();
}

std::optional<std::string> interactive_select(
    const std::vector<SelectorItem>& items,
    const SelectorOptions& opts)
{
    if (items.empty()) return std::nullopt;
    if (!can_use_interactive()) return std::nullopt;

    RawModeGuard raw;
    if (!raw.valid) return std::nullopt;

    hide_cursor();

    std::string query;
    size_t cursor_pos = 0;
    size_t scroll_offset = 0;
    int prev_lines = 0;

    auto visible = filter_items(items, query, opts.case_sensitive);

    // Initial render.
    write_to_stderr("\n");  // Make room.
    prev_lines = render(items, visible, cursor_pos, scroll_offset,
                        query, opts, 0);

    while (true) {
        int ch = raw.read_char();
        if (ch < 0) break;

        bool needs_refilter = false;

        if (ch == 27) {  // Esc
            clear_render(prev_lines);
            show_cursor();
            return std::nullopt;
        }
        if (ch == 3) {   // Ctrl-C
            clear_render(prev_lines);
            show_cursor();
            return std::nullopt;
        }
        if (ch == '\r' || ch == '\n') {  // Enter
            clear_render(prev_lines);
            show_cursor();
            if (!visible.empty() && cursor_pos < visible.size()) {
                return items[visible[cursor_pos]].id;
            }
            return std::nullopt;
        }

        // Arrow keys (sentinels from read_char).
        if (ch == 1000) {  // Up
            if (cursor_pos > 0) {
                cursor_pos--;
                if (cursor_pos < scroll_offset) {
                    scroll_offset = cursor_pos;
                }
            }
        } else if (ch == 1001) {  // Down
            if (!visible.empty() && cursor_pos < visible.size() - 1) {
                cursor_pos++;
                if (cursor_pos >= scroll_offset +
                    static_cast<size_t>(opts.max_visible)) {
                    scroll_offset = cursor_pos -
                        static_cast<size_t>(opts.max_visible) + 1;
                }
            }
        } else if (ch == 'k' && query.empty()) {  // vim up (only when no query)
            if (cursor_pos > 0) {
                cursor_pos--;
                if (cursor_pos < scroll_offset) scroll_offset = cursor_pos;
            }
        } else if (ch == 'j' && query.empty()) {  // vim down
            if (!visible.empty() && cursor_pos < visible.size() - 1) {
                cursor_pos++;
                if (cursor_pos >= scroll_offset +
                    static_cast<size_t>(opts.max_visible)) {
                    scroll_offset = cursor_pos -
                        static_cast<size_t>(opts.max_visible) + 1;
                }
            }
        } else if (ch == 127 || ch == 8) {  // Backspace
            if (!query.empty()) {
                query.pop_back();
                needs_refilter = true;
            }
        } else if (ch >= 32 && ch < 127) {  // Printable ASCII
            query += static_cast<char>(ch);
            needs_refilter = true;
        }

        if (needs_refilter) {
            visible = filter_items(items, query, opts.case_sensitive);
            cursor_pos = 0;
            scroll_offset = 0;
        }

        prev_lines = render(items, visible, cursor_pos, scroll_offset,
                            query, opts, prev_lines);
    }

    clear_render(prev_lines);
    show_cursor();
    return std::nullopt;
}

}  // namespace kairos::cli
