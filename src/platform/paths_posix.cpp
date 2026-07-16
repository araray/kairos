/// src/platform/paths_posix.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  paths_posix.cpp — POSIX path normalization (Linux + macOS)               ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/platform/paths.hpp"
#include "kairos/platform/environment.hpp"

#include <cstdlib>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace kairos::platform {

std::filesystem::path normalize_path(
    const std::filesystem::path& input,
    const std::optional<std::filesystem::path>& base)
{
    std::string str = input.string();

    // Step 1: Expand ~ prefix.
    if (!str.empty() && str[0] == '~') {
        std::string expanded;
        if (str.size() == 1 || str[1] == '/') {
            // ~/... → home dir
            expanded = get_home_dir().string();
            if (str.size() > 1) {
                expanded += str.substr(1);
            }
        } else {
            // ~user/... → that user's home dir
            auto slash_pos = str.find('/');
            std::string username = str.substr(1, slash_pos - 1);
            if (struct passwd* pw = getpwnam(username.c_str()); pw) {
                expanded = pw->pw_dir;
                if (slash_pos != std::string::npos) {
                    expanded += str.substr(slash_pos);
                }
            } else {
                // Unknown user — leave as-is.
                expanded = str;
            }
        }
        str = expanded;
    }

    fs::path result(str);

    // Step 2: Resolve relative paths.
    if (result.is_relative()) {
        if (base.has_value()) {
            result = base.value() / result;
        } else {
            result = fs::current_path() / result;
        }
    }

    // Step 3: Collapse . and .. segments (lexically, without requiring existence).
    result = result.lexically_normal();

    return result;
}

std::string path_to_utf8(const std::filesystem::path& p) {
    // On POSIX, paths are already UTF-8 (or at least byte sequences).
    return p.string();
}

std::filesystem::path utf8_to_path(const std::string& s) {
    return fs::path(s);
}

std::filesystem::path get_home_dir() {
    // Prefer $HOME, fall back to getpwuid.
    if (auto home = get_env("HOME"); home && !home->empty()) {
        return fs::path(*home);
    }
    if (struct passwd* pw = getpwuid(getuid()); pw) {
        return fs::path(pw->pw_dir);
    }
    return fs::path("/tmp");  // last resort
}

std::filesystem::path get_config_dir() {
#ifdef __APPLE__
    return get_home_dir() / "Library" / "Application Support" / "kairos";
#else
    // XDG Base Directory spec.
    if (auto xdg = get_env("XDG_CONFIG_HOME"); xdg && !xdg->empty()) {
        return fs::path(*xdg) / "kairos";
    }
    return get_home_dir() / ".config" / "kairos";
#endif
}

std::filesystem::path get_data_dir() {
#ifdef __APPLE__
    return get_home_dir() / "Library" / "Application Support" / "kairos";
#else
    if (auto xdg = get_env("XDG_DATA_HOME"); xdg && !xdg->empty()) {
        return fs::path(*xdg) / "kairos";
    }
    return get_home_dir() / ".local" / "share" / "kairos";
#endif
}

std::filesystem::path get_log_dir() {
#ifdef __APPLE__
    return get_home_dir() / "Library" / "Logs" / "kairos";
#else
    if (auto xdg = get_env("XDG_STATE_HOME"); xdg && !xdg->empty()) {
        return fs::path(*xdg) / "kairos";
    }
    return get_home_dir() / ".local" / "state" / "kairos";
#endif
}

}  // namespace kairos::platform
