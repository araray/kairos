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

// ── Helper: a platform-appropriate absolute test path ────────────────────
// On POSIX "/a/b/c" is absolute.  On Windows we need "C:\\a\\b\\c" or
// use the current drive root so that normalize_path doesn't prepend cwd.
#ifdef _WIN32
static fs::path abs_test_path(const char* posix_path) {
    // Prefix with C: so Windows treats it as absolute on the C: drive.
    return fs::path(std::string("C:") + posix_path);
}
#else
static fs::path abs_test_path(const char* posix_path) {
    return fs::path(posix_path);
}
#endif

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
    auto base = abs_test_path("/opt/kairos");
    auto result = normalize_path("subdir/file.txt", base);
    EXPECT_EQ(result, (base / "subdir" / "file.txt").lexically_normal());
}

TEST(PathNormalize, AbsolutePathUnchanged) {
    auto input = abs_test_path("/absolute/path/to/file");
    auto result = normalize_path(input);
    EXPECT_EQ(result, input.lexically_normal());
}

// ═══════════════════════════════════════════════════════════════════════════
// Dot and dotdot collapsing
// ═══════════════════════════════════════════════════════════════════════════

TEST(PathNormalize, DotCollapsed) {
    auto result = normalize_path(abs_test_path("/a/./b/./c"));
    EXPECT_EQ(result, abs_test_path("/a/b/c").lexically_normal());
}

TEST(PathNormalize, DotDotCollapsed) {
    auto result = normalize_path(abs_test_path("/a/b/../c"));
    EXPECT_EQ(result, abs_test_path("/a/c").lexically_normal());
}

TEST(PathNormalize, ComplexDotDot) {
    auto result = normalize_path(abs_test_path("/a/b/c/../../d"));
    EXPECT_EQ(result, abs_test_path("/a/d").lexically_normal());
}

TEST(PathNormalize, TrailingSlashNormalized) {
    auto result = normalize_path(abs_test_path("/a/b/c/"));
    // Should contain the path components regardless of separator style.
    auto s = result.string();
    EXPECT_TRUE(s.find("a") != std::string::npos);
    EXPECT_TRUE(s.find("b") != std::string::npos);
    EXPECT_TRUE(s.find("c") != std::string::npos);
    EXPECT_TRUE(result.is_absolute());
}

// ═══════════════════════════════════════════════════════════════════════════
// UTF-8 round-trip
// ═══════════════════════════════════════════════════════════════════════════

TEST(PathUtf8, RoundTrip) {
    fs::path original = abs_test_path("/tmp/test_dir/日本語");
    auto utf8 = path_to_utf8(original);
    auto back = utf8_to_path(utf8);
    EXPECT_EQ(original, back);
}

TEST(PathUtf8, AsciiPath) {
    fs::path original = abs_test_path("/usr/local/bin/kairos");
    auto utf8 = path_to_utf8(original);
    auto back = utf8_to_path(utf8);
    EXPECT_EQ(original, back);
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
