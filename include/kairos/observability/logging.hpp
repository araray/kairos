/// include/kairos/observability/logging.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/observability/logging.hpp — Structured logging initialization     ║
// ║                                                                           ║
// ║  Configures spdlog with JSON formatter (daemon mode) or colored text      ║
// ║  (interactive mode).  Supports async mode via bounded ring buffer.        ║
// ║                                                                           ║
// ║  Spec reference: §19                                                      ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <spdlog/spdlog.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kairos::observability {

/// Configuration for the logging subsystem, sourced from confy-cpp.
struct LogConfig {
    spdlog::level::level_enum level = spdlog::level::info;
    bool        json_format      = true;     ///< JSON for daemon, text for interactive
    std::string file_path;                   ///< Empty = no file sink
    size_t      max_file_size_mb = 100;
    size_t      max_files        = 5;
    size_t      async_queue_size = 8192;
};

/// Parse a log level string (e.g., "info", "debug") into spdlog enum.
/// Returns spdlog::level::info on unrecognized input.
spdlog::level::level_enum parse_log_level(std::string_view level_str);

/// Initialize the Kairos logging subsystem.
///
/// Creates a shared logger named "kairos" with:
///   - stdout sink (JSON or text depending on config + TTY detection)
///   - Optional rotating file sink (always JSON)
///   - Async mode with bounded queue
///
/// Must be called once, early in main(), before any other logging calls.
void initialize_logging(const LogConfig& config);

/// Shut down the logging subsystem.  Flushes all pending messages.
void shutdown_logging();

/// Get the shared kairos logger.  Returns the default spdlog logger
/// which is set during initialize_logging().
std::shared_ptr<spdlog::logger> get_logger();

/// Set secret mask values on all active KairosJsonFormatters.
/// This is the defense-in-depth masking path (§17.3): any log message
/// containing secret values will have them replaced with "***".
///
/// The primary masking path is OutputMultiplexer (process output).
/// This function provides a secondary masking layer for Kairos's own
/// log messages that might accidentally include secret values.
///
/// @param values  Secret values, pre-sorted longest-first.
void set_log_mask_values(const std::vector<std::string>& values);

}  // namespace kairos::observability
