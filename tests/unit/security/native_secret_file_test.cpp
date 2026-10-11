/// tests/unit/security/native_secret_file_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Native encrypted secrets file tests — Roadmap §12.2.1                    ║
// ║                                                                          ║
// ║  Real crypto round-trips run under KAIROS_VAULT=ON (OpenSSL); the        ║
// ║  stub path is asserted when OpenSSL is not compiled in.                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/security/native_secret_file.hpp"

#include "test_helpers.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace kairos::security {
namespace {

using Map = std::unordered_map<std::string, std::string>;

#ifdef KAIROS_VAULT

TEST(NativeSecretFileTest, RoundTrip) {
    kairos::testing::TempDir dir;
    auto path = dir.path() / "secrets.enc";
    Map secrets{{"deploy_api_key", "tok-abc-123"},
                {"db_password", "hunter2"}};

    ASSERT_NO_THROW(NativeSecretFile::save(path, "master-pw", secrets));
    EXPECT_TRUE(NativeSecretFile::exists(path));

    auto loaded = NativeSecretFile::load(path, "master-pw");
    EXPECT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded["deploy_api_key"], "tok-abc-123");
    EXPECT_EQ(loaded["db_password"], "hunter2");
}

TEST(NativeSecretFileTest, WrongPasswordThrows) {
    kairos::testing::TempDir dir;
    auto path = dir.path() / "secrets.enc";
    NativeSecretFile::save(path, "correct", {{"k", "v"}});
    EXPECT_THROW(NativeSecretFile::load(path, "wrong"),
                 std::runtime_error);
}

TEST(NativeSecretFileTest, TamperDetection) {
    kairos::testing::TempDir dir;
    auto path = dir.path() / "secrets.enc";
    NativeSecretFile::save(path, "pw", {{"k", "some-value"}});

    // Flip a byte in the ciphertext (5th line).
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }
    ASSERT_EQ(lines.size(), 5u);
    auto& ct = lines[4];
    ct[ct.size() - 1] = (ct[ct.size() - 1] == '0') ? '1' : '0';
    {
        std::ofstream out(path, std::ios::trunc);
        for (const auto& l : lines) out << l << "\n";
    }

    EXPECT_THROW(NativeSecretFile::load(path, "pw"),
                 std::runtime_error);
}

TEST(NativeSecretFileTest, OverwriteAndRotate) {
    kairos::testing::TempDir dir;
    auto path = dir.path() / "secrets.enc";
    NativeSecretFile::save(path, "old-pw", {{"a", "1"}});
    // Save under a new password (rotation) with an extra key.
    NativeSecretFile::save(path, "new-pw", {{"a", "1"}, {"b", "2"}});
    auto loaded = NativeSecretFile::load(path, "new-pw");
    EXPECT_EQ(loaded.size(), 2u);
    EXPECT_THROW(NativeSecretFile::load(path, "old-pw"),
                 std::runtime_error);
}

TEST(NativeSecretFileTest, EmptyStore) {
    kairos::testing::TempDir dir;
    auto path = dir.path() / "secrets.enc";
    NativeSecretFile::save(path, "pw", {});
    EXPECT_TRUE(NativeSecretFile::load(path, "pw").empty());
}

TEST(NativeSecretFileTest, MissingFileThrows) {
    EXPECT_THROW(NativeSecretFile::load("/nonexistent/secrets.enc", "pw"),
                 std::runtime_error);
}

TEST(NativeSecretFileTest, NotASecretFileThrows) {
    kairos::testing::TempDir dir;
    auto path = dir.path() / "junk.enc";
    std::ofstream(path) << "garbage\nmore\n";
    EXPECT_THROW(NativeSecretFile::load(path, "pw"),
                 std::runtime_error);
}

#else  // !KAIROS_VAULT — stub behavior

TEST(NativeSecretFileStubTest, StubThrowsWithReason) {
    kairos::testing::TempDir dir;
    auto path = dir.path() / "secrets.enc";
    std::ofstream(path) << "KAIROS_SECRET_STORE v1\n";
    try {
        (void)NativeSecretFile::load(path, "pw");
        FAIL() << "expected throw";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("KAIROS_VAULT"),
                  std::string::npos);
    }
    EXPECT_THROW(NativeSecretFile::save(path, "pw", {{"k", "v"}}),
                 std::runtime_error);
    // exists() still works without OpenSSL.
    EXPECT_TRUE(NativeSecretFile::exists(path));
}

#endif  // KAIROS_VAULT

}  // namespace
}  // namespace kairos::security
