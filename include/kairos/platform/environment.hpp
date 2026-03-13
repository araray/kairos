/// include/kairos/platform/environment.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/platform/environment.hpp — Thread-safe environment variable API  ║
// ║                                                                          ║
// ║  On Windows, std::getenv is NOT thread-safe (MSVC deprecates it).        ║
// ║  This abstraction uses _dupenv_s on Windows and std::getenv on POSIX.    ║
// ║                                                                          ║
// ║  Roadmap §13: Windows _dupenv_s migration                                ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <optional>
#include <string>

namespace kairos::platform {

/// Thread-safe environment variable lookup.
///
/// On Windows, uses _dupenv_s (which allocates a copy, avoiding the
/// data race inherent in std::getenv's shared static buffer).
/// On POSIX, uses std::getenv (which is safe if no concurrent
/// setenv/putenv, which Kairos never calls outside of tests).
///
/// @param name  The environment variable name.
/// @return The variable's value, or std::nullopt if unset.
[[nodiscard]] std::optional<std::string> get_env(const char* name);

/// Thread-safe environment variable setter.
/// On Windows, uses _putenv_s. On POSIX, uses setenv.
///
/// @param name   Variable name.
/// @param value  Variable value.
/// @return true on success.
bool set_env(const char* name, const char* value);

/// Remove an environment variable.
/// On Windows, uses _putenv_s(name, ""). On POSIX, uses unsetenv.
///
/// @param name  Variable name.
/// @return true on success.
bool unset_env(const char* name);

}  // namespace kairos::platform
