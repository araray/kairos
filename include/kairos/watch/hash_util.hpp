/// include/kairos/watch/hash_util.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/watch/hash_util.hpp — Portable file hashing (MD5 + SHA-256)     ║
// ║                                                                          ║
// ║  Production-grade, portable hash computation for the watch engine.      ║
// ║  Files are read in 64 KB chunks, feeding both MD5 and SHA-256 contexts  ║
// ║  simultaneously in a single pass (§12.6.3).                             ║
// ║                                                                          ║
// ║  This uses bundled implementations — no OpenSSL required. If OpenSSL    ║
// ║  is available at build time, a future version can delegate to it for    ║
// ║  hardware-accelerated hashing (SHA-NI, AES-NI).                        ║
// ║                                                                          ║
// ║  Spec reference: §12.6.3                                                ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>

namespace kairos::watch {

/// Result of hashing a file (both MD5 and SHA-256 computed in one pass).
struct HashResult {
    std::string md5;        ///< 32-char lowercase hex string.
    std::string sha256;     ///< 64-char lowercase hex string.
};

/// Compute MD5 and SHA-256 of a file in a single pass.
///
/// Reads the file in 64 KB chunks, feeding both hash contexts simultaneously.
/// If the file cannot be read, returns std::nullopt.
///
/// @param path  Absolute file path.
/// @param stop  Cooperative cancellation (aborts if triggered).
/// @return HashResult with both hashes, or nullopt on error/cancel.
[[nodiscard]] std::optional<HashResult> compute_file_hashes(
    const std::filesystem::path& path,
    std::stop_token stop = {});

/// Compute SHA-256 of a raw byte buffer. Used for testing.
[[nodiscard]] std::string sha256_hex(const void* data, std::size_t len);

/// Compute MD5 of a raw byte buffer. Used for testing.
[[nodiscard]] std::string md5_hex(const void* data, std::size_t len);

}  // namespace kairos::watch
