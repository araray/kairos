/// src/persist/db_writer.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  DBWriter implementation — async batch writer with backpressure          ║
// ║  Spec reference: §16.5                                                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/db_writer.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace kairos::persist {

DBWriter::DBWriter(SQLite::Database& db, DBWriterConfig config)
    : db_(db)
    , config_(config)
    , queue_(config.queue_capacity) {
    prepare_statements();
}

DBWriter::~DBWriter() {
    // Close the queue so the writer thread's pop() unblocks.
    queue_.close();

    // Join the writer thread first — it performs its own final flush
    // in writer_loop(). We must wait for that to complete before we
    // touch any shared state (prepared statements, database).
    if (writer_thread_.joinable()) {
        writer_thread_.request_stop();
        writer_thread_.join();
    }

    // Now safe: no other thread accesses the statements.
    // Drain anything enqueued after the writer thread's final flush
    // (e.g., items pushed between thread exit and queue_.close()).
    flush();
}

void DBWriter::prepare_statements() {
    stmt_insert_run_ = std::make_unique<SQLite::Statement>(db_,
        "INSERT INTO runs (run_id, target_type, target_id, target_name, "
        "trigger_type, trigger_id, correlation_id, status, start_ts) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");

    stmt_update_run_ = std::make_unique<SQLite::Statement>(db_,
        "UPDATE runs SET status = ?, end_ts = ?, exit_code = ?, "
        "duration_ms = ? WHERE run_id = ?");

    stmt_insert_job_run_ = std::make_unique<SQLite::Statement>(db_,
        "INSERT INTO job_runs (run_id, job_id, job_name, status, "
        "start_ts, condition_result) VALUES (?, ?, ?, ?, ?, ?)");

    stmt_update_job_run_ = std::make_unique<SQLite::Statement>(db_,
        "UPDATE job_runs SET status = ?, end_ts = ?, exit_code = ?, "
        "duration_ms = ? WHERE run_id = ? AND job_id = ?");

    stmt_insert_step_run_ = std::make_unique<SQLite::Statement>(db_,
        "INSERT INTO step_runs (run_id, job_id, step_id, step_name, "
        "command, status, start_ts) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)");

    stmt_update_step_ = std::make_unique<SQLite::Statement>(db_,
        "UPDATE step_runs SET status = ?, end_ts = ?, exit_code = ?, "
        "duration_ms = ? WHERE run_id = ? AND job_id = ? AND step_id = ?");

    stmt_insert_log_chunk_ = std::make_unique<SQLite::Statement>(db_,
        "INSERT INTO log_chunks (run_id, job_id, step_id, chunk_index, "
        "stream, content) VALUES (?, ?, ?, ?, ?, ?)");

    stmt_insert_trigger_ = std::make_unique<SQLite::Statement>(db_,
        "INSERT INTO trigger_history (trigger_id, trigger_type, "
        "target_id, fired_at, status, run_id) "
        "VALUES (?, ?, ?, ?, ?, ?)");

    stmt_insert_watch_sample_ = std::make_unique<SQLite::Statement>(db_,
        "INSERT INTO watch_samples (watch_group, sample_epoch, file_path, "
        "file_size, mtime, hash, scan_duration_ms) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)");

    stmt_insert_watch_event_ = std::make_unique<SQLite::Statement>(db_,
        "INSERT INTO watch_events (watch_group, event_type, file_path, "
        "rule_name, details_json, action_taken, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))");

    stmt_insert_metrics_snapshot_ = std::make_unique<SQLite::Statement>(db_,
        "INSERT INTO metrics_snapshots (metric_name, metric_type, value, "
        "labels_json) VALUES (?, ?, ?, ?)");
}

void DBWriter::start(std::stop_token stop) {
    writer_thread_ = std::jthread([this, stop](std::stop_token) {
        writer_loop(stop);
    });
    spdlog::info("DB writer started (queue_capacity={}, batch_size={})",
                 config_.queue_capacity, config_.max_batch_size);
}

bool DBWriter::enqueue(DBWriteRequest req,
                       std::chrono::milliseconds timeout) {
    return queue_.push(std::move(req), timeout);
}

void DBWriter::flush() {
    // Drain and process all remaining items.
    while (true) {
        auto batch = queue_.drain(config_.max_batch_size);
        if (batch.empty()) break;
        process_batch(batch);
    }
}

std::size_t DBWriter::pending_count() const {
    return queue_.size();
}

int64_t DBWriter::total_writes() const {
    return total_writes_.load(std::memory_order_relaxed);
}

void DBWriter::writer_loop(std::stop_token stop) {
    spdlog::debug("DB writer thread started");

    while (!stop.stop_requested()) {
        // Wait for the first item (with timeout).
        auto first = queue_.pop(config_.batch_timeout);
        if (!first.has_value()) {
            continue;  // Timeout — loop to check stop.
        }

        // Start a batch with the first item.
        std::vector<DBWriteRequest> batch;
        batch.push_back(std::move(*first));

        // Drain up to max_batch_size - 1 more items (non-blocking).
        auto more = queue_.drain(config_.max_batch_size - 1);
        batch.insert(batch.end(),
                     std::make_move_iterator(more.begin()),
                     std::make_move_iterator(more.end()));

        process_batch(batch);
    }

    // Final flush on shutdown.
    flush();
    spdlog::debug("DB writer thread stopped");
}

void DBWriter::process_batch(std::vector<DBWriteRequest>& batch) {
    if (batch.empty()) return;

    try {
        // Execute all requests in a single transaction.
        SQLite::Transaction txn(db_);

        for (auto& req : batch) {
            execute_request(req);
        }

        txn.commit();

        total_writes_.fetch_add(
            static_cast<int64_t>(batch.size()),
            std::memory_order_relaxed);

        spdlog::trace("DB writer committed batch of {} requests",
                      batch.size());
    } catch (const std::exception& e) {
        spdlog::error("DB writer batch commit failed: {}. "
                      "Batch of {} requests lost.",
                      e.what(), batch.size());
    }
}

void DBWriter::execute_request(const DBWriteRequest& req) {
    std::visit([this](const auto& r) {
        using T = std::decay_t<decltype(r)>;

        if constexpr (std::is_same_v<T, InsertRun>) {
            stmt_insert_run_->reset();
            stmt_insert_run_->bind(1, r.run_id);
            stmt_insert_run_->bind(2, r.target_type);
            stmt_insert_run_->bind(3, r.target_id);
            stmt_insert_run_->bind(4, r.target_name);
            stmt_insert_run_->bind(5, r.trigger_type);
            stmt_insert_run_->bind(6, r.trigger_id);
            stmt_insert_run_->bind(7, r.correlation_id);
            stmt_insert_run_->bind(8, r.status);
            stmt_insert_run_->bind(9, r.start_ts);
            stmt_insert_run_->exec();

        } else if constexpr (std::is_same_v<T, UpdateRunComplete>) {
            stmt_update_run_->reset();
            stmt_update_run_->bind(1, r.status);
            stmt_update_run_->bind(2, r.end_ts);
            stmt_update_run_->bind(3, r.exit_code);
            stmt_update_run_->bind(4, r.duration_ms);
            stmt_update_run_->bind(5, r.run_id);
            stmt_update_run_->exec();

        } else if constexpr (std::is_same_v<T, InsertJobRun>) {
            stmt_insert_job_run_->reset();
            stmt_insert_job_run_->bind(1, r.run_id);
            stmt_insert_job_run_->bind(2, r.job_id);
            stmt_insert_job_run_->bind(3, r.job_name);
            stmt_insert_job_run_->bind(4, r.status);
            stmt_insert_job_run_->bind(5, r.start_ts);
            stmt_insert_job_run_->bind(6, r.condition_result);
            stmt_insert_job_run_->exec();

        } else if constexpr (std::is_same_v<T, UpdateJobRunComplete>) {
            stmt_update_job_run_->reset();
            stmt_update_job_run_->bind(1, r.status);
            stmt_update_job_run_->bind(2, r.end_ts);
            stmt_update_job_run_->bind(3, r.exit_code);
            stmt_update_job_run_->bind(4, r.duration_ms);
            stmt_update_job_run_->bind(5, r.run_id);
            stmt_update_job_run_->bind(6, r.job_id);
            stmt_update_job_run_->exec();

        } else if constexpr (std::is_same_v<T, InsertStepRun>) {
            stmt_insert_step_run_->reset();
            stmt_insert_step_run_->bind(1, r.run_id);
            stmt_insert_step_run_->bind(2, r.job_id);
            stmt_insert_step_run_->bind(3, r.step_id);
            stmt_insert_step_run_->bind(4, r.step_name);
            stmt_insert_step_run_->bind(5, r.command);
            stmt_insert_step_run_->bind(6, r.status);
            stmt_insert_step_run_->bind(7, r.start_ts);
            stmt_insert_step_run_->exec();

        } else if constexpr (std::is_same_v<T, UpdateStepComplete>) {
            stmt_update_step_->reset();
            stmt_update_step_->bind(1, r.status);
            stmt_update_step_->bind(2, r.end_ts);
            stmt_update_step_->bind(3, r.exit_code);
            stmt_update_step_->bind(4, r.duration_ms);
            stmt_update_step_->bind(5, r.run_id);
            stmt_update_step_->bind(6, r.job_id);
            stmt_update_step_->bind(7, r.step_id);
            stmt_update_step_->exec();

        } else if constexpr (std::is_same_v<T, InsertLogChunk>) {
            stmt_insert_log_chunk_->reset();
            stmt_insert_log_chunk_->bind(1, r.run_id);
            stmt_insert_log_chunk_->bind(2, r.job_id);
            stmt_insert_log_chunk_->bind(3, r.step_id);
            stmt_insert_log_chunk_->bind(4, r.chunk_index);
            stmt_insert_log_chunk_->bind(5, r.stream);
            stmt_insert_log_chunk_->bind(6, r.content);
            stmt_insert_log_chunk_->exec();

        } else if constexpr (std::is_same_v<T, InsertTriggerHistory>) {
            stmt_insert_trigger_->reset();
            stmt_insert_trigger_->bind(1, r.trigger_id);
            stmt_insert_trigger_->bind(2, r.trigger_type);
            stmt_insert_trigger_->bind(3, r.target_id);
            stmt_insert_trigger_->bind(4, r.fired_at);
            stmt_insert_trigger_->bind(5, r.status);
            stmt_insert_trigger_->bind(6, r.run_id);
            stmt_insert_trigger_->exec();

        } else if constexpr (std::is_same_v<T, InsertWatchSample>) {
            stmt_insert_watch_sample_->reset();
            stmt_insert_watch_sample_->bind(1, r.watch_group);
            stmt_insert_watch_sample_->bind(2, r.sample_epoch);
            stmt_insert_watch_sample_->bind(3, r.file_path);
            stmt_insert_watch_sample_->bind(4, r.size);
            stmt_insert_watch_sample_->bind(5, r.mtime);
            stmt_insert_watch_sample_->bind(6, r.hash);
            stmt_insert_watch_sample_->bind(7, r.scan_duration_ms);
            stmt_insert_watch_sample_->exec();

        } else if constexpr (std::is_same_v<T, InsertWatchEvent>) {
            stmt_insert_watch_event_->reset();
            stmt_insert_watch_event_->bind(1, r.watch_group);
            stmt_insert_watch_event_->bind(2, r.event_type);
            stmt_insert_watch_event_->bind(3, r.affected_files_json);
            stmt_insert_watch_event_->bind(4, r.rule_name);
            stmt_insert_watch_event_->bind(5, r.details_json);
            stmt_insert_watch_event_->bind(6, r.severity);
            stmt_insert_watch_event_->exec();

        } else if constexpr (std::is_same_v<T, PruneOlderThan>) {
            // Prune runs older than cutoff.
            SQLite::Statement prune_runs(db_,
                "DELETE FROM runs WHERE start_ts < ?");
            prune_runs.bind(1, r.cutoff_date);
            int deleted = prune_runs.exec();
            spdlog::info("Pruned {} runs older than {}",
                         deleted, r.cutoff_date);

        } else if constexpr (std::is_same_v<T, BatchInsertWatchSamples>) {
            // Batch insert: all entries for one scan epoch in the
            // enclosing transaction. This is the optimized path
            // from §16.5 — avoids per-file enqueue overhead.
            for (const auto& entry : r.entries) {
                stmt_insert_watch_sample_->reset();
                stmt_insert_watch_sample_->bind(1, r.watch_group);
                stmt_insert_watch_sample_->bind(2, r.sample_epoch);
                stmt_insert_watch_sample_->bind(3, entry.file_path);
                stmt_insert_watch_sample_->bind(4, entry.size);
                stmt_insert_watch_sample_->bind(5, entry.mtime);
                stmt_insert_watch_sample_->bind(6, entry.hash);
                stmt_insert_watch_sample_->bind(7, r.scan_duration_ms);
                stmt_insert_watch_sample_->exec();
            }
            spdlog::trace("Batch-inserted {} sample entries for group '{}' "
                          "epoch {}",
                          r.entries.size(), r.watch_group, r.sample_epoch);

        } else if constexpr (std::is_same_v<T, PruneWatchSamples>) {
            // Prune old sample epochs, keeping the most recent N
            // per group. §16.8 retention policy for watch samples.
            //
            // Strategy: find the Nth most-recent distinct epoch for
            // this group. Delete all epochs older than that.
            SQLite::Statement find_cutoff(db_,
                "SELECT sample_epoch FROM watch_samples "
                "WHERE watch_group = ? "
                "GROUP BY sample_epoch "
                "ORDER BY sample_epoch DESC "
                "LIMIT 1 OFFSET ?");
            find_cutoff.bind(1, r.watch_group);
            find_cutoff.bind(2, r.max_epochs - 1);

            if (find_cutoff.executeStep()) {
                int64_t cutoff_epoch = find_cutoff.getColumn(0).getInt64();
                SQLite::Statement prune(db_,
                    "DELETE FROM watch_samples "
                    "WHERE watch_group = ? AND sample_epoch < ?");
                prune.bind(1, r.watch_group);
                prune.bind(2, cutoff_epoch);
                int deleted = prune.exec();
                if (deleted > 0) {
                    spdlog::info("Pruned {} watch sample rows for group '{}' "
                                 "(kept {} epochs)",
                                 deleted, r.watch_group, r.max_epochs);
                }
            }

        } else if constexpr (std::is_same_v<T, InsertMetricsSnapshot>) {
            // Single metric snapshot row (§20.5).
            stmt_insert_metrics_snapshot_->reset();
            stmt_insert_metrics_snapshot_->bind(1, r.metric_name);
            stmt_insert_metrics_snapshot_->bind(2, r.metric_type);
            stmt_insert_metrics_snapshot_->bind(3, r.value);
            stmt_insert_metrics_snapshot_->bind(4, r.labels_json);
            stmt_insert_metrics_snapshot_->exec();

        } else if constexpr (std::is_same_v<T, BatchInsertMetricsSnapshots>) {
            // Batch insert all metric values as a snapshot epoch (§20.5).
            for (const auto& entry : r.entries) {
                stmt_insert_metrics_snapshot_->reset();
                stmt_insert_metrics_snapshot_->bind(1, entry.metric_name);
                stmt_insert_metrics_snapshot_->bind(2, entry.metric_type);
                stmt_insert_metrics_snapshot_->bind(3, entry.value);
                stmt_insert_metrics_snapshot_->bind(4, entry.labels_json);
                stmt_insert_metrics_snapshot_->exec();
            }
            spdlog::trace("Batch-inserted {} metrics snapshot entries",
                          r.entries.size());

        } else if constexpr (std::is_same_v<T, PruneMetricsSnapshots>) {
            // Prune metrics snapshots older than retention_days (§16.8).
            // Default: 7 days (hardcoded in spec).
            SQLite::Statement prune(db_,
                "DELETE FROM metrics_snapshots "
                "WHERE recorded_at < datetime('now', '-' || ? || ' days')");
            prune.bind(1, r.retention_days);
            int deleted = prune.exec();
            if (deleted > 0) {
                spdlog::info("Pruned {} metrics snapshot rows older than {} days",
                             deleted, r.retention_days);
            }
        }
    }, req);
}

}  // namespace kairos::persist
