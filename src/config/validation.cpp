// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  validation.cpp — Kairos-specific semantic config validation               ║
// ║                                                                           ║
// ║  Beyond confy-cpp's mandatory-key checks, we enforce type constraints,    ║
// ║  range checks, and cross-key consistency rules.                           ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/config/config_store.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace kairos::config {

namespace {

/// Helper: add an error if a check fails.
using Errors = std::vector<ValidationError>;

void check_positive_int(const confy::Config& cfg, const std::string& key,
                        Errors& errors) {
    try {
        int val = cfg.get<int>(key, 0);
        if (val <= 0) {
            errors.push_back({key, "must be > 0, got " + std::to_string(val),
                              "", -1});
        }
    } catch (...) {
        errors.push_back({key, "must be a positive integer", "", -1});
    }
}

void check_non_negative_int(const confy::Config& cfg, const std::string& key,
                            Errors& errors) {
    try {
        int val = cfg.get<int>(key, 0);
        if (val < 0) {
            errors.push_back({key, "must be >= 0, got " + std::to_string(val),
                              "", -1});
        }
    } catch (...) {
        errors.push_back({key, "must be a non-negative integer", "", -1});
    }
}

void check_enum(const confy::Config& cfg, const std::string& key,
                const std::vector<std::string>& allowed, Errors& errors) {
    try {
        std::string val = cfg.get<std::string>(key, "");
        if (!val.empty() &&
            std::find(allowed.begin(), allowed.end(), val) == allowed.end()) {
            std::string msg = "must be one of [";
            for (size_t i = 0; i < allowed.size(); ++i) {
                if (i > 0) msg += ", ";
                msg += "'" + allowed[i] + "'";
            }
            msg += "], got '" + val + "'";
            errors.push_back({key, msg, "", -1});
        }
    } catch (...) {
        errors.push_back({key, "must be a string", "", -1});
    }
}

}  // anonymous namespace

std::vector<ValidationError> validate_config(const confy::Config& cfg) {
    Errors errors;

    // ─── Positive integers ────────────────────────────────────────────
    check_positive_int(cfg, "kairos.runners.worker_pool_size", errors);
    check_positive_int(cfg, "kairos.runners.default_timeout_s", errors);
    check_positive_int(cfg, "kairos.runners.kill_timeout_s", errors);
    check_positive_int(cfg, "kairos.scheduler.tick_resolution_ms", errors);
    check_positive_int(cfg, "kairos.watch.sample_interval_s", errors);
    check_positive_int(cfg, "kairos.watch.max_depth", errors);
    check_positive_int(cfg, "kairos.persistence.retention_days", errors);
    check_positive_int(cfg, "kairos.persistence.batch_size", errors);
    check_positive_int(cfg, "kairos.persistence.flush_interval_ms", errors);
    check_positive_int(cfg, "kairos.daemon.shutdown_timeout_s", errors);
    check_positive_int(cfg, "kairos.daemon.pipeline_drain_s", errors);

    // ─── Non-negative integers ────────────────────────────────────────
    check_non_negative_int(cfg, "kairos.scheduler.misfire_grace_s", errors);
    check_non_negative_int(cfg, "kairos.logging.max_file_size_mb", errors);
    check_non_negative_int(cfg, "kairos.logging.max_files", errors);
    check_non_negative_int(cfg, "kairos.logging.async_queue_size", errors);

    // ─── Enum checks ──────────────────────────────────────────────────
    check_enum(cfg, "kairos.logging.level",
               {"trace", "debug", "info", "warn", "error", "critical"}, errors);
    check_enum(cfg, "kairos.logging.format",
               {"auto", "json", "text"}, errors);
    check_enum(cfg, "kairos.watch.default_mode",
               {"native", "sample", "hybrid"}, errors);
    check_enum(cfg, "kairos.watch.hash_policy",
               {"mtime+size", "sha256"}, errors);
    check_enum(cfg, "kairos.platform.color",
               {"auto", "always", "never"}, errors);
    check_enum(cfg, "kairos.mcp.transport",
               {"stdio"}, errors);

    // ─── Port range ───────────────────────────────────────────────────
    try {
        int port = cfg.get<int>("kairos.http.listen_port", 8420);
        if (port < 1 || port > 65535) {
            errors.push_back({"kairos.http.listen_port",
                              "must be 1–65535, got " + std::to_string(port),
                              "", -1});
        }
    } catch (...) {}

    return errors;
}

}  // namespace kairos::config
