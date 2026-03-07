// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/config/config_store.hpp — Configuration loading and validation    ║
// ║                                                                           ║
// ║  Wraps confy-cpp with Kairos-specific defaults, mandatory keys,           ║
// ║  semantic validation, and atomic hot-reload.                              ║
// ║                                                                           ║
// ║  Spec reference: §6                                                       ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <confy/Config.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kairos::config {

/// Errors accumulated during semantic validation (beyond confy-cpp's
/// mandatory-key and type checks).
struct ValidationError {
    std::string key_path;      ///< e.g., "kairos.runners.worker_pool_size"
    std::string message;       ///< e.g., "must be >= 1, got 0"
    std::string source_file;   ///< which file the error originated from
    int         source_line;   ///< line number if available, -1 otherwise
};

/// Snapshot of fully-loaded configuration, shared across threads
/// via std::shared_ptr<const ConfigState>.
struct ConfigState {
    confy::Config              global;           ///< confy-cpp config object
    std::filesystem::path      config_file_path; ///< resolved config path
    std::filesystem::path      data_dir;         ///< kairos.data_dir
    std::filesystem::path      db_path;          ///< kairos.db_path
    uint32_t                   schema_version;   ///< current DB schema version
};

/// Result of configuration loading: either a valid state or errors.
struct LoadResult {
    std::shared_ptr<const ConfigState>  state;   ///< nullptr on failure
    std::vector<ValidationError>        errors;  ///< empty on success

    [[nodiscard]] bool ok() const noexcept { return state != nullptr; }
};

/// Build the complete Kairos defaults map for confy-cpp.
/// Every config key has a hardcoded default value here.
confy::Value build_kairos_defaults();

/// Resolve the config file path from CLI flag, environment, or platform
/// default.  Search order:
///   1. cli_override (if non-empty)
///   2. KAIROS_CONFIG_FILE environment variable
///   3. Platform default (~/.config/kairos/kairos.toml on Linux, etc.)
///   4. ./kairos.toml (cwd fallback)
std::filesystem::path resolve_config_path(
    const std::string& cli_override = "");

/// Load all configuration: global TOML + semantic validation.
/// Returns LoadResult with either a valid ConfigState or all validation errors.
LoadResult load_config(
    const std::filesystem::path& config_path,
    const std::unordered_map<std::string, confy::Value>& cli_overrides = {});

/// Run semantic validation on a loaded confy::Config.
/// Returns empty vector on success.
std::vector<ValidationError> validate_config(const confy::Config& cfg);

}  // namespace kairos::config
