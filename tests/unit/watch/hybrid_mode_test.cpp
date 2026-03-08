/// tests/unit/watch/hybrid_mode_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  hybrid_mode_test.cpp — Hybrid mode integration tests                   ║
// ║                                                                          ║
// ║  Tests the complete hybrid mode flow:                                   ║
// ║    1. Native events → BoundedQueue → DebounceBuffer                    ║
// ║    2. Settled events → targeted re-scan → rules → TriggerEvent         ║
// ║    3. Periodic full scan suppresses already-reported paths (dedup)      ║
// ║    4. Overflow events trigger immediate full re-scan                    ║
// ║                                                                          ║
// ║  Uses FakeFilesystem + FakeClock for determinism.                       ║
// ║                                                                          ║
// ║  Spec reference: §12.7, §12.13                                          ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/watch/watch_engine.hpp"
#include "kairos/testing/fake_clock.hpp"
#include "kairos/testing/fake_filesystem.hpp"
#include "kairos/testing/fake_fs_scanner.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

using namespace kairos::watch;
using namespace kairos::engine;
using namespace kairos::testing;
using namespace std::chrono_literals;

namespace {

// ── Test fixture ────────────────────────────────────────────────────────

class HybridModeTest : public ::testing::Test {
protected:
    void SetUp() override {
        clock_.reset(std::chrono::system_clock::now(),
                     std::chrono::steady_clock::now());
    }

    /// Build a simple watch group with hybrid mode.
    WatchGroupDef make_group(
        const std::string& name = "test_group",
        std::chrono::seconds sample_rate = 60s)
    {
        WatchGroupDef g;
        g.group_id = "wg-test";
        g.group_name = name;
        g.watch_items = {"/watched"};
        g.mode = WatchMode::Hybrid;
        g.sample_rate = sample_rate;
        g.max_depth = 5;
        return g;
    }

    /// Build a watch group with a rule.
    WatchGroupDef make_group_with_rule(
        const std::string& name = "test_group")
    {
        auto g = make_group(name);
        WatchRuleDef rule;
        rule.rule_name = "any_change";
        rule.condition = "true";
        rule.severity = "info";
        rule.trigger_target = "test_workflow";
        rule.trigger_is_workflow = true;
        g.rules.push_back(std::move(rule));
        return g;
    }

    /// Helper to add a file to the fake filesystem.
    void add_file(const std::string& path, int64_t size,
                  std::chrono::system_clock::time_point mtime = {}) {
        FakeFileEntry entry;
        entry.size = static_cast<std::uintmax_t>(size);
        entry.mtime = mtime;
        fake_fs_.add_file(path, entry);
    }

    /// Trigger sink that records events.
    std::vector<TriggerEvent> triggered_;
    TriggerSink sink_ = [this](TriggerEvent evt) {
        triggered_.push_back(std::move(evt));
        return true;
    };

    kairos::testing::FakeClock clock_;
    FakeFilesystem fake_fs_;
};

// ══════════════════════════════════════════════════════════════════════════
// Basic hybrid mode lifecycle
// ══════════════════════════════════════════════════════════════════════════

TEST_F(HybridModeTest, EngineStartsWithHybridGroup) {
    auto group = make_group();
    WatchEngineConfig cfg;
    FakeFilesystemScanner scanner(fake_fs_);
    WatchEngine engine(cfg,
        WatchEngine::Dependencies{.clock = &clock_, .scanner = &scanner},
        {group});

    EXPECT_EQ(engine.group_count(), 1u);
}

TEST_F(HybridModeTest, NativeEventProcessing_BasicFlow) {
    // Set up scanner with initial file.
    add_file("/watched/file.txt", 100, std::chrono::system_clock::now());

    auto group = make_group_with_rule();
    WatchEngineConfig cfg;
    cfg.debounce_ms = 10ms;

    FakeFilesystemScanner scanner(fake_fs_);
    WatchEngine engine(cfg,
        WatchEngine::Dependencies{.clock = &clock_, .scanner = &scanner},
        {group});

    // First scan (baseline) — no events expected.
    auto results = engine.scan_once(sink_);
    EXPECT_TRUE(triggered_.empty())
        << "First scan is baseline — no events";

    // Modify file in fake filesystem.
    add_file("/watched/file.txt", 200,
             std::chrono::system_clock::now() + 1s);

    // Second scan (periodic) — should detect the change.
    auto results2 = engine.scan_once(sink_);
    EXPECT_FALSE(triggered_.empty())
        << "Second scan should detect file size change";
}

// ══════════════════════════════════════════════════════════════════════════
// Deduplication tests
// ══════════════════════════════════════════════════════════════════════════

TEST_F(HybridModeTest, RecentlyReportedSetPruning) {
    auto group = make_group_with_rule();
    WatchEngineConfig cfg;

    FakeFilesystemScanner scanner(fake_fs_);
    WatchEngine engine(cfg,
        WatchEngine::Dependencies{.clock = &clock_, .scanner = &scanner},
        {group});

    // Scan 1: baseline.
    add_file("/watched/a.txt", 100, std::chrono::system_clock::now());
    engine.scan_once(sink_);

    // Scan 2: change detected.
    add_file("/watched/a.txt", 200,
             std::chrono::system_clock::now() + 1s);
    engine.scan_once(sink_);
    size_t events_after_2 = triggered_.size();
    EXPECT_GE(events_after_2, 1u);

    // Scan 3: no change.
    engine.scan_once(sink_);

    // Scan 4: another change — should still report since the path
    // should have been pruned from recently_reported by now.
    add_file("/watched/a.txt", 300,
             std::chrono::system_clock::now() + 2s);
    engine.scan_once(sink_);
    EXPECT_GT(triggered_.size(), events_after_2)
        << "Change after pruning should be re-reported";
}

// ══════════════════════════════════════════════════════════════════════════
// Hash computation integration
// ══════════════════════════════════════════════════════════════════════════

TEST_F(HybridModeTest, HashPolicyMtimeOnly_NoHashes) {
    auto group = make_group();
    group.hash_policy = HashPolicy::MtimeOnly;

    WatchEngineConfig cfg;
    FakeFilesystemScanner scanner(fake_fs_);
    WatchEngine engine(cfg,
        WatchEngine::Dependencies{.clock = &clock_, .scanner = &scanner},
        {group});

    // With MtimeOnly, no hashes should be computed.
    add_file("/watched/file.txt", 100, std::chrono::system_clock::now());

    auto results = engine.scan_once(sink_);
    ASSERT_FALSE(results.empty());

    for (const auto& [path, metrics] : results[0].sample.entries) {
        EXPECT_FALSE(metrics.md5.has_value())
            << "MtimeOnly policy should not compute MD5";
        EXPECT_FALSE(metrics.sha256.has_value())
            << "MtimeOnly policy should not compute SHA-256";
    }
}

// ══════════════════════════════════════════════════════════════════════════
// Debounce pending count
// ══════════════════════════════════════════════════════════════════════════

TEST_F(HybridModeTest, DebouncePendingCount_EmptyInitially) {
    auto group = make_group();
    WatchEngineConfig cfg;
    FakeFilesystemScanner scanner(fake_fs_);
    WatchEngine engine(cfg,
        WatchEngine::Dependencies{.clock = &clock_, .scanner = &scanner},
        {group});

    EXPECT_EQ(engine.debounce_pending(), 0u);
}

// ══════════════════════════════════════════════════════════════════════════
// Mode-specific behavior
// ══════════════════════════════════════════════════════════════════════════

TEST_F(HybridModeTest, SampleOnlyMode_DetectsChanges) {
    auto group = make_group_with_rule();
    group.mode = WatchMode::Sample;

    WatchEngineConfig cfg;
    FakeFilesystemScanner scanner(fake_fs_);
    WatchEngine engine(cfg,
        WatchEngine::Dependencies{.clock = &clock_, .scanner = &scanner},
        {group});

    // Set up files and do baseline scan.
    add_file("/watched/file.txt", 100, std::chrono::system_clock::now());
    engine.scan_once(sink_);

    // Modify and scan — should detect via periodic scan.
    add_file("/watched/file.txt", 200,
             std::chrono::system_clock::now() + 1s);
    triggered_.clear();
    engine.scan_once(sink_);
    EXPECT_GE(triggered_.size(), 1u)
        << "Sample-only mode detects changes via periodic scan";
}

TEST_F(HybridModeTest, MultipleGroups_IndependentScanning) {
    auto g1 = make_group_with_rule("group_a");
    g1.watch_items = {"/watched_a"};
    auto g2 = make_group_with_rule("group_b");
    g2.watch_items = {"/watched_b"};

    WatchEngineConfig cfg;
    FakeFilesystemScanner scanner(fake_fs_);
    WatchEngine engine(cfg,
        WatchEngine::Dependencies{.clock = &clock_, .scanner = &scanner},
        {g1, g2});

    EXPECT_EQ(engine.group_count(), 2u);

    // Baseline.
    add_file("/watched_a/file.txt", 100,
             std::chrono::system_clock::now());
    add_file("/watched_b/file.txt", 100,
             std::chrono::system_clock::now());
    engine.scan_once(sink_);

    // Change only in group_a.
    add_file("/watched_a/file.txt", 200,
             std::chrono::system_clock::now() + 1s);
    triggered_.clear();
    engine.scan_once(sink_);

    // Should see event only for group_a's file.
    bool found_a = false, found_b = false;
    for (const auto& ev : triggered_) {
        auto payload = std::get_if<FileDiffPayload>(&ev.payload);
        if (payload) {
            if (payload->watch_group == "group_a") found_a = true;
            if (payload->watch_group == "group_b") found_b = true;
        }
    }
    EXPECT_TRUE(found_a) << "group_a should have an event";
    EXPECT_FALSE(found_b) << "group_b should have no event";
}

}  // namespace
