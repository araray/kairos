/// src/platform/environment_win32.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Windows environment variable implementation                               ║
// ║  Uses _dupenv_s (thread-safe, allocates a copy) per MSVC recommendation.   ║
// ║ https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/dupenv-s-wdupenv-s
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef _WIN32

#include "kairos/platform/environment.hpp"

#include <cstdlib>  // _dupenv_s, _putenv_s, free

namespace kairos::platform {

std::optional<std::string> get_env(const char* name) {
    if (!name) return std::nullopt;

    char* buf = nullptr;
    size_t len = 0;

    // _dupenv_s is thread-safe: it allocates a private copy of the
    // environment variable value (unlike std::getenv which returns
    // a pointer to shared internal storage).
    errno_t err = _dupenv_s(&buf, &len, name);
    if (err != 0 || buf == nullptr) {
        return std::nullopt;
    }

    std::string result(buf);
    free(buf);
    return result;
}

bool set_env(const char* name, const char* value) {
    if (!name || !value) return false;
    return _putenv_s(name, value) == 0;
}

bool unset_env(const char* name) {
    if (!name) return false;
    // Setting to empty string removes the variable on Windows.
    return _putenv_s(name, "") == 0;
}

}  // namespace kairos::platform

#endif  // _WIN32
