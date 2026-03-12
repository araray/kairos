/// src/platform/timezone.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  timezone.cpp — UTC-first timezone handling implementation                ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/platform/timezone.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <ctime>
#endif

namespace kairos::platform {

// ── TimezoneConfig ───────────────────────────────────────────────────────

TimezoneConfig TimezoneConfig::parse(std::string_view tz_str) {
    TimezoneConfig cfg;

    if (tz_str.empty() || tz_str == "UTC" || tz_str == "utc" ||
        tz_str == "Z" || tz_str == "z") {
        cfg.mode = TimezoneMode::UTC;
        cfg.offset_minutes = 0;
        return cfg;
    }

    // "local" / "Local" / "LOCAL" — use system timezone.
    {
        std::string lower(tz_str);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower == "local" || lower == "system") {
            cfg.mode = TimezoneMode::Local;
            cfg.offset_minutes = 0;
            return cfg;
        }
    }

    // Fixed offset: "+HH:MM", "-HH:MM", "+HHMM", "-HHMM", "+HH", "-HH".
    if (tz_str.size() >= 3 && (tz_str[0] == '+' || tz_str[0] == '-')) {
        int sign = (tz_str[0] == '-') ? -1 : 1;
        int hours = 0, minutes = 0;

        auto digits = tz_str.substr(1);
        if (digits.size() >= 4 && digits[2] == ':') {
            // +HH:MM
            hours   = (digits[0] - '0') * 10 + (digits[1] - '0');
            minutes = (digits[3] - '0') * 10 + (digits[4] - '0');
        } else if (digits.size() >= 4) {
            // +HHMM
            hours   = (digits[0] - '0') * 10 + (digits[1] - '0');
            minutes = (digits[2] - '0') * 10 + (digits[3] - '0');
        } else if (digits.size() >= 2) {
            // +HH
            hours = (digits[0] - '0') * 10 + (digits[1] - '0');
        }

        cfg.mode = TimezoneMode::Offset;
        cfg.offset_minutes = sign * (hours * 60 + minutes);
        return cfg;
    }

    // Unrecognized — default to UTC.
    cfg.mode = TimezoneMode::UTC;
    cfg.offset_minutes = 0;
    return cfg;
}

std::string TimezoneConfig::suffix() const {
    switch (mode) {
        case TimezoneMode::UTC:
            return "Z";
        case TimezoneMode::Local:
            return "";  // System-dependent, no fixed suffix.
        case TimezoneMode::Offset: {
            if (offset_minutes == 0) return "Z";
            char buf[16];
            int abs_min = std::abs(offset_minutes);
            std::snprintf(buf, sizeof(buf), "%c%02d:%02d",
                          offset_minutes >= 0 ? '+' : '-',
                          abs_min / 60, abs_min % 60);
            return std::string(buf);
        }
    }
    return "Z";
}

// ── UTC timestamp parsing ────────────────────────────────────────────────

std::chrono::system_clock::time_point
parse_utc_timestamp(const std::string& utc_ts)
{
    if (utc_ts.empty()) return {};

    std::tm tm{};
    const char* p = utc_ts.c_str();

    // Try "YYYY-MM-DDTHH:MM:SS" or "YYYY-MM-DD HH:MM:SS".
    // Manual parsing for reliability across compilers.
    if (utc_ts.size() < 19) return {};

    auto digit2 = [](const char* s) -> int {
        return (s[0] - '0') * 10 + (s[1] - '0');
    };
    auto digit4 = [](const char* s) -> int {
        return (s[0] - '0') * 1000 + (s[1] - '0') * 100 +
               (s[2] - '0') * 10 + (s[3] - '0');
    };

    tm.tm_year = digit4(p) - 1900;       // YYYY
    tm.tm_mon  = digit2(p + 5) - 1;       // MM
    tm.tm_mday = digit2(p + 8);           // DD
    tm.tm_hour = digit2(p + 11);          // HH
    tm.tm_min  = digit2(p + 14);          // MM
    tm.tm_sec  = digit2(p + 17);          // SS

    // Interpret as UTC.
#ifdef _WIN32
    std::time_t t = _mkgmtime(&tm);
#else
    std::time_t t = timegm(&tm);
#endif

    if (t == static_cast<std::time_t>(-1)) return {};
    return std::chrono::system_clock::from_time_t(t);
}

// ── Formatting helpers ───────────────────────────────────────────────────

namespace {

/// Convert a time_t to std::tm in UTC.
std::tm to_utc_tm(std::time_t t) {
    std::tm result{};
#ifdef _WIN32
    gmtime_s(&result, &t);
#else
    gmtime_r(&t, &result);
#endif
    return result;
}

/// Convert a time_t to std::tm in local time.
std::tm to_local_tm(std::time_t t) {
    std::tm result{};
#ifdef _WIN32
    localtime_s(&result, &t);
#else
    localtime_r(&t, &result);
#endif
    return result;
}

/// Format a std::tm to "YYYY-MM-DDTHH:MM:SS".
std::string format_tm(const std::tm& tm) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

}  // anonymous namespace

// ── Display formatting ───────────────────────────────────────────────────

std::string format_display_time(
    std::chrono::system_clock::time_point tp,
    const TimezoneConfig& tz)
{
    if (tp.time_since_epoch().count() == 0) return "--";

    auto t = std::chrono::system_clock::to_time_t(tp);

    switch (tz.mode) {
        case TimezoneMode::UTC:
            return format_tm(to_utc_tm(t)) + "Z";

        case TimezoneMode::Local:
            return format_tm(to_local_tm(t));

        case TimezoneMode::Offset: {
            // Apply offset manually: add offset to UTC time.
            t += tz.offset_minutes * 60;
            return format_tm(to_utc_tm(t)) + tz.suffix();
        }
    }
    return format_tm(to_utc_tm(t)) + "Z";
}

std::string format_display_time(
    const std::string& utc_ts,
    const TimezoneConfig& tz)
{
    if (tz.mode == TimezoneMode::UTC) {
        // Fast path: UTC display returns the stored string as-is
        // (it's already in UTC). Just ensure Z suffix.
        if (!utc_ts.empty() && utc_ts.back() != 'Z') {
            // Truncate fractional seconds if present.
            auto dot = utc_ts.find('.', 17);
            if (dot != std::string::npos) {
                return utc_ts.substr(0, dot) + "Z";
            }
            return utc_ts + "Z";
        }
        return utc_ts;
    }
    auto tp = parse_utc_timestamp(utc_ts);
    return format_display_time(tp, tz);
}

// ── Relative time formatting ─────────────────────────────────────────────

std::string format_relative(std::chrono::system_clock::time_point tp) {
    if (tp.time_since_epoch().count() == 0) return "--";

    auto now = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(
        now - tp).count();

    if (diff < 0) {
        diff = -diff;
        if (diff < 60)    return std::to_string(diff) + "s from now";
        if (diff < 3600)  return std::to_string(diff / 60) + "m from now";
        if (diff < 86400) {
            auto h = diff / 3600;
            auto m = (diff % 3600) / 60;
            return std::to_string(h) + "h " + std::to_string(m) + "m from now";
        }
        return std::to_string(diff / 86400) + "d from now";
    }

    if (diff < 5)     return "just now";
    if (diff < 60)    return std::to_string(diff) + "s ago";
    if (diff < 3600)  return std::to_string(diff / 60) + "m ago";
    if (diff < 86400) return std::to_string(diff / 3600) + "h ago";
    return std::to_string(diff / 86400) + "d ago";
}

std::string format_relative(const std::string& utc_ts) {
    return format_relative(parse_utc_timestamp(utc_ts));
}

// ── Current time ─────────────────────────────────────────────────────────

std::string format_now(const TimezoneConfig& tz) {
    return format_display_time(std::chrono::system_clock::now(), tz);
}

// ── System UTC offset ────────────────────────────────────────────────────

int system_utc_offset_seconds() {
    std::time_t now = std::time(nullptr);
    std::tm local_tm{}, utc_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &now);
    gmtime_s(&utc_tm, &now);
#else
    localtime_r(&now, &local_tm);
    gmtime_r(&now, &utc_tm);
#endif
    // mktime interprets tm as local time; we use it on both to get
    // the difference. The UTC tm fed to mktime will be "wrong" by
    // the local offset, which is exactly what we want to measure.
    std::time_t local_tt = std::mktime(&local_tm);
    std::time_t utc_tt   = std::mktime(&utc_tm);
    return static_cast<int>(std::difftime(local_tt, utc_tt));
}

int config_utc_offset_seconds(const TimezoneConfig& tz) {
    switch (tz.mode) {
        case TimezoneMode::UTC:
            return 0;
        case TimezoneMode::Local:
            return system_utc_offset_seconds();
        case TimezoneMode::Offset:
            return tz.offset_minutes * 60;
    }
    return 0;
}

int cron_tz_delta_seconds(const TimezoneConfig& tz) {
    // croncpp uses localtime_r internally, which applies the system
    // timezone.  To make it evaluate cron expressions in the configured
    // timezone instead, we shift the time_t by the difference.
    return config_utc_offset_seconds(tz) - system_utc_offset_seconds();
}

}  // namespace kairos::platform
