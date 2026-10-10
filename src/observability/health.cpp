/// src/observability/health.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Self-monitoring checks — see include/kairos/observability/health.hpp    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/observability/health.hpp"

#include <spdlog/spdlog.h>

#include <chrono>

namespace kairos::observability {

namespace {

nlohmann::json make_check(const std::string& name, bool ok,
                          const std::string& detail) {
    return {
        {"name", name},
        {"ok", ok},
        {"detail", detail},
    };
}

}  // namespace

nlohmann::json compute_health(
    const HealthConfig& cfg,
    const std::filesystem::path& db_path,
    const persist::QueryReader* reader,
    int64_t queue_depth)
{
    nlohmann::json checks = nlohmann::json::array();
    bool degraded = false;

    // ── Database size ────────────────────────────────────────────
    {
        int64_t size_bytes =
            persist::QueryReader::query_db_size(db_path);
        int64_t size_mb = size_bytes / (1024 * 1024);
        bool ok = size_mb <= cfg.max_db_size_mb;
        auto check = make_check(
            "db_size", ok,
            std::to_string(size_mb) + " MiB / limit " +
                std::to_string(cfg.max_db_size_mb) + " MiB");
        check["value_mb"] = size_mb;
        checks.push_back(check);
        if (!ok) degraded = true;
    }

    // ── Stale RUNNING runs ───────────────────────────────────────
    if (reader) {
        int64_t stale =
            reader->count_stale_runs(cfg.stale_run_timeout_s);
        bool ok = stale == 0;
        auto check = make_check(
            "stale_runs", ok,
            stale == 0
                ? "no runs stuck longer than " +
                      std::to_string(cfg.stale_run_timeout_s) + "s"
                : std::to_string(stale) +
                      " runs RUNNING longer than " +
                      std::to_string(cfg.stale_run_timeout_s) + "s");
        check["value"] = stale;
        checks.push_back(check);
        if (!ok) degraded = true;
    }

    // ── Trigger-bus depth ────────────────────────────────────────
    if (queue_depth >= 0) {
        bool ok = queue_depth <= cfg.max_queue_depth;
        auto check = make_check(
            "queue_depth", ok,
            std::to_string(queue_depth) + " pending / limit " +
                std::to_string(cfg.max_queue_depth));
        check["value"] = queue_depth;
        checks.push_back(check);
        if (!ok) degraded = true;
    }

    return {
        {"status", degraded ? "degraded" : "ok"},
        {"checks", std::move(checks)},
    };
}

}  // namespace kairos::observability
