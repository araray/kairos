/// include/kairos/persist/occurrence_store.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/persist/occurrence_store.hpp — Durable scheduled occurrences      ║
// ║                                                                           ║
// ║  Villa integration contract v1. Synchronous (not via DBWriter): an        ║
// ║  occurrence row must be durable BEFORE its trigger enters the execution   ║
// ║  queue. Owns a dedicated SQLite connection (WAL) guarded by a mutex, so   ║
// ║  it can be called from the scheduler, pipeline and HTTP threads.          ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <SQLiteCpp/SQLiteCpp.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace kairos::persist {

/// One persisted scheduled occurrence.
struct OccurrenceRecord {
    int64_t seq = 0;
    int64_t rev = 0;
    std::string occurrence_id;
    std::string workflow_name;
    std::string schedule_id;      ///< Empty when not a Villa schedule.
    std::string trigger_id;
    std::string registry_digest;
    std::string scheduled_for;    ///< RFC 3339 UTC ms.
    std::string disposition;      ///< emitted | skipped | abandoned
    std::optional<std::string> engine_run_id;
    std::string created_at;
};

/// Input for a new occurrence.
struct NewOccurrence {
    std::string workflow_name;
    std::string schedule_id;
    std::string trigger_id;
    std::string registry_digest;
    std::string scheduled_for;
    std::string disposition;
};

class OccurrenceStore {
public:
    /// Open a dedicated connection to the Kairos database (runs migrations).
    explicit OccurrenceStore(const std::filesystem::path& db_path);
    ~OccurrenceStore();

    OccurrenceStore(const OccurrenceStore&) = delete;
    OccurrenceStore& operator=(const OccurrenceStore&) = delete;

    /// Insert a new occurrence with a fresh random occurrence_id.
    /// Throws on database failure (callers must then NOT deliver).
    OccurrenceRecord record(const NewOccurrence& occ);

    /// Bind the engine run id to an emitted occurrence.
    /// @return false when the occurrence does not exist.
    bool attach_run(const std::string& occurrence_id,
                    const std::string& run_id);

    /// Change disposition (e.g. emitted → abandoned).
    bool set_disposition(const std::string& occurrence_id,
                         const std::string& disposition);

    /// Daemon-start recovery (skip-only misfire policy): every occurrence
    /// still `emitted` that never started a run becomes `skipped`.
    /// @return Number of rows changed.
    int mark_unstarted_skipped();

    /// True if an occurrence for this due instant already exists, matched
    /// by workflow name or (when non-empty) Villa schedule id.
    bool exists_for(const std::string& workflow_name,
                    const std::string& schedule_id,
                    const std::string& scheduled_for);

    /// Items with seq > cursor, ascending by seq.
    std::vector<OccurrenceRecord> list_after_seq(int64_t cursor, int limit);

    /// Items with rev > rev_cursor, ascending by rev (change feed).
    std::vector<OccurrenceRecord> list_after_rev(int64_t rev_cursor, int limit);

    /// Delete rows created before now - retention.
    int prune(std::chrono::system_clock::duration retention);

    /// Look up one occurrence (tests/diagnostics).
    std::optional<OccurrenceRecord> get(const std::string& occurrence_id);

private:
    int64_t next_rev_locked();
    void maybe_prune_locked();
    std::vector<OccurrenceRecord> list_locked(const char* sql,
                                              int64_t cursor, int limit);

    std::mutex mu_;
    std::unique_ptr<SQLite::Database> db_;
    std::chrono::steady_clock::time_point last_prune_{};
};

}  // namespace kairos::persist
