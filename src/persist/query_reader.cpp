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

}  // namespace kairos::persist
