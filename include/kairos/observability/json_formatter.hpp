/// include/kairos/observability/json_formatter.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/observability/json_formatter.hpp — JSON log formatter             ║
// ║                                                                           ║
// ║  Produces JSON-structured log lines per the schema defined in §19.2.      ║
// ║  Each line is a complete JSON object for easy parsing.                     ║
// ║                                                                           ║
// ║  Supports secret masking (§17.3 defense-in-depth): if mask values are    ║
// ║  configured, all occurrences in the message are replaced with "***".     ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <spdlog/formatter.h>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace kairos::observability {

/// Custom spdlog formatter that emits one JSON object per log line.
///
/// Output format:
/// {
///   "ts":"2026-03-02T14:30:05.123Z",
///   "level":"info",
///   "logger":"kairos",
///   "msg":"…",
///   "thread":"main",
///   "src":"file.cpp:42"
/// }
///
/// Secret masking (§17.3 defense-in-depth):
///   After the message is rendered, any configured secret values are
///   replaced with "***". This protects against Kairos's own log
///   messages accidentally including secret values — complements the
///   primary masking in OutputMultiplexer.
class KairosJsonFormatter : public spdlog::formatter {
public:
    void format(const spdlog::details::log_msg& msg,
                spdlog::memory_buf_t& dest) override;

    [[nodiscard]] std::unique_ptr<spdlog::formatter> clone() const override;

    /// Set the list of secret values to mask in log output.
    /// Thread-safe. Values should be pre-sorted longest-first
    /// (same ordering as OutputMultiplexer).
    void set_mask_values(std::vector<std::string> values);

    /// @return true if masking is active (at least one mask value set).
    [[nodiscard]] bool has_mask_values() const;

private:
    /// Apply masking to a string, replacing secret values with "***".
    [[nodiscard]] std::string apply_masking(std::string_view input) const;

    mutable std::mutex mask_mutex_;
    std::vector<std::string> mask_values_;
};

}  // namespace kairos::observability
