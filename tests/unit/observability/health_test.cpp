/// tests/unit/observability/health_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Health check tests — Roadmap §15.5                                       ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/observability/health.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/persist/query_reader.hpp"

#include "test_helpers.hpp"

#include <gtest/gtest.h>

namespace kairos::observability {
namespace {

TEST(HealthTest, OkWhenEverythingSmall) {
    kairos::testing::TempDir dir;
    auto db = kairos::persist::open_database(":memory:");
    kairos::persist::apply_migrations(*db, kairos::persist::get_migrations());
    kairos::persist::QueryReader reader(*db);

    HealthConfig cfg;  // defaults: 1024 MiB, depth 100, stale 3600s
    auto j = compute_health(cfg, dir.path() / "nope.db", &reader, 0);
    // Missing db file → size 0 → ok.
    EXPECT_EQ(j["status"], "ok");
    EXPECT_FALSE(j["checks"].empty());
}

TEST(HealthTest, StaleRunsDegrade) {
    auto db = kairos::persist::open_database(":memory:");
    kairos::persist::apply_migrations(*db, kairos::persist::get_migrations());
    kairos::persist::QueryReader reader(*db);

    // Seed a RUNNING run started 2h ago (older than the 3600s limit).
    {
        SQLite::Statement q(*db,
            "INSERT INTO runs (run_id, target_type, target_id, "
            "target_name, trigger_type, trigger_id, correlation_id, "
            "status, start_ts) VALUES "
            "('run-old','workflow','w','w','manual','t','c','RUNNING',"
            "'2020-01-01T00:00:00Z')");
        q.exec();
    }

    HealthConfig cfg;
    auto j = compute_health(cfg, std::filesystem::path("nope.db"),
                            &reader, -1);
    EXPECT_EQ(j["status"], "degraded");
    bool found = false;
    for (const auto& c : j["checks"]) {
        if (c["name"] == "stale_runs") {
            EXPECT_FALSE(c["ok"].get<bool>());
            EXPECT_EQ(c["value"], 1);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(HealthTest, QueueDepthDegrade) {
    auto db = kairos::persist::open_database(":memory:");
    kairos::persist::apply_migrations(*db, kairos::persist::get_migrations());
    kairos::persist::QueryReader reader(*db);

    HealthConfig cfg;
    cfg.max_queue_depth = 10;
    auto j = compute_health(cfg, std::filesystem::path("nope.db"),
                            &reader, 42);
    EXPECT_EQ(j["status"], "degraded");
    bool found = false;
    for (const auto& c : j["checks"]) {
        if (c["name"] == "queue_depth") {
            EXPECT_FALSE(c["ok"].get<bool>());
            EXPECT_EQ(c["value"], 42);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(HealthTest, QueueDepthUnknownIsSkipped) {
    auto db = kairos::persist::open_database(":memory:");
    kairos::persist::apply_migrations(*db, kairos::persist::get_migrations());
    kairos::persist::QueryReader reader(*db);

    HealthConfig cfg;
    auto j = compute_health(cfg, std::filesystem::path("nope.db"),
                            &reader, -1);
    for (const auto& c : j["checks"]) {
        EXPECT_NE(c["name"], "queue_depth");
    }
}

}  // namespace
}  // namespace kairos::observability
