/// include/kairos/platform/threading.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/platform/threading.hpp — Thread naming utility                    ║
// ║  Spec reference: §25.5                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <string_view>

namespace kairos::platform {

/// Set the name of the current thread (visible in debuggers and top/htop).
/// On Linux: pthread_setname_np (max 15 chars, silently truncated).
/// On macOS: pthread_setname_np (current thread only).
/// On Windows: SetThreadDescription (UTF-16 conversion).
void set_thread_name(std::string_view name);

}  // namespace kairos::platform
