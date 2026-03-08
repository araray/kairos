/// tests/unit/watch/kel_rules_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for KEL rule evaluation in the watch engine                        ║
// ║                                                                           ║
// ║  Validates that the watch engine correctly evaluates KEL conditions        ║
// ║  against file metrics: size checks, pattern_found, event type filters,    ║
// ║  and boolean logic.                                                       ║
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

// ── Helpers ─────────────────────────────────────────────────────────────

/// Create a FakeFileEntry with a given size.
FakeFileEntry make_entry(std::uintmax_t size) {
    FakeFileEntry e;
    e.size = size;
    e.mtime = std::chrono::system_clock::now();
    return e;
}

/// A TriggerSink that accepts everything (returns true).
TriggerSink make_null_sink() {
    return [](TriggerEvent) { return true; };
}

class KelRulesTest : public ::testing::Test {
protected:
    void SetUp() override {
        clock_ = std::make_unique<FakeClock>();
        fake_fs_ = std::make_unique<FakeFilesystem>();
        scanner_ = std::make_unique<FakeFilesystemScanner>(*fake_fs_);
    }

    std::unique_ptr<WatchEngine> make_engine(
        std::vector<WatchGroupDef> groups)
    {
        WatchEngineConfig config;
        config.enabled = true;

        WatchEngine::Dependencies deps;
        deps.clock = clock_.get();
        deps.scanner = scanner_.get();
        deps.db_writer = nullptr;

        return std::make_unique<WatchEngine>(
            config, deps, std::move(groups));
    }

    WatchGroupDef make_group(
        const std::string& name,
        const std::vector<std::string>& watch_items,
        std::vector<WatchRuleDef> rules)
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

    std::unique_ptr<FakeClock> clock_;
    std::unique_ptr<FakeFilesystem> fake_fs_;
    std::unique_ptr<FakeFilesystemScanner> scanner_;
};

// ═══════════════════════════════════════════════════════════════════════
//  KEL CONDITION: always true
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KelRulesTest, ConditionTrue_AlwaysFires) {
    auto group = make_group("test", {"/data"}, {
        make_rule("always_fire", "true"),
    });

    auto engine = make_engine({group});

    auto sink = make_null_sink();
    engine->scan_once(sink);

    fake_fs_->add_file("/data/new.txt", make_entry(7));
    auto results = engine->scan_once(sink);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_GE(results[0].triggered.size(), 1u);
    EXPECT_EQ(results[0].triggered[0].rule_name, "always_fire");
}

// ═══════════════════════════════════════════════════════════════════════
//  KEL CONDITION: always false
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KelRulesTest, ConditionFalse_NeverFires) {
    auto group = make_group("test", {"/data"}, {
        make_rule("never_fire", "false"),
    });

    auto engine = make_engine({group});

    auto sink = make_null_sink();
    engine->scan_once(sink);

    fake_fs_->add_file("/data/new.txt", make_entry(7));
    auto results = engine->scan_once(sink);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].triggered.size(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════
//  KEL CONDITION: file_size check
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KelRulesTest, FileSizeCondition_LargeFile) {
    auto group = make_group("test", {"/data"}, {
        make_rule("large_file", "file_size > 1000"),
    });

    auto engine = make_engine({group});

    auto sink = make_null_sink();
    engine->scan_once(sink);

    // Small file — should NOT trigger.
    fake_fs_->add_file("/data/small.txt", make_entry(2));
    // Large file — SHOULD trigger.
    fake_fs_->add_file("/data/big.txt", make_entry(2000));

    auto results = engine->scan_once(sink);

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
    auto group = make_group("test", {"/data"}, {
        make_rule("large_file", "file_size > 1000"),
    });

    auto engine = make_engine({group});

    auto sink = make_null_sink();
    engine->scan_once(sink);

    fake_fs_->add_file("/data/a.txt", make_entry(5));
    fake_fs_->add_file("/data/b.txt", make_entry(5));

    auto results = engine->scan_once(sink);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].triggered.size(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════
//  KEL CONDITION: pattern_found
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KelRulesTest, PatternFoundCondition) {
    auto group = make_group("test", {"/data"}, {
        make_rule("error_detected", "file_pattern_found == true"),
    });

    auto engine = make_engine({group});

    auto sink = make_null_sink();
    engine->scan_once(sink);

    // FakeFilesystem doesn't set pattern_found, so it defaults to false.
    fake_fs_->add_file("/data/normal.txt", make_entry(15));

    auto results = engine->scan_once(sink);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].triggered.size(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════
//  KEL CONDITION: boolean logic
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KelRulesTest, BooleanAndCondition) {
    auto group = make_group("test", {"/data"}, {
        make_rule("large_and_file",
                  "file_size > 100 and file_type == \"file\""),
    });

    auto engine = make_engine({group});

    auto sink = make_null_sink();
    engine->scan_once(sink);

    fake_fs_->add_file("/data/big.txt", make_entry(500));

    auto results = engine->scan_once(sink);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_GE(results[0].triggered.size(), 1u);
}

TEST_F(KelRulesTest, BooleanOrCondition) {
    auto group = make_group("test", {"/data"}, {
        make_rule("big_or_small",
                  "file_size > 10000 or file_size < 5"),
    });

    auto engine = make_engine({group});

    auto sink = make_null_sink();
    engine->scan_once(sink);

    // Tiny file (size 2) — should trigger via "or file_size < 5".
    fake_fs_->add_file("/data/tiny.txt", make_entry(2));
    // Medium file (size 50) — should NOT trigger.
    fake_fs_->add_file("/data/medium.txt", make_entry(50));

    auto results = engine->scan_once(sink);
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
    auto group = make_group("test", {"/data"}, {
        make_rule("big_change", "file_size > 100",
                  {"content_changed"}),
        make_rule("any_created", "true", {"file_created"}),
    });

    auto engine = make_engine({group});

    auto sink = make_null_sink();
    engine->scan_once(sink);

    fake_fs_->add_file("/data/new.txt", make_entry(11));

    auto results = engine->scan_once(sink);
    ASSERT_EQ(results.size(), 1u);

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
    auto group = make_group("test", {"/data"}, {
        make_rule("bad_rule", "this is >>> not valid kel"),
    });

    auto engine = make_engine({group});

    auto sink = make_null_sink();
    engine->scan_once(sink);

    fake_fs_->add_file("/data/test.txt", make_entry(4));

    auto results = engine->scan_once(sink);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].triggered.size(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════
//  KEL CONDITION: empty condition (legacy parity — always fires)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KelRulesTest, EmptyCondition_AlwaysFires) {
    auto group = make_group("test", {"/data"}, {
        make_rule("no_condition", ""),
    });

    auto engine = make_engine({group});

    auto sink = make_null_sink();
    engine->scan_once(sink);

    fake_fs_->add_file("/data/test.txt", make_entry(4));

    auto results = engine->scan_once(sink);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_GE(results[0].triggered.size(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════
//  MULTIPLE RULES: only matching ones fire
// ═══════════════════════════════════════════════════════════════════════

TEST_F(KelRulesTest, MultipleRules_OnlyMatchingFire) {
    auto group = make_group("test", {"/data"}, {
        make_rule("always", "true"),
        make_rule("never", "false"),
        make_rule("size_check", "file_size > 50"),
    });

    auto engine = make_engine({group});

    auto sink = make_null_sink();
    engine->scan_once(sink);

    fake_fs_->add_file("/data/test.txt", make_entry(5));

    auto results = engine->scan_once(sink);
    ASSERT_EQ(results.size(), 1u);

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
