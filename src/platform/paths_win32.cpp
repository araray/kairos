/// src/platform/paths_win32.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  paths_win32.cpp — Windows path normalization, known folders, UTF-8/16    ║
// ║                                                                           ║
// ║  Spec reference: §25.3                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef _WIN32

#include "kairos/platform/paths.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <ShlObj.h>   // SHGetKnownFolderPath, FOLDERID_*

#include <cstdlib>
#include <memory>     // std::unique_ptr for CoTaskMemFree

namespace fs = std::filesystem;

namespace kairos::platform {

// ── Internal helpers ─────────────────────────────────────────────────────

namespace {

/// RAII wrapper for CoTaskMemFree on SHGetKnownFolderPath results.
struct CoTaskMemDeleter {
    void operator()(wchar_t* p) const noexcept {
        if (p) ::CoTaskMemFree(p);
    }
};
using CoString = std::unique_ptr<wchar_t, CoTaskMemDeleter>;

/// Convert a UTF-8 std::string to UTF-16 std::wstring.
std::wstring to_utf16(const std::string& utf8) {
    if (utf8.empty()) return {};
    int needed = ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        result.data(), needed);
    return result;
}

/// Convert a UTF-16 std::wstring to UTF-8 std::string.
std::string to_utf8(const std::wstring& utf16) {
    if (utf16.empty()) return {};
    int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string result(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
        result.data(), needed, nullptr, nullptr);
    return result;
}

/// Retrieve a Windows known folder path by KNOWNFOLDERID.
/// Returns empty path on failure.
fs::path get_known_folder(const KNOWNFOLDERID& folder_id) {
    wchar_t* raw = nullptr;
    HRESULT hr = ::SHGetKnownFolderPath(folder_id, 0, nullptr, &raw);
    CoString guard(raw);
    if (SUCCEEDED(hr) && raw) {
        return fs::path(raw);
    }
    return {};
}

}  // anonymous namespace

// ── Public API ───────────────────────────────────────────────────────────

std::filesystem::path normalize_path(
    const std::filesystem::path& input,
    const std::optional<std::filesystem::path>& base)
{
    std::string str = path_to_utf8(input);

    // Step 1: Expand ~ prefix (bash-compat on Windows too).
    if (!str.empty() && str[0] == '~') {
        std::string expanded;
        if (str.size() == 1 || str[1] == '/' || str[1] == '\\') {
            expanded = path_to_utf8(get_home_dir());
            if (str.size() > 1) {
                expanded += str.substr(1);
            }
        } else {
            // ~user not supported on Windows — leave as-is.
            expanded = str;
        }
        str = expanded;
    }

    // Step 2: Expand %ENVVAR% patterns.
    {
        std::wstring wstr = to_utf16(str);
        DWORD needed = ::ExpandEnvironmentStringsW(
            wstr.c_str(), nullptr, 0);
        if (needed > 0) {
            std::wstring expanded(static_cast<std::size_t>(needed), L'\0');
            DWORD written = ::ExpandEnvironmentStringsW(
                wstr.c_str(), expanded.data(), needed);
            if (written > 0 && written <= needed) {
                // Remove trailing NUL that ExpandEnvironmentStringsW counts.
                expanded.resize(written - 1);
                str = to_utf8(expanded);
            }
        }
    }

    fs::path result = utf8_to_path(str);

    // Step 3: Resolve relative paths.
    if (result.is_relative()) {
        if (base.has_value()) {
            result = base.value() / result;
        } else {
            result = fs::current_path() / result;
        }
    }

    // Step 4: Collapse . and .. segments lexically.
    result = result.lexically_normal();

    return result;
}

std::string path_to_utf8(const std::filesystem::path& p) {
    // On Windows, std::filesystem::path stores wchar_t internally.
    // Convert to UTF-8 via the u8string() method (C++17/20).
    auto u8s = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8s.data()), u8s.size());
}

std::filesystem::path utf8_to_path(const std::string& s) {
    // Convert UTF-8 → UTF-16 → path.
    std::wstring wide = to_utf16(s);
    return fs::path(wide);
}

std::filesystem::path get_home_dir() {
    // Prefer %USERPROFILE%, fall back to SHGetKnownFolderPath.
    if (const char* home = std::getenv("USERPROFILE"); home && home[0]) {
        return fs::path(to_utf16(home));
    }
    fs::path folder = get_known_folder(FOLDERID_Profile);
    if (!folder.empty()) return folder;
    return fs::path("C:\\Users\\Default");  // last resort
}

std::filesystem::path get_config_dir() {
    // %APPDATA%\kairos  (Roaming — follows user across domain machines).
    fs::path roaming = get_known_folder(FOLDERID_RoamingAppData);
    if (roaming.empty()) {
        if (const char* v = std::getenv("APPDATA"); v && v[0]) {
            roaming = fs::path(to_utf16(v));
        } else {
            roaming = get_home_dir() / "AppData" / "Roaming";
        }
    }
    return roaming / "kairos";
}

std::filesystem::path get_data_dir() {
    // %LOCALAPPDATA%\kairos
    fs::path local = get_known_folder(FOLDERID_LocalAppData);
    if (local.empty()) {
        if (const char* v = std::getenv("LOCALAPPDATA"); v && v[0]) {
            local = fs::path(to_utf16(v));
        } else {
            local = get_home_dir() / "AppData" / "Local";
        }
    }
    return local / "kairos";
}

std::filesystem::path get_log_dir() {
    return get_data_dir() / "logs";
}

}  // namespace kairos::platform

#endif  // _WIN32
