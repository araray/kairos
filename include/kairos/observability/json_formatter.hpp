// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/observability/json_formatter.hpp — JSON log formatter             ║
// ║                                                                           ║
// ║  Produces JSON-structured log lines per the schema defined in §19.2.      ║
// ║  Each line is a complete JSON object for easy parsing.                     ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <spdlog/formatter.h>

#include <memory>

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
class KairosJsonFormatter : public spdlog::formatter {
public:
    void format(const spdlog::details::log_msg& msg,
                spdlog::memory_buf_t& dest) override;

    [[nodiscard]] std::unique_ptr<spdlog::formatter> clone() const override;
};

}  // namespace kairos::observability
