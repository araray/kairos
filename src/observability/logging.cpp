/// src/observability/logging.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  logging.cpp — Logging subsystem initialization                           ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/observability/logging.hpp"
#include "kairos/observability/json_formatter.hpp"

#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <vector>

namespace kairos::observability {

spdlog::level::level_enum parse_log_level(std::string_view level_str) {
    std::string lower(level_str);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (lower == "trace")    return spdlog::level::trace;
    if (lower == "debug")    return spdlog::level::debug;
    if (lower == "info")     return spdlog::level::info;
    if (lower == "warn" || lower == "warning")
                             return spdlog::level::warn;
    if (lower == "error" || lower == "err")
                             return spdlog::level::err;
    if (lower == "critical" || lower == "fatal")
                             return spdlog::level::critical;
    if (lower == "off")      return spdlog::level::off;

    return spdlog::level::info;  // safe default
}

void initialize_logging(const LogConfig& config) {
    // Initialize the async thread pool for spdlog.
    spdlog::init_thread_pool(config.async_queue_size, 1);

    std::vector<spdlog::sink_ptr> sinks;

    // ─── Console sink ─────────────────────────────────────────────────
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    if (config.json_format) {
        console_sink->set_formatter(std::make_unique<KairosJsonFormatter>());
    } else {
        // Human-friendly pattern: [timestamp] [level] [logger] message
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
    }
    console_sink->set_level(config.level);
    sinks.push_back(console_sink);

    // ─── File sink (optional) ─────────────────────────────────────────
    if (!config.file_path.empty()) {
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            config.file_path,
            config.max_file_size_mb * 1024 * 1024,
            config.max_files
        );
        // File sink always uses JSON for machine parsing.
        file_sink->set_formatter(std::make_unique<KairosJsonFormatter>());
        file_sink->set_level(config.level);
        sinks.push_back(file_sink);
    }

    // ─── Create the async logger ──────────────────────────────────────
    auto logger = std::make_shared<spdlog::async_logger>(
        "kairos",
        sinks.begin(), sinks.end(),
        spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest
    );
    logger->set_level(config.level);
    logger->flush_on(spdlog::level::warn);  // Flush on warn and above

    // Register as default logger.
    spdlog::set_default_logger(logger);
}

void shutdown_logging() {
    spdlog::default_logger()->flush();
    spdlog::shutdown();
}

std::shared_ptr<spdlog::logger> get_logger() {
    return spdlog::default_logger();
}

void set_log_mask_values(const std::vector<std::string>& values) {
    auto logger = spdlog::default_logger();
    if (!logger) return;

    // Iterate all sinks and set mask values on any KairosJsonFormatter.
    for (auto& sink : logger->sinks()) {
        // spdlog doesn't expose the formatter directly, but we can
        // set a new formatter with mask values.  Since we can't inspect
        // the existing formatter type, we create a new KairosJsonFormatter
        // with mask values and replace it.
        auto fmt = std::make_unique<KairosJsonFormatter>();
        fmt->set_mask_values(values);
        sink->set_formatter(std::move(fmt));
    }
}

}  // namespace kairos::observability
