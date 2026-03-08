/// tests/unit/watch/kel_rules_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for KEL rule evaluation in the watch engine                        ║
// ║                                                                           ║
// ║  Validates that the watch engine correctly evaluates KEL conditions        ║
// ║  against file metrics: size checks, pattern_found, event type filters,    ║
// ║  and boolean logic.                                                       ║
// ║                                                                           ║
// ║  Test pattern (matching EventWatcher baseline parity):                    ║
// ║    1. Pre-populate FakeFilesystem with initial files                      ║
// ║    2. First scan → baseline (no events emitted)                          ║
// ║    3. Mutate filesystem (add/modify/delete)                              ║
// ║    4. Second scan → diff detected → rules evaluated                      ║
// ║                                                                           ║
// ║  Spec reference: §12.11 (rule evaluation via KEL)                        ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/watch/watch_engine.hpp"
#include "kairos/testing/fake_clock.hpp"
#include "kairos/testing/fake_fs_scanner.hpp"

#include <gtest/gtest.h>

namespace kairos::watch {
namespace {

using namespace kairos::engine;
using namespace kairos::testing;
using namespace std::chrono_literals;

// ── Test fixture ────────────────────────────────────────────────────────

class KelRulesTest : public ::testing::Test {
protected:
    WatchGroupDef make_group(
        const std::string& name,
        const std::vector<std::string>& watch_items,
        std::vector<WatchRuleDef> rules = {})
    {
        WatchGroupDef group;
        group.group_id = "wgr-test-" + name;
        group.group_name = name;
        group.watch_items = watch_items;
        group.mode = WatchMode::Sample;
        group.sample_rate = std::chrono::seconds{60};
        group.max_depth = 5;
        group.rules = std::move(rules);
        group.enabled = true;
        return group;
    }

    WatchRuleDef make_rule(
        const std::string& name,
        const std::string& condition,
        std::vector<std::string> event_types = {},
        const std::string& severity = "info")
    {
        WatchRuleDef rule;
        rule.rule_name = name;
        rule.condition = condition;
        rule.event_types = std::move(event_types);
        rule.severity = severity;
        return rule;
    }

    FakeClock clock_;
    FakeFilesystem fs_;
};

// ═══════════════════════════════════════════════════════════════════════
//  KEL CONDITION: always true
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KelRulesTest, ConditionTrue_AlwaysFires) {
    // Baseline: one existing file.
    fs_.add_file("/watched/a.txt", FakeFileEntry{
        .size = 100, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("test", {"/watched"}, {
        make_rule("always_fire", "true"),
    });

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_, .scanner = &scanner}, {group});

    TriggerSink sink = [](TriggerEvent) { return true; };

    // First scan: baseline.
    engine.scan_once(sink);

    // Add a new file.
    clock_.advance(60s);
    fs_.add_file("/watched/new.txt", FakeFileEntry{
        .size = 50, .mtime = clock_.now()});

    // Second scan: diff should detect created file, rule fires.
    auto results = engine.scan_once(sink);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].triggered.empty());
    EXPECT_EQ(results[0].triggered[0].rule_name, "always_fire");
}

// ═══════════════════════════════════════════════════════════════════════
//  KEL CONDITION: always false
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KelRulesTest, ConditionFalse_NeverFires) {
    fs_.add_file("/watched/a.txt", FakeFileEntry{
        .size = 100, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("test", {"/watched"}, {
        make_rule("never_fire", "false"),
    });

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_, .scanner = &scanner}, {group});

    TriggerSink sink = [](TriggerEvent) { return true; };
    engine.scan_once(sink);

    clock_.advance(60s);
    fs_.add_file("/watched/new.txt", FakeFileEntry{
        .size = 50, .mtime = clock_.now()});

    auto results = engine.scan_once(sink);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].diff.empty());  // Diff exists.
    EXPECT_TRUE(results[0].triggered.empty());  // But rule doesn't fire.
}

// ═══════════════════════════════════════════════════════════════════════
//  KEL CONDITION: file_size check
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KelRulesTest, FileSizeCondition_LargeFile) {
    fs_.add_file("/watched/existing.txt", FakeFileEntry{
        .size = 10, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("test", {"/watched"}, {
        make_rule("large_file", "file_size > 1000"),
    });

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_, .scanner = &scanner}, {group});

    TriggerSink sink = [](TriggerEvent) { return true; };
    engine.scan_once(sink);

    // Add a small file (should NOT trigger) and a large file (SHOULD).
    clock_.advance(60s);
    fs_.add_file("/watched/small.txt", FakeFileEntry{
        .size = 2, .mtime = clock_.now()});
    fs_.add_file("/watched/big.txt", FakeFileEntry{
        .size = 2000, .mtime = clock_.now()});

    auto results = engine.scan_once(sink);
    ASSERT_EQ(results.size(), 1u);

    int triggered_count = 0;
    for (const auto& tr : results[0].triggered) {
        if (tr.rule_name == "large_file") {
            ++triggered_count;
            ASSERT_EQ(tr.affected_paths.size(), 1u);
            EXPECT_NE(tr.affected_paths[0].find("big.txt"),
                      std::string::npos);
        }
    }
    EXPECT_EQ(triggered_count, 1);
}

TEST_F(KelRulesTest, FileSizeCondition_SmallFileOnly) {
    fs_.add_file("/watched/existing.txt", FakeFileEntry{
        .size = 10, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("test", {"/watched"}, {
        make_rule("large_file", "file_size > 1000"),
    });

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_, .scanner = &scanner}, {group});

    TriggerSink sink = [](TriggerEvent) { return true; };
    engine.scan_once(sink);

    // Only small files — no triggers.
    clock_.advance(60s);
    fs_.add_file("/watched/a.txt", FakeFileEntry{
        .size = 5, .mtime = clock_.now()});

    auto results = engine.scan_once(sink);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].triggered.empty());
}

// ═══════════════════════════════════════════════════════════════════════
//  KEL CONDITION: pattern_found
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KelRulesTest, PatternFoundCondition) {
    fs_.add_file("/watched/existing.txt", FakeFileEntry{
        .size = 10, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("test", {"/watched"}, {
        make_rule("error_detected", "file_pattern_found == true"),
    });

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_, .scanner = &scanner}, {group});

    TriggerSink sink = [](TriggerEvent) { return true; };
    engine.scan_once(sink);

    // FakeFilesystem doesn't set pattern_found → defaults to false.
    clock_.advance(60s);
    fs_.add_file("/watched/normal.txt", FakeFileEntry{
        .size = 15, .mtime = clock_.now()});

    auto results = engine.scan_once(sink);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].triggered.empty());
}

// ═══════════════════════════════════════════════════════════════════════
//  KEL CONDITION: boolean logic
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KelRulesTest, BooleanAndCondition) {
    fs_.add_file("/watched/existing.txt", FakeFileEntry{
        .size = 10, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("test", {"/watched"}, {
        make_rule("large_and_file",
                  "file_size > 100 and file_type == \"file\""),
    });

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_, .scanner = &scanner}, {group});

    TriggerSink sink = [](TriggerEvent) { return true; };
    engine.scan_once(sink);

    clock_.advance(60s);
    fs_.add_file("/watched/big.txt", FakeFileEntry{
        .size = 500, .mtime = clock_.now()});

    auto results = engine.scan_once(sink);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].triggered.empty());
}

TEST_F(KelRulesTest, BooleanOrCondition) {
    fs_.add_file("/watched/existing.txt", FakeFileEntry{
        .size = 10, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("test", {"/watched"}, {
        make_rule("big_or_small",
                  "file_size > 10000 or file_size < 5"),
    });

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_, .scanner = &scanner}, {group});

    TriggerSink sink = [](TriggerEvent) { return true; };
    engine.scan_once(sink);

    // Tiny file (size 2) — should trigger via "or file_size < 5".
    // Medium file (size 50) — should NOT trigger.
    clock_.advance(60s);
    fs_.add_file("/watched/tiny.txt", FakeFileEntry{
        .size = 2, .mtime = clock_.now()});
    fs_.add_file("/watched/medium.txt", FakeFileEntry{
        .size = 50, .mtime = clock_.now()});

    auto results = engine.scan_once(sink);
    ASSERT_EQ(results.size(), 1u);

    int triggered_count = 0;
    for (const auto& tr : results[0].triggered) {
        if (tr.rule_name == "big_or_small") {
            ++triggered_count;
        }
    }
    EXPECT_EQ(triggered_count, 1);
}

// ═══════════════════════════════════════════════════════════════════════
//  KEL CONDITION: combined with event_types filter
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KelRulesTest, EventTypeFilterWithKel) {
    fs_.add_file("/watched/existing.txt", FakeFileEntry{
        .size = 10, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("test", {"/watched"}, {
        // Only fires on content_changed (not file_created).
        make_rule("big_change", "file_size > 100",
                  {"content_changed"}),
        // Fires on file_created.
        make_rule("any_created", "true", {"file_created"}),
    });

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_, .scanner = &scanner}, {group});

    TriggerSink sink = [](TriggerEvent) { return true; };
    engine.scan_once(sink);

    clock_.advance(60s);
    fs_.add_file("/watched/new.txt", FakeFileEntry{
        .size = 11, .mtime = clock_.now()});

    auto results = engine.scan_once(sink);
    ASSERT_EQ(results.size(), 1u);

    // "any_created" should fire (file_created event, condition=true).
    // "big_change" should NOT fire (event is file_created, not content_changed).
    bool found_created = false;
    bool found_big_change = false;
    for (const auto& tr : results[0].triggered) {
        if (tr.rule_name == "any_created") found_created = true;
        if (tr.rule_name == "big_change") found_big_change = true;
    }
    EXPECT_TRUE(found_created);
    EXPECT_FALSE(found_big_change);
}

// ═══════════════════════════════════════════════════════════════════════
//  KEL CONDITION: invalid expression (error handling)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KelRulesTest, InvalidKelExpression_DoesNotFire) {
    fs_.add_file("/watched/existing.txt", FakeFileEntry{
        .size = 10, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("test", {"/watched"}, {
        make_rule("bad_rule", "this is >>> not valid kel"),
    });

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_, .scanner = &scanner}, {group});

    TriggerSink sink = [](TriggerEvent) { return true; };
    engine.scan_once(sink);

    clock_.advance(60s);
    fs_.add_file("/watched/test.txt", FakeFileEntry{
        .size = 4, .mtime = clock_.now()});

    auto results = engine.scan_once(sink);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].triggered.empty());
}

// ═══════════════════════════════════════════════════════════════════════
//  KEL CONDITION: empty condition (legacy parity — always fires)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KelRulesTest, EmptyCondition_AlwaysFires) {
    fs_.add_file("/watched/existing.txt", FakeFileEntry{
        .size = 10, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("test", {"/watched"}, {
        make_rule("no_condition", ""),
    });

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_, .scanner = &scanner}, {group});

    TriggerSink sink = [](TriggerEvent) { return true; };
    engine.scan_once(sink);

    clock_.advance(60s);
    fs_.add_file("/watched/test.txt", FakeFileEntry{
        .size = 4, .mtime = clock_.now()});

    auto results = engine.scan_once(sink);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].triggered.empty());
}

// ═══════════════════════════════════════════════════════════════════════
//  MULTIPLE RULES: only matching ones fire
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KelRulesTest, MultipleRules_OnlyMatchingFire) {
    fs_.add_file("/watched/existing.txt", FakeFileEntry{
        .size = 10, .mtime = clock_.now()});

    FakeFilesystemScanner scanner(fs_);
    auto group = make_group("test", {"/watched"}, {
        make_rule("always", "true"),
        make_rule("never", "false"),
        make_rule("size_check", "file_size > 50"),
    });

    WatchEngine engine(WatchEngineConfig{}, WatchEngine::Dependencies{
        .clock = &clock_, .scanner = &scanner}, {group});

    TriggerSink sink = [](TriggerEvent) { return true; };
    engine.scan_once(sink);

    clock_.advance(60s);
    fs_.add_file("/watched/test.txt", FakeFileEntry{
        .size = 5, .mtime = clock_.now()});

    auto results = engine.scan_once(sink);
    ASSERT_EQ(results.size(), 1u);

    // "always" fires, "never" doesn't, "size_check" doesn't (5 < 50).
    bool found_always = false, found_never = false, found_size = false;
    for (const auto& tr : results[0].triggered) {
        if (tr.rule_name == "always") found_always = true;
        if (tr.rule_name == "never") found_never = true;
        if (tr.rule_name == "size_check") found_size = true;
    }
    EXPECT_TRUE(found_always);
    EXPECT_FALSE(found_never);
    EXPECT_FALSE(found_size);
}

}  // anonymous namespace
}  // namespace kairos::watch
