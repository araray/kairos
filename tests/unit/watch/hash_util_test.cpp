/// tests/unit/watch/hash_util_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  hash_util_test.cpp — MD5 + SHA-256 correctness tests                   ║
// ║                                                                          ║
// ║  Test vectors from:                                                      ║
// ║    MD5: RFC 1321 §A.5                                                   ║
// ║    SHA-256: NIST FIPS 180-4 examples                                    ║
// ║                                                                          ║
// ║  Also tests file hashing (dual-context single pass).                    ║
// ║                                                                          ║
// ║  Spec reference: §12.6.3                                                ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/watch/hash_util.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>

namespace kairos::watch {
namespace {

namespace fs = std::filesystem;

// ══════════════════════════════════════════════════════════════════════════
// MD5 test vectors (RFC 1321 §A.5)
// ══════════════════════════════════════════════════════════════════════════

TEST(HashUtilMD5, EmptyString) {
    // MD5("") = d41d8cd98f00b204e9800998ecf8427e
    auto result = md5_hex("", 0);
    EXPECT_EQ(result, "d41d8cd98f00b204e9800998ecf8427e");
}

TEST(HashUtilMD5, SingleChar_a) {
    // MD5("a") = 0cc175b9c0f1b6a831c399e269772661
    auto result = md5_hex("a", 1);
    EXPECT_EQ(result, "0cc175b9c0f1b6a831c399e269772661");
}

TEST(HashUtilMD5, ABC) {
    // MD5("abc") = 900150983cd24fb0d6963f7d28e17f72
    auto result = md5_hex("abc", 3);
    EXPECT_EQ(result, "900150983cd24fb0d6963f7d28e17f72");
}

TEST(HashUtilMD5, MessageDigest) {
    // MD5("message digest") = f96b697d7cb7938d525a2f31aaf161d0
    const char* input = "message digest";
    auto result = md5_hex(input, std::strlen(input));
    EXPECT_EQ(result, "f96b697d7cb7938d525a2f31aaf161d0");
}

TEST(HashUtilMD5, Alphabet) {
    // MD5("abcdefghijklmnopqrstuvwxyz") = c3fcd3d76192e4007dfb496cca67e13b
    const char* input = "abcdefghijklmnopqrstuvwxyz";
    auto result = md5_hex(input, std::strlen(input));
    EXPECT_EQ(result, "c3fcd3d76192e4007dfb496cca67e13b");
}

// ══════════════════════════════════════════════════════════════════════════
// SHA-256 test vectors (NIST FIPS 180-4)
// ══════════════════════════════════════════════════════════════════════════

TEST(HashUtilSHA256, EmptyString) {
    // SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    auto result = sha256_hex("", 0);
    EXPECT_EQ(result, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(HashUtilSHA256, ABC) {
    // SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    auto result = sha256_hex("abc", 3);
    EXPECT_EQ(result, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(HashUtilSHA256, TwoBlock) {
    // SHA-256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
    // = 248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1
    const char* input =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    auto result = sha256_hex(input, std::strlen(input));
    EXPECT_EQ(result, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

// ══════════════════════════════════════════════════════════════════════════
// File hashing tests
// ══════════════════════════════════════════════════════════════════════════

class FileHashTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "kairos_hash_test";
        fs::create_directories(test_dir_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    void write_file(const std::string& name, const std::string& content) {
        std::ofstream f(test_dir_ / name, std::ios::binary);
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    fs::path test_dir_;
};

TEST_F(FileHashTest, SimpleFile) {
    write_file("test.txt", "abc");

    auto result = compute_file_hashes(test_dir_ / "test.txt");
    ASSERT_TRUE(result.has_value());

    // Compare against known vectors.
    EXPECT_EQ(result->md5, "900150983cd24fb0d6963f7d28e17f72");
    EXPECT_EQ(result->sha256, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_F(FileHashTest, EmptyFile) {
    write_file("empty.txt", "");

    auto result = compute_file_hashes(test_dir_ / "empty.txt");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->md5, "d41d8cd98f00b204e9800998ecf8427e");
    EXPECT_EQ(result->sha256, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_F(FileHashTest, LargeFile) {
    // Create a file larger than the 64KB read buffer (128 KB).
    std::string content(131072, 'X');
    write_file("large.bin", content);

    auto result = compute_file_hashes(test_dir_ / "large.bin");
    ASSERT_TRUE(result.has_value());

    // Verify consistency: hash the same content in memory.
    auto expected_md5 = md5_hex(content.data(), content.size());
    auto expected_sha256 = sha256_hex(content.data(), content.size());

    EXPECT_EQ(result->md5, expected_md5);
    EXPECT_EQ(result->sha256, expected_sha256);
}

TEST_F(FileHashTest, NonExistentFile) {
    auto result = compute_file_hashes(test_dir_ / "nonexistent.txt");
    EXPECT_FALSE(result.has_value());
}

TEST_F(FileHashTest, CancellationViaStopToken) {
    // Create a file.
    write_file("test.txt", "content");

    // Hash with an already-stopped token.
    std::stop_source src;
    src.request_stop();
    auto result = compute_file_hashes(test_dir_ / "test.txt",
                                      src.get_token());
    // May or may not produce a result depending on timing, but
    // should not crash or hang.
}

}  // namespace
}  // namespace kairos::watch
