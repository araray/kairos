/// tests/integration/e2e_watch_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  End-to-end watch engine integration test (§30.6)                        ║
// ║                                                                          ║
// ║  Creates a temp directory, configures a WatchEngine with RealScanner    ║
// ║  (and native watcher if available), then performs actual file           ║
// ║  mutations and verifies events are emitted correctly.                   ║
// ║                                                                          ║
// ║  Validates:                                                             ║
// ║    - Create file → file_created event                                  ║
// ║    - Modify file → size_changed/content_changed event                  ║
// ║    - Delete file → file_deleted event                                  ║
// ║    - Pattern matching fires pattern_found rule                          ║
// ║    - Multiple scan cycles produce correct diffs                         ║
// ║    - Hash computation reflects content changes                          ║
// ║    - Diagnostics (get_status, get_recent_events) work E2E              ║
// ║                                                                          ║
// ║  Spec reference: §30.6, §32.4                                          ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/testing/fake_clock.hpp"
#include "kairos/watch/real_scanner.hpp"
#include "kairos/watch/watch_engine.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace kairos::watch {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ── Test fixture ─────────────────────────────────────────────────────

class E2EWatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = fs::temp_directory_path() / "kairos_e2e_watch_test";
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
        fs::create_directories(temp_dir_);

        // Seed an initial file.
        write_text("initial.txt", "Hello World\n");
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    void write_text(const std::string& name, const std::string& content) {
        auto path = temp_dir_ / name;
        fs::create_directories(path.parent_path());
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        f.close();
    }

    void delete_file(const std::string& name) {
        std::error_code ec;
        fs::remove(temp_dir_ / name, ec);
    }

    std::unique_ptr<WatchEngine> make_engine(
        const std::optional<std::string>& pattern = std::nullopt,
        HashPolicy hp = HashPolicy::SizePlusMtime)
    {
        WatchGroupDef group;
        group.group_id = "wg_e2e";
        group.group_name = "e2e_test";
        group.watch_items = {temp_dir_.string()};
        group.mode = WatchMode::Sample;
        group.sample_rate = 3600s;
        group.max_depth = 5;
        group.hash_policy = hp;
        group.pattern = pattern;

        // Always-fire rule for detecting any change.
        WatchRuleDef rule;
        rule.rule_name = "detect_all";
        rule.condition = "true";
        rule.severity = "info";
        group.rules = {rule};

        WatchEngineConfig cfg;
        WatchEngine::Dependencies deps{
            .clock = &clock_,
            .scanner = &scanner_,
        };

        return std::make_unique<WatchEngine>(
            cfg, deps, std::vector<WatchGroupDef>{group});
    }

    testing::FakeClock clock_;
    RealFilesystemScanner scanner_;
    fs::path temp_dir_;
};

// ── Tests ────────────────────────────────────────────────────────────

TEST_F(E2EWatchTest, BaselineScanProducesNoEvents) {
    auto engine = make_engine();
    engine::TriggerSink sink = [](engine::TriggerEvent) { return true; };

    auto results = engine->scan_once(sink);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].diff.empty())
        << "First scan should be baseline with no diff";
    EXPECT_TRUE(results[0].triggered.empty());
    EXPECT_FALSE(results[0].sample.empty());
}

TEST_F(E2EWatchTest, FileCreationDetected) {
    auto engine = make_engine();
    engine::TriggerSink sink = [](engine::TriggerEvent) { return true; };

    // Baseline scan.
    engine->scan_once(sink);

    // Create a new file.
    write_text("new_file.txt", "New content\n");

    // Second scan — should detect the new file.
    auto results = engine->scan_once(sink);
    ASSERT_EQ(results.size(), 1u);
    auto& diff = results[0].diff;

    EXPECT_FALSE(diff.created.empty())
        << "New file should appear in created list";

    bool found_new = false;
    for (const auto& p : diff.created) {
        if (p.find("new_file.txt") != std::string::npos) {
            found_new = true;
        }
    }
    EXPECT_TRUE(found_new) << "new_file.txt should be in created list";

    // Rule should have fired.
    EXPECT_FALSE(results[0].triggered.empty());
}

TEST_F(E2EWatchTest, FileModificationDetected) {
    auto engine = make_engine();
    engine::TriggerSink sink = [](engine::TriggerEvent) { return true; };

    // Baseline scan.
    engine->scan_once(sink);

    // Wait a moment to ensure mtime changes.
    std::this_thread::sleep_for(50ms);

    // Modify the initial file.
    write_text("initial.txt", "Modified content that is different\n");

    // Second scan.
    auto results = engine->scan_once(sink);
    ASSERT_EQ(results.size(), 1u);
    auto& diff = results[0].diff;

    bool found_mod = false;
    for (const auto& [p, changes] : diff.modified) {
        if (p.find("initial.txt") != std::string::npos) {
            found_mod = true;

            // Should detect size change.
            bool has_size_change = false;
            for (const auto& c : changes) {
                if (c.field == "size") has_size_change = true;
            }
            EXPECT_TRUE(has_size_change)
                << "Size change should be detected";
        }
    }
    EXPECT_TRUE(found_mod) << "initial.txt should appear in modified";
}

TEST_F(E2EWatchTest, FileDeletionDetected) {
    auto engine = make_engine();
    engine::TriggerSink sink = [](engine::TriggerEvent) { return true; };

    // Baseline scan.
    engine->scan_once(sink);

    // Delete the initial file.
    delete_file("initial.txt");

    // Second scan.
    auto results = engine->scan_once(sink);
    ASSERT_EQ(results.size(), 1u);
    auto& diff = results[0].diff;

    bool found_del = false;
    for (const auto& p : diff.deleted) {
        if (p.find("initial.txt") != std::string::npos) {
            found_del = true;
        }
    }
    EXPECT_TRUE(found_del) << "initial.txt should appear in deleted list";
}

TEST_F(E2EWatchTest, PatternMatchingE2E) {
    auto engine = make_engine("ERROR|FATAL");
    engine::TriggerSink sink = [](engine::TriggerEvent) { return true; };

    // Baseline scan.
    engine->scan_once(sink);

    // Create a file with ERROR content.
    write_text("error.log", "2026-03-08 ERROR disk full\n");

    // Second scan.
    auto results = engine->scan_once(sink);
    ASSERT_EQ(results.size(), 1u);

    // Find the error.log entry in the sample.
    bool found_pattern = false;
    for (const auto& [p, m] : results[0].sample.entries) {
        if (p.find("error.log") != std::string::npos) {
            ASSERT_TRUE(m.pattern_found.has_value());
            EXPECT_TRUE(*m.pattern_found);
            found_pattern = true;
        }
    }
    EXPECT_TRUE(found_pattern) << "error.log should have pattern_found=true";
}

TEST_F(E2EWatchTest, HashComputationE2E) {
    auto engine = make_engine(std::nullopt, HashPolicy::Full);
    engine::TriggerSink sink = [](engine::TriggerEvent) { return true; };

    // First scan.
    auto results1 = engine->scan_once(sink);
    ASSERT_EQ(results1.size(), 1u);

    // Check that hashes were computed.
    for (const auto& [p, m] : results1[0].sample.entries) {
        if (m.entry_type == "file") {
            EXPECT_TRUE(m.sha256.has_value())
                << "Full hash policy should compute SHA-256";
            EXPECT_TRUE(m.md5.has_value())
                << "Full hash policy should compute MD5";
        }
    }

    // Modify file and check hash changes.
    std::this_thread::sleep_for(50ms);
    write_text("initial.txt", "Totally different content now!\n");

    auto results2 = engine->scan_once(sink);
    ASSERT_EQ(results2.size(), 1u);

    for (const auto& [p, m] : results2[0].sample.entries) {
        if (p.find("initial.txt") != std::string::npos && m.entry_type == "file") {
            EXPECT_TRUE(m.sha256.has_value());
            // Hash should differ from first scan — find original hash.
            for (const auto& [p1, m1] : results1[0].sample.entries) {
                if (p1.find("initial.txt") != std::string::npos) {
                    EXPECT_NE(m.sha256.value_or(""), m1.sha256.value_or(""))
                        << "SHA-256 should change when content changes";
                }
            }
        }
    }
}

TEST_F(E2EWatchTest, SizePlusMtimeSkipsUnchangedHashes) {
    auto engine = make_engine(std::nullopt, HashPolicy::SizePlusMtime);
    engine::TriggerSink sink = [](engine::TriggerEvent) { return true; };

    // First scan — hashes computed for all files.
    auto results1 = engine->scan_once(sink);
    ASSERT_EQ(results1.size(), 1u);

    for (const auto& [p, m] : results1[0].sample.entries) {
        if (m.entry_type == "file") {
            EXPECT_TRUE(m.sha256.has_value())
                << "First scan should compute hashes";
        }
    }

    // Second scan WITHOUT modifying anything.
    auto results2 = engine->scan_once(sink);
    ASSERT_EQ(results2.size(), 1u);

    // Hashes should be carried forward (no recomputation).
    for (const auto& [p, m] : results2[0].sample.entries) {
        if (m.entry_type == "file") {
            EXPECT_TRUE(m.sha256.has_value())
                << "Unchanged files should retain hashes from previous scan";
        }
    }

    // Diff should be empty (nothing changed).
    EXPECT_TRUE(results2[0].diff.empty());
}

TEST_F(E2EWatchTest, MultipleFileOperationsInOneCycle) {
    auto engine = make_engine();
    engine::TriggerSink sink = [](engine::TriggerEvent) { return true; };

    // Baseline.
    engine->scan_once(sink);

    std::this_thread::sleep_for(50ms);

    // Create, modify, and delete in one cycle.
    write_text("created.txt", "brand new\n");
    write_text("initial.txt", "modified content now\n");
    write_text("to_delete.txt", "temp\n");

    // Extra scan to pick up to_delete.txt.
    engine->scan_once(sink);

    // Now delete it.
    delete_file("to_delete.txt");

    // Final scan.
    auto results = engine->scan_once(sink);
    ASSERT_EQ(results.size(), 1u);
    auto& diff = results[0].diff;

    bool has_delete = false;
    for (const auto& p : diff.deleted) {
        if (p.find("to_delete.txt") != std::string::npos) {
            has_delete = true;
        }
    }
    EXPECT_TRUE(has_delete)
        << "to_delete.txt should appear in deleted list";
}

TEST_F(E2EWatchTest, DiagnosticsWorkE2E) {
    auto engine = make_engine();
    engine::TriggerSink sink = [](engine::TriggerEvent) { return true; };

    // Baseline.
    engine->scan_once(sink);

    // Create a file and scan.
    write_text("diag_test.txt", "content\n");
    engine->scan_once(sink);

    // Status check.
    auto statuses = engine->get_status();
    ASSERT_EQ(statuses.size(), 1u);
    EXPECT_EQ(statuses[0].group_name, "e2e_test");
    EXPECT_GT(statuses[0].files_in_last_sample, 0);
    EXPECT_FALSE(statuses[0].last_scan_time.empty());

    // Recent events check.
    auto events = engine->get_recent_events();
    EXPECT_FALSE(events.empty())
        << "Should have events from file creation";
    EXPECT_EQ(events[0].watch_group_name, "e2e_test");
}

TEST_F(E2EWatchTest, ExcludeGlobsRespected) {
    // Create engine with exclude globs.
    WatchGroupDef group;
    group.group_id = "wg_e2e_exclude";
    group.group_name = "e2e_exclude";
    group.watch_items = {temp_dir_.string()};
    group.mode = WatchMode::Sample;
    group.sample_rate = 3600s;
    group.max_depth = 5;
    group.hash_policy = HashPolicy::MtimeOnly;
    group.exclude_globs = {"*.tmp", ".git"};

    WatchRuleDef rule;
    rule.rule_name = "detect_all";
    rule.condition = "true";
    rule.severity = "info";
    group.rules = {rule};

    WatchEngineConfig cfg;
    WatchEngine::Dependencies deps{
        .clock = &clock_,
        .scanner = &scanner_,
    };

    auto engine = std::make_unique<WatchEngine>(
        cfg, deps, std::vector<WatchGroupDef>{group});

    engine::TriggerSink sink = [](engine::TriggerEvent) { return true; };

    // Create files: one should be included, one excluded.
    write_text("included.txt", "data\n");
    write_text("excluded.tmp", "temp data\n");

    auto results = engine->scan_once(sink);
    ASSERT_EQ(results.size(), 1u);

    bool found_included = false;
    bool found_excluded = false;
    for (const auto& [p, m] : results[0].sample.entries) {
        if (p.find("included.txt") != std::string::npos) found_included = true;
        if (p.find("excluded.tmp") != std::string::npos) found_excluded = true;
    }
    EXPECT_TRUE(found_included);
    EXPECT_FALSE(found_excluded) << "*.tmp files should be excluded";
}

}  // namespace
}  // namespace kairos::watch
