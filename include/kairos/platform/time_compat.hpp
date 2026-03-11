/// include/kairos/platform/time_compat.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  time_compat.hpp — Cross-platform thread-safe time conversion             ║
// ║                                                                           ║
// ║  Wraps gmtime_r / localtime_r (POSIX) and gmtime_s / localtime_s (MSVC)  ║
// ║  into a uniform interface.                                                ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <ctime>

namespace kairos::platform {

/// Thread-safe UTC time conversion.
/// Equivalent to gmtime_r on POSIX, gmtime_s on MSVC.
inline std::tm* gmtime_safe(const std::time_t* timer, std::tm* buf) {
#ifdef _WIN32
    return (::gmtime_s(buf, timer) == 0) ? buf : nullptr;
#else
    return ::gmtime_r(timer, buf);
#endif
}

/// Thread-safe local time conversion.
/// Equivalent to localtime_r on POSIX, localtime_s on MSVC.
inline std::tm* localtime_safe(const std::time_t* timer, std::tm* buf) {
#ifdef _WIN32
    return (::localtime_s(buf, timer) == 0) ? buf : nullptr;
#else
    return ::localtime_r(timer, buf);
#endif
}

}  // namespace kairos::platform
