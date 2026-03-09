/// include/kairos/persist/db_writer.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/persist/db_writer.hpp — Async batch database writer              ║
// ║                                                                          ║
// ║  Background thread consumes write requests from a bounded queue,         ║
// ║  batches them into transactions, and commits. Backpressure blocks        ║
// ║  producers when the queue is full.                                       ║
// ║                                                                          ║
// ║  Spec reference: §16.5                                                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/core/bounded_queue.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace kairos::persist {

// ── Write request types ──────────────────────────────────────────────────
// Each struct maps to a specific INSERT/UPDATE/DELETE operation.

/// Insert a new run record.
struct InsertRun {
    std::string run_id;
    std::string target_type;        ///< "workflow" or "job"
    std::string target_id;          ///< workflow/job content-hash ID
    std::string target_name;        ///< workflow/job human name
    std::string trigger_type;       ///< "schedule", "watch", "manual", "mcp"
    std::string trigger_id;
    std::string correlation_id;
    std::string status;             ///< "RUNNING"
    std::string start_ts;           ///< ISO-8601
};

/// Update a completed run record.
struct UpdateRunComplete {
    std::string run_id;
    std::string status;             ///< "SUCCESS", "FAILURE", "CANCELLED"
    std::string end_ts;             ///< ISO-8601
    int exit_code = 0;
    int64_t duration_ms = 0;
};

/// Insert a job-level run record.
struct InsertJobRun {
    std::string run_id;
    std::string job_id;
    std::string job_name;
    std::string status;             ///< "RUNNING", "SKIPPED"
    std::string start_ts;
    std::string condition_result;   ///< "true", "false", or empty
};

/// Update a completed job-level record.
struct UpdateJobRunComplete {
    std::string run_id;
    std::string job_id;
    std::string status;             ///< "SUCCESS", "FAILURE", "CANCELLED"
    std::string end_ts;
    int exit_code = 0;
    int64_t duration_ms = 0;
};

/// Insert a step-level run record.
struct InsertStepRun {
    std::string run_id;
    std::string job_id;
    std::string step_id;
    std::string step_name;
    std::string command;
    std::string status;             ///< "RUNNING"
    std::string start_ts;
};

/// Update a completed step record.
struct UpdateStepComplete {
    std::string run_id;
    std::string job_id;
    std::string step_id;
    std::string status;             ///< "SUCCESS", "FAILURE"
    std::string end_ts;
    int exit_code = 0;
    int64_t duration_ms = 0;
};

/// Insert a log chunk (captured stdout/stderr).
struct InsertLogChunk {
    std::string run_id;
    std::string job_id;
    std::string step_id;
    int64_t chunk_index = 0;
    std::string stream;             ///< "stdout" or "stderr"
    std::string content;
};

/// Insert a trigger fire record.
struct InsertTriggerHistory {
    std::string trigger_id;
    std::string trigger_type;
    std::string target_id;
    std::string fired_at;
    std::string status;             ///< "fired", "skipped"
    std::string run_id;             ///< associated run, if any
};

/// Insert a watch sample record (one row per file per epoch).
/// Spec reference: §16.5, §16.2 (watch_samples table).
struct InsertWatchSample {
    std::string watch_group;
    int64_t sample_epoch = 0;
    std::string file_path;
    bool is_dir = false;
    int64_t size = 0;
    std::string mtime;              ///< ISO-8601
    std::string hash;               ///< MD5 or SHA256 (whichever computed).
    int scan_duration_ms = 0;
};

/// Insert a watch event record (emitted rule trigger).
/// Spec reference: §16.5, §16.2 (watch_events table).
struct InsertWatchEvent {
    std::string event_uid;          ///< Deterministic ID.
    std::string watch_group;
    std::string rule_name;
    std::string event_type;         ///< WatchEventType string.
    std::string severity;           ///< "info"|"warning"|"critical"
    std::string affected_files_json;///< JSON array of paths.
    int64_t sample_epoch = 0;
    std::string details_json;       ///< Arbitrary JSON payload.
};

/// Batch insert watch samples (all entries in one transaction).
/// This is the optimized path for persisting an entire scan epoch.
/// Spec reference: §16.5, §16.8 (batch inserts).
struct BatchInsertWatchSamples {
    std::string watch_group;
    int64_t sample_epoch = 0;
    int scan_duration_ms = 0;

    /// Individual file entries for this epoch.
    struct Entry {
        std::string file_path;
        bool is_dir = false;
        int64_t size = 0;
        std::string mtime;           ///< ISO-8601
        std::string hash;
    };
    std::vector<Entry> entries;
};

/// Prune old watch samples, keeping only the most recent N epochs
/// per group. Spec reference: §16.8 (retention policy).
struct PruneWatchSamples {
    std::string watch_group;
    int max_epochs = 10;             ///< Keep this many most-recent epochs.
};

/// Prune records older than a given date.
struct PruneOlderThan {
    std::string cutoff_date;        ///< ISO-8601
};

/// Insert a metrics snapshot (periodic persistence per §20.5).
struct InsertMetricsSnapshot {
    std::string metric_name;
    std::string metric_type;        ///< "counter", "gauge", "histogram"
    double value = 0.0;
    std::string labels_json;        ///< JSON object of label key-values (empty if no labels)
};

/// Batch insert of all metrics in one transaction (one snapshot epoch).
struct BatchInsertMetricsSnapshots {
    std::vector<InsertMetricsSnapshot> entries;
};

/// Union of all write request types.
using DBWriteRequest = std::variant<
    InsertRun, UpdateRunComplete,
    InsertJobRun, UpdateJobRunComplete,
    InsertStepRun, UpdateStepComplete,
    InsertLogChunk,
    InsertTriggerHistory,
    InsertWatchSample, InsertWatchEvent,
    BatchInsertWatchSamples, PruneWatchSamples,
    PruneOlderThan,
    InsertMetricsSnapshot, BatchInsertMetricsSnapshots
>;

// ── DB Writer ────────────────────────────────────────────────────────────

/// Configuration for the DB writer.
struct DBWriterConfig {
    std::size_t queue_capacity = 512;
    std::size_t max_batch_size = 64;
    std::chrono::milliseconds batch_timeout{50};
};

/// Async batch database writer.
///
/// Thread safety:
///   - enqueue() is thread-safe (called from runner threads, pipeline).
///   - start()/flush()/shutdown are called from a single thread.
class DBWriter {
public:
    /// Construct with a database and configuration.
    explicit DBWriter(SQLite::Database& db,
                      DBWriterConfig config = {});

    ~DBWriter();

    /// Start the writer thread.
    void start(std::stop_token stop);

    /// Enqueue a write request. Blocks if queue is full (backpressure).
    /// @return true if enqueued, false on timeout or closed.
    bool enqueue(DBWriteRequest req,
                 std::chrono::milliseconds timeout =
                     std::chrono::milliseconds(1000));

    /// Flush all pending writes (blocking). Used during shutdown.
    void flush();

    /// @return Number of pending (unprocessed) write requests.
    [[nodiscard]] std::size_t pending_count() const;

    /// @return Total number of write requests processed.
    [[nodiscard]] int64_t total_writes() const;

private:
    void writer_loop(std::stop_token stop);
    void process_batch(std::vector<DBWriteRequest>& batch);
    void execute_request(const DBWriteRequest& req);
    void prepare_statements();

    SQLite::Database& db_;
    DBWriterConfig config_;
    core::BoundedQueue<DBWriteRequest> queue_;
    std::jthread writer_thread_;
    std::atomic<int64_t> total_writes_{0};

    // Pre-compiled prepared statements.
    std::unique_ptr<SQLite::Statement> stmt_insert_run_;
    std::unique_ptr<SQLite::Statement> stmt_update_run_;
    std::unique_ptr<SQLite::Statement> stmt_insert_job_run_;
    std::unique_ptr<SQLite::Statement> stmt_update_job_run_;
    std::unique_ptr<SQLite::Statement> stmt_insert_step_run_;
    std::unique_ptr<SQLite::Statement> stmt_update_step_;
    std::unique_ptr<SQLite::Statement> stmt_insert_log_chunk_;
    std::unique_ptr<SQLite::Statement> stmt_insert_trigger_;
    std::unique_ptr<SQLite::Statement> stmt_insert_watch_sample_;
    std::unique_ptr<SQLite::Statement> stmt_insert_watch_event_;
    std::unique_ptr<SQLite::Statement> stmt_insert_metrics_snapshot_;
};

}  // namespace kairos::persist
