/// src/watch/hash_util.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  hash_util.cpp — Portable MD5 + SHA-256 implementation                  ║
// ║                                                                          ║
// ║  MD5: RFC 1321. SHA-256: FIPS 180-4.                                    ║
// ║  Both operate on 64-byte (512-bit) blocks with Merkle-Damgård.          ║
// ║                                                                          ║
// ║  File I/O: 64 KB read buffer, dual-context update in single pass.       ║
// ║  Cooperative cancellation via std::stop_token.                           ║
// ║                                                                          ║
// ║  Spec reference: §12.6.3                                                ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/watch/hash_util.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace kairos::watch {

// ══════════════════════════════════════════════════════════════════════════
// SHA-256 implementation (FIPS 180-4)
// ══════════════════════════════════════════════════════════════════════════

namespace {

struct SHA256Context {
    uint32_t state[8]{};
    uint64_t bit_count = 0;
    uint8_t buffer[64]{};
    uint32_t buffer_len = 0;
};

static constexpr uint32_t sha256_k[64] = {
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
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

inline uint32_t sigma0(uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

inline uint32_t sigma1(uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

inline uint32_t gamma0(uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

inline uint32_t gamma1(uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

void sha256_init(SHA256Context& ctx) {
    ctx.state[0] = 0x6a09e667;
    ctx.state[1] = 0xbb67ae85;
    ctx.state[2] = 0x3c6ef372;
    ctx.state[3] = 0xa54ff53a;
    ctx.state[4] = 0x510e527f;
    ctx.state[5] = 0x9b05688c;
    ctx.state[6] = 0x1f83d9ab;
    ctx.state[7] = 0x5be0cd19;
    ctx.bit_count = 0;
    ctx.buffer_len = 0;
}

void sha256_transform(SHA256Context& ctx, const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4]) << 24) |
               (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) |
               (uint32_t(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        w[i] = gamma1(w[i - 2]) + w[i - 7] +
               gamma0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = ctx.state[0], b = ctx.state[1];
    uint32_t c = ctx.state[2], d = ctx.state[3];
    uint32_t e = ctx.state[4], f = ctx.state[5];
    uint32_t g = ctx.state[6], h = ctx.state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + sigma1(e) + ch(e, f, g) + sha256_k[i] + w[i];
        uint32_t t2 = sigma0(a) + maj(a, b, c);
        h = g; g = f; f = e;
        e = d + t1;
        d = c; c = b; b = a;
        a = t1 + t2;
    }

    ctx.state[0] += a; ctx.state[1] += b;
    ctx.state[2] += c; ctx.state[3] += d;
    ctx.state[4] += e; ctx.state[5] += f;
    ctx.state[6] += g; ctx.state[7] += h;
}

void sha256_update(SHA256Context& ctx,
                   const uint8_t* data, size_t len) {
    ctx.bit_count += len * 8;
    size_t offset = 0;

    // Fill partial buffer.
    if (ctx.buffer_len > 0) {
        uint32_t fill = 64 - ctx.buffer_len;
        if (len < fill) {
            std::memcpy(ctx.buffer + ctx.buffer_len, data, len);
            ctx.buffer_len += static_cast<uint32_t>(len);
            return;
        }
        std::memcpy(ctx.buffer + ctx.buffer_len, data, fill);
        sha256_transform(ctx, ctx.buffer);
        ctx.buffer_len = 0;
        offset = fill;
    }

    // Process full blocks.
    while (offset + 64 <= len) {
        sha256_transform(ctx, data + offset);
        offset += 64;
    }

    // Buffer remaining.
    if (offset < len) {
        ctx.buffer_len = static_cast<uint32_t>(len - offset);
        std::memcpy(ctx.buffer, data + offset, ctx.buffer_len);
    }
}

std::array<uint8_t, 32> sha256_final(SHA256Context& ctx) {
    // Pad: append 1-bit, zeros, then 64-bit big-endian length.
    uint8_t pad[64]{};
    pad[0] = 0x80;

    uint32_t pad_len = (ctx.buffer_len < 56)
        ? (56 - ctx.buffer_len)
        : (120 - ctx.buffer_len);

    sha256_update(ctx, pad, pad_len);

    // Append length as big-endian 64-bit.
    uint8_t len_bytes[8];
    for (int i = 7; i >= 0; --i) {
        len_bytes[i] = static_cast<uint8_t>(ctx.bit_count & 0xFF);
        ctx.bit_count >>= 8;
    }
    // Update without counting these bytes toward bit_count.
    // We directly transform since we know the buffer has exactly 56 bytes.
    std::memcpy(ctx.buffer + ctx.buffer_len, len_bytes, 8);
    sha256_transform(ctx, ctx.buffer);
    ctx.buffer_len = 0;

    // Extract digest (big-endian).
    std::array<uint8_t, 32> digest{};
    for (int i = 0; i < 8; ++i) {
        digest[i * 4 + 0] = static_cast<uint8_t>(ctx.state[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(ctx.state[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(ctx.state[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(ctx.state[i]);
    }
    return digest;
}

// ══════════════════════════════════════════════════════════════════════════
// MD5 implementation (RFC 1321)
// ══════════════════════════════════════════════════════════════════════════

struct MD5Context {
    uint32_t state[4]{};
    uint64_t bit_count = 0;
    uint8_t buffer[64]{};
    uint32_t buffer_len = 0;
};

// Per-round shift amounts.
static constexpr int md5_s[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21,
};

// Per-round constants (floor(2^32 * abs(sin(i+1)))).
static constexpr uint32_t md5_t[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

inline uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

void md5_init(MD5Context& ctx) {
    ctx.state[0] = 0x67452301;
    ctx.state[1] = 0xefcdab89;
    ctx.state[2] = 0x98badcfe;
    ctx.state[3] = 0x10325476;
    ctx.bit_count = 0;
    ctx.buffer_len = 0;
}

void md5_transform(MD5Context& ctx, const uint8_t block[64]) {
    // Decode block as 16 little-endian uint32_t words.
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = (uint32_t(block[i * 4])) |
               (uint32_t(block[i * 4 + 1]) << 8) |
               (uint32_t(block[i * 4 + 2]) << 16) |
               (uint32_t(block[i * 4 + 3]) << 24);
    }

    uint32_t a = ctx.state[0], b = ctx.state[1];
    uint32_t c = ctx.state[2], d = ctx.state[3];

    for (int i = 0; i < 64; ++i) {
        uint32_t f, g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = static_cast<uint32_t>(i);
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = static_cast<uint32_t>((5 * i + 1) % 16);
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = static_cast<uint32_t>((3 * i + 5) % 16);
        } else {
            f = c ^ (b | ~d);
            g = static_cast<uint32_t>((7 * i) % 16);
        }

        uint32_t temp = d;
        d = c;
        c = b;
        b = b + rotl32(a + f + md5_t[i] + m[g], md5_s[i]);
        a = temp;
    }

    ctx.state[0] += a; ctx.state[1] += b;
    ctx.state[2] += c; ctx.state[3] += d;
}

void md5_update(MD5Context& ctx, const uint8_t* data, size_t len) {
    ctx.bit_count += len * 8;
    size_t offset = 0;

    if (ctx.buffer_len > 0) {
        uint32_t fill = 64 - ctx.buffer_len;
        if (len < fill) {
            std::memcpy(ctx.buffer + ctx.buffer_len, data, len);
            ctx.buffer_len += static_cast<uint32_t>(len);
            return;
        }
        std::memcpy(ctx.buffer + ctx.buffer_len, data, fill);
        md5_transform(ctx, ctx.buffer);
        ctx.buffer_len = 0;
        offset = fill;
    }

    while (offset + 64 <= len) {
        md5_transform(ctx, data + offset);
        offset += 64;
    }

    if (offset < len) {
        ctx.buffer_len = static_cast<uint32_t>(len - offset);
        std::memcpy(ctx.buffer, data + offset, ctx.buffer_len);
    }
}

std::array<uint8_t, 16> md5_final(MD5Context& ctx) {
    // Save the bit count before padding mutates it.
    uint64_t total_bits = ctx.bit_count;

    // Pad: 0x80 then zeros until 56 mod 64 bytes.
    uint8_t pad_byte = 0x80;
    md5_update(ctx, &pad_byte, 1);
    while (ctx.buffer_len != 56) {
        uint8_t zero = 0;
        md5_update(ctx, &zero, 1);
    }

    // Append original length as little-endian 64-bit.
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; ++i) {
        len_bytes[i] = static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFF);
    }
    md5_update(ctx, len_bytes, 8);

    // Extract digest (little-endian).
    std::array<uint8_t, 16> digest{};
    for (int i = 0; i < 4; ++i) {
        digest[i * 4 + 0] = static_cast<uint8_t>(ctx.state[i]);
        digest[i * 4 + 1] = static_cast<uint8_t>(ctx.state[i] >> 8);
        digest[i * 4 + 2] = static_cast<uint8_t>(ctx.state[i] >> 16);
        digest[i * 4 + 3] = static_cast<uint8_t>(ctx.state[i] >> 24);
    }
    return digest;
}

// ══════════════════════════════════════════════════════════════════════════
// Hex encoding
// ══════════════════════════════════════════════════════════════════════════

std::string to_hex(const uint8_t* data, size_t len) {
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        result += hex_chars[(data[i] >> 4) & 0x0F];
        result += hex_chars[data[i] & 0x0F];
    }
    return result;
}

}  // namespace

// ── Public API ──────────────────────────────────────────────────────────

std::string sha256_hex(const void* data, std::size_t len) {
    SHA256Context ctx;
    sha256_init(ctx);
    sha256_update(ctx,
                  static_cast<const uint8_t*>(data),
                  len);
    auto digest = sha256_final(ctx);
    return to_hex(digest.data(), digest.size());
}

std::string md5_hex(const void* data, std::size_t len) {
    MD5Context ctx;
    md5_init(ctx);
    md5_update(ctx,
               static_cast<const uint8_t*>(data),
               len);
    auto digest = md5_final(ctx);
    return to_hex(digest.data(), digest.size());
}

std::optional<HashResult> compute_file_hashes(
    const std::filesystem::path& path,
    std::stop_token stop)
{
    // Open file in binary mode.
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }

    SHA256Context sha_ctx;
    MD5Context md5_ctx;
    sha256_init(sha_ctx);
    md5_init(md5_ctx);

    // 64 KB read buffer per spec §12.6.3.
    constexpr size_t kChunkSize = 65536;
    std::array<char, kChunkSize> buffer{};

    while (file) {
        if (stop.stop_requested()) {
            return std::nullopt;
        }

        file.read(buffer.data(), kChunkSize);
        auto bytes_read = static_cast<size_t>(file.gcount());
        if (bytes_read == 0) break;

        auto* data = reinterpret_cast<const uint8_t*>(buffer.data());
        sha256_update(sha_ctx, data, bytes_read);
        md5_update(md5_ctx, data, bytes_read);
    }

    auto sha_digest = sha256_final(sha_ctx);
    auto md5_digest = md5_final(md5_ctx);

    return HashResult{
        .md5 = to_hex(md5_digest.data(), md5_digest.size()),
        .sha256 = to_hex(sha_digest.data(), sha_digest.size()),
    };
}

}  // namespace kairos::watch
