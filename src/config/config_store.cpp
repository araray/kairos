/// src/config/config_store.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  config_store.cpp — Config loading and path resolution                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/config/config_store.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace kairos::config {

// ═══════════════════════════════════════════════════════════════════════════
// Config file path resolution (§6.2, Step 1)
// ═══════════════════════════════════════════════════════════════════════════

std::filesystem::path resolve_config_path(const std::string& cli_override) {
    // 1. CLI flag.
    if (!cli_override.empty()) {
        return fs::path(cli_override);
    }

    // 2. Environment variable.
    if (const char* env = std::getenv("KAIROS_CONFIG_FILE"); env && env[0]) {
        return fs::path(env);
    }

    // 3. Platform default.
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA"); appdata && appdata[0]) {
        auto p = fs::path(appdata) / "kairos" / "kairos.toml";
        if (fs::exists(p)) return p;
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && home[0]) {
        auto p = fs::path(home) / "Library" / "Application Support" / "kairos" / "kairos.toml";
        if (fs::exists(p)) return p;
    }
#else  // Linux / other POSIX
    // XDG_CONFIG_HOME or ~/.config/kairos/
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0]) {
        auto p = fs::path(xdg) / "kairos" / "kairos.toml";
        if (fs::exists(p)) return p;
    }
    if (const char* home = std::getenv("HOME"); home && home[0]) {
        auto p = fs::path(home) / ".config" / "kairos" / "kairos.toml";
        if (fs::exists(p)) return p;
    }
    // System default.
    if (fs::exists("/etc/kairos/kairos.toml")) {
        return "/etc/kairos/kairos.toml";
    }
#endif

    // 4. CWD fallback.
    return fs::current_path() / "kairos.toml";
}

// ═══════════════════════════════════════════════════════════════════════════
// Config loading (§6.2, Steps 2–4)
// ═══════════════════════════════════════════════════════════════════════════

LoadResult load_config(
    const std::filesystem::path& config_path,
    const std::unordered_map<std::string, confy::Value>& cli_overrides)
{
    LoadResult result;

    try {
        // Build confy-cpp LoadOptions.
        confy::LoadOptions opts;
        opts.defaults       = build_kairos_defaults();
        opts.prefix         = "KAIROS";
        opts.load_dotenv_file = true;
        opts.mandatory      = {"kairos.data_dir", "kairos.db_path"};

        // Only set file_path if the config file actually exists.
        // This allows running with just env vars / defaults when there
        // is no config file.
        if (fs::exists(config_path)) {
            opts.file_path = config_path.string();
        }

        // Apply CLI overrides.
        for (const auto& [key, val] : cli_overrides) {
            opts.overrides[key] = val;
        }

        // Step 3: Load via confy-cpp.
        confy::Config cfg = confy::Config::load(opts);

        // Step 4: Semantic validation.
        auto errors = validate_config(cfg);
        if (!errors.empty()) {
            result.errors = std::move(errors);
            return result;
        }

        // Build ConfigState snapshot.
        auto state = std::make_shared<ConfigState>();
        state->config_file_path = config_path;
        state->data_dir = cfg.get<std::string>("kairos.data_dir", ".");
        state->db_path  = cfg.get<std::string>("kairos.db_path", "./kairos.db");
        state->global   = std::move(cfg);
        state->schema_version = 0;  // Set after migration

        result.state = std::move(state);
    }
    catch (const confy::MissingMandatoryConfig& e) {
        ValidationError err;
        err.key_path    = "mandatory";
        err.message     = e.what();
        err.source_file = config_path.string();
        err.source_line = -1;
        result.errors.push_back(std::move(err));
    }
    catch (const confy::FileNotFoundError& e) {
        ValidationError err;
        err.key_path    = "file";
        err.message     = std::string("Config file not found: ") + e.what();
        err.source_file = config_path.string();
        err.source_line = -1;
        result.errors.push_back(std::move(err));
    }
    catch (const confy::ConfigParseError& e) {
        ValidationError err;
        err.key_path    = "parse";
        err.message     = std::string("Config parse error: ") + e.what();
        err.source_file = config_path.string();
        err.source_line = -1;
        result.errors.push_back(std::move(err));
    }
    catch (const std::exception& e) {
        ValidationError err;
        err.key_path    = "unknown";
        err.message     = std::string("Unexpected error: ") + e.what();
        err.source_file = config_path.string();
        err.source_line = -1;
        result.errors.push_back(std::move(err));
    }

    return result;
}

}  // namespace kairos::config
