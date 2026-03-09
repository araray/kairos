/// include/kairos/persist/query_reader.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/persist/query_reader.hpp — Read-only run history queries         ║
// ║                                                                          ║
// ║  Provides the SQLite-backed implementations for KEL job() functions:    ║
// ║    job("id").last_success, job("id").finished_within(dur), etc.          ║
// ║                                                                          ║
// ║  Thread safety: read-only queries on WAL-mode SQLite are safe to call   ║
// ║  concurrently with the DB writer thread.                                 ║
// ║                                                                          ║
// ║  Spec reference: §16.6, §7.7–7.8                                       ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/kel/evaluator.hpp"
#include "kairos/kel/value.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace kairos::persist {

/// Read-only query interface for run history.
///
/// All methods accept a job_name (the human-readable name used in
/// workflow YAML) and query the `runs` and `job_runs` tables.
///
/// The "last run" semantics: the most recent run for the given job,
/// regardless of which workflow it belonged to. This enables
/// cross-workflow dependencies like:
///   condition: 'job("backup").last_success'
class QueryReader {
public:
    /// Construct with a read-only database reference.
    /// The database must be opened in WAL mode.
    explicit QueryReader(SQLite::Database& db);

    // ── KEL job() function support ───────────────────────────────

    /// Did the most recent run of this job succeed?
    /// Returns false if the job has never run.
    [[nodiscard]] bool last_success(const std::string& job_name) const;

    /// What was the status of the most recent run?
    /// Returns "NEVER_RUN" if no runs exist.
    [[nodiscard]] std::string last_status(
        const std::string& job_name) const;

    /// What was the exit code of the most recent run?
    /// Returns -1 if no runs exist.
    [[nodiscard]] int last_exit_code(const std::string& job_name) const;

    /// Did the job finish successfully within the given duration
    /// from `reference_time`?
    ///
    /// This is the key function for KEL expressions like:
    ///   job("backup").finished_within(24h)
    ///
    /// @param job_name       The job name to query.
    /// @param window         Duration window to look back.
    /// @param reference_time The "now" for the query (captured once
    ///                       per pipeline run for determinism).
    [[nodiscard]] bool finished_within(
        const std::string& job_name,
        std::chrono::milliseconds window,
        std::chrono::system_clock::time_point reference_time =
            std::chrono::system_clock::now()) const;

    /// Has the job ever run?
    [[nodiscard]] bool has_run(const std::string& job_name) const;

    /// Total number of runs for this job.
    [[nodiscard]] int64_t run_count(const std::string& job_name) const;

    /// Success rate as a fraction (0.0–1.0).
    /// Returns 0.0 if the job has never run.
    [[nodiscard]] double success_rate(
        const std::string& job_name) const;

    // ── KEL integration ──────────────────────────────────────────

    /// Register the job() function and its member/method resolvers
    /// into a KEL EvalContext.
    ///
    /// After calling this, KEL expressions like:
    ///   job("build").last_success
    ///   job("backup").finished_within(24h)
    /// will work.
    ///
    /// @param ctx            The eval context to register into.
    /// @param reference_time The "now" for finished_within queries.
    void register_kel_bindings(
        kairos::kel::EvalContext& ctx,
        std::chrono::system_clock::time_point reference_time =
            std::chrono::system_clock::now()) const;

    // ── KEL aggregate/previous support ───────────────────────────

    /// Query the most recent prior sample epoch for a watch group.
    /// Returns -1 if no prior sample exists.
    [[nodiscard]] int64_t last_sample_epoch(
        const std::string& group_name) const;

    /// Query sample data for a specific group + epoch.
    /// Returns a map of file_path → (metric_name → metric_value).
    /// The metric values are: "size" (int64), "mtime" (string),
    /// "hash" (string), "pattern_found" (0 or 1).
    struct SampleFileRow {
        std::string file_path;
        bool is_dir = false;
        int64_t size = 0;
        std::string mtime;
        std::string hash;
    };
    [[nodiscard]] std::vector<SampleFileRow> query_sample(
        const std::string& group_name,
        int64_t epoch) const;

    /// Register aggregate() and previous() KEL functions for
    /// watch-rule evaluation contexts (spec §7.7 category 3).
    ///
    /// - aggregate(data, glob, metric, func):
    ///     Operates on in-memory sample data passed via context variable.
    /// - previous(group, glob, metric):
    ///     Queries SQLite for prior sample and aggregates.
    ///
    /// @param ctx The KEL context to register into.
    void register_watch_kel_bindings(
        kairos::kel::EvalContext& ctx) const;

    // ── Watch event queries (for CLI / MCP) ──────────────────────

    /// A watch event row from the database.
    struct WatchEventRow {
        std::string event_uid;
        std::string watch_group;
        std::string rule_name;
        std::string event_type;
        std::string severity;
        std::string affected_files_json;
        int64_t sample_epoch = 0;
        std::string details_json;
        std::string created_at;
    };

    /// Query recent watch events from the database.
    /// @param limit Max number of events to return.
    /// @param watch_group Optional filter by group name (empty = all).
    [[nodiscard]] std::vector<WatchEventRow> query_watch_events(
        int limit = 50,
        const std::string& watch_group = "") const;

    // ── Run summary stats (for CLI status) ───────────────────────

    /// Summary statistics for `kairos status` display.
    struct RunStats {
        int64_t total_runs = 0;       ///< All-time run count.
        int64_t runs_today = 0;       ///< Runs started in the last 24h.
        int64_t failures_today = 0;   ///< Failed runs in the last 24h.
        int64_t active_runs = 0;      ///< Runs with status='RUNNING'.
    };

    /// Query aggregate run statistics from the database.
    [[nodiscard]] RunStats query_run_stats() const;

    /// Query the database file size in bytes.
    /// @param db_path  Path to the SQLite database file.
    [[nodiscard]] static int64_t query_db_size(
        const std::filesystem::path& db_path);

private:
    SQLite::Database& db_;
};

}  // namespace kairos::persist
