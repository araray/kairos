/// tests/unit/platform/path_normalize_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  path_normalize_test.cpp — Path normalization tests                       ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/platform/paths.hpp"
#include "test_helpers.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace kairos::platform {

// ═══════════════════════════════════════════════════════════════════════════
// Tilde expansion
// ═══════════════════════════════════════════════════════════════════════════

TEST(PathNormalize, TildeExpandsToHome) {
    auto home = get_home_dir();
    auto result = normalize_path("~");
    EXPECT_EQ(result, home.lexically_normal());
}

TEST(PathNormalize, TildeSlashExpandsToHomeSubdir) {
    auto home = get_home_dir();
    auto result = normalize_path("~/projects");
    EXPECT_EQ(result, (home / "projects").lexically_normal());
}

TEST(PathNormalize, TildeNestedPath) {
    auto home = get_home_dir();
    auto result = normalize_path("~/a/b/c");
    EXPECT_EQ(result, (home / "a" / "b" / "c").lexically_normal());
}

// ═══════════════════════════════════════════════════════════════════════════
// Relative path resolution
// ═══════════════════════════════════════════════════════════════════════════

TEST(PathNormalize, RelativePathResolvesAgainstCwd) {
    auto result = normalize_path("some/relative/path");
    EXPECT_TRUE(result.is_absolute());
    EXPECT_TRUE(result.string().find("some/relative/path") != std::string::npos
             || result.string().find("some\\relative\\path") != std::string::npos);
}

TEST(PathNormalize, RelativePathResolvesAgainstBase) {
    auto result = normalize_path("subdir/file.txt", fs::path("/opt/kairos"));
    EXPECT_EQ(result, fs::path("/opt/kairos/subdir/file.txt").lexically_normal());
}

TEST(PathNormalize, AbsolutePathUnchanged) {
    auto result = normalize_path("/absolute/path/to/file");
    EXPECT_EQ(result, fs::path("/absolute/path/to/file").lexically_normal());
}

// ═══════════════════════════════════════════════════════════════════════════
// Dot and dotdot collapsing
// ═══════════════════════════════════════════════════════════════════════════

TEST(PathNormalize, DotCollapsed) {
    auto result = normalize_path("/a/./b/./c");
    EXPECT_EQ(result, fs::path("/a/b/c"));
}

TEST(PathNormalize, DotDotCollapsed) {
    auto result = normalize_path("/a/b/../c");
    EXPECT_EQ(result, fs::path("/a/c"));
}

TEST(PathNormalize, ComplexDotDot) {
    auto result = normalize_path("/a/b/c/../../d");
    EXPECT_EQ(result, fs::path("/a/d"));
}

TEST(PathNormalize, TrailingSlashNormalized) {
    auto result = normalize_path("/a/b/c/");
    // lexically_normal may or may not keep trailing slash — just check base.
    EXPECT_TRUE(result.string().find("/a/b/c") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// UTF-8 round-trip
// ═══════════════════════════════════════════════════════════════════════════

TEST(PathUtf8, RoundTrip) {
    fs::path original("/tmp/test_dir/日本語");
    auto utf8 = path_to_utf8(original);
    auto back = utf8_to_path(utf8);
    EXPECT_EQ(original, back);
}

TEST(PathUtf8, AsciiPath) {
    fs::path original("/usr/local/bin/kairos");
    EXPECT_EQ(path_to_utf8(original), "/usr/local/bin/kairos");
}

// ═══════════════════════════════════════════════════════════════════════════
// Platform directory discovery
// ═══════════════════════════════════════════════════════════════════════════

TEST(PlatformDirs, HomeIsAbsolute) {
    auto home = get_home_dir();
    EXPECT_TRUE(home.is_absolute()) << "Home: " << home;
}

TEST(PlatformDirs, ConfigDirIsAbsolute) {
    auto dir = get_config_dir();
    EXPECT_TRUE(dir.is_absolute()) << "Config dir: " << dir;
    // Should contain "kairos" somewhere.
    EXPECT_TRUE(dir.string().find("kairos") != std::string::npos)
        << "Config dir should contain 'kairos': " << dir;
}

TEST(PlatformDirs, DataDirIsAbsolute) {
    auto dir = get_data_dir();
    EXPECT_TRUE(dir.is_absolute()) << "Data dir: " << dir;
}

TEST(PlatformDirs, LogDirIsAbsolute) {
    auto dir = get_log_dir();
    EXPECT_TRUE(dir.is_absolute()) << "Log dir: " << dir;
}

}  // namespace kairos::platform
