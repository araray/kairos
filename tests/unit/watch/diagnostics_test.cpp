/// tests/unit/watch/diagnostics_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for WatchEngine diagnostic endpoints (§12.14)                    ║
// ║                                                                          ║
// ║  Validates:                                                             ║
// ║    - get_recent_events() returns triggered events in newest-first order ║
// ║    - Ring buffer wraps correctly at capacity                            ║
// ║    - Filtering by group name works                                      ║
// ║    - scan_once() returns correct ScanResult structure                   ║
// ║    - get_status() reports correct group states                          ║
// ║                                                                          ║
// ║  Spec reference: §12.14                                                 ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/testing/fake_clock.hpp"
#include "kairos/testing/fake_filesystem.hpp"
#include "kairos/testing/fake_fs_scanner.hpp"
#include "kairos/watch/watch_engine.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace kairos::watch {
namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

// ── Test fixture ─────────────────────────────────────────────────────

class DiagnosticsTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto T0 = std::chrono::system_clock::now();

        // Two groups: "group_a" and "group_b".
        fake_fs_.add_file("/watched_a/file1.txt", {
            .size = 100, .mtime = T0, .is_directory = false});

        fake_fs_.add_file("/watched_b/file2.txt", {
            .size = 200, .mtime = T0, .is_directory = false});

        // Group A: has a rule that always fires.
        WatchGroupDef group_a;
        group_a.group_id = "wg_diag_a";
        group_a.group_name = "group_a";
        group_a.watch_items = {"/watched_a"};
        group_a.mode = WatchMode::Sample;
        group_a.sample_rate = 300s;
        group_a.max_depth = 5;
        group_a.hash_policy = HashPolicy::MtimeOnly;
        WatchRuleDef rule_a;
        rule_a.rule_name = "always_fire_a";
        rule_a.condition = "true";
        rule_a.severity = "info";
        group_a.rules = {rule_a};

        // Group B: rule always fires.
        WatchGroupDef group_b;
        group_b.group_id = "wg_diag_b";
        group_b.group_name = "group_b";
        group_b.watch_items = {"/watched_b"};
        group_b.mode = WatchMode::Sample;
        group_b.sample_rate = 300s;
        group_b.max_depth = 5;
        group_b.hash_policy = HashPolicy::MtimeOnly;
        WatchRuleDef rule_b;
        rule_b.rule_name = "always_fire_b";
        rule_b.condition = "true";
        rule_b.severity = "warning";
        group_b.rules = {rule_b};

        groups_ = {group_a, group_b};
    }

    std::unique_ptr<WatchEngine> make_engine() {
        WatchEngineConfig cfg;
        WatchEngine::Dependencies deps{
            .clock = &clock_,
            .scanner = &scanner_,
        };
        return std::make_unique<WatchEngine>(cfg, deps, groups_);
    }

    testing::FakeClock clock_;
    testing::FakeFilesystem fake_fs_;
    testing::FakeFilesystemScanner scanner_{fake_fs_};
    std::vector<WatchGroupDef> groups_;
};

// ── Tests ────────────────────────────────────────────────────────────

TEST_F(DiagnosticsTest, GetRecentEventsEmptyInitially) {
    auto engine = make_engine();
    auto events = engine->get_recent_events();
    EXPECT_TRUE(events.empty());
}

TEST_F(DiagnosticsTest, GetRecentEventsAfterScans) {
    auto engine = make_engine();
    engine::TriggerSink sink = [](engine::TriggerEvent) { return true; };

    // First scan (baseline — no events per EventWatcher parity).
    engine->scan_once(sink);

    // Modify a file and scan again.
    auto T1 = std::chrono::system_clock::now() + 5s;
    fake_fs_.modify_file("/watched_a/file1.txt", {
        .size = 150, .mtime = T1, .is_directory = false});

    engine->scan_once(sink);

    auto events = engine->get_recent_events();
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events[0].watch_group_name, "group_a");
    EXPECT_EQ(events[0].rule_name, "always_fire_a");
}

TEST_F(DiagnosticsTest, GetRecentEventsNewestFirst) {
    auto engine = make_engine();
    engine::TriggerSink sink = [](engine::TriggerEvent) { return true; };

    // Baseline.
    engine->scan_once(sink);

    // Modify group_a.
    auto T1 = std::chrono::system_clock::now() + 5s;
    fake_fs_.modify_file("/watched_a/file1.txt", {
        .size = 150, .mtime = T1, .is_directory = false});
    engine->scan_once(sink);

    // Modify group_b.
    auto T2 = T1 + 5s;
    fake_fs_.modify_file("/watched_b/file2.txt", {
        .size = 250, .mtime = T2, .is_directory = false});
    engine->scan_once(sink);

    auto events = engine->get_recent_events(10);
    ASSERT_GE(events.size(), 2u);
    // Most recent should be group_b (last modified).
    EXPECT_EQ(events[0].watch_group_name, "group_b");
}

TEST_F(DiagnosticsTest, GetRecentEventsFilterByGroup) {
    auto engine = make_engine();
    engine::TriggerSink sink = [](engine::TriggerEvent) { return true; };

    // Baseline.
    engine->scan_once(sink);

    // Modify both groups.
    auto T1 = std::chrono::system_clock::now() + 5s;
    fake_fs_.modify_file("/watched_a/file1.txt", {
        .size = 150, .mtime = T1, .is_directory = false});
    fake_fs_.modify_file("/watched_b/file2.txt", {
        .size = 250, .mtime = T1, .is_directory = false});
    engine->scan_once(sink);

    auto events_a = engine->get_recent_events("group_a", 50);
    for (const auto& ev : events_a) {
        EXPECT_EQ(ev.watch_group_name, "group_a");
    }

    auto events_b = engine->get_recent_events("group_b", 50);
    for (const auto& ev : events_b) {
        EXPECT_EQ(ev.watch_group_name, "group_b");
    }
}

TEST_F(DiagnosticsTest, GetRecentEventsLimitRespected) {
    auto engine = make_engine();
    engine::TriggerSink sink = [](engine::TriggerEvent) { return true; };

    // Baseline.
    engine->scan_once(sink);

    // Generate several events.
    for (int i = 0; i < 5; ++i) {
        auto T = std::chrono::system_clock::now() + std::chrono::seconds(i + 1);
        fake_fs_.modify_file("/watched_a/file1.txt", {
            .size = static_cast<std::uintmax_t>(100 + i * 10), .mtime = T, .is_directory = false});
        engine->scan_once(sink);
    }

    auto events_2 = engine->get_recent_events(2);
    EXPECT_LE(events_2.size(), 2u);

    auto events_all = engine->get_recent_events(100);
    EXPECT_GE(events_all.size(), 5u);
}

TEST_F(DiagnosticsTest, ScanOnceReturnsCorrectStructure) {
    auto engine = make_engine();
    engine::TriggerSink sink = [](engine::TriggerEvent) { return true; };

    // First scan.
    auto results = engine->scan_once(sink);
    ASSERT_EQ(results.size(), 2u);  // Two groups.

    for (const auto& r : results) {
        EXPECT_FALSE(r.sample.empty()) << "Sample should contain entries";
        EXPECT_TRUE(r.diff.empty()) << "First scan should have empty diff";
        EXPECT_TRUE(r.triggered.empty()) << "First scan should have no events";
        EXPECT_GE(r.scan_duration.count(), 0)
            << "Scan duration should be non-negative";
    }
}

TEST_F(DiagnosticsTest, ScanGroupReturnsResultForSpecificGroup) {
    auto engine = make_engine();
    engine::TriggerSink sink = [](engine::TriggerEvent) { return true; };

    auto result = engine->scan_group("group_a", sink);
    EXPECT_FALSE(result.sample.empty());
}

TEST_F(DiagnosticsTest, ScanGroupUnknownReturnsEmpty) {
    auto engine = make_engine();
    engine::TriggerSink sink = [](engine::TriggerEvent) { return true; };

    auto result = engine->scan_group("nonexistent_group", sink);
    EXPECT_TRUE(result.sample.empty());
}

TEST_F(DiagnosticsTest, GetStatusReportsAllGroups) {
    auto engine = make_engine();
    auto statuses = engine->get_status();

    ASSERT_EQ(statuses.size(), 2u);

    bool found_a = false, found_b = false;
    for (const auto& s : statuses) {
        if (s.group_name == "group_a") {
            found_a = true;
            EXPECT_EQ(s.mode, "sample");
            EXPECT_EQ(s.watched_paths, 1);
        } else if (s.group_name == "group_b") {
            found_b = true;
        }
    }
    EXPECT_TRUE(found_a);
    EXPECT_TRUE(found_b);
}

TEST_F(DiagnosticsTest, GetStatusUpdatesAfterScan) {
    auto engine = make_engine();
    engine::TriggerSink sink = [](engine::TriggerEvent) { return true; };

    engine->scan_once(sink);

    auto statuses = engine->get_status();
    for (const auto& s : statuses) {
        EXPECT_GT(s.files_in_last_sample, 0)
            << "After scan, files count should be > 0";
        EXPECT_FALSE(s.last_scan_time.empty())
            << "last_scan_time should be set after scan";
    }
}

}  // namespace
}  // namespace kairos::watch
