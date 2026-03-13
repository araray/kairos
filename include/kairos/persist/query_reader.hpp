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

    // ── CLI run history queries (§16.6, §23.2) ──────────────────

    /// A summary record for one run (used by `runs list` and `runs show`).
    struct RunSummary {
        std::string run_id;
        std::string target_type;       ///< "workflow" or "job"
        std::string target_id;
        std::string target_name;
        std::string trigger_type;
        std::string status;
        int exit_code = 0;
        std::string start_ts;
        std::string end_ts;
        int64_t duration_ms = 0;
    };

    /// Query recent runs with optional filters.
    /// @param limit          Max number of results.
    /// @param status_filter  Filter by status (empty = all).
    /// @param target_filter  Filter by target_name (empty = all).
    /// @param since          Only runs started after this ISO-8601 timestamp.
    [[nodiscard]] std::vector<RunSummary> query_recent_runs(
        int limit = 20,
        const std::string& status_filter = "",
        const std::string& target_filter = "",
        const std::string& since = "") const;

    // ── Run ID prefix resolution (§1.1 — Roadmap Phase 6) ──────

    /// Result of resolving a run ID prefix.
    struct PrefixResult {
        enum Status { kExact, kUnique, kAmbiguous, kNotFound, kEmpty };
        Status status = kNotFound;
        std::string resolved_id;        ///< Full run_id (if kExact or kUnique).
        std::vector<std::string> candidates;  ///< Multiple matches (if kAmbiguous).
    };

    /// Resolve a (possibly truncated) run ID prefix to a full run_id.
    ///
    /// Logic:
    ///  - If prefix is empty → kEmpty.
    ///  - If prefix length >= 36 (full UUID) → exact match.
    ///  - Otherwise → LIKE prefix% query.
    ///    - 0 results → kNotFound.
    ///    - 1 result  → kUnique (returns full ID).
    ///    - N results → kAmbiguous (returns up to 10 candidates).
    [[nodiscard]] PrefixResult resolve_run_id_prefix(
        const std::string& prefix) const;

    /// Get a single run summary by run_id.
    [[nodiscard]] std::optional<RunSummary> get_run_summary(
        const std::string& run_id) const;

    /// Full run detail with jobs and steps (§16.6).
    struct StepDetail {
        std::string step_id;
        std::string step_name;
        std::string status;
        int exit_code = 0;
        std::string start_ts;
        std::string end_ts;
        int64_t duration_ms = 0;
        std::string command;
    };

    struct JobDetail {
        std::string job_id;
        std::string job_name;
        std::string status;
        int exit_code = 0;
        std::string start_ts;
        std::string end_ts;
        int64_t duration_ms = 0;
        std::string condition_result;
        std::vector<StepDetail> steps;
    };

    struct RunDetail {
        RunSummary run;
        std::vector<JobDetail> jobs;
    };

    /// Get full run detail including jobs and steps.
    [[nodiscard]] std::optional<RunDetail> get_run_detail(
        const std::string& run_id) const;

    // ── Log chunk queries (§23.8) ───────────────────────────────

    /// A log chunk record.
    struct LogChunk {
        int64_t id = 0;              ///< Row ID (used as sequence/cursor).
        std::string run_id;
        std::string job_id;
        std::string step_id;
        std::string stream;          ///< "stdout" or "stderr"
        int64_t chunk_index = 0;
        std::string content;
        std::string created_at;
    };

    /// Get log chunks for a run, after a given cursor (row ID).
    /// Used by `kairos logs --follow` polling loop.
    /// @param run_id    The run to get logs for.
    /// @param after_id  Only return chunks with id > after_id (cursor).
    /// @param limit     Max chunks to return.
    [[nodiscard]] std::vector<LogChunk> get_log_chunks(
        const std::string& run_id,
        int64_t after_id = 0,
        int limit = 100) const;

    // ── Metrics snapshot queries (§20.5) ────────────────────────

    /// A metrics snapshot row.
    struct MetricsSnapshotRow {
        std::string metric_name;
        std::string metric_type;
        double value = 0.0;
        std::string labels_json;
        std::string created_at;
    };

    /// Query recent metrics snapshots for `kairos status --history`.
    /// @param limit  Number of snapshot epochs to return.
    [[nodiscard]] std::vector<MetricsSnapshotRow> query_metrics_snapshots(
        int limit = 10) const;

    // ── Prune preview queries (§16.8, §23.2 — prune --dry-run) ──

    /// Summary of what a prune operation would delete.
    struct PrunePreview {
        int64_t runs_to_delete = 0;
        int64_t run_jobs_to_delete = 0;
        int64_t run_steps_to_delete = 0;
        int64_t log_chunks_to_delete = 0;
        int64_t watch_events_to_delete = 0;
        int64_t watch_samples_to_delete = 0;
        int64_t metrics_snapshots_to_delete = 0;
    };

    /// Preview what a prune operation with the given cutoff would delete.
    /// @param older_than_days  Delete records older than this many days.
    [[nodiscard]] PrunePreview query_prune_preview(
        int older_than_days) const;

    // ── Events tail (cursor-based, for events tail CLI) ─────────

    /// Query watch events with a cursor (row id > after_id).
    /// Used by `kairos events tail` for poll-based streaming.
    /// @param after_id     Only return events with rowid > after_id.
    /// @param limit        Max events to return.
    /// @param watch_group  Optional filter by group name (empty = all).
    [[nodiscard]] std::vector<WatchEventRow> query_watch_events_since(
        int64_t after_id,
        int limit = 50,
        const std::string& watch_group = "") const;

    /// Query the maximum rowid in watch_events table.
    /// Returns 0 if no events exist.
    [[nodiscard]] int64_t query_max_event_id() const;

private:
    SQLite::Database& db_;
};

}  // namespace kairos::persist
