/// src/platform/threading_posix.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  threading_posix.cpp — Thread naming (Linux/macOS)                        ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/platform/threading.hpp"

#include <pthread.h>
#include <algorithm>
#include <string>

namespace kairos::platform {

void set_thread_name(std::string_view name) {
    // Linux: pthread_setname_np takes max 15 chars + NUL.
    std::string truncated(name.substr(0, 15));

#ifdef __APPLE__
    // macOS: pthread_setname_np only takes the name (sets current thread).
    pthread_setname_np(truncated.c_str());
#else
    // Linux: pthread_setname_np takes thread handle + name.
    pthread_setname_np(pthread_self(), truncated.c_str());
#endif
}

}  // namespace kairos::platform
