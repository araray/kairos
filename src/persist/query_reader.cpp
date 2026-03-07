/// src/persist/query_reader.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  QueryReader implementation — run history queries for KEL               ║
// ║  Spec reference: §16.6, §7.7–7.8                                       ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/query_reader.hpp"

#include <chrono>
#include <cmath>
#include <iomanip>
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

}  // namespace kairos::persist
