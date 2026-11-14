/// src/persist/occurrence_store.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Durable scheduled occurrences (Villa integration contract v1)           ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/occurrence_store.hpp"
#include "kairos/core/id_generator.hpp"
#include "kairos/persist/database.hpp"

#include <spdlog/spdlog.h>

#include <ctime>

namespace kairos::persist {

namespace {

constexpr const char* kSelectCols =
    "seq, rev, occurrence_id, workflow_name, COALESCE(schedule_id,''), "
    "trigger_id, registry_digest, scheduled_for, disposition, "
    "engine_run_id, created_at";

OccurrenceRecord read_row(SQLite::Statement& q) {
    OccurrenceRecord r;
    r.seq = q.getColumn(0).getInt64();
    r.rev = q.getColumn(1).getInt64();
    r.occurrence_id = q.getColumn(2).getString();
    r.workflow_name = q.getColumn(3).getString();
    r.schedule_id = q.getColumn(4).getString();
    r.trigger_id = q.getColumn(5).getString();
    r.registry_digest = q.getColumn(6).getString();
    r.scheduled_for = q.getColumn(7).getString();
    r.disposition = q.getColumn(8).getString();
    if (!q.getColumn(9).isNull()) r.engine_run_id = q.getColumn(9).getString();
    r.created_at = q.getColumn(10).getString();
    return r;
}

/// SQLite-compatible UTC timestamp (matches strftime('%Y-%m-%dT%H:%M:%fZ')).
std::string sqlite_utc(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    auto ms_total = duration_cast<milliseconds>(tp.time_since_epoch()).count();
    std::time_t tt = static_cast<std::time_t>(ms_total / 1000);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &tt);
#else
    gmtime_r(&tt, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03dZ", buf,
                  static_cast<int>(ms_total % 1000));
    return out;
}

}  // namespace

OccurrenceStore::OccurrenceStore(const std::filesystem::path& db_path)
    : db_(open_database(db_path))
{
    // Occurrence rows are an external contract: favour durability.
    db_->exec("PRAGMA synchronous = FULL");
}

OccurrenceStore::~OccurrenceStore() = default;

int64_t OccurrenceStore::next_rev_locked() {
    db_->exec("UPDATE scheduled_occurrence_counters SET value = value + 1 "
              "WHERE name = 'rev'");
    SQLite::Statement q(*db_,
        "SELECT value FROM scheduled_occurrence_counters WHERE name = 'rev'");
    q.executeStep();
    return q.getColumn(0).getInt64();
}

void OccurrenceStore::maybe_prune_locked() {
    auto now = std::chrono::steady_clock::now();
    if (last_prune_ != std::chrono::steady_clock::time_point{} &&
        now - last_prune_ < std::chrono::hours(1)) {
        return;
    }
    last_prune_ = now;
    try {
        SQLite::Statement del(*db_,
            "DELETE FROM scheduled_occurrences WHERE created_at < ?");
        del.bind(1, sqlite_utc(std::chrono::system_clock::now() -
                               std::chrono::hours(24 * 7)));
        int n = del.exec();
        if (n > 0) spdlog::info("Pruned {} scheduled occurrence(s)", n);
    } catch (const std::exception& e) {
        spdlog::warn("Occurrence prune failed: {}", e.what());
    }
}

OccurrenceRecord OccurrenceStore::record(const NewOccurrence& occ) {
    std::lock_guard lock(mu_);
    maybe_prune_locked();

    SQLite::Transaction tx(*db_);
    auto rev = next_rev_locked();
    auto id = "occ_" + core::generate_correlation_id();
    SQLite::Statement ins(*db_,
        "INSERT INTO scheduled_occurrences (occurrence_id, workflow_name, "
        "schedule_id, trigger_id, registry_digest, scheduled_for, "
        "disposition, rev) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    ins.bind(1, id);
    ins.bind(2, occ.workflow_name);
    if (occ.schedule_id.empty()) ins.bind(3); else ins.bind(3, occ.schedule_id);
    ins.bind(4, occ.trigger_id);
    ins.bind(5, occ.registry_digest);
    ins.bind(6, occ.scheduled_for);
    ins.bind(7, occ.disposition);
    ins.bind(8, rev);
    ins.exec();
    auto seq = db_->getLastInsertRowid();
    tx.commit();

    SQLite::Statement q(*db_, std::string("SELECT ") + kSelectCols +
        " FROM scheduled_occurrences WHERE seq = ?");
    q.bind(1, seq);
    q.executeStep();
    return read_row(q);
}

bool OccurrenceStore::attach_run(const std::string& occurrence_id,
                                 const std::string& run_id) {
    std::lock_guard lock(mu_);
    SQLite::Transaction tx(*db_);
    auto rev = next_rev_locked();
    SQLite::Statement up(*db_,
        "UPDATE scheduled_occurrences SET engine_run_id = ?, rev = ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
        "WHERE occurrence_id = ?");
    up.bind(1, run_id);
    up.bind(2, rev);
    up.bind(3, occurrence_id);
    int n = up.exec();
    tx.commit();
    return n > 0;
}

bool OccurrenceStore::set_disposition(const std::string& occurrence_id,
                                      const std::string& disposition) {
    std::lock_guard lock(mu_);
    SQLite::Transaction tx(*db_);
    auto rev = next_rev_locked();
    SQLite::Statement up(*db_,
        "UPDATE scheduled_occurrences SET disposition = ?, rev = ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
        "WHERE occurrence_id = ?");
    up.bind(1, disposition);
    up.bind(2, rev);
    up.bind(3, occurrence_id);
    int n = up.exec();
    tx.commit();
    return n > 0;
}

int OccurrenceStore::mark_unstarted_skipped() {
    std::lock_guard lock(mu_);
    SQLite::Transaction tx(*db_);
    std::vector<int64_t> seqs;
    {
        SQLite::Statement q(*db_,
            "SELECT seq FROM scheduled_occurrences "
            "WHERE disposition = 'emitted' AND engine_run_id IS NULL "
            "ORDER BY seq");
        while (q.executeStep()) seqs.push_back(q.getColumn(0).getInt64());
    }
    for (auto seq : seqs) {
        auto rev = next_rev_locked();
        SQLite::Statement up(*db_,
            "UPDATE scheduled_occurrences SET disposition = 'skipped', "
            "rev = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
            "WHERE seq = ?");
        up.bind(1, rev);
        up.bind(2, seq);
        up.exec();
    }
    tx.commit();
    return static_cast<int>(seqs.size());
}

bool OccurrenceStore::exists_for(const std::string& workflow_name,
                                 const std::string& schedule_id,
                                 const std::string& scheduled_for) {
    std::lock_guard lock(mu_);
    SQLite::Statement q(*db_,
        "SELECT 1 FROM scheduled_occurrences WHERE scheduled_for = ? AND "
        "(workflow_name = ? OR (? <> '' AND schedule_id = ?)) LIMIT 1");
    q.bind(1, scheduled_for);
    q.bind(2, workflow_name);
    q.bind(3, schedule_id);
    q.bind(4, schedule_id);
    return q.executeStep();
}

std::vector<OccurrenceRecord> OccurrenceStore::list_locked(
    const char* where_order, int64_t cursor, int limit) {
    std::vector<OccurrenceRecord> out;
    SQLite::Statement q(*db_, std::string("SELECT ") + kSelectCols +
        " FROM scheduled_occurrences " + where_order + " LIMIT ?");
    q.bind(1, cursor);
    q.bind(2, limit);
    while (q.executeStep()) out.push_back(read_row(q));
    return out;
}

std::vector<OccurrenceRecord> OccurrenceStore::list_after_seq(
    int64_t cursor, int limit) {
    std::lock_guard lock(mu_);
    return list_locked("WHERE seq > ? ORDER BY seq ASC", cursor, limit);
}

std::vector<OccurrenceRecord> OccurrenceStore::list_after_rev(
    int64_t rev_cursor, int limit) {
    std::lock_guard lock(mu_);
    return list_locked("WHERE rev > ? ORDER BY rev ASC", rev_cursor, limit);
}

int OccurrenceStore::prune(std::chrono::system_clock::duration retention) {
    std::lock_guard lock(mu_);
    SQLite::Statement del(*db_,
        "DELETE FROM scheduled_occurrences WHERE created_at < ?");
    del.bind(1, sqlite_utc(std::chrono::system_clock::now() - retention));
    last_prune_ = std::chrono::steady_clock::now();
    return del.exec();
}

std::optional<OccurrenceRecord> OccurrenceStore::get(
    const std::string& occurrence_id) {
    std::lock_guard lock(mu_);
    SQLite::Statement q(*db_, std::string("SELECT ") + kSelectCols +
        " FROM scheduled_occurrences WHERE occurrence_id = ?");
    q.bind(1, occurrence_id);
    if (!q.executeStep()) return std::nullopt;
    return read_row(q);
}

}  // namespace kairos::persist
