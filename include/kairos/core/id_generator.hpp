// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/core/id_generator.hpp — Content-addressable ID generation         ║
// ║                                                                           ║
// ║  Entity IDs are derived from content hashing (SHA-256 truncated to 6      ║
// ║  bytes → 12 hex chars) for determinism: the same workflow YAML always     ║
// ║  produces the same ID.                                                    ║
// ║                                                                           ║
// ║  Run IDs use UUID v4 because each run is a unique event.                  ║
// ║                                                                           ║
// ║  Spec reference: §4.2                                                     ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kairos::core {

/// Prefixes for different entity types.  Prefixes make IDs self-describing
/// in logs, database records, and CLI output.
enum class EntityType {
    kWorkflow,        // "wfl-"
    kJob,             // "job-"
    kStep,            // "stp-"
    kTrigger,         // "trg-"
    kWatchGroup,      // "wgr-"
    kWatchRule,       // "wru-"
};

/// Return the string prefix for an entity type (e.g., "wfl-").
std::string_view entity_prefix(EntityType type);

/// Generate a content-addressable ID from arbitrary input bytes.
///
/// Algorithm: SHA-256(content) → take first 6 bytes → hex encode → prefix.
/// Result format: "<prefix><12-hex-chars>" (e.g., "wfl-a3f82c1b9d04").
///
/// The SHA-256 implementation is a minimal, self-contained one (no OpenSSL
/// required at this stage). It is NOT used for cryptographic purposes —
/// only for deterministic fingerprinting.
///
/// @param type    Entity type (determines prefix)
/// @param content Raw bytes to hash (typically the YAML/config fragment)
/// @return        Prefixed content-hash ID
std::string generate_content_id(EntityType type, std::string_view content);

/// Generate a content-addressable ID from multiple content fragments.
/// Fragments are concatenated with null-byte separators before hashing
/// to avoid ambiguity (e.g., "ab" + "cd" ≠ "a" + "bcd").
std::string generate_content_id(EntityType type,
                                const std::vector<std::string_view>& fragments);

/// Generate a UUID v4 run ID.  Format: "run-<32-hex-chars>".
/// Uses std::random_device for seeding + mt19937_64.
std::string generate_run_id();

/// Generate a UUID v4 correlation ID. Format: "<32-hex-chars>" (no prefix).
std::string generate_correlation_id();

// ── SHA-256 (minimal, self-contained) ─────────────────────────────────────

/// Compute SHA-256 of input data.  Returns 32-byte digest.
/// This is a standalone implementation — no external crypto library needed.
std::array<uint8_t, 32> sha256(const uint8_t* data, size_t length);

/// Convenience: SHA-256 of a string_view.
std::array<uint8_t, 32> sha256(std::string_view data);

/// Encode bytes as lowercase hexadecimal.
std::string to_hex(const uint8_t* data, size_t length);

}  // namespace kairos::core
