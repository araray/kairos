/// tests/unit/core/id_generator_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  id_generator_test.cpp — Content-hash ID and UUID generation tests        ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/core/id_generator.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>

namespace kairos::core {

// ═══════════════════════════════════════════════════════════════════════════
// SHA-256 correctness (NIST test vectors)
// ═══════════════════════════════════════════════════════════════════════════

TEST(Sha256, EmptyString) {
    // SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    auto digest = sha256("");
    EXPECT_EQ(to_hex(digest.data(), 32),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256, Abc) {
    // SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    auto digest = sha256("abc");
    EXPECT_EQ(to_hex(digest.data(), 32),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256, LongerString) {
    // SHA-256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
    auto digest = sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    EXPECT_EQ(to_hex(digest.data(), 32),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, SingleCharA) {
    // SHA-256("a")
    auto digest = sha256("a");
    EXPECT_EQ(to_hex(digest.data(), 32),
              "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb");
}

// ═══════════════════════════════════════════════════════════════════════════
// Hex encoding
// ═══════════════════════════════════════════════════════════════════════════

TEST(HexEncoding, EmptyInput) {
    EXPECT_EQ(to_hex(nullptr, 0), "");
}

TEST(HexEncoding, SingleByte) {
    uint8_t data[] = {0xAB};
    EXPECT_EQ(to_hex(data, 1), "ab");
}

TEST(HexEncoding, MultipleBytes) {
    uint8_t data[] = {0x00, 0xFF, 0x42, 0x13};
    EXPECT_EQ(to_hex(data, 4), "00ff4213");
}

// ═══════════════════════════════════════════════════════════════════════════
// Content-hash ID generation
// ═══════════════════════════════════════════════════════════════════════════

TEST(ContentId, PrefixIsCorrect) {
    auto id = generate_content_id(EntityType::kWorkflow, "test content");
    EXPECT_TRUE(id.starts_with("wfl-")) << "Got: " << id;

    id = generate_content_id(EntityType::kJob, "test content");
    EXPECT_TRUE(id.starts_with("job-")) << "Got: " << id;

    id = generate_content_id(EntityType::kStep, "test content");
    EXPECT_TRUE(id.starts_with("stp-")) << "Got: " << id;

    id = generate_content_id(EntityType::kTrigger, "test content");
    EXPECT_TRUE(id.starts_with("trg-")) << "Got: " << id;

    id = generate_content_id(EntityType::kWatchGroup, "test content");
    EXPECT_TRUE(id.starts_with("wgr-")) << "Got: " << id;

    id = generate_content_id(EntityType::kWatchRule, "test content");
    EXPECT_TRUE(id.starts_with("wru-")) << "Got: " << id;
}

TEST(ContentId, LengthIsCorrect) {
    // prefix (4 chars) + 12 hex chars = 16 total
    auto id = generate_content_id(EntityType::kWorkflow, "test");
    EXPECT_EQ(id.size(), 16u) << "Got: " << id;
}

TEST(ContentId, DeterministicSameInput) {
    // Same input → same ID (the whole point of content-addressable IDs).
    auto id1 = generate_content_id(EntityType::kWorkflow, "deploy pipeline v1");
    auto id2 = generate_content_id(EntityType::kWorkflow, "deploy pipeline v1");
    EXPECT_EQ(id1, id2);
}

TEST(ContentId, DifferentInputDifferentId) {
    auto id1 = generate_content_id(EntityType::kWorkflow, "version 1");
    auto id2 = generate_content_id(EntityType::kWorkflow, "version 2");
    EXPECT_NE(id1, id2);
}

TEST(ContentId, DifferentTypeSameContentDifferentId) {
    // Even with same content, different prefix → different ID string.
    auto wfl = generate_content_id(EntityType::kWorkflow, "same content");
    auto job = generate_content_id(EntityType::kJob, "same content");
    EXPECT_NE(wfl, job);
}

TEST(ContentId, MultiFragmentsSeparated) {
    // Fragments with null-byte separator avoid ambiguity.
    std::vector<std::string_view> frags1 = {"ab", "cd"};
    std::vector<std::string_view> frags2 = {"a", "bcd"};
    auto id1 = generate_content_id(EntityType::kJob, frags1);
    auto id2 = generate_content_id(EntityType::kJob, frags2);
    EXPECT_NE(id1, id2) << "Fragment separation should prevent ambiguity";
}

TEST(ContentId, HexCharsOnly) {
    auto id = generate_content_id(EntityType::kWorkflow, "some yaml content");
    std::string hex_part = id.substr(4);  // strip prefix
    EXPECT_EQ(hex_part.size(), 12u);
    for (char c : hex_part) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "Non-hex char: " << c;
    }
}

TEST(ContentId, EmptyContentStillValid) {
    auto id = generate_content_id(EntityType::kWorkflow, "");
    EXPECT_EQ(id.size(), 16u);
    EXPECT_TRUE(id.starts_with("wfl-"));
}

// ═══════════════════════════════════════════════════════════════════════════
// Run ID generation (UUID v4)
// ═══════════════════════════════════════════════════════════════════════════

TEST(RunId, HasCorrectPrefix) {
    auto id = generate_run_id();
    EXPECT_TRUE(id.starts_with("run-")) << "Got: " << id;
}

TEST(RunId, HasCorrectLength) {
    // "run-" (4) + 32 hex chars = 36
    auto id = generate_run_id();
    EXPECT_EQ(id.size(), 36u) << "Got: " << id;
}

TEST(RunId, UniqueAcrossCalls) {
    std::set<std::string> ids;
    for (int i = 0; i < 1000; ++i) {
        ids.insert(generate_run_id());
    }
    EXPECT_EQ(ids.size(), 1000u) << "Run IDs should be unique";
}

TEST(RunId, HexCharsOnly) {
    auto id = generate_run_id();
    std::string hex_part = id.substr(4);
    for (char c : hex_part) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "Non-hex char: " << c;
    }
}

TEST(RunId, UuidV4VersionBit) {
    // In UUID v4, byte 6 has version nibble = 4 (0100).
    // In our hex encoding: position 12-13 of the hex string (byte 6).
    auto id = generate_run_id();
    std::string hex_part = id.substr(4);
    // Byte 6 = hex_part[12..13], high nibble should be 4.
    EXPECT_EQ(hex_part[12], '4') << "UUID v4 version nibble should be 4";
}

// ═══════════════════════════════════════════════════════════════════════════
// Correlation ID generation
// ═══════════════════════════════════════════════════════════════════════════

TEST(CorrelationId, NoPrefix) {
    auto id = generate_correlation_id();
    // No prefix — just 32 hex chars.
    EXPECT_EQ(id.size(), 32u) << "Got: " << id;
}

TEST(CorrelationId, Unique) {
    std::set<std::string> ids;
    for (int i = 0; i < 100; ++i) {
        ids.insert(generate_correlation_id());
    }
    EXPECT_EQ(ids.size(), 100u);
}

}  // namespace kairos::core
