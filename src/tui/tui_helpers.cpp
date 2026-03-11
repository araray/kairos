/// src/tui/tui_helpers.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  TUI helpers — always compiled (no FTXUI dependency)                    ║
// ║                                                                          ║
// ║  When KAIROS_TUI=ON, the full tui_dashboard.cpp also defines these     ║
// ║  functions (they share the same namespace). To avoid ODR violations,    ║
// ║  this file is only compiled when KAIROS_TUI=OFF. The test target        ║
// ║  always links this file directly.                                       ║
// ║                                                                          ║
// ║  Spec reference: §23 (CLI/TUI), §30 (testing)                          ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/tui/tui_helpers.hpp"

#include <chrono>
#include <ctime>
#include <string>

namespace kairos::tui::helpers {

std::string format_duration(double seconds) {
    if (seconds < 0) return "—";
    if (seconds < 60) return std::to_string(static_cast<int>(seconds)) + "s";
    if (seconds < 3600) {
        int m = static_cast<int>(seconds) / 60;
        int s = static_cast<int>(seconds) % 60;
        return std::to_string(m) + "m " + std::to_string(s) + "s";
    }
    int h = static_cast<int>(seconds) / 3600;
    int m = (static_cast<int>(seconds) % 3600) / 60;
    return std::to_string(h) + "h " + std::to_string(m) + "m";
}

std::string format_relative(const std::string& iso_ts) {
    if (iso_ts.empty()) return "—";
    try {
        std::tm tm{};
        if (iso_ts.size() >= 19) {
            tm.tm_year = std::stoi(iso_ts.substr(0, 4)) - 1900;
            tm.tm_mon  = std::stoi(iso_ts.substr(5, 2)) - 1;
            tm.tm_mday = std::stoi(iso_ts.substr(8, 2));
            tm.tm_hour = std::stoi(iso_ts.substr(11, 2));
            tm.tm_min  = std::stoi(iso_ts.substr(14, 2));
            tm.tm_sec  = std::stoi(iso_ts.substr(17, 2));
            tm.tm_isdst = -1;
            auto t = std::mktime(&tm);
            if (t == -1) return iso_ts.substr(11, 8);
            auto now = std::time(nullptr);
            auto diff = std::difftime(now, t);
            if (diff < 0)    return "just now";
            if (diff < 60)   return std::to_string(static_cast<int>(diff)) + "s ago";
            if (diff < 3600) return std::to_string(static_cast<int>(diff / 60)) + "m ago";
            if (diff < 86400) return std::to_string(static_cast<int>(diff / 3600)) + "h ago";
            return std::to_string(static_cast<int>(diff / 86400)) + "d ago";
        }
    } catch (...) {}
    return iso_ts.size() > 11 ? iso_ts.substr(11, 8) : iso_ts;
}

std::string status_icon(const std::string& status) {
    if (status == "SUCCESS")   return "✓";
    if (status == "FAILED")    return "✗";
    if (status == "RUNNING")   return "●";
    if (status == "CANCELLED") return "⊘";
    if (status == "SKIPPED")   return "○";
    if (status == "TIMED_OUT") return "⏱";
    return "?";
}

std::string format_local_time_now() {
    auto now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

}  // namespace kairos::tui::helpers
