/// include/kairos/platform/timezone.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  timezone.hpp — UTC-first timezone handling                               ║
// ║                                                                           ║
// ║  Design:                                                                  ║
// ║    - Storage: always UTC (ISO 8601 with Z suffix).                        ║
// ║    - Display: convert to configured timezone on output.                   ║
// ║    - Config: kairos.timezone = "UTC" | "local" | "+HH:MM" | "-HH:MM"     ║
// ║                                                                           ║
// ║  All internal timestamps pass through as UTC strings. Only display/CLI   ║
// ║  output paths call format_display_time() to convert for the user.        ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <chrono>
#include <ctime>
#include <string>
#include <string_view>

namespace kairos::platform {

/// Timezone display mode.
enum class TimezoneMode {
    UTC,     ///< Display as-is (UTC, suffix Z).
    Local,   ///< Convert to system local time.
    Offset,  ///< Apply a fixed offset (+HH:MM / -HH:MM).
};

/// Parsed timezone configuration.
struct TimezoneConfig {
    TimezoneMode mode = TimezoneMode::UTC;
    int offset_minutes = 0;  ///< Signed offset from UTC in minutes.

    /// Parse from config string: "UTC", "local", "+05:30", "-08:00".
    static TimezoneConfig parse(std::string_view tz_str);

    /// Format the timezone suffix for display (e.g., "Z", "+05:30", "").
    [[nodiscard]] std::string suffix() const;
};

/// Parse a UTC ISO-8601 timestamp string to a time_point.
/// Accepts "YYYY-MM-DDTHH:MM:SS", "YYYY-MM-DDTHH:MM:SSZ",
/// "YYYY-MM-DDTHH:MM:SS.fffZ", and "YYYY-MM-DD HH:MM:SS".
/// Returns epoch (time_point{}) on parse failure.
[[nodiscard]] std::chrono::system_clock::time_point
parse_utc_timestamp(const std::string& utc_ts);

/// Format a time_point as ISO-8601 in the configured timezone.
/// Produces "YYYY-MM-DDTHH:MM:SS" with appropriate suffix.
[[nodiscard]] std::string format_display_time(
    std::chrono::system_clock::time_point tp,
    const TimezoneConfig& tz);

/// Convenience: parse a UTC timestamp string and format for display.
/// This is the primary API for all CLI/TUI/HTTP display paths.
[[nodiscard]] std::string format_display_time(
    const std::string& utc_ts,
    const TimezoneConfig& tz);

/// Format a time_point as a relative string ("2h ago", "just now", etc.)
/// using the system clock for "now".
[[nodiscard]] std::string format_relative(
    std::chrono::system_clock::time_point tp);

/// Convenience: parse a UTC timestamp and format as relative.
[[nodiscard]] std::string format_relative(
    const std::string& utc_ts);

/// Get the current time formatted in the configured timezone.
[[nodiscard]] std::string format_now(const TimezoneConfig& tz);

}  // namespace kairos::platform
