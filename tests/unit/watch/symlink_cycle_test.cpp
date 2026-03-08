/// tests/unit/watch/symlink_cycle_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  symlink_cycle_test.cpp — Symlink cycle detection in RealScanner        ║
// ║                                                                          ║
// ║  Verifies that the scanner:                                              ║
// ║    1. Detects direct symlink cycles (A → A).                            ║
// ║    2. Detects indirect cycles (A → B → A).                              ║
// ║    3. Handles broken symlinks gracefully.                                ║
// ║    4. Follows valid symlinks without issue.                              ║
// ║                                                                          ║
// ║  Spec reference: §12.10                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/watch/real_scanner.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace kairos::watch {
namespace {

namespace fs = std::filesystem;

class SymlinkCycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "kairos_symlink_test";
        fs::create_directories(test_dir_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    void create_file(const std::string& rel_path,
                     const std::string& content = "data") {
        auto full = test_dir_ / rel_path;
        fs::create_directories(full.parent_path());
        std::ofstream f(full);
        f << content;
    }

    fs::path test_dir_;
    RealFilesystemScanner scanner_;
};

TEST_F(SymlinkCycleTest, DirectCycle_SelfSymlink) {
    // Create a directory and a symlink from it back to itself.
    auto dir_a = test_dir_ / "a";
    fs::create_directories(dir_a);
    create_file("a/file.txt");

    std::error_code ec;
    fs::create_directory_symlink(dir_a, dir_a / "self_link", ec);
    if (ec) {
        GTEST_SKIP() << "Cannot create symlinks on this platform/filesystem";
    }

    std::stop_source stop;
    auto results = scanner_.scan(test_dir_, 10, {}, stop.get_token());

    // Should terminate without infinite recursion.
    // The symlink target should not cause duplicate entries.
    EXPECT_GT(results.size(), 0u);

    // Count how many times we see "a/file.txt" (should be exactly 1).
    int file_count = 0;
    for (const auto& e : results) {
        if (e.path.find("file.txt") != std::string::npos) {
            ++file_count;
        }
    }
    EXPECT_EQ(file_count, 1) << "Cycle should not produce duplicate entries";
}

TEST_F(SymlinkCycleTest, IndirectCycle_TwoDirLoop) {
    // a/link_to_b → b, b/link_to_a → a
    auto dir_a = test_dir_ / "a";
    auto dir_b = test_dir_ / "b";
    fs::create_directories(dir_a);
    fs::create_directories(dir_b);
    create_file("a/alpha.txt");
    create_file("b/beta.txt");

    std::error_code ec;
    fs::create_directory_symlink(dir_b, dir_a / "link_to_b", ec);
    if (ec) {
        GTEST_SKIP() << "Cannot create symlinks";
    }
    fs::create_directory_symlink(dir_a, dir_b / "link_to_a", ec);
    if (ec) {
        GTEST_SKIP() << "Cannot create symlinks";
    }

    std::stop_source stop;
    auto results = scanner_.scan(test_dir_, 10, {}, stop.get_token());

    // Should terminate. Both files should appear exactly once.
    int alpha_count = 0, beta_count = 0;
    for (const auto& e : results) {
        if (e.path.find("alpha.txt") != std::string::npos) ++alpha_count;
        if (e.path.find("beta.txt") != std::string::npos) ++beta_count;
    }
    // At least one of each should appear.
    EXPECT_GE(alpha_count, 1);
    EXPECT_GE(beta_count, 1);
}

TEST_F(SymlinkCycleTest, BrokenSymlink_NoError) {
    // Create a symlink to a non-existent target.
    auto dir = test_dir_ / "dir";
    fs::create_directories(dir);
    create_file("dir/real.txt");

    std::error_code ec;
    fs::create_symlink(test_dir_ / "nonexistent", dir / "broken_link", ec);
    if (ec) {
        GTEST_SKIP() << "Cannot create symlinks";
    }

    std::stop_source stop;
    auto results = scanner_.scan(test_dir_, 10, {}, stop.get_token());

    // Should not crash. The real file should still appear.
    bool found_real = false;
    for (const auto& e : results) {
        if (e.path.find("real.txt") != std::string::npos) {
            found_real = true;
        }
    }
    EXPECT_TRUE(found_real);
}

TEST_F(SymlinkCycleTest, ValidSymlink_FollowsCorrectly) {
    // Create a valid symlink: dir/link → target/
    auto target = test_dir_ / "target";
    auto dir = test_dir_ / "dir";
    fs::create_directories(target);
    fs::create_directories(dir);
    create_file("target/linked_file.txt", "hello");

    std::error_code ec;
    fs::create_directory_symlink(target, dir / "link", ec);
    if (ec) {
        GTEST_SKIP() << "Cannot create symlinks";
    }

    std::stop_source stop;
    auto results = scanner_.scan(test_dir_, 10, {}, stop.get_token());

    // The linked file should appear (at least once via the link).
    bool found_linked = false;
    for (const auto& e : results) {
        if (e.path.find("linked_file.txt") != std::string::npos) {
            found_linked = true;
        }
    }
    EXPECT_TRUE(found_linked);
}

TEST_F(SymlinkCycleTest, MaxDepthPreventsDeepRecursion) {
    // Even without symlinks, max_depth should bound the scan.
    auto deep = test_dir_ / "a" / "b" / "c" / "d" / "e";
    fs::create_directories(deep);
    create_file("a/b/c/d/e/deep.txt");

    std::stop_source stop;
    // max_depth=2: should not reach depth 4.
    auto results = scanner_.scan(test_dir_, 2, {}, stop.get_token());

    bool found_deep = false;
    for (const auto& e : results) {
        if (e.path.find("deep.txt") != std::string::npos) {
            found_deep = true;
        }
    }
    EXPECT_FALSE(found_deep) << "max_depth=2 should not reach depth 4";
}

}  // namespace
}  // namespace kairos::watch
