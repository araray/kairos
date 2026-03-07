// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/platform/paths.hpp — Cross-platform path normalization            ║
// ║                                                                           ║
// ║  Spec reference: §25.3                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace kairos::platform {

/// Canonicalize a path:
///   1. Expand ~ and ~user prefixes (POSIX) / %USERPROFILE% (Windows).
///   2. Resolve relative paths against `base` (or cwd if nullopt).
///   3. Normalize separators (forward slash on all platforms).
///   4. Collapse . and .. segments.
///
/// Does NOT require the path to exist (unlike std::filesystem::canonical).
std::filesystem::path normalize_path(
    const std::filesystem::path& input,
    const std::optional<std::filesystem::path>& base = std::nullopt);

/// Convert a std::filesystem::path to a UTF-8 string.
/// On POSIX this is typically a no-op; on Windows it converts from
/// the native wchar_t representation.
std::string path_to_utf8(const std::filesystem::path& p);

/// Convert a UTF-8 string to a std::filesystem::path.
std::filesystem::path utf8_to_path(const std::string& s);

/// Get the user's home directory.
std::filesystem::path get_home_dir();

/// Get the platform-appropriate config directory for Kairos.
/// Linux:   $XDG_CONFIG_HOME/kairos or ~/.config/kairos
/// macOS:   ~/Library/Application Support/kairos
/// Windows: %APPDATA%\kairos
std::filesystem::path get_config_dir();

/// Get the platform-appropriate data directory for Kairos.
/// Linux:   $XDG_DATA_HOME/kairos or ~/.local/share/kairos
/// macOS:   ~/Library/Application Support/kairos
/// Windows: %LOCALAPPDATA%\kairos
std::filesystem::path get_data_dir();

/// Get the platform-appropriate log directory.
/// Linux:   $XDG_STATE_HOME/kairos or ~/.local/state/kairos
/// macOS:   ~/Library/Logs/kairos
/// Windows: %LOCALAPPDATA%\kairos\logs
std::filesystem::path get_log_dir();

}  // namespace kairos::platform
