// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  defaults.cpp — Kairos configuration default values                       ║
// ║                                                                           ║
// ║  Every config key MUST appear here.  If it's not listed, it doesn't       ║
// ║  exist in Kairos configuration.                                           ║
// ║                                                                           ║
// ║  Spec reference: §6.3                                                     ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/config/config_store.hpp"

namespace kairos::config {

confy::Value build_kairos_defaults() {
    return confy::Value({
        {"kairos", {
            // ─── Core paths ───────────────────────────────────────────
            {"data_dir",    ""},          // Set by resolve_config_path
            {"db_path",     ""},          // Set by resolve_config_path
            {"workflows_dir", "workflows"},
            {"watch_groups_dir", "watch_groups"},

            // ─── Logging ──────────────────────────────────────────────
            {"logging", {
                {"level",              "info"},
                {"format",             "auto"},      // "auto"|"json"|"text"
                {"file",               ""},          // empty = no file sink
                {"max_file_size_mb",   100},
                {"max_files",          5},
                {"async_queue_size",   8192},
            }},

            // ─── Runners ──────────────────────────────────────────────
            {"runners", {
                {"worker_pool_size",   4},
                {"default_timeout_s",  3600},
                {"default_shell",      ""},          // empty = platform default
                {"kill_timeout_s",     10},
            }},

            // ─── Scheduler ────────────────────────────────────────────
            {"scheduler", {
                {"tick_resolution_ms", 100},
                {"misfire_grace_s",    300},
            }},

            // ─── Watch engine ─────────────────────────────────────────
            {"watch", {
                {"default_mode",       "hybrid"},    // "native"|"sample"|"hybrid"
                {"sample_interval_s",  30},
                {"max_depth",          10},
                {"hash_policy",        "mtime+size"},// "mtime+size"|"sha256"
                {"follow_symlinks",    false},
                {"exclude_patterns",   nlohmann::json::array(
                    {".git", "node_modules", "__pycache__", ".DS_Store"}
                )},
            }},

            // ─── Persistence ──────────────────────────────────────────
            {"persistence", {
                {"retention_days",     90},
                {"batch_size",         100},
                {"flush_interval_ms",  1000},
                {"wal_mode",           true},
            }},

            // ─── MCP ─────────────────────────────────────────────────
            {"mcp", {
                {"enabled",            false},
                {"transport",          "stdio"},
                {"log_chunk_size",     4096},
            }},

            // ─── Platform ─────────────────────────────────────────────
            {"platform", {
                {"pid_file",           ""},
                {"default_shell",      ""},
                {"config_search_paths", nlohmann::json::array()},
                {"color",              "auto"},       // "auto"|"always"|"never"
            }},

            // ─── HTTP (Phase 4, but defaults included for completeness)
            {"http", {
                {"enabled",            false},
                {"listen_addr",        "127.0.0.1"},
                {"listen_port",        8420},
                {"api_token",          ""},
                {"static_dir",         ""},
                {"cors_enabled",       false},
                {"cors_origins",       "*"},
                {"read_timeout_s",     30},
            }},

            // ─── Daemon ───────────────────────────────────────────────
            {"daemon", {
                {"shutdown_timeout_s", 90},
                {"pipeline_drain_s",   30},
                {"watchdog_override",  false},
            }},
        }},
    });
}

}  // namespace kairos::config
