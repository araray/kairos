// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  id_generator.cpp — Content-hash IDs and UUID v4 run IDs                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/core/id_generator.hpp"

#include <algorithm>
#include <cstring>
#include <random>
#include <sstream>
#include <iomanip>

namespace kairos::core {

// ═══════════════════════════════════════════════════════════════════════════
// Entity prefix mapping
// ═══════════════════════════════════════════════════════════════════════════

std::string_view entity_prefix(EntityType type) {
    switch (type) {
        case EntityType::kWorkflow:   return "wfl-";
        case EntityType::kJob:        return "job-";
        case EntityType::kStep:       return "stp-";
        case EntityType::kTrigger:    return "trg-";
        case EntityType::kWatchGroup: return "wgr-";
        case EntityType::kWatchRule:  return "wru-";
    }
    return "unk-";  // unreachable, but satisfies compiler
}

// ═══════════════════════════════════════════════════════════════════════════
// SHA-256 (self-contained, FIPS 180-4 compliant)
//
// This is NOT used for cryptographic security — only for deterministic
// content fingerprinting.  A minimal implementation avoids pulling in
// OpenSSL as a Phase 1 dependency.
//
// Reference: FIPS PUB 180-4, Section 6.2
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// SHA-256 round constants (first 32 bits of the fractional parts of the
// cube roots of the first 64 primes).
constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

constexpr uint32_t rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

constexpr uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

constexpr uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

constexpr uint32_t Sigma0(uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

constexpr uint32_t Sigma1(uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

constexpr uint32_t sigma0(uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

constexpr uint32_t sigma1(uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

/// Read big-endian uint32 from byte pointer.
inline uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  | uint32_t(p[3]);
}

/// Write big-endian uint32 to byte pointer.
inline void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

}  // anonymous namespace

std::array<uint8_t, 32> sha256(const uint8_t* data, size_t length) {
    // Initial hash values (first 32 bits of fractional parts of square
    // roots of the first 8 primes).
    uint32_t h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372,
             h3 = 0xa54ff53a, h4 = 0x510e527f, h5 = 0x9b05688c,
             h6 = 0x1f83d9ab, h7 = 0x5be0cd19;

    // Pre-processing: pad message to multiple of 512 bits (64 bytes).
    // Append bit '1', then zeros, then 64-bit big-endian length.
    size_t bit_len = length * 8;
    size_t padded_len = ((length + 8) / 64 + 1) * 64;

    std::vector<uint8_t> msg(padded_len, 0);
    std::memcpy(msg.data(), data, length);
    msg[length] = 0x80;

    // Store length in big-endian at the end.
    for (int i = 0; i < 8; ++i) {
        msg[padded_len - 1 - i] = uint8_t(bit_len >> (i * 8));
    }

    // Process each 512-bit (64-byte) block.
    for (size_t offset = 0; offset < padded_len; offset += 64) {
        uint32_t w[64];

        // Prepare message schedule.
        for (int i = 0; i < 16; ++i) {
            w[i] = be32(&msg[offset + i * 4]);
        }
        for (int i = 16; i < 64; ++i) {
            w[i] = sigma1(w[i - 2]) + w[i - 7] + sigma0(w[i - 15]) + w[i - 16];
        }

        // Initialize working variables.
        uint32_t a = h0, b = h1, c = h2, d = h3;
        uint32_t e = h4, f = h5, g = h6, h = h7;

        // 64 rounds.
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = h + Sigma1(e) + ch(e, f, g) + K[i] + w[i];
            uint32_t t2 = Sigma0(a) + maj(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        // Add compressed chunk to hash.
        h0 += a; h1 += b; h2 += c; h3 += d;
        h4 += e; h5 += f; h6 += g; h7 += h;
    }

    // Produce the final 32-byte digest.
    std::array<uint8_t, 32> digest;
    put_be32(&digest[0],  h0); put_be32(&digest[4],  h1);
    put_be32(&digest[8],  h2); put_be32(&digest[12], h3);
    put_be32(&digest[16], h4); put_be32(&digest[20], h5);
    put_be32(&digest[24], h6); put_be32(&digest[28], h7);

    return digest;
}

std::array<uint8_t, 32> sha256(std::string_view data) {
    return sha256(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

// ═══════════════════════════════════════════════════════════════════════════
// Hex encoding
// ═══════════════════════════════════════════════════════════════════════════

std::string to_hex(const uint8_t* data, size_t length) {
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        result.push_back(hex_chars[(data[i] >> 4) & 0x0F]);
        result.push_back(hex_chars[data[i] & 0x0F]);
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Content-addressable ID generation
// ═══════════════════════════════════════════════════════════════════════════

std::string generate_content_id(EntityType type, std::string_view content) {
    auto digest = sha256(content);
    // Take first 6 bytes → 12 hex characters.
    std::string hex = to_hex(digest.data(), 6);
    return std::string(entity_prefix(type)) + hex;
}

std::string generate_content_id(EntityType type,
                                const std::vector<std::string_view>& fragments) {
    // Concatenate with null-byte separator to avoid ambiguity.
    std::string combined;
    for (size_t i = 0; i < fragments.size(); ++i) {
        if (i > 0) combined.push_back('\0');
        combined.append(fragments[i]);
    }
    return generate_content_id(type, std::string_view(combined));
}

// ═══════════════════════════════════════════════════════════════════════════
// UUID v4 generation (RFC 4122)
// ═══════════════════════════════════════════════════════════════════════════

namespace {

/// Thread-local PRNG seeded from std::random_device.
std::mt19937_64& get_rng() {
    thread_local std::mt19937_64 rng(std::random_device{}());
    return rng;
}

}  // anonymous namespace

std::string generate_run_id() {
    auto& rng = get_rng();
    std::uniform_int_distribution<uint64_t> dist;

    uint64_t hi = dist(rng);
    uint64_t lo = dist(rng);

    // Set version (4) and variant (10xx) bits per RFC 4122.
    hi = (hi & 0xFFFFFFFFFFFF0FFF) | 0x0000000000004000;  // version 4
    lo = (lo & 0x3FFFFFFFFFFFFFFF) | 0x8000000000000000;  // variant 10

    // Format as 32 hex chars (no dashes for compactness).
    uint8_t bytes[16];
    for (int i = 7; i >= 0; --i) { bytes[i]     = uint8_t(hi); hi >>= 8; }
    for (int i = 7; i >= 0; --i) { bytes[8 + i] = uint8_t(lo); lo >>= 8; }

    return "run-" + to_hex(bytes, 16);
}

std::string generate_correlation_id() {
    auto& rng = get_rng();
    std::uniform_int_distribution<uint64_t> dist;

    uint64_t hi = dist(rng);
    uint64_t lo = dist(rng);

    hi = (hi & 0xFFFFFFFFFFFF0FFF) | 0x0000000000004000;
    lo = (lo & 0x3FFFFFFFFFFFFFFF) | 0x8000000000000000;

    uint8_t bytes[16];
    for (int i = 7; i >= 0; --i) { bytes[i]     = uint8_t(hi); hi >>= 8; }
    for (int i = 7; i >= 0; --i) { bytes[8 + i] = uint8_t(lo); lo >>= 8; }

    return to_hex(bytes, 16);
}

}  // namespace kairos::core
