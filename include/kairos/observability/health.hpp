/// include/kairos/observability/health.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/observability/health.hpp — self-monitoring checks (Roadmap §15.5)║
// ║                                                                          ║
// ║  Computes a health snapshot from locally available signals:              ║
// ║    • database size vs max_db_size_mb                                     ║
// ║    • stale RUNNING runs (running longer than stale_run_timeout)          ║
// ║    • trigger-bus queue depth vs max_queue_depth                          ║
// ║                                                                          ║
// ║  Shared by `kairos status --health` (CLI) and the HTTP /health           ║
// ║  endpoint (via the daemon), so both surfaces always agree.               ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/persist/query_reader.hpp"

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace kairos::observability {

/// Health check thresholds (Roadmap §15.5 defaults).
struct HealthConfig {
    bool enabled = true;
    int check_interval_s = 60;
    int64_t max_db_size_mb = 1024;
    int max_queue_depth = 100;
    int stale_run_timeout_s = 3600;
};

/// Parse HealthConfig from a confy-style config accessor.
/// Expects keys under kairos.health: enabled, check_interval_seconds,
/// max_db_size_mb, max_queue_depth, stale_run_timeout_seconds.
template <typename ConfigView>
HealthConfig parse_health_config(const ConfigView& global) {
    HealthConfig cfg;
    cfg.enabled = global.template get<bool>(
        "kairos.health.enabled", true);
    cfg.check_interval_s = global.template get<int>(
        "kairos.health.check_interval_seconds", 60);
    cfg.max_db_size_mb = global.template get<int64_t>(
        "kairos.health.max_db_size_mb", 1024);
    cfg.max_queue_depth = global.template get<int>(
        "kairos.health.max_queue_depth", 100);
    cfg.stale_run_timeout_s = global.template get<int>(
        "kairos.health.stale_run_timeout_seconds", 3600);
    return cfg;
}

/// Compute the health snapshot.
///
/// @param cfg     Thresholds.
/// @param db_path Database file (size check; missing file is "warn").
/// @param reader  Run-history reader (stale-run check; may be null).
/// @param queue_depth  Current trigger-bus depth (-1 = unknown).
/// @return JSON: {"status": "ok"|"degraded", "checks": [
///   {name, ok, detail, value?}, …]}.
[[nodiscard]] nlohmann::json compute_health(
    const HealthConfig& cfg,
    const std::filesystem::path& db_path,
    const persist::QueryReader* reader,
    int64_t queue_depth = -1);

}  // namespace kairos::observability
