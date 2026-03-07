// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  migration.cpp — Schema migration engine and initial schema               ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/migration.hpp"

#include <stdexcept>

namespace kairos::persist {

// ═══════════════════════════════════════════════════════════════════════════
// Migration registry
// ═══════════════════════════════════════════════════════════════════════════

static const std::vector<Migration> kMigrations = {
    {
        1,
        "Initial Kairos schema",
        R"SQL(
-- ╔════════════════════════════════════════════════════════════════╗
-- ║  SCHEMA METADATA                                              ║
-- ╚════════════════════════════════════════════════════════════════╝

CREATE TABLE IF NOT EXISTS schema_version (
    version     INTEGER NOT NULL,
    applied_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    description TEXT    NOT NULL
);

-- ╔════════════════════════════════════════════════════════════════╗
-- ║  RUNS — Workflow and standalone job executions                 ║
-- ╚════════════════════════════════════════════════════════════════╝

CREATE TABLE IF NOT EXISTS runs (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    run_id          TEXT    NOT NULL UNIQUE,
    target_type     TEXT    NOT NULL,
    target_id       TEXT    NOT NULL,
    target_name     TEXT    NOT NULL,
    trigger_type    TEXT    NOT NULL,
    trigger_id      TEXT    NOT NULL,
    correlation_id  TEXT    NOT NULL,
    status          TEXT    NOT NULL DEFAULT 'PENDING',
    exit_code       INTEGER,
    start_ts        TEXT    NOT NULL,
    end_ts          TEXT,
    duration_ms     INTEGER,
    plan_json       TEXT,
    error_message   TEXT,
    created_at      TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_runs_target_status
    ON runs (target_id, status, end_ts DESC);
CREATE INDEX IF NOT EXISTS idx_runs_target_end
    ON runs (target_id, end_ts DESC);
CREATE INDEX IF NOT EXISTS idx_runs_created
    ON runs (created_at);
CREATE INDEX IF NOT EXISTS idx_runs_correlation
    ON runs (correlation_id);

-- ╔════════════════════════════════════════════════════════════════╗
-- ║  JOB_RUNS — Per-job results within a workflow run             ║
-- ╚════════════════════════════════════════════════════════════════╝

CREATE TABLE IF NOT EXISTS job_runs (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    run_id          TEXT    NOT NULL REFERENCES runs(run_id),
    job_id          TEXT    NOT NULL,
    job_name        TEXT    NOT NULL,
    status          TEXT    NOT NULL DEFAULT 'PENDING',
    exit_code       INTEGER,
    start_ts        TEXT,
    end_ts          TEXT,
    duration_ms     INTEGER,
    runner_type     TEXT,
    condition_expr  TEXT,
    condition_result TEXT,
    error_message   TEXT,
    details_json    TEXT,
    created_at      TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_job_runs_run
    ON job_runs (run_id);
CREATE INDEX IF NOT EXISTS idx_job_runs_job_status
    ON job_runs (job_id, status, end_ts DESC);

-- ╔════════════════════════════════════════════════════════════════╗
-- ║  STEP_RUNS — Per-step results within a job run                ║
-- ╚════════════════════════════════════════════════════════════════╝

CREATE TABLE IF NOT EXISTS step_runs (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    run_id          TEXT    NOT NULL,
    job_id          TEXT    NOT NULL,
    step_id         TEXT    NOT NULL,
    step_name       TEXT    NOT NULL,
    status          TEXT    NOT NULL DEFAULT 'PENDING',
    exit_code       INTEGER,
    start_ts        TEXT,
    end_ts          TEXT,
    duration_ms     INTEGER,
    command         TEXT,
    error_message   TEXT,
    created_at      TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_step_runs_run_job
    ON step_runs (run_id, job_id);

-- ╔════════════════════════════════════════════════════════════════╗
-- ║  LOG_CHUNKS — Captured stdout/stderr from step executions     ║
-- ╚════════════════════════════════════════════════════════════════╝

CREATE TABLE IF NOT EXISTS log_chunks (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    run_id          TEXT    NOT NULL,
    job_id          TEXT    NOT NULL,
    step_id         TEXT    NOT NULL DEFAULT '',
    stream          TEXT    NOT NULL DEFAULT 'stdout',
    chunk_index     INTEGER NOT NULL DEFAULT 0,
    content         TEXT    NOT NULL,
    created_at      TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_log_chunks_run
    ON log_chunks (run_id, job_id, step_id, chunk_index);

-- ╔════════════════════════════════════════════════════════════════╗
-- ║  WATCH_SAMPLES — Filesystem snapshot samples                  ║
-- ╚════════════════════════════════════════════════════════════════╝

CREATE TABLE IF NOT EXISTS watch_samples (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    watch_group     TEXT    NOT NULL,
    sample_epoch    INTEGER NOT NULL,
    file_path       TEXT    NOT NULL,
    file_size       INTEGER,
    mtime           TEXT,
    hash            TEXT,
    scan_duration_ms INTEGER,
    created_at      TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_samples_group_epoch
    ON watch_samples (watch_group, sample_epoch DESC);
CREATE INDEX IF NOT EXISTS idx_samples_group_path
    ON watch_samples (watch_group, file_path, sample_epoch);

-- ╔════════════════════════════════════════════════════════════════╗
-- ║  WATCH_EVENTS — Detected file change events                   ║
-- ╚════════════════════════════════════════════════════════════════╝

CREATE TABLE IF NOT EXISTS watch_events (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    watch_group     TEXT    NOT NULL,
    event_type      TEXT    NOT NULL,
    file_path       TEXT    NOT NULL,
    old_hash        TEXT,
    new_hash        TEXT,
    old_size        INTEGER,
    new_size        INTEGER,
    rule_name       TEXT,
    action_taken    TEXT,
    details_json    TEXT,
    created_at      TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_events_group
    ON watch_events (watch_group, created_at DESC);

-- ╔════════════════════════════════════════════════════════════════╗
-- ║  TRIGGER_HISTORY — Record of all trigger fires                ║
-- ╚════════════════════════════════════════════════════════════════╝

CREATE TABLE IF NOT EXISTS trigger_history (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    trigger_id      TEXT    NOT NULL,
    trigger_type    TEXT    NOT NULL,
    target_id       TEXT    NOT NULL,
    fired_at        TEXT    NOT NULL,
    run_id          TEXT,
    status          TEXT    NOT NULL DEFAULT 'fired',
    details_json    TEXT,
    created_at      TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_trigger_history_trigger
    ON trigger_history (trigger_id, fired_at DESC);

-- ╔════════════════════════════════════════════════════════════════╗
-- ║  METRICS_SNAPSHOTS — Periodic metrics persistence             ║
-- ╚════════════════════════════════════════════════════════════════╝

CREATE TABLE IF NOT EXISTS metrics_snapshots (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    metric_name     TEXT    NOT NULL,
    metric_type     TEXT    NOT NULL,
    value           REAL    NOT NULL,
    labels_json     TEXT,
    created_at      TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_metrics_name
    ON metrics_snapshots (metric_name, created_at DESC);

-- ╔════════════════════════════════════════════════════════════════╗
-- ║  CONFIG_SNAPSHOTS — Config reload history                     ║
-- ╚════════════════════════════════════════════════════════════════╝

CREATE TABLE IF NOT EXISTS config_snapshots (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    config_hash     TEXT    NOT NULL,
    config_json     TEXT    NOT NULL,
    source          TEXT    NOT NULL DEFAULT 'reload',
    created_at      TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);
        )SQL"
    }
};

const std::vector<Migration>& get_migrations() {
    return kMigrations;
}

// ═══════════════════════════════════════════════════════════════════════════
// Schema version query
// ═══════════════════════════════════════════════════════════════════════════

int get_schema_version(SQLite::Database& db) {
    try {
        SQLite::Statement query(db,
            "SELECT MAX(version) FROM schema_version");
        if (query.executeStep()) {
            return query.getColumn(0).getInt();
        }
    } catch (...) {
        // Table doesn't exist yet → version 0.
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Migration engine
// ═══════════════════════════════════════════════════════════════════════════

int apply_migrations(SQLite::Database& db,
                     const std::vector<Migration>& migrations) {
    int current = get_schema_version(db);

    for (const auto& m : migrations) {
        if (m.version <= current) continue;

        // Run each migration in a transaction.
        SQLite::Transaction txn(db);
        try {
            db.exec(m.sql);

            // Record the migration in schema_version.
            // (The table is created by migration 1 itself, so this
            //  insert works for all migrations including the first.)
            SQLite::Statement insert(db,
                "INSERT INTO schema_version (version, description) VALUES (?, ?)");
            insert.bind(1, m.version);
            insert.bind(2, m.description);
            insert.exec();

            txn.commit();
            current = m.version;
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "Migration " + std::to_string(m.version) +
                " (" + m.description + ") failed: " + e.what());
        }
    }

    return current;
}

}  // namespace kairos::persist
