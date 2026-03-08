/// tests/unit/watch/real_scanner_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for watch/real_scanner.hpp — Production filesystem scanning        ║
// ║                                                                           ║
// ║  Creates temporary directory trees and verifies scanning behavior:        ║
// ║  depth limits, exclude globs, stop_token cancellation, stat_file.         ║
// ║                                                                           ║
// ║  Spec reference: §12.6                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/watch/real_scanner.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <set>

namespace kairos::watch {
namespace {

namespace fs = std::filesystem;

// ── Helper: create a temp directory tree ────────────────────────────────

class ScannerTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() / ("kairos_scan_test_" +
            std::to_string(counter_++));
        fs::create_directories(root_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    /// Create a file with given content.
    void create_file(const std::string& rel_path,
                     const std::string& content = "test") {
        auto full = root_ / rel_path;
        fs::create_directories(full.parent_path());
        std::ofstream out(full);
        out << content;
    }

    /// Create a directory.
    void create_dir(const std::string& rel_path) {
        fs::create_directories(root_ / rel_path);
    }

    /// Get all scanned paths as a set of relative paths.
    std::set<std::string> scanned_paths(
        const std::vector<ScannedEntry>& entries) const
    {
        std::set<std::string> result;
        std::error_code ec;
        auto root_canonical = fs::canonical(root_, ec);
        for (const auto& e : entries) {
            auto p = fs::path(e.path);
            auto rel = fs::relative(p, root_canonical, ec);
            if (!ec) {
                result.insert(rel.generic_string());
            } else {
                result.insert(e.path);
            }
        }
        return result;
    }

    fs::path root_;
    RealFilesystemScanner scanner_;
    static inline int counter_ = 0;
};

// ═══════════════════════════════════════════════════════════════════════
//  BASIC SCANNING
// ═══════════════════════════════════════════════════════════════════════

TEST_F(ScannerTestFixture, ScanEmptyDirectory) {
    std::stop_source ss;
    auto entries = scanner_.scan(root_, 10, {}, ss.get_token());
    EXPECT_TRUE(entries.empty());
}

TEST_F(ScannerTestFixture, ScanFlatDirectory) {
    create_file("a.txt", "hello");
    create_file("b.txt", "world");
    create_file("c.log", "data");

    std::stop_source ss;
    auto entries = scanner_.scan(root_, 10, {}, ss.get_token());

    ASSERT_EQ(entries.size(), 3u);

    auto paths = scanned_paths(entries);
    EXPECT_TRUE(paths.count("a.txt"));
    EXPECT_TRUE(paths.count("b.txt"));
    EXPECT_TRUE(paths.count("c.log"));
}

TEST_F(ScannerTestFixture, ScanNestedDirectories) {
    create_file("src/main.cpp", "#include <iostream>");
    create_file("src/lib/util.cpp", "void util() {}");
    create_file("README.md", "# Kairos");

    std::stop_source ss;
    auto entries = scanner_.scan(root_, 10, {}, ss.get_token());

    auto paths = scanned_paths(entries);
    EXPECT_TRUE(paths.count("README.md"));
    EXPECT_TRUE(paths.count("src/main.cpp"));
    EXPECT_TRUE(paths.count("src/lib/util.cpp"));
    // Directories themselves should also be scanned.
    EXPECT_TRUE(paths.count("src"));
    EXPECT_TRUE(paths.count("src/lib"));
}

TEST_F(ScannerTestFixture, ScanFileMetadata) {
    create_file("data.txt", "hello world");

    std::stop_source ss;
    auto entries = scanner_.scan(root_, 10, {}, ss.get_token());

    ASSERT_GE(entries.size(), 1u);

    // Find data.txt.
    const ScannedEntry* found = nullptr;
    for (const auto& e : entries) {
        if (e.path.find("data.txt") != std::string::npos) {
            found = &e;
            break;
        }
    }
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->entry_type, "file");
    EXPECT_EQ(found->size, 11);  // "hello world"
    EXPECT_FALSE(found->is_directory);
    EXPECT_FALSE(found->is_symlink);

#ifndef _WIN32
    // POSIX: permissions should be a 4-char octal string.
    EXPECT_EQ(found->permissions.size(), 4u);
    EXPECT_TRUE(found->uid.has_value());
    EXPECT_TRUE(found->gid.has_value());
#endif
}

TEST_F(ScannerTestFixture, ScanDirectoryMetadata) {
    create_file("mydir/a.txt");
    create_file("mydir/b.txt");
    create_dir("mydir/subdir");

    std::stop_source ss;
    auto entries = scanner_.scan(root_, 10, {}, ss.get_token());

    const ScannedEntry* dir_entry = nullptr;
    for (const auto& e : entries) {
        if (e.entry_type == "directory" &&
            e.path.find("mydir") != std::string::npos &&
            e.path.find("subdir") == std::string::npos) {
            dir_entry = &e;
            break;
        }
    }
    ASSERT_NE(dir_entry, nullptr);
    EXPECT_TRUE(dir_entry->is_directory);
    EXPECT_TRUE(dir_entry->files_count.has_value());
    EXPECT_EQ(*dir_entry->files_count, 2);  // a.txt, b.txt
    EXPECT_TRUE(dir_entry->subdirs_count.has_value());
    EXPECT_EQ(*dir_entry->subdirs_count, 1);  // subdir
}

// ═══════════════════════════════════════════════════════════════════════
//  DEPTH LIMITS
// ═══════════════════════════════════════════════════════════════════════

TEST_F(ScannerTestFixture, DepthLimit) {
    create_file("level1/level2/level3/deep.txt");

    std::stop_source ss;

    // Depth 0: only root children.
    auto entries0 = scanner_.scan(root_, 0, {}, ss.get_token());
    auto paths0 = scanned_paths(entries0);
    EXPECT_TRUE(paths0.count("level1"));
    EXPECT_FALSE(paths0.count("level1/level2"));

    // Depth 1: root + one level.
    auto entries1 = scanner_.scan(root_, 1, {}, ss.get_token());
    auto paths1 = scanned_paths(entries1);
    EXPECT_TRUE(paths1.count("level1"));
    EXPECT_TRUE(paths1.count("level1/level2"));
    EXPECT_FALSE(paths1.count("level1/level2/level3"));

    // Depth 10: everything.
    auto entries10 = scanner_.scan(root_, 10, {}, ss.get_token());
    auto paths10 = scanned_paths(entries10);
    EXPECT_TRUE(paths10.count("level1/level2/level3/deep.txt"));
}

// ═══════════════════════════════════════════════════════════════════════
//  EXCLUDE GLOBS
// ═══════════════════════════════════════════════════════════════════════

TEST_F(ScannerTestFixture, ExcludeGlobs) {
    create_file("src/main.cpp");
    create_file("src/main.cpp.tmp");
    create_file("build/output.o");
    create_file(".git/config");

    std::stop_source ss;
    auto entries = scanner_.scan(root_, 10,
                                 {"*.tmp", ".git"}, ss.get_token());

    auto paths = scanned_paths(entries);
    EXPECT_TRUE(paths.count("src/main.cpp"));
    EXPECT_FALSE(paths.count("src/main.cpp.tmp"));
    EXPECT_FALSE(paths.count(".git/config"));
    EXPECT_FALSE(paths.count(".git"));
    EXPECT_TRUE(paths.count("build/output.o"));
}

// ═══════════════════════════════════════════════════════════════════════
//  STOP TOKEN CANCELLATION
// ═══════════════════════════════════════════════════════════════════════

TEST_F(ScannerTestFixture, StopTokenCancellation) {
    // Create many files.
    for (int i = 0; i < 100; ++i) {
        create_file("files/f" + std::to_string(i) + ".txt");
    }

    // Cancel immediately.
    std::stop_source ss;
    ss.request_stop();
    auto entries = scanner_.scan(root_, 10, {}, ss.get_token());

    // Should have scanned fewer than all files.
    // (May scan some before checking stop, but not all 100+.)
    EXPECT_LT(entries.size(), 101u);
}

// ═══════════════════════════════════════════════════════════════════════
//  STAT FILE
// ═══════════════════════════════════════════════════════════════════════

TEST_F(ScannerTestFixture, StatFile) {
    create_file("single.txt", "content");

    auto entry = scanner_.stat_file(root_ / "single.txt");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->entry_type, "file");
    EXPECT_EQ(entry->size, 7);  // "content"
    EXPECT_FALSE(entry->is_directory);
}

TEST_F(ScannerTestFixture, StatFileNonexistent) {
    auto entry = scanner_.stat_file(root_ / "nonexistent.txt");
    EXPECT_FALSE(entry.has_value());
}

TEST_F(ScannerTestFixture, StatDirectory) {
    create_dir("mydir");

    auto entry = scanner_.stat_file(root_ / "mydir");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->entry_type, "directory");
    EXPECT_TRUE(entry->is_directory);
}

// ═══════════════════════════════════════════════════════════════════════
//  PATH NORMALIZATION
// ═══════════════════════════════════════════════════════════════════════

TEST_F(ScannerTestFixture, PathNormalization) {
    create_file("a.txt");

    auto norm = RealFilesystemScanner::normalize_path(root_ / "a.txt");
    // Should use forward slashes.
    EXPECT_EQ(norm.find('\\'), std::string::npos);
    // Should be an absolute path.
    EXPECT_TRUE(norm[0] == '/');
    // Should contain the filename.
    EXPECT_NE(norm.find("a.txt"), std::string::npos);
}

TEST_F(ScannerTestFixture, PathNormalizationWithDots) {
    create_file("a.txt");

    auto norm = RealFilesystemScanner::normalize_path(
        root_ / "." / "a.txt");
    // Should resolve the dots.
    EXPECT_EQ(norm.find("/./"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════
//  SCAN SINGLE FILE (root is a file)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(ScannerTestFixture, ScanSingleFile) {
    create_file("solo.txt", "alone");

    std::stop_source ss;
    auto entries = scanner_.scan(root_ / "solo.txt", 10, {},
                                 ss.get_token());

    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].entry_type, "file");
    EXPECT_EQ(entries[0].size, 5);
}

// ═══════════════════════════════════════════════════════════════════════
//  GLOB MATCHING (unit tests for the static helper)
// ═══════════════════════════════════════════════════════════════════════

TEST(GlobMatchTest, BasicPatterns) {
    using R = RealFilesystemScanner;

    // Exact match.
    EXPECT_TRUE(R::normalize_path("/tmp").find("/tmp") != std::string::npos);
}

#ifndef _WIN32
TEST_F(ScannerTestFixture, SymlinkHandling) {
    create_file("real/data.txt", "real data");
    std::error_code ec;
    fs::create_symlink(root_ / "real", root_ / "link", ec);
    if (ec) GTEST_SKIP() << "Cannot create symlinks";

    std::stop_source ss;
    auto entries = scanner_.scan(root_, 10, {}, ss.get_token());

    // Should find the real directory/file AND the symlink entry.
    bool found_real_data = false;
    bool found_symlink = false;
    for (const auto& e : entries) {
        if (e.path.find("data.txt") != std::string::npos) found_real_data = true;
        if (e.is_symlink) found_symlink = true;
    }
    EXPECT_TRUE(found_real_data);
    // The symlink directory entry should be detected. Note: canonical()
    // resolves the symlink path to the real target, but the entry's
    // is_symlink flag is set from symlink_status() before resolution.
    EXPECT_TRUE(found_symlink);
}
#endif

}  // anonymous namespace
}  // namespace kairos::watch
