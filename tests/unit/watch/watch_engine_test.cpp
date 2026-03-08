/// tests/unit/watch/watch_engine_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Watch engine component tests                                             ║
// ║  Uses FakeFilesystem + FakeClock for deterministic testing.              ║
// ║  Spec reference: §30.4, §12.11, §12.12                                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/watch/watch_engine.hpp"
#include "kairos/testing/fake_clock.hpp"
#include "kairos/testing/fake_filesystem.hpp"
#include "kairos/testing/fake_fs_scanner.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stop_token>
#include <vector>

using namespace kairos::watch;
using namespace kairos::engine;
using namespace kairos::testing;
using namespace std::chrono_literals;

// ── Test fixture ────────────────────────────────────────────────────────

class WatchEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        clock_.set_now(std::chrono::system_clock::time_point{
            std::chrono::hours(24 * 365 * 56)});  // ~2026
        clock_.set_steady(std::chrono::steady_clock::time_point{
            std::chrono::hours(1)});
    }

    /// Build a simple watch group definition.
    WatchGroupDef make_group(
        const std::string& name,
        std::vector<std::string> items,
        std::vector<WatchRuleDef> rules = {},
        std::chrono::seconds sample_rate = 60s)
    {
        WatchGroupDef g;
        g.group_id = "wg-" + name;
        g.group_name = name;
        g.watch_items = std::move(items);
        g.rules = std::move(rules);
        g.sample_rate = sample_rate;
        g.mode = WatchMode::Sample;  // v1 = sample only.
        return g;
    }

    /// Build a catch-all rule that triggers on any event.
    WatchRuleDef make_catch_all_rule(
        const std::string& name,
        const std::string& target = "")
    {
        WatchRuleDef r;
        r.rule_name = name;
        r.condition = "true";
        r.severity = "info";
        if (!target.empty()) {
            r.trigger_target = target;
            r.trigger_is_workflow = true;
        }
        return r;
    }

    /// Build a rule that only fires on specific event types.
    WatchRuleDef make_filtered_rule(
        const std::string& name,
        std::vector<std::string> event_types,
        const std::string& target = "")
    {
        WatchRuleDef r;
        r.rule_name = name;
        r.condition = "true";
        r.event_types = std::move(event_types);
        r.severity = "warning";
        if (!target.empty()) {
            r.trigger_target = target;
            r.trigger_is_workflow = true;
        }
        return r;
    }

    kairos::testing::FakeClock clock_;
    FakeFilesystem fs_;
};

// ── Tests ───────────────────────────────────────────────────────────────

TEST_F(WatchEngineTest, ConstructionWithNoGroups) {
    FakeFilesystemScanner scanner(fs_);
    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_,
        .scanner = &scanner,
    }, {});

    EXPECT_EQ(engine.group_count(), 0u);
}

TEST_F(WatchEngineTest, ConstructionRegistersEnabledGroups) {
    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("logs", {"/var/log"});

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_,
        .scanner = &scanner,
    }, {group});

    EXPECT_EQ(engine.group_count(), 1u);
}

TEST_F(WatchEngineTest, DisabledGroupsExcluded) {
    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("logs", {"/var/log"});
    group.enabled = false;

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_,
        .scanner = &scanner,
    }, {group});

    EXPECT_EQ(engine.group_count(), 0u);
}

TEST_F(WatchEngineTest, FirstScanIsBaselineNoEvents) {
    // EventWatcher parity: first scan produces no events.
    fs_.add_file("/watched/a.txt", FakeFileEntry{
        .size = 100, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("test", {"/watched"},
        {make_catch_all_rule("any_change", "wfl-test")});

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_,
        .scanner = &scanner,
    }, {group});

    std::vector<TriggerEvent> emitted;
    TriggerSink sink = [&](TriggerEvent evt) {
        emitted.push_back(std::move(evt));
        return true;
    };

    auto results = engine.scan_once(sink);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].sample.empty());
    EXPECT_TRUE(results[0].diff.empty());     // First scan = no diff.
    EXPECT_TRUE(results[0].triggered.empty()); // No events.
    EXPECT_TRUE(emitted.empty());              // No TriggerEvents.
}

TEST_F(WatchEngineTest, SecondScanDetectsNewFile) {
    fs_.add_file("/watched/a.txt", FakeFileEntry{
        .size = 100, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("test", {"/watched"},
        {make_catch_all_rule("any_change", "wfl-test")});

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_,
        .scanner = &scanner,
    }, {group});

    std::vector<TriggerEvent> emitted;
    TriggerSink sink = [&](TriggerEvent evt) {
        emitted.push_back(std::move(evt));
        return true;
    };

    // First scan (baseline).
    engine.scan_once(sink);
    EXPECT_TRUE(emitted.empty());

    // Add a new file.
    clock_.advance(60s);
    fs_.add_file("/watched/b.txt", FakeFileEntry{
        .size = 50, .mtime = clock_.now()});

    // Second scan.
    auto results = engine.scan_once(sink);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].diff.empty());
    ASSERT_EQ(results[0].diff.created.size(), 1u);
    EXPECT_EQ(results[0].diff.created[0], "/watched/b.txt");
    EXPECT_FALSE(results[0].triggered.empty());
    EXPECT_EQ(emitted.size(), 1u);  // 1 TriggerEvent.
}

TEST_F(WatchEngineTest, FileDeletedDetected) {
    fs_.add_file("/watched/a.txt", FakeFileEntry{
        .size = 100, .mtime = clock_.now()});
    fs_.add_file("/watched/b.txt", FakeFileEntry{
        .size = 200, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("test", {"/watched"},
        {make_catch_all_rule("any_change")});

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_,
        .scanner = &scanner,
    }, {group});

    TriggerSink sink = [](TriggerEvent) { return true; };

    // Baseline.
    engine.scan_once(sink);

    // Delete a file.
    clock_.advance(60s);
    fs_.remove_file("/watched/b.txt");

    auto results = engine.scan_once(sink);

    ASSERT_EQ(results.size(), 1u);
    ASSERT_EQ(results[0].diff.deleted.size(), 1u);
    EXPECT_EQ(results[0].diff.deleted[0], "/watched/b.txt");
}

TEST_F(WatchEngineTest, FileSizeChangeDetected) {
    fs_.add_file("/watched/a.txt", FakeFileEntry{
        .size = 100, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("test", {"/watched"},
        {make_catch_all_rule("any_change")});

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_,
        .scanner = &scanner,
    }, {group});

    TriggerSink sink = [](TriggerEvent) { return true; };

    // Baseline.
    engine.scan_once(sink);

    // Modify size.
    clock_.advance(60s);
    fs_.modify_file("/watched/a.txt", FakeFileEntry{
        .size = 500, .mtime = clock_.now()});

    auto results = engine.scan_once(sink);

    ASSERT_EQ(results.size(), 1u);
    ASSERT_EQ(results[0].diff.modified.size(), 1u);
    EXPECT_TRUE(results[0].diff.modified.count("/watched/a.txt"));
}

TEST_F(WatchEngineTest, FilteredRuleOnlyMatchesSpecificEvents) {
    fs_.add_file("/watched/a.txt", FakeFileEntry{
        .size = 100, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);

    // Rule that only triggers on file_created.
    auto group = make_group("test", {"/watched"},
        {make_filtered_rule("create_only", {"file_created"})});

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_,
        .scanner = &scanner,
    }, {group});

    TriggerSink sink = [](TriggerEvent) { return true; };

    // Baseline.
    engine.scan_once(sink);

    // Modify file (not a creation).
    clock_.advance(60s);
    fs_.modify_file("/watched/a.txt", FakeFileEntry{
        .size = 500, .mtime = clock_.now()});

    auto results = engine.scan_once(sink);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].diff.empty());
    EXPECT_TRUE(results[0].triggered.empty());  // size_changed != file_created.
}

TEST_F(WatchEngineTest, MultipleGroupsScanIndependently) {
    fs_.add_file("/logs/app.log", FakeFileEntry{
        .size = 1000, .mtime = clock_.now()});
    fs_.add_file("/data/db.dat", FakeFileEntry{
        .size = 2000, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);

    auto group1 = make_group("logs", {"/logs"},
        {make_catch_all_rule("log_change")});
    auto group2 = make_group("data", {"/data"},
        {make_catch_all_rule("data_change")});

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_,
        .scanner = &scanner,
    }, {group1, group2});

    EXPECT_EQ(engine.group_count(), 2u);

    TriggerSink sink = [](TriggerEvent) { return true; };

    // Baseline.
    engine.scan_once(sink);

    // Only modify /logs/app.log.
    clock_.advance(60s);
    fs_.modify_file("/logs/app.log", FakeFileEntry{
        .size = 1500, .mtime = clock_.now()});

    auto results = engine.scan_once(sink);

    ASSERT_EQ(results.size(), 2u);

    // Find the logs result and data result.
    bool found_logs_change = false;
    bool data_has_changes = false;
    for (const auto& r : results) {
        if (!r.diff.modified.empty() &&
            r.diff.modified.count("/logs/app.log")) {
            found_logs_change = true;
        }
        if (!r.diff.modified.empty() &&
            r.diff.modified.count("/data/db.dat")) {
            data_has_changes = true;
        }
    }
    EXPECT_TRUE(found_logs_change);
    EXPECT_FALSE(data_has_changes);
}

TEST_F(WatchEngineTest, TriggerEventContainsCorrectPayload) {
    fs_.add_file("/watched/a.txt", FakeFileEntry{
        .size = 100, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("test", {"/watched"},
        {make_catch_all_rule("any_change", "wfl-deploy")});

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_,
        .scanner = &scanner,
    }, {group});

    std::vector<TriggerEvent> emitted;
    TriggerSink sink = [&](TriggerEvent evt) {
        emitted.push_back(std::move(evt));
        return true;
    };

    // Baseline.
    engine.scan_once(sink);

    // Add file.
    clock_.advance(60s);
    fs_.add_file("/watched/new.txt", FakeFileEntry{
        .size = 42, .mtime = clock_.now()});

    engine.scan_once(sink);

    ASSERT_EQ(emitted.size(), 1u);
    const auto& evt = emitted[0];

    EXPECT_EQ(evt.type, TriggerType::FileDiff);
    EXPECT_EQ(evt.target_id, "wfl-deploy");
    EXPECT_EQ(evt.target_kind, TriggerEvent::TargetKind::Workflow);
    EXPECT_FALSE(evt.correlation_id.empty());

    // Check payload.
    ASSERT_TRUE(std::holds_alternative<FileDiffPayload>(evt.payload));
    const auto& payload = std::get<FileDiffPayload>(evt.payload);
    EXPECT_EQ(payload.watch_group, "test");
    EXPECT_FALSE(payload.affected_paths.empty());
}

TEST_F(WatchEngineTest, ReloadUpdatesGroups) {
    fs_.add_file("/watched/a.txt", FakeFileEntry{
        .size = 100, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("test", {"/watched"});

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_,
        .scanner = &scanner,
    }, {group});

    EXPECT_EQ(engine.group_count(), 1u);

    // Reload with two groups.
    auto g1 = make_group("logs", {"/logs"});
    auto g2 = make_group("data", {"/data"});
    engine.request_reload({g1, g2});

    // Give reload a chance to process (in synchronous mode,
    // we'd call run() which handles it, but scan_once also works
    // since we can trigger the reload check via a scan cycle).
    // For the unit test, directly verify after a short operation.
    // The reload is processed at the top of coordinator_loop,
    // but scan_once doesn't call that. So we verify via group_count
    // after a brief start/stop cycle.

    // Actually, for unit testing reload, we need to go through the
    // coordinator loop. Let's use a different approach: call
    // run() in a thread briefly.
    std::stop_source ss;
    std::thread t([&] {
        // Just let the coordinator spin once.
        engine.run(ss.get_token(), [](TriggerEvent) { return true; });
    });

    // Give it a moment to process the reload.
    std::this_thread::sleep_for(50ms);
    ss.request_stop();
    clock_.wake();
    t.join();

    EXPECT_EQ(engine.group_count(), 2u);
}

TEST_F(WatchEngineTest, RuleWithoutTargetDoesNotEmitTrigger) {
    fs_.add_file("/watched/a.txt", FakeFileEntry{
        .size = 100, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    // Rule without trigger_target — fires but doesn't emit a TriggerEvent.
    auto group = make_group("test", {"/watched"},
        {make_catch_all_rule("log_only")});  // No target.

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_,
        .scanner = &scanner,
    }, {group});

    std::vector<TriggerEvent> emitted;
    TriggerSink sink = [&](TriggerEvent evt) {
        emitted.push_back(std::move(evt));
        return true;
    };

    engine.scan_once(sink);

    clock_.advance(60s);
    fs_.add_file("/watched/b.txt", FakeFileEntry{
        .size = 50, .mtime = clock_.now()});

    auto results = engine.scan_once(sink);

    // Rule fires, but no TriggerEvent emitted (no target).
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].triggered.empty());
    EXPECT_TRUE(emitted.empty());
}

TEST_F(WatchEngineTest, GetStatusReturnsGroupInfo) {
    fs_.add_file("/watched/a.txt", FakeFileEntry{
        .size = 100, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("logs", {"/watched"});

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_,
        .scanner = &scanner,
    }, {group});

    auto statuses = engine.get_status();
    ASSERT_EQ(statuses.size(), 1u);
    EXPECT_EQ(statuses[0].group_name, "logs");
    EXPECT_EQ(statuses[0].mode, "sample");
    EXPECT_EQ(statuses[0].status, "active");
}

TEST_F(WatchEngineTest, ScanResultRecordsDuration) {
    fs_.add_file("/watched/a.txt", FakeFileEntry{
        .size = 100, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("test", {"/watched"});

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_,
        .scanner = &scanner,
    }, {group});

    TriggerSink sink = [](TriggerEvent) { return true; };
    auto results = engine.scan_once(sink);

    ASSERT_EQ(results.size(), 1u);
    // Duration should be non-negative (might be 0ms for fast fake scan).
    EXPECT_GE(results[0].scan_duration.count(), 0);
}
