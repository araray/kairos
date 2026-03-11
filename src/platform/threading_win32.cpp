/// src/platform/threading_win32.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  threading_win32.cpp — Thread naming via SetThreadDescription             ║
// ║                                                                           ║
// ║  Requires Windows 10 version 1607 (build 14393) or later.                ║
// ║  Thread names are visible in Visual Studio debugger, WinDbg, and          ║
// ║  Process Explorer.                                                        ║
// ║                                                                           ║
// ║  Spec reference: §25.5                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef _WIN32

#include "kairos/platform/threading.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <string>

namespace kairos::platform {

void set_thread_name(std::string_view name) {
    // Convert UTF-8 name to UTF-16 for SetThreadDescription.
    if (name.empty()) return;

    int needed = ::MultiByteToWideChar(
        CP_UTF8, 0, name.data(), static_cast<int>(name.size()),
        nullptr, 0);
    if (needed <= 0) return;

    std::wstring wname(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, name.data(), static_cast<int>(name.size()),
        wname.data(), needed);

    // SetThreadDescription is available on Win10 1607+.
    // Since we target Win10+, we call it directly.
    (void)::SetThreadDescription(::GetCurrentThread(), wname.c_str());
}

}  // namespace kairos::platform

#endif  // _WIN32
