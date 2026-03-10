/// src/observability/json_formatter.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  json_formatter.cpp — JSON-structured log line formatter                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/observability/json_formatter.hpp"

#include <spdlog/details/fmt_helper.h>

#include <chrono>
#include <ctime>

namespace kairos::observability {

namespace {

/// Format timestamp as ISO 8601 with milliseconds, always UTC.
void format_timestamp(const spdlog::log_clock::time_point& tp,
                      spdlog::memory_buf_t& dest) {
    auto epoch     = tp.time_since_epoch();
    auto secs      = std::chrono::duration_cast<std::chrono::seconds>(epoch);
    auto millis    = std::chrono::duration_cast<std::chrono::milliseconds>(epoch) -
                     std::chrono::duration_cast<std::chrono::milliseconds>(secs);
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);

    std::tm utc_tm{};
#ifdef _WIN32
    gmtime_s(&utc_tm, &time_t_val);
#else
    gmtime_r(&time_t_val, &utc_tm);
#endif

    // "2026-03-02T14:30:05.123Z"
    fmt::format_to(std::back_inserter(dest),
        "{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:03d}Z",
        utc_tm.tm_year + 1900, utc_tm.tm_mon + 1, utc_tm.tm_mday,
        utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec,
        static_cast<int>(millis.count()));
}

/// Escape a string for JSON output (handles quotes, backslashes, control chars).
void json_escape(std::string_view sv, spdlog::memory_buf_t& dest) {
    for (char c : sv) {
        switch (c) {
            case '"':  dest.push_back('\\'); dest.push_back('"'); break;
            case '\\': dest.push_back('\\'); dest.push_back('\\'); break;
            case '\n': dest.push_back('\\'); dest.push_back('n'); break;
            case '\r': dest.push_back('\\'); dest.push_back('r'); break;
            case '\t': dest.push_back('\\'); dest.push_back('t'); break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    fmt::format_to(std::back_inserter(dest), "\\u{:04x}",
                                   static_cast<unsigned int>(c));
                } else {
                    dest.push_back(c);
                }
                break;
        }
    }
}

/// Get spdlog level name as a short lowercase string.
std::string_view level_name(spdlog::level::level_enum lvl) {
    switch (lvl) {
        case spdlog::level::trace:    return "trace";
        case spdlog::level::debug:    return "debug";
        case spdlog::level::info:     return "info";
        case spdlog::level::warn:     return "warn";
        case spdlog::level::err:      return "error";
        case spdlog::level::critical: return "critical";
        default:                      return "off";
    }
}

}  // anonymous namespace

void KairosJsonFormatter::format(const spdlog::details::log_msg& msg,
                                 spdlog::memory_buf_t& dest) {
    // Open JSON object.
    dest.push_back('{');

    // "ts":"…"
    fmt::format_to(std::back_inserter(dest), "\"ts\":\"");
    format_timestamp(msg.time, dest);
    dest.push_back('"');

    // "level":"…"
    fmt::format_to(std::back_inserter(dest), ",\"level\":\"{}\"",
                   level_name(msg.level));

    // "logger":"…"
    fmt::format_to(std::back_inserter(dest), ",\"logger\":\"");
    json_escape(std::string_view(msg.logger_name.data(), msg.logger_name.size()), dest);
    dest.push_back('"');

    // "msg":"…" — with secret masking (§17.3 defense-in-depth).
    std::string_view payload(msg.payload.data(), msg.payload.size());
    fmt::format_to(std::back_inserter(dest), ",\"msg\":\"");
    if (has_mask_values()) {
        std::string masked = apply_masking(payload);
        json_escape(masked, dest);
    } else {
        json_escape(payload, dest);
    }
    dest.push_back('"');

    // "thread":"…"
    fmt::format_to(std::back_inserter(dest), ",\"thread\":{}", msg.thread_id);

    // "src":"file:line" (only if source location is available)
    if (!msg.source.empty()) {
        fmt::format_to(std::back_inserter(dest), ",\"src\":\"{}:{}\"",
                       msg.source.filename, msg.source.line);
    }

    // Close JSON object + newline.
    dest.push_back('}');
    dest.push_back('\n');
}

std::unique_ptr<spdlog::formatter> KairosJsonFormatter::clone() const {
    auto cloned = std::make_unique<KairosJsonFormatter>();
    std::lock_guard lock(mask_mutex_);
    cloned->mask_values_ = mask_values_;
    return cloned;
}

void KairosJsonFormatter::set_mask_values(std::vector<std::string> values) {
    std::lock_guard lock(mask_mutex_);
    mask_values_ = std::move(values);
}

bool KairosJsonFormatter::has_mask_values() const {
    std::lock_guard lock(mask_mutex_);
    return !mask_values_.empty();
}

std::string KairosJsonFormatter::apply_masking(std::string_view input) const {
    std::string result(input);
    std::lock_guard lock(mask_mutex_);

    // Mask values are pre-sorted longest-first to prevent partial
    // match interference (e.g., "password123" masked before "password").
    for (const auto& secret : mask_values_) {
        if (secret.empty()) continue;
        std::string::size_type pos = 0;
        while ((pos = result.find(secret, pos)) != std::string::npos) {
            result.replace(pos, secret.size(), "***");
            pos += 3;  // Skip past "***".
        }
    }
    return result;
}

}  // namespace kairos::observability
