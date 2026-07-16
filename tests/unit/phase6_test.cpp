/// tests/unit/phase6_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Phase 6 — Critical Fixes & Quick Wins tests                              ║
// ║                                                                           ║
// ║  Tests for:                                                               ║
// ║    §1.1  Run ID prefix matching (QueryReader::resolve_run_id_prefix)      ║
// ║    §1.2  Events affected files summary (CLI helper)                       ║
// ║    §1.3  Table --no-header / --full-id modes                              ║
// ║    §13   platform::get_env thread-safe environment access                 ║
// ║    §15.8 Cron expression preview (CronTrigger::next_fire_after)           ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include <gtest/gtest.h>

#include "kairos/cli/table.hpp"
#include "kairos/engine/trigger_types.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/persist/query_reader.hpp"
#include "kairos/platform/environment.hpp"

#include <chrono>
#include <sstream>

namespace {

using namespace kairos;

// ═══════════════════════════════════════════════════════════════════════════
// §1.1: Run ID prefix matching
// ═══════════════════════════════════════════════════════════════════════════

class PrefixMatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = std::make_unique<SQLite::Database>(
            ":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        persist::apply_migrations(*db_, persist::get_migrations());

        // Insert some test runs.
        db_->exec(
            "INSERT INTO runs (run_id, target_type, target_id, target_name, "
            "trigger_type, trigger_id, correlation_id, status, exit_code, start_ts) VALUES "
            "('run-a1b2c3d4-5678-9abc-def0-111111111111', 'workflow', 'wfl-1', "
            " 'test-wf', 'manual', 'trg-1', 'corr-1', 'SUCCESS', 0, '2026-03-10T10:00:00'),"
            "('run-a1b2c3d4-5678-9abc-def0-222222222222', 'workflow', 'wfl-1', "
            " 'test-wf', 'schedule', 'trg-2', 'corr-2', 'FAILURE', 1, '2026-03-10T11:00:00'),"
            "('run-deadbeef-0000-0000-0000-333333333333', 'job', 'job-1', "
            " 'test-job', 'manual', 'trg-3', 'corr-3', 'SUCCESS', 0, '2026-03-10T12:00:00')");
    }

    std::unique_ptr<SQLite::Database> db_;
};

TEST_F(PrefixMatchTest, EmptyPrefixReturnsEmpty) {
    persist::QueryReader reader(*db_);
    auto result = reader.resolve_run_id_prefix("");
    EXPECT_EQ(result.status, persist::QueryReader::PrefixResult::kEmpty);
}

TEST_F(PrefixMatchTest, FullIdExactMatch) {
    persist::QueryReader reader(*db_);
    auto result = reader.resolve_run_id_prefix(
        "run-a1b2c3d4-5678-9abc-def0-111111111111");
    EXPECT_EQ(result.status, persist::QueryReader::PrefixResult::kExact);
    EXPECT_EQ(result.resolved_id,
              "run-a1b2c3d4-5678-9abc-def0-111111111111");
}

TEST_F(PrefixMatchTest, FullIdNotFound) {
    persist::QueryReader reader(*db_);
    auto result = reader.resolve_run_id_prefix(
        "run-00000000-0000-0000-0000-000000000000");
    EXPECT_EQ(result.status, persist::QueryReader::PrefixResult::kNotFound);
}

TEST_F(PrefixMatchTest, UniquePrefixResolved) {
    persist::QueryReader reader(*db_);
    // "run-dead" matches only the deadbeef run.
    auto result = reader.resolve_run_id_prefix("run-dead");
    EXPECT_EQ(result.status, persist::QueryReader::PrefixResult::kUnique);
    EXPECT_EQ(result.resolved_id,
              "run-deadbeef-0000-0000-0000-333333333333");
}

TEST_F(PrefixMatchTest, AmbiguousPrefixReported) {
    persist::QueryReader reader(*db_);
    // "run-a1b2" matches both a1b2c3d4 runs.
    auto result = reader.resolve_run_id_prefix("run-a1b2");
    EXPECT_EQ(result.status, persist::QueryReader::PrefixResult::kAmbiguous);
    EXPECT_EQ(result.candidates.size(), 2u);
}

TEST_F(PrefixMatchTest, ShortPrefixNotFound) {
    persist::QueryReader reader(*db_);
    auto result = reader.resolve_run_id_prefix("run-zzz");
    EXPECT_EQ(result.status, persist::QueryReader::PrefixResult::kNotFound);
}

TEST_F(PrefixMatchTest, TruncatedIdResolvesUnambiguously) {
    persist::QueryReader reader(*db_);
    // The truncated form "run-deadbee" is unambiguous.
    auto result = reader.resolve_run_id_prefix("run-deadbee");
    EXPECT_EQ(result.status, persist::QueryReader::PrefixResult::kUnique);
    EXPECT_EQ(result.resolved_id,
              "run-deadbeef-0000-0000-0000-333333333333");
}

// ═══════════════════════════════════════════════════════════════════════════
// §1.3: Table --no-header mode
// ═══════════════════════════════════════════════════════════════════════════

TEST(TableTest, NoHeaderMode) {
    cli::Table table({"NAME", "VALUE"});
    table.set_show_header(false);
    table.add_row({"alpha", "100"});
    table.add_row({"beta", "200"});

    std::ostringstream oss;
    table.render(oss, false);

    std::string output = oss.str();
    // Should NOT contain header or separator.
    EXPECT_EQ(output.find("NAME"), std::string::npos);
    EXPECT_EQ(output.find("VALUE"), std::string::npos);
    // Should contain data rows.
    EXPECT_NE(output.find("alpha"), std::string::npos);
    EXPECT_NE(output.find("beta"), std::string::npos);
}

TEST(TableTest, WithHeaderMode) {
    cli::Table table({"NAME", "VALUE"});
    // Default: headers shown.
    table.add_row({"alpha", "100"});

    std::ostringstream oss;
    table.render(oss, false);

    std::string output = oss.str();
    EXPECT_NE(output.find("NAME"), std::string::npos);
    EXPECT_NE(output.find("alpha"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// §13: platform::get_env
// ═══════════════════════════════════════════════════════════════════════════

TEST(EnvironmentTest, GetExistingVar) {
    // PATH should exist on all platforms.
    auto result = platform::get_env("PATH");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->empty());
}

TEST(EnvironmentTest, GetNonExistentVar) {
    auto result = platform::get_env("KAIROS_TEST_NONEXISTENT_12345");
    EXPECT_FALSE(result.has_value());
}

TEST(EnvironmentTest, SetAndGetVar) {
    const char* name = "KAIROS_TEST_PHASE6_ENV";
    ASSERT_TRUE(platform::set_env(name, "hello_phase6"));

    auto result = platform::get_env(name);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "hello_phase6");

    // Cleanup.
    platform::unset_env(name);
    auto after = platform::get_env(name);
    EXPECT_FALSE(after.has_value());
}

TEST(EnvironmentTest, NullNameReturnsNullopt) {
    auto result = platform::get_env(nullptr);
    EXPECT_FALSE(result.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// §15.8: Cron expression preview
// ═══════════════════════════════════════════════════════════════════════════

TEST(CronPreviewTest, StandardCronProducesMonotonicFireTimes) {
    // "*/5 * * * *" = every 5 minutes.
    engine::CronTrigger cron;
    cron.expression = "*/5 * * * *";
    cron.cron_tz_delta_s = 0;

    auto now = std::chrono::system_clock::now();
    auto cursor = now;

    std::vector<std::chrono::system_clock::time_point> fire_times;
    for (int i = 0; i < 5; ++i) {
        auto next = cron.next_fire_after(cursor);
        // Must be strictly after cursor.
        EXPECT_GT(next, cursor) << "Fire time " << i << " not monotonic";
        fire_times.push_back(next);
        cursor = next;
    }

    // Check intervals are ~5 minutes apart.
    for (size_t i = 1; i < fire_times.size(); ++i) {
        auto delta = std::chrono::duration_cast<std::chrono::seconds>(
            fire_times[i] - fire_times[i - 1]);
        // Should be exactly 300s (5 min), allow 1s tolerance for DST edge.
        EXPECT_GE(delta.count(), 299);
        EXPECT_LE(delta.count(), 301);
    }
}

TEST(CronPreviewTest, HourlyCronProducesCorrectGap) {
    engine::CronTrigger cron;
    cron.expression = "0 * * * *";
    cron.cron_tz_delta_s = 0;

    auto now = std::chrono::system_clock::now();
    auto first = cron.next_fire_after(now);
    auto second = cron.next_fire_after(first);

    auto gap = std::chrono::duration_cast<std::chrono::minutes>(
        second - first);
    EXPECT_EQ(gap.count(), 60);
}

TEST(CronPreviewTest, InvalidExpressionThrows) {
    engine::CronTrigger cron;
    cron.expression = "not a cron expression";
    cron.cron_tz_delta_s = 0;

    auto now = std::chrono::system_clock::now();
    EXPECT_THROW(
        { [[maybe_unused]] auto _ = cron.next_fire_after(now); },
        std::exception);
}

}  // namespace
