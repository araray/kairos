/// src/persist/query_reader.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  QueryReader implementation — run history queries for KEL               ║
// ║  Spec reference: §16.6, §7.7–7.8                                       ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/query_reader.hpp"
#include "kairos/kel/errors.hpp"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace kairos::persist {

// ── Utility: format a time_point as ISO-8601 ─────────────────────────────

static std::string format_iso8601(
    std::chrono::system_clock::time_point tp) {
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &time_t_val);
#else
    gmtime_r(&time_t_val, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

// ── QueryReader ──────────────────────────────────────────────────────────

QueryReader::QueryReader(SQLite::Database& db) : db_(db) {}

bool QueryReader::last_success(const std::string& job_name) const {
    // Query the most recent run for this job name.
    // We look at job_runs joined with runs, ordered by started_at desc.
    SQLite::Statement stmt(db_,
        "SELECT jr.status FROM job_runs jr "
        "JOIN runs r ON r.run_id = jr.run_id "
        "WHERE jr.job_name = ? "
        "ORDER BY jr.start_ts DESC LIMIT 1");
    stmt.bind(1, job_name);

    if (stmt.executeStep()) {
        std::string status = stmt.getColumn(0).getString();
        return status == "SUCCESS";
    }
    return false;  // Never run.
}

std::string QueryReader::last_status(const std::string& job_name) const {
    SQLite::Statement stmt(db_,
        "SELECT jr.status FROM job_runs jr "
        "JOIN runs r ON r.run_id = jr.run_id "
        "WHERE jr.job_name = ? "
        "ORDER BY jr.start_ts DESC LIMIT 1");
    stmt.bind(1, job_name);

    if (stmt.executeStep()) {
        return stmt.getColumn(0).getString();
    }
    return "NEVER_RUN";
}

int QueryReader::last_exit_code(const std::string& job_name) const {
    SQLite::Statement stmt(db_,
        "SELECT jr.exit_code FROM job_runs jr "
        "JOIN runs r ON r.run_id = jr.run_id "
        "WHERE jr.job_name = ? "
        "ORDER BY jr.start_ts DESC LIMIT 1");
    stmt.bind(1, job_name);

    if (stmt.executeStep()) {
        return stmt.getColumn(0).getInt();
    }
    return -1;  // Never run.
}

bool QueryReader::finished_within(
    const std::string& job_name,
    std::chrono::milliseconds window,
    std::chrono::system_clock::time_point reference_time) const {
    auto cutoff = reference_time - window;
    std::string cutoff_str = format_iso8601(cutoff);

    SQLite::Statement stmt(db_,
        "SELECT COUNT(*) FROM job_runs jr "
        "JOIN runs r ON r.run_id = jr.run_id "
        "WHERE jr.job_name = ? AND jr.status = 'SUCCESS' "
        "AND jr.end_ts >= ?");
    stmt.bind(1, job_name);
    stmt.bind(2, cutoff_str);

    if (stmt.executeStep()) {
        return stmt.getColumn(0).getInt64() > 0;
    }
    return false;
}

bool QueryReader::has_run(const std::string& job_name) const {
    SQLite::Statement stmt(db_,
        "SELECT COUNT(*) FROM job_runs WHERE job_name = ?");
    stmt.bind(1, job_name);

    if (stmt.executeStep()) {
        return stmt.getColumn(0).getInt64() > 0;
    }
    return false;
}

int64_t QueryReader::run_count(const std::string& job_name) const {
    SQLite::Statement stmt(db_,
        "SELECT COUNT(*) FROM job_runs WHERE job_name = ?");
    stmt.bind(1, job_name);

    if (stmt.executeStep()) {
        return stmt.getColumn(0).getInt64();
    }
    return 0;
}

double QueryReader::success_rate(const std::string& job_name) const {
    SQLite::Statement stmt(db_,
        "SELECT "
        "  COALESCE(SUM(CASE WHEN status = 'SUCCESS' THEN 1 ELSE 0 END), 0),"
        "  COUNT(*) "
        "FROM job_runs WHERE job_name = ?");
    stmt.bind(1, job_name);

    if (stmt.executeStep()) {
        int64_t successes = stmt.getColumn(0).getInt64();
        int64_t total = stmt.getColumn(1).getInt64();
        if (total == 0) return 0.0;
        return static_cast<double>(successes) / static_cast<double>(total);
    }
    return 0.0;
}

// ── KEL integration ──────────────────────────────────────────────────────

void QueryReader::register_kel_bindings(
    kairos::kel::EvalContext& ctx,
    std::chrono::system_clock::time_point reference_time) const {

    // Register job() as a free function that returns a string reference.
    // The returned string is a marker: "job_ref:<job_name>"
    // Members and methods resolve this marker via the QueryReader.
    ctx.functions["job"] = [](const std::vector<kairos::kel::KelValue>& args)
        -> kairos::kel::KelValue {
        if (args.empty() || !std::holds_alternative<std::string>(args[0].data)) {
            throw std::runtime_error(
                "job() requires a string argument (job name)");
        }
        return kairos::kel::KelValue(
            std::string("job_ref:") +
            std::get<std::string>(args[0].data));
    };

    // Register now() — captured once per pipeline run for determinism.
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        reference_time.time_since_epoch()).count();
    ctx.functions["now"] = [now_ms](const std::vector<kairos::kel::KelValue>&)
        -> kairos::kel::KelValue {
        return kairos::kel::KelValue(static_cast<int64_t>(now_ms));
    };

    // Helper: extract job name from a "job_ref:name" string.
    auto extract_job_name = [](const kairos::kel::KelValue& val)
        -> std::string {
        if (!std::holds_alternative<std::string>(val.data)) {
            throw std::runtime_error(
                "Expected a job reference (call job(\"name\") first)");
        }
        const auto& ref = std::get<std::string>(val.data);
        const std::string prefix = "job_ref:";
        if (ref.substr(0, prefix.size()) != prefix) {
            throw std::runtime_error(
                "Expected a job reference, got: " + ref);
        }
        return ref.substr(prefix.size());
    };

    // ── Members: job("id").last_success, .last_status, etc. ──────

    // last_success → bool
    ctx.members["string.last_success"] =
        [this, extract_job_name](const kairos::kel::KelValue& obj)
            -> kairos::kel::KelValue {
        return kairos::kel::KelValue(last_success(extract_job_name(obj)));
    };

    // last_status → string
    ctx.members["string.last_status"] =
        [this, extract_job_name](const kairos::kel::KelValue& obj)
            -> kairos::kel::KelValue {
        return kairos::kel::KelValue(last_status(extract_job_name(obj)));
    };

    // last_exit_code → int
    ctx.members["string.last_exit_code"] =
        [this, extract_job_name](const kairos::kel::KelValue& obj)
            -> kairos::kel::KelValue {
        return kairos::kel::KelValue(
            static_cast<int64_t>(last_exit_code(extract_job_name(obj))));
    };

    // has_run → bool
    ctx.members["string.has_run"] =
        [this, extract_job_name](const kairos::kel::KelValue& obj)
            -> kairos::kel::KelValue {
        return kairos::kel::KelValue(has_run(extract_job_name(obj)));
    };

    // run_count → int
    ctx.members["string.run_count"] =
        [this, extract_job_name](const kairos::kel::KelValue& obj)
            -> kairos::kel::KelValue {
        return kairos::kel::KelValue(run_count(extract_job_name(obj)));
    };

    // success_rate → float
    ctx.members["string.success_rate"] =
        [this, extract_job_name](const kairos::kel::KelValue& obj)
            -> kairos::kel::KelValue {
        return kairos::kel::KelValue(success_rate(extract_job_name(obj)));
    };

    // last_run_at → duration (time elapsed since last run)
    ctx.members["string.last_run_at"] =
        [this, extract_job_name, reference_time](
            const kairos::kel::KelValue& obj) -> kairos::kel::KelValue {
        auto job_name = extract_job_name(obj);
        // Query the start_ts of the most recent run for this job.
        SQLite::Statement stmt(db_,
            "SELECT jr.start_ts FROM job_runs jr "
            "WHERE jr.job_name = ? "
            "ORDER BY jr.start_ts DESC LIMIT 1");
        stmt.bind(1, job_name);
        if (stmt.executeStep()) {
            auto ts_str = stmt.getColumn(0).getString();
            // Parse ISO 8601 timestamp to time_point.
            std::tm tm{};
            std::istringstream ss(ts_str);
            ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
            if (!ss.fail()) {
                auto tp = std::chrono::system_clock::from_time_t(
                    std::mktime(&tm));
                auto elapsed = std::chrono::duration_cast<
                    std::chrono::milliseconds>(reference_time - tp);
                return kairos::kel::KelValue(elapsed);
            }
        }
        // Never run — return a very large duration.
        return kairos::kel::KelValue(
            std::chrono::milliseconds(
                std::chrono::hours(24 * 365 * 100)));
    };

    // ── Methods: job("id").finished_within(24h) ──────────────────

    ctx.methods["string.finished_within"] =
        [this, extract_job_name, reference_time](
            const kairos::kel::KelValue& obj,
            const std::vector<kairos::kel::KelValue>& args)
                -> kairos::kel::KelValue {
        if (args.empty()) {
            throw std::runtime_error(
                "finished_within() requires a duration argument");
        }

        // The argument should be a duration (milliseconds).
        std::chrono::milliseconds window;
        if (std::holds_alternative<std::chrono::milliseconds>(args[0].data)) {
            window = std::get<std::chrono::milliseconds>(args[0].data);
        } else if (std::holds_alternative<int64_t>(args[0].data)) {
            // Interpret as milliseconds.
            window = std::chrono::milliseconds(
                std::get<int64_t>(args[0].data));
        } else {
            throw std::runtime_error(
                "finished_within() requires a duration or integer argument");
        }

        return kairos::kel::KelValue(
            finished_within(extract_job_name(obj), window, reference_time));
    };
}

// ═══════════════════════════════════════════════════════════════════════════
// Watch sample queries for aggregate()/previous() — spec §7.7 category 3
// ═══════════════════════════════════════════════════════════════════════════

int64_t QueryReader::last_sample_epoch(
    const std::string& group_name) const
{
    SQLite::Statement query(db_,
        "SELECT MAX(sample_epoch) FROM watch_samples WHERE watch_group = ?");
    query.bind(1, group_name);
    if (query.executeStep()) {
        if (!query.isColumnNull(0)) {
            return query.getColumn(0).getInt64();
        }
    }
    return -1;  // No samples exist.
}

std::vector<QueryReader::SampleFileRow> QueryReader::query_sample(
    const std::string& group_name, int64_t epoch) const
{
    std::vector<SampleFileRow> rows;

    SQLite::Statement query(db_,
        "SELECT file_path, file_size, mtime, hash "
        "FROM watch_samples "
        "WHERE watch_group = ? AND sample_epoch = ?");
    query.bind(1, group_name);
    query.bind(2, epoch);

    while (query.executeStep()) {
        SampleFileRow row;
        row.file_path = query.getColumn(0).getString();
        row.size = query.getColumn(1).isNull()
            ? 0 : query.getColumn(1).getInt64();
        row.mtime = query.getColumn(2).isNull()
            ? "" : query.getColumn(2).getString();
        row.hash = query.getColumn(3).isNull()
            ? "" : query.getColumn(3).getString();
        rows.push_back(std::move(row));
    }

    return rows;
}

// ── Portable glob matching ──────────────────────────────────────────────
// Simple glob for patterns: * matches any chars, ? matches one char.
// No ** or character classes in v1 (per spec §12.6).

namespace {

bool glob_match(const std::string& pattern, const std::string& str) {
    size_t pi = 0, si = 0;
    size_t star_pi = std::string::npos, star_si = 0;

    while (si < str.size()) {
        if (pi < pattern.size() && (pattern[pi] == str[si] || pattern[pi] == '?')) {
            ++pi;
            ++si;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            star_pi = pi;
            star_si = si;
            ++pi;
        } else if (star_pi != std::string::npos) {
            pi = star_pi + 1;
            ++star_si;
            si = star_si;
        } else {
            return false;
        }
    }

    while (pi < pattern.size() && pattern[pi] == '*') ++pi;
    return pi == pattern.size();
}

/// Extract just the filename from a path string.
std::string filename_from_path(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

/// Perform aggregation over a list of numeric values.
double aggregate_values(
    const std::vector<double>& values,
    const std::string& func)
{
    if (values.empty()) return 0.0;

    if (func == "sum") {
        double total = 0.0;
        for (double v : values) total += v;
        return total;
    }
    if (func == "count") {
        return static_cast<double>(values.size());
    }
    if (func == "min") {
        double m = values[0];
        for (size_t i = 1; i < values.size(); ++i)
            if (values[i] < m) m = values[i];
        return m;
    }
    if (func == "max") {
        double m = values[0];
        for (size_t i = 1; i < values.size(); ++i)
            if (values[i] > m) m = values[i];
        return m;
    }
    if (func == "avg") {
        double total = 0.0;
        for (double v : values) total += v;
        return total / static_cast<double>(values.size());
    }
    return 0.0;  // Unknown function.
}

}  // namespace

// ── register_watch_kel_bindings ─────────────────────────────────────────

void QueryReader::register_watch_kel_bindings(
    kairos::kel::EvalContext& ctx) const
{
    // ── aggregate(data, glob, metric, func) ─────────────────────────
    //
    // `data` is expected to be a list of KelValues, each representing
    // a file entry with members: path (string), size (int), hash (string),
    // pattern_found (bool).
    //
    // For simplicity in v1, the watch engine binds the sample data as
    // a special string sentinel "__sample_data__". The actual sample
    // reference is captured by value in the lambda (via the closure in
    // the caller's register call). Here we provide the function
    // infrastructure; the caller passes the sample data through the
    // context variable "data" as a KelValue list.
    //
    // Alternative approach (implemented): the watch engine registers
    // aggregate() as a closure that captures the current sample.
    // The `data` argument is accepted but not used (the captured
    // sample is authoritative). This preserves the spec's 4-arg syntax.

    // Note: aggregate() registration is done by the watch engine
    // (see WatchEngine::build_watch_kel_context_with_aggregate),
    // because it needs the current sample. Here we only provide
    // previous(), which queries SQLite.

    // ── previous(group, glob, metric) ───────────────────────────────
    //
    // Queries the most recent prior sample for the named watch group,
    // filters files matching glob, and returns the sum of the specified
    // metric. Returns 0 if no prior sample exists.
    //
    // This is the only KEL function that performs I/O during evaluation.
    // The evaluation timeout (default 100ms) bounds the total time.

    ctx.functions["previous"] =
        [this](const std::vector<kairos::kel::KelValue>& args)
            -> kairos::kel::KelValue
    {
        if (args.size() != 3) {
            throw kairos::kel::KelEvalError(
                "previous() requires 3 arguments: (group, glob, metric)");
        }
        if (!args[0].is_string() || !args[1].is_string() ||
            !args[2].is_string()) {
            throw kairos::kel::KelEvalError(
                "previous() arguments must be strings");
        }

        const auto& group = args[0].as_string();
        const auto& glob = args[1].as_string();
        const auto& metric = args[2].as_string();

        // Find the most recent sample epoch.
        int64_t epoch = last_sample_epoch(group);
        if (epoch < 0) {
            return kairos::kel::KelValue(int64_t(0));
        }

        // Query the sample data for that epoch.
        auto rows = query_sample(group, epoch);

        // Filter by glob and collect metric values.
        std::vector<double> values;
        for (const auto& row : rows) {
            auto fname = filename_from_path(row.file_path);
            if (!glob_match(glob, fname) && !glob_match(glob, row.file_path)) {
                continue;
            }

            if (metric == "size") {
                values.push_back(static_cast<double>(row.size));
            } else if (metric == "hash" || metric == "mtime") {
                // For string metrics, count non-empty values.
                const auto& val = (metric == "hash") ? row.hash : row.mtime;
                values.push_back(val.empty() ? 0.0 : 1.0);
            } else if (metric == "pattern_found") {
                // Not stored directly in sample rows; default to 0.
                values.push_back(0.0);
            } else {
                throw kairos::kel::KelEvalError(
                    "previous(): unknown metric '" + metric + "'");
            }
        }

        // Sum by default for previous() (matches EventWatcher behavior).
        double result = aggregate_values(values, "sum");

        // Return as int if it's a whole number, float otherwise.
        if (result == std::floor(result) &&
            result >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
            result <= static_cast<double>(std::numeric_limits<int64_t>::max())) {
            return kairos::kel::KelValue(static_cast<int64_t>(result));
        }
        return kairos::kel::KelValue(result);
    };
}

std::vector<QueryReader::WatchEventRow> QueryReader::query_watch_events(
    int limit, const std::string& watch_group) const
{
    std::vector<WatchEventRow> results;

    // The watch_events table uses:
    //   file_path → stores affected_files JSON
    //   action_taken → stores severity
    //   (no event_uid or sample_epoch columns in v1 schema)
    std::string sql =
        "SELECT id, watch_group, rule_name, event_type, action_taken, "
        "file_path, details_json, created_at "
        "FROM watch_events ";

    if (!watch_group.empty()) {
        sql += "WHERE watch_group = ? ";
    }
    sql += "ORDER BY created_at DESC LIMIT ?";

    SQLite::Statement query(db_, sql);

    int bind_idx = 1;
    if (!watch_group.empty()) {
        query.bind(bind_idx++, watch_group);
    }
    query.bind(bind_idx, limit);

    while (query.executeStep()) {
        WatchEventRow row;
        row.event_uid          = std::to_string(query.getColumn(0).getInt64());
        row.watch_group        = query.getColumn(1).getString();
        row.rule_name          = query.getColumn(2).getString();
        row.event_type         = query.getColumn(3).getString();
        row.severity           = query.getColumn(4).getString();
        row.affected_files_json= query.getColumn(5).getString();
        row.details_json       = query.getColumn(6).getString();
        row.created_at         = query.getColumn(7).getString();
        row.sample_epoch       = 0;  // Not stored in v1 schema.
        results.push_back(std::move(row));
    }

    return results;
}

// ── Run summary stats ──────────────────────────────────────────────────

QueryReader::RunStats QueryReader::query_run_stats() const {
    RunStats stats;

    // Total runs (all time).
    try {
        SQLite::Statement q1(db_,
            "SELECT COUNT(*) FROM runs");
        if (q1.executeStep()) {
            stats.total_runs = q1.getColumn(0).getInt64();
        }
    } catch (...) {
        // Table may not exist in a fresh DB — leave at 0.
    }

    // Runs in last 24h + failures in last 24h.
    try {
        SQLite::Statement q2(db_,
            "SELECT "
            "  COUNT(*), "
            "  SUM(CASE WHEN status = 'FAILURE' THEN 1 ELSE 0 END) "
            "FROM runs "
            "WHERE created_at >= datetime('now', '-1 day')");
        if (q2.executeStep()) {
            stats.runs_today = q2.getColumn(0).getInt64();
            stats.failures_today = q2.getColumn(1).getInt64();
        }
    } catch (...) {}

    // Currently running.
    try {
        SQLite::Statement q3(db_,
            "SELECT COUNT(*) FROM runs WHERE status = 'RUNNING'");
        if (q3.executeStep()) {
            stats.active_runs = q3.getColumn(0).getInt64();
        }
    } catch (...) {}

    return stats;
}

int64_t QueryReader::query_db_size(
    const std::filesystem::path& db_path)
{
    std::error_code ec;
    auto sz = std::filesystem::file_size(db_path, ec);
    if (ec) return 0;
    return static_cast<int64_t>(sz);
}

// ── CLI run history queries ───────────────────────────────────────────

std::vector<QueryReader::RunSummary> QueryReader::query_recent_runs(
    int limit,
    const std::string& status_filter,
    const std::string& target_filter,
    const std::string& since) const
{
    std::vector<RunSummary> results;

    // Build dynamic SQL with optional WHERE clauses.
    std::string sql =
        "SELECT run_id, target_type, target_id, target_name, "
        "trigger_type, status, COALESCE(exit_code, 0), "
        "start_ts, COALESCE(end_ts, ''), COALESCE(duration_ms, 0) "
        "FROM runs WHERE 1=1 ";

    if (!status_filter.empty()) {
        sql += "AND status = ? ";
    }
    if (!target_filter.empty()) {
        sql += "AND target_name LIKE ? ";
    }
    if (!since.empty()) {
        sql += "AND start_ts >= ? ";
    }
    sql += "ORDER BY start_ts DESC LIMIT ?";

    SQLite::Statement query(db_, sql);

    int bind_idx = 1;
    if (!status_filter.empty()) {
        query.bind(bind_idx++, status_filter);
    }
    if (!target_filter.empty()) {
        query.bind(bind_idx++, "%" + target_filter + "%");
    }
    if (!since.empty()) {
        query.bind(bind_idx++, since);
    }
    query.bind(bind_idx, limit);

    while (query.executeStep()) {
        RunSummary r;
        r.run_id       = query.getColumn(0).getString();
        r.target_type  = query.getColumn(1).getString();
        r.target_id    = query.getColumn(2).getString();
        r.target_name  = query.getColumn(3).getString();
        r.trigger_type = query.getColumn(4).getString();
        r.status       = query.getColumn(5).getString();
        r.exit_code    = query.getColumn(6).getInt();
        r.start_ts     = query.getColumn(7).getString();
        r.end_ts       = query.getColumn(8).getString();
        r.duration_ms  = query.getColumn(9).getInt64();
        results.push_back(std::move(r));
    }

    return results;
}

// ── Run ID prefix resolution (§1.1) ──────────────────────────────────────

QueryReader::PrefixResult QueryReader::resolve_run_id_prefix(
    const std::string& prefix) const
{
    PrefixResult result;

    if (prefix.empty()) {
        result.status = PrefixResult::kEmpty;
        return result;
    }

    // Full UUID length (run-XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX = 40 chars,
    // or just the UUID part = 36). If the input is already >= 36 chars,
    // treat as exact match to avoid LIKE overhead.
    if (prefix.size() >= 36) {
        SQLite::Statement query(db_,
            "SELECT run_id FROM runs WHERE run_id = ?");
        query.bind(1, prefix);
        if (query.executeStep()) {
            result.status = PrefixResult::kExact;
            result.resolved_id = query.getColumn(0).getString();
        } else {
            result.status = PrefixResult::kNotFound;
        }
        return result;
    }

    // Prefix search: LIKE prefix% (limited to 10 candidates for
    // ambiguity reporting).
    std::string like_pattern = prefix + "%";
    SQLite::Statement query(db_,
        "SELECT run_id FROM runs WHERE run_id LIKE ? "
        "ORDER BY start_ts DESC LIMIT 10");
    query.bind(1, like_pattern);

    std::vector<std::string> matches;
    while (query.executeStep()) {
        matches.push_back(query.getColumn(0).getString());
    }

    if (matches.empty()) {
        result.status = PrefixResult::kNotFound;
    } else if (matches.size() == 1) {
        result.status = PrefixResult::kUnique;
        result.resolved_id = matches[0];
    } else {
        result.status = PrefixResult::kAmbiguous;
        result.candidates = std::move(matches);
    }

    return result;
}

std::optional<QueryReader::RunSummary> QueryReader::get_run_summary(
    const std::string& run_id) const
{
    SQLite::Statement query(db_,
        "SELECT run_id, target_type, target_id, target_name, "
        "trigger_type, status, COALESCE(exit_code, 0), "
        "start_ts, COALESCE(end_ts, ''), COALESCE(duration_ms, 0) "
        "FROM runs WHERE run_id = ?");
    query.bind(1, run_id);

    if (query.executeStep()) {
        RunSummary r;
        r.run_id       = query.getColumn(0).getString();
        r.target_type  = query.getColumn(1).getString();
        r.target_id    = query.getColumn(2).getString();
        r.target_name  = query.getColumn(3).getString();
        r.trigger_type = query.getColumn(4).getString();
        r.status       = query.getColumn(5).getString();
        r.exit_code    = query.getColumn(6).getInt();
        r.start_ts     = query.getColumn(7).getString();
        r.end_ts       = query.getColumn(8).getString();
        r.duration_ms  = query.getColumn(9).getInt64();
        return r;
    }
    return std::nullopt;
}

std::optional<QueryReader::RunDetail> QueryReader::get_run_detail(
    const std::string& run_id) const
{
    // Get the run summary first.
    auto run_opt = get_run_summary(run_id);
    if (!run_opt) return std::nullopt;

    RunDetail detail;
    detail.run = std::move(*run_opt);

    // Fetch jobs for this run.
    SQLite::Statement job_query(db_,
        "SELECT job_id, job_name, status, COALESCE(exit_code, 0), "
        "COALESCE(start_ts, ''), COALESCE(end_ts, ''), "
        "COALESCE(duration_ms, 0), COALESCE(condition_result, '') "
        "FROM job_runs WHERE run_id = ? "
        "ORDER BY start_ts ASC");
    job_query.bind(1, run_id);

    while (job_query.executeStep()) {
        JobDetail job;
        job.job_id           = job_query.getColumn(0).getString();
        job.job_name         = job_query.getColumn(1).getString();
        job.status           = job_query.getColumn(2).getString();
        job.exit_code        = job_query.getColumn(3).getInt();
        job.start_ts         = job_query.getColumn(4).getString();
        job.end_ts           = job_query.getColumn(5).getString();
        job.duration_ms      = job_query.getColumn(6).getInt64();
        job.condition_result = job_query.getColumn(7).getString();

        // Fetch steps for this job.
        SQLite::Statement step_query(db_,
            "SELECT step_id, step_name, status, COALESCE(exit_code, 0), "
            "COALESCE(start_ts, ''), COALESCE(end_ts, ''), "
            "COALESCE(duration_ms, 0), COALESCE(command, '') "
            "FROM step_runs WHERE run_id = ? AND job_id = ? "
            "ORDER BY start_ts ASC");
        step_query.bind(1, run_id);
        step_query.bind(2, job.job_id);

        while (step_query.executeStep()) {
            StepDetail step;
            step.step_id     = step_query.getColumn(0).getString();
            step.step_name   = step_query.getColumn(1).getString();
            step.status      = step_query.getColumn(2).getString();
            step.exit_code   = step_query.getColumn(3).getInt();
            step.start_ts    = step_query.getColumn(4).getString();
            step.end_ts      = step_query.getColumn(5).getString();
            step.duration_ms = step_query.getColumn(6).getInt64();
            step.command     = step_query.getColumn(7).getString();
            job.steps.push_back(std::move(step));
        }

        detail.jobs.push_back(std::move(job));
    }

    return detail;
}

// ── Log chunk queries ─────────────────────────────────────────────────

std::vector<QueryReader::LogChunk> QueryReader::get_log_chunks(
    const std::string& run_id,
    int64_t after_id,
    int limit) const
{
    std::vector<LogChunk> results;

    SQLite::Statement query(db_,
        "SELECT id, run_id, job_id, step_id, stream, chunk_index, "
        "content, created_at "
        "FROM log_chunks WHERE run_id = ? AND id > ? "
        "ORDER BY id ASC LIMIT ?");
    query.bind(1, run_id);
    query.bind(2, after_id);
    query.bind(3, limit);

    while (query.executeStep()) {
        LogChunk chunk;
        chunk.id          = query.getColumn(0).getInt64();
        chunk.run_id      = query.getColumn(1).getString();
        chunk.job_id      = query.getColumn(2).getString();
        chunk.step_id     = query.getColumn(3).getString();
        chunk.stream      = query.getColumn(4).getString();
        chunk.chunk_index = query.getColumn(5).getInt64();
        chunk.content     = query.getColumn(6).getString();
        chunk.created_at  = query.getColumn(7).getString();
        results.push_back(std::move(chunk));
    }

    return results;
}

// ── Metrics snapshot queries ──────────────────────────────────────────

std::vector<QueryReader::MetricsSnapshotRow> QueryReader::query_metrics_snapshots(
    int limit) const
{
    std::vector<MetricsSnapshotRow> results;

    // Get the most recent `limit` snapshot epochs (distinct created_at values)
    // and return all entries in those epochs.
    SQLite::Statement query(db_,
        "SELECT metric_name, metric_type, value, "
        "COALESCE(labels_json, ''), created_at "
        "FROM metrics_snapshots "
        "WHERE created_at IN ("
        "  SELECT DISTINCT created_at FROM metrics_snapshots "
        "  ORDER BY created_at DESC LIMIT ?"
        ") ORDER BY created_at DESC, metric_name ASC");
    query.bind(1, limit);

    while (query.executeStep()) {
        MetricsSnapshotRow row;
        row.metric_name = query.getColumn(0).getString();
        row.metric_type = query.getColumn(1).getString();
        row.value       = query.getColumn(2).getDouble();
        row.labels_json = query.getColumn(3).getString();
        row.created_at  = query.getColumn(4).getString();
        results.push_back(std::move(row));
    }

    return results;
}

// ── Prune preview queries ─────────────────────────────────────────────

QueryReader::PrunePreview QueryReader::query_prune_preview(
    int older_than_days) const
{
    PrunePreview preview;

    // Build the cutoff date string for SQLite's datetime comparison.
    // datetime('now', '-N days') in the query.
    std::string cutoff_expr =
        "datetime('now', '-" + std::to_string(older_than_days) + " days')";

    // Runs older than cutoff.
    try {
        SQLite::Statement q(db_,
            "SELECT COUNT(*) FROM runs WHERE start_ts < " + cutoff_expr);
        if (q.executeStep()) {
            preview.runs_to_delete = q.getColumn(0).getInt64();
        }
    } catch (...) {}

    // Run jobs associated with old runs.
    try {
        SQLite::Statement q(db_,
            "SELECT COUNT(*) FROM job_runs WHERE run_id IN "
            "(SELECT run_id FROM runs WHERE start_ts < " + cutoff_expr + ")");
        if (q.executeStep()) {
            preview.run_jobs_to_delete = q.getColumn(0).getInt64();
        }
    } catch (...) {}

    // Run steps associated with old runs.
    try {
        SQLite::Statement q(db_,
            "SELECT COUNT(*) FROM step_runs WHERE run_id IN "
            "(SELECT run_id FROM runs WHERE start_ts < " + cutoff_expr + ")");
        if (q.executeStep()) {
            preview.run_steps_to_delete = q.getColumn(0).getInt64();
        }
    } catch (...) {}

    // Log chunks associated with old runs.
    try {
        SQLite::Statement q(db_,
            "SELECT COUNT(*) FROM log_chunks WHERE run_id IN "
            "(SELECT run_id FROM runs WHERE start_ts < " + cutoff_expr + ")");
        if (q.executeStep()) {
            preview.log_chunks_to_delete = q.getColumn(0).getInt64();
        }
    } catch (...) {}

    // Watch events older than cutoff.
    try {
        SQLite::Statement q(db_,
            "SELECT COUNT(*) FROM watch_events WHERE created_at < " +
            cutoff_expr);
        if (q.executeStep()) {
            preview.watch_events_to_delete = q.getColumn(0).getInt64();
        }
    } catch (...) {}

    // Watch samples older than cutoff (by collected_at).
    try {
        SQLite::Statement q(db_,
            "SELECT COUNT(*) FROM watch_samples WHERE collected_at < " +
            cutoff_expr);
        if (q.executeStep()) {
            preview.watch_samples_to_delete = q.getColumn(0).getInt64();
        }
    } catch (...) {}

    // Metrics snapshots older than cutoff.
    try {
        SQLite::Statement q(db_,
            "SELECT COUNT(*) FROM metrics_snapshots WHERE recorded_at < " +
            cutoff_expr);
        if (q.executeStep()) {
            preview.metrics_snapshots_to_delete = q.getColumn(0).getInt64();
        }
    } catch (...) {}

    return preview;
}

// ── Events tail (cursor-based) ────────────────────────────────────────

std::vector<QueryReader::WatchEventRow> QueryReader::query_watch_events_since(
    int64_t after_id,
    int limit,
    const std::string& watch_group) const
{
    std::vector<WatchEventRow> results;

    std::string sql =
        "SELECT id, watch_group, rule_name, event_type, action_taken, "
        "file_path, details_json, created_at "
        "FROM watch_events "
        "WHERE id > ? ";

    if (!watch_group.empty()) {
        sql += "AND watch_group = ? ";
    }
    sql += "ORDER BY id ASC LIMIT ?";

    SQLite::Statement query(db_, sql);

    int bind_idx = 1;
    query.bind(bind_idx++, after_id);
    if (!watch_group.empty()) {
        query.bind(bind_idx++, watch_group);
    }
    query.bind(bind_idx, limit);

    while (query.executeStep()) {
        WatchEventRow row;
        row.event_uid          = std::to_string(query.getColumn(0).getInt64());
        row.watch_group        = query.getColumn(1).getString();
        row.rule_name          = query.getColumn(2).getString();
        row.event_type         = query.getColumn(3).getString();
        row.severity           = query.getColumn(4).getString();
        row.affected_files_json= query.getColumn(5).getString();
        row.details_json       = query.getColumn(6).getString();
        row.created_at         = query.getColumn(7).getString();
        row.sample_epoch       = 0;
        results.push_back(std::move(row));
    }

    return results;
}

int64_t QueryReader::query_max_event_id() const {
    try {
        SQLite::Statement q(db_,
            "SELECT COALESCE(MAX(id), 0) FROM watch_events");
        if (q.executeStep()) {
            return q.getColumn(0).getInt64();
        }
    } catch (...) {}
    return 0;
}

}  // namespace kairos::persist
