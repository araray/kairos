/// include/kairos/engine/villa_integration.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/engine/villa_integration.hpp — Villa integration contract v1      ║
// ║                                                                           ║
// ║  Pure helpers shared by the scheduler, pipeline, daemon and HTTP layer:   ║
// ║    - registry digest (SHA-256 over canonical [name, sha256(src)] pairs)   ║
// ║    - trigger digest  (SHA-256 over canonical normalized trigger JSON)     ║
// ║    - Villa schedule-name parsing (villa.schedule.sch_<hex>.g<N>)          ║
// ║    - RFC 3339 UTC millisecond formatting, opaque random identifiers       ║
// ║                                                                           ║
// ║  See docs/villa-integration.md for the contract.                          ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/engine/trigger_types.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kairos::engine::villa {

/// Integration contract version reported by the status endpoint.
inline constexpr int kContractVersion = 1;

/// Upstream baseline commit this integration was built on.
inline constexpr std::string_view kBaselineCommit =
    "6391343c7b606aa2b187e43c9d6334c8fb0bf886";

/// Timer continuity marker reported by the status endpoint.
inline constexpr std::string_view kTimerContinuity = "preserve_unchanged_v1";

/// Retention window for scheduled occurrence records.
inline constexpr std::chrono::hours kOccurrenceRetention{24 * 7};

/// Occurrence dispositions.
inline constexpr std::string_view kDispositionEmitted = "emitted";
inline constexpr std::string_view kDispositionSkipped = "skipped";
inline constexpr std::string_view kDispositionAbandoned = "abandoned";

/// Trusted environment variables injected into scheduled steps.
inline constexpr std::string_view kEnvOccurrenceId = "VILLA_KAIROS_OCCURRENCE_ID";
inline constexpr std::string_view kEnvScheduledFor = "VILLA_KAIROS_SCHEDULED_FOR";
inline constexpr std::string_view kEnvConfigHash = "VILLA_KAIROS_CONFIG_HASH";
/// Every variable with this prefix is reserved: workflow/job/step env and
/// the inherited daemon environment can never supply it.
inline constexpr std::string_view kReservedEnvPrefix = "VILLA_KAIROS_";

/// Lowercase hex SHA-256 of arbitrary bytes.
[[nodiscard]] std::string sha256_hex(std::string_view bytes);

/// Registry digest over (workflow_name, sha256_hex(source_bytes)) pairs.
/// Pairs are sorted by name (then by hash for duplicate names) and
/// serialized as compact JSON `[["name","hex"],...]`; the digest is the
/// lowercase hex SHA-256 of that UTF-8 text. Empty input → digest of "[]".
[[nodiscard]] std::string registry_digest(
    std::vector<std::pair<std::string, std::string>> name_and_source_hash);

/// Canonical normalized JSON for a trigger spec (type + timing fields
/// only; sorted keys, no whitespace):
///   cron:     {"expression":"<expr>","type":"cron"}
///   interval: {"interval_ms":<int>,"type":"interval"}
///   date:     {"at":"<RFC3339 UTC ms>","type":"date"}
[[nodiscard]] std::string canonical_trigger_json(const TriggerSpec& spec);

/// SHA-256 hex of canonical_trigger_json(spec).
[[nodiscard]] std::string trigger_digest(const TriggerSpec& spec);

/// Parsed Villa schedule workflow identity.
struct ScheduleIdentity {
    std::string schedule_id;   ///< "sch_<32 lowercase hex>"
    int64_t generation = 0;    ///< Decimal generation number.
};

/// Parse `^villa\.schedule\.(sch_[0-9a-f]{32})\.g([0-9]+)$`.
[[nodiscard]] std::optional<ScheduleIdentity> parse_schedule_name(
    std::string_view workflow_name);

/// Format a wall-clock instant as RFC 3339 UTC with milliseconds,
/// e.g. "2026-11-13T09:00:00.000Z".
[[nodiscard]] std::string format_rfc3339_ms(
    std::chrono::system_clock::time_point tp);

/// Random opaque identifier "<prefix><32 lowercase hex>".
[[nodiscard]] std::string random_id(std::string_view prefix);

}  // namespace kairos::engine::villa
