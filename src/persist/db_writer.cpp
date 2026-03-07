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
    // Flush remaining items before destruction.
    flush();
    if (writer_thread_.joinable()) {
        writer_thread_.request_stop();
    }
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

        } else if constexpr (std::is_same_v<T, PruneOlderThan>) {
            // Prune runs older than cutoff.
            SQLite::Statement prune_runs(db_,
                "DELETE FROM runs WHERE start_ts < ?");
            prune_runs.bind(1, r.cutoff_date);
            int deleted = prune_runs.exec();
            spdlog::info("Pruned {} runs older than {}",
                         deleted, r.cutoff_date);
        }
    }, req);
}

}  // namespace kairos::persist
