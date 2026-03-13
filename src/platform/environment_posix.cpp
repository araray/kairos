/// src/platform/environment_posix.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  POSIX environment variable implementation                                ║
// ║  Uses std::getenv (safe when no concurrent setenv from other threads).    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifndef _WIN32

#include "kairos/platform/environment.hpp"

#include <cstdlib>

namespace kairos::platform {

std::optional<std::string> get_env(const char* name) {
    if (!name) return std::nullopt;
    // std::getenv is thread-safe for reads on POSIX when no concurrent
    // setenv/putenv. Kairos never calls setenv outside of tests.
    const char* val = std::getenv(name);
    if (!val) return std::nullopt;
    return std::string(val);
}

bool set_env(const char* name, const char* value) {
    if (!name || !value) return false;
    return ::setenv(name, value, /*overwrite=*/1) == 0;
}

bool unset_env(const char* name) {
    if (!name) return false;
    return ::unsetenv(name) == 0;
}

}  // namespace kairos::platform

#endif  // !_WIN32
