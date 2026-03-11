/// src/exec/env_builder.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  EnvBuilder implementation                                               ║
// ║  Spec reference: §14.6                                                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/exec/env_builder.hpp"

#include <cstdlib>
#include <regex>

#ifdef _WIN32
// Windows: environment block via GetEnvironmentStringsW → UTF-8 conversion.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
extern char** environ;
#endif

namespace kairos::exec {

// ── Internal helper for Win32 UTF-16 → UTF-8 conversion ─────────────────

#ifdef _WIN32
namespace {
std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string result(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
        result.data(), needed, nullptr, nullptr);
    return result;
}
}  // anonymous namespace
#endif

// ── Capture current environment ──────────────────────────────────────────

std::unordered_map<std::string, std::string> capture_current_env() {
    std::unordered_map<std::string, std::string> env;
#ifdef _WIN32
    wchar_t* env_block = ::GetEnvironmentStringsW();
    if (env_block) {
        for (const wchar_t* p = env_block; *p; p += wcslen(p) + 1) {
            std::wstring entry(p);
            auto pos = entry.find(L'=');
            // Skip entries starting with '=' (Windows internal vars like =C:).
            if (pos != std::wstring::npos && pos > 0) {
                std::string key = wstring_to_utf8(entry.substr(0, pos));
                std::string val = wstring_to_utf8(entry.substr(pos + 1));
                env[key] = val;
            }
        }
        ::FreeEnvironmentStringsW(env_block);
    }
#else
    if (environ == nullptr) return env;
    for (char** p = environ; *p != nullptr; ++p) {
        std::string entry(*p);
        auto pos = entry.find('=');
        if (pos != std::string::npos) {
            env[entry.substr(0, pos)] = entry.substr(pos + 1);
        }
    }
#endif
    return env;
}

// ── EnvBuilder methods ──────────────────────────────────────────────────

EnvBuilder& EnvBuilder::inherit_parent() {
    env_ = capture_current_env();
    return *this;
}

void EnvBuilder::merge(
    const std::unordered_map<std::string, std::string>& layer) {
    for (const auto& [key, value] : layer) {
        env_[key] = value;
    }
}

EnvBuilder& EnvBuilder::add_global(
    const std::unordered_map<std::string, std::string>& global_env) {
    merge(global_env);
    return *this;
}

EnvBuilder& EnvBuilder::add_workflow(
    const std::unordered_map<std::string, std::string>& wf_env) {
    merge(wf_env);
    return *this;
}

EnvBuilder& EnvBuilder::add_job(
    const std::unordered_map<std::string, std::string>& job_env) {
    merge(job_env);
    return *this;
}

EnvBuilder& EnvBuilder::add_step(
    const std::unordered_map<std::string, std::string>& step_env) {
    merge(step_env);
    return *this;
}

EnvBuilder& EnvBuilder::resolve_secrets(SecretResolver resolver) {
    if (!resolver) return *this;
    scan_and_resolve(resolver);
    return *this;
}

EnvBuilder& EnvBuilder::add_kairos_vars(
    const std::string& run_id,
    const std::string& job_id,
    const std::string& step_id,
    const std::string& workflow_id,
    const std::string& trigger_type,
    const std::string& correlation_id,
    const std::string& data_dir,
    const std::string& db_path) {
    env_["KAIROS_RUN_ID"]        = run_id;
    env_["KAIROS_JOB_ID"]        = job_id;
    env_["KAIROS_STEP_ID"]       = step_id;
    env_["KAIROS_WORKFLOW_ID"]   = workflow_id;
    env_["KAIROS_TRIGGER_TYPE"]  = trigger_type;
    env_["KAIROS_CORRELATION_ID"] = correlation_id;
    env_["KAIROS_DATA_DIR"]      = data_dir;
    env_["KAIROS_DB_PATH"]       = db_path;
    return *this;
}

std::unordered_map<std::string, std::string> EnvBuilder::build() const {
    // Ensure PATH is always present (defensive).
    auto result = env_;
    if (result.find("PATH") == result.end()) {
        // Minimal fallback PATH.
#ifdef _WIN32
        result["PATH"] = R"(C:\Windows\System32;C:\Windows)";
#else
        result["PATH"] = "/usr/local/bin:/usr/bin:/bin";
#endif
    }
    return result;
}

const std::vector<std::string>& EnvBuilder::resolved_secret_keys() const {
    return resolved_secrets_;
}

void EnvBuilder::scan_and_resolve(SecretResolver& resolver) {
    // Pattern: ${{ secrets.key_name }}
    // Allows whitespace around the key name.
    static const std::regex secret_re(
        R"(\$\{\{\s*secrets\.(\w+)\s*\}\})",
        std::regex_constants::ECMAScript);

    for (auto& [key, value] : env_) {
        std::string result;
        std::string::const_iterator search_start = value.cbegin();
        std::smatch match;
        bool modified = false;

        while (std::regex_search(search_start, value.cend(),
                                 match, secret_re)) {
            // Append text before the match.
            result.append(search_start, match[0].first);

            std::string secret_key = match[1].str();
            auto resolved = resolver(secret_key);
            if (resolved.has_value()) {
                result += *resolved;
                resolved_secrets_.push_back(secret_key);
            } else {
                // Leave unresolved reference as-is.
                result += match[0].str();
            }

            search_start = match[0].second;
            modified = true;
        }

        if (modified) {
            // Append remaining text after the last match.
            result.append(search_start, value.cend());
            value = std::move(result);
        }
    }
}

}  // namespace kairos::exec
