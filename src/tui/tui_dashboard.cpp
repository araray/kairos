/// src/tui/tui_dashboard.cpp
// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  TUI Dashboard implementation (FTXUI)                                    ║
// ║                                                                          ║
// ║  Layout:                                                                 ║
// ║  ┌─────────────────────────────────────────────────────────────────┐     ║
// ║  │  KAIROS v2.0.0  │  ● RUNNING  │  Uptime: 2d 14h  │  q=quit      │     ║
// ║  ├──────────────────────────┬──────────────────────────────────────┤     ║
// ║  │  Active Runs             │  Recent Triggers                     │     ║
// ║  │  ● deploy (1m 23s)       │  deploy     cron   2m ago            │     ║
// ║  │  ● backup (0m 45s)       │  backup     manual 5m ago            │     ║
// ║  ├──────────────────────────┴──────────────────────────────────────┤     ║
// ║  │  Recent Runs                                                    │     ║
// ║  │  run-a1b2  deploy  ✓ ok    2h ago   4.5s                        │     ║
// ║  │  run-c3d4  test    ✗ fail  14h ago  10m 5s                      │     ║
// ║  ├─────────────────────────────────────────────────────────────────┤     ║
// ║  │  Watch Groups              │  Recent Logs                       │     ║
// ║  │  log_monitor  4 events     │  14:30:05 [OUT] Job completed      │     ║
// ║  │  git_repo     2 events     │  14:30:04 [ERR] Step failed        │     ║
// ║  └────────────────────────────┴────────────────────────────────────┘     ║
// ║                                                                          ║
// ║  Schema corrections (vs. prior version):                                 ║
// ║    1. log_chunks.content (was: .data)                                    ║
// ║    2. step_runs table (was: run_steps)                                   ║
// ║    3. trigger_history table (was: trigger_state — doesn't exist)         ║
// ║    4. watch_events.created_at (was: .timestamp)                          ║
// ║    5. Cross-platform localtime_r/localtime_s                             ║
// ║    6. runs.target_name for display (was: target_id)                      ║
// ║                                                                          ║
// ║  Spec reference: §23, §5.12                                              ║
// ╚══════════════════════════════════════════════════════════════════════════╝

#include "kairos/tui/tui_dashboard.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/screen/color.hpp>

#include "kairos/platform/timezone.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <csignal>   // kill(pid, 0) for process liveness check
#else
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>  // OpenProcess for process liveness check
#endif

namespace kairos::tui {

using namespace ftxui;
using Clock = std::chrono::steady_clock;
using SysClock = std::chrono::system_clock;

// ── Data models ──────────────────────────────────────────────────────────

struct RunInfo {
    std::string run_id;
    std::string workflow;
    std::string status;
    std::string started;
    std::string duration;
    int exit_code = 0;
};

struct WatchGroupInfo {
    std::string name;
    std::string mode;
    int event_count = 0;
    std::string last_scan;
};

/// Represents a recent trigger fire (from trigger_history table).
/// The scheduler's next-fire time lives in-memory only (§10.5);
/// SQLite stores historical fire records in trigger_history.
struct TriggerFireInfo {
    std::string trigger_id;
    std::string trigger_type;
    std::string target_id;
    std::string fired_at;
    std::string status;
};

struct LogEntry {
    std::string timestamp;
    std::string level;
    std::string message;
};

struct EventInfo {
    std::string event_uid;
    std::string watch_group;
    std::string rule_name;
    std::string event_type;
    std::string affected_files;
    std::string created_at;
};

struct DashboardState {
    // Daemon status.
    bool daemon_running = false;
    std::string version = KAIROS_VERSION;
    std::string config_path;
    int64_t runs_today = 0;
    int64_t failures_today = 0;
    int64_t db_size_kb = 0;

    // Active runs.
    std::vector<RunInfo> active_runs;

    // Recent completed runs.
    std::vector<RunInfo> recent_runs;

    // Watch groups.
    std::vector<WatchGroupInfo> watch_groups;

    // Recent watch events.
    std::vector<EventInfo> events;

    // Recent trigger fires.
    std::vector<TriggerFireInfo> trigger_fires;

    // Log tail.
    std::vector<LogEntry> log_entries;

    // Error message (if DB connection fails).
    std::string error;

    // Last refresh time.
    std::string last_refresh;
};

// ── Daemon detection via PID file ────────────────────────────────────────
//
// Check if the daemon is running by reading the PID file (kairos.lock)
// and verifying the process is alive.  This matches the logic used by
// `kairos status` (cli_app.cpp §23.4).

namespace {

/// Check if a process with the given PID is alive.
bool is_process_alive(int pid) {
    if (pid <= 0) return false;
#ifndef _WIN32
    return (::kill(static_cast<pid_t>(pid), 0) == 0);
#else
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                             FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) return false;
    ::CloseHandle(h);
    return true;
#endif
}

/// Check the PID lock file and verify daemon liveness.
/// @param data_dir  The Kairos data directory (contains kairos.lock).
/// @return pair<running, pid_string>
std::pair<bool, std::string> check_daemon_pid(
    const std::filesystem::path& data_dir)
{
    auto lock_path = data_dir / "kairos.lock";
    std::string pid_str;
    try {
        std::ifstream pf(lock_path);
        if (!pf.is_open()) return {false, ""};
        std::getline(pf, pid_str);
    } catch (...) {
        return {false, ""};
    }

    if (pid_str.empty()) return {false, ""};

    try {
        int pid = std::stoi(pid_str);
        return {is_process_alive(pid), pid_str};
    } catch (...) {
        return {false, pid_str};
    }
}

}  // anonymous namespace

// ── Helper functions ─────────────────────────────────────────────────────
//
// These are in the kairos::tui::helpers namespace so tui_helpers.hpp can
// declare them for unit testing without pulling in FTXUI.

namespace helpers {

std::string format_duration(double seconds) {
    if (seconds < 0) return "—";
    if (seconds < 60) return std::to_string(static_cast<int>(seconds)) + "s";
    if (seconds < 3600) {
        int m = static_cast<int>(seconds) / 60;
        int s = static_cast<int>(seconds) % 60;
        return std::to_string(m) + "m " + std::to_string(s) + "s";
    }
    int h = static_cast<int>(seconds) / 3600;
    int m = (static_cast<int>(seconds) % 3600) / 60;
    return std::to_string(h) + "h " + std::to_string(m) + "m";
}

std::string format_relative(const std::string& iso_ts) {
    if (iso_ts.empty()) return "—";
    try {
        std::tm tm{};
        if (iso_ts.size() >= 19) {
            tm.tm_year = std::stoi(iso_ts.substr(0, 4)) - 1900;
            tm.tm_mon  = std::stoi(iso_ts.substr(5, 2)) - 1;
            tm.tm_mday = std::stoi(iso_ts.substr(8, 2));
            tm.tm_hour = std::stoi(iso_ts.substr(11, 2));
            tm.tm_min  = std::stoi(iso_ts.substr(14, 2));
            tm.tm_sec  = std::stoi(iso_ts.substr(17, 2));
            tm.tm_isdst = -1;
            auto t = std::mktime(&tm);
            if (t == -1) return iso_ts.substr(11, 8);
            auto now = std::time(nullptr);
            auto diff = std::difftime(now, t);
            if (diff < 0)    return "just now";
            if (diff < 60)   return std::to_string(static_cast<int>(diff)) + "s ago";
            if (diff < 3600) return std::to_string(static_cast<int>(diff / 60)) + "m ago";
            if (diff < 86400) return std::to_string(static_cast<int>(diff / 3600)) + "h ago";
            return std::to_string(static_cast<int>(diff / 86400)) + "d ago";
        }
    } catch (...) {}
    return iso_ts.size() > 11 ? iso_ts.substr(11, 8) : iso_ts;
}

std::string status_icon(const std::string& status) {
    if (status == "SUCCESS")   return "✓";
    if (status == "FAILED")    return "✗";
    if (status == "RUNNING")   return "●";
    if (status == "CANCELLED") return "⊘";
    if (status == "SKIPPED")   return "○";
    if (status == "TIMED_OUT") return "⏱";
    return "?";
}

std::string format_local_time_now() {
    auto now = SysClock::to_time_t(SysClock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

}  // namespace helpers

// ── FTXUI color helpers (file-local) ────────────────────────────────────

namespace {

Color status_color(const std::string& status) {
    if (status == "SUCCESS")   return Color::Green;
    if (status == "FAILED")    return Color::Red;
    if (status == "RUNNING")   return Color::Cyan;
    if (status == "CANCELLED") return Color::Yellow;
    if (status == "INTERRUPTED") return Color::Magenta;
    if (status == "TIMED_OUT") return Color::Red;
    return Color::White;
}

Color level_color(const std::string& level) {
    if (level == "ERROR" || level == "CRITICAL" || level == "ERR")
        return Color::Red;
    if (level == "WARN" || level == "WARNING")
        return Color::Yellow;
    if (level == "INFO" || level == "OUT")
        return Color::Green;
    if (level == "DEBUG")
        return Color::GrayDark;
    return Color::White;
}

// ── Database refresh ────────────────────────────────────────────────────
//
// Opens a read-only SQLite connection each cycle. Each query block is
// wrapped in its own try/catch so that missing tables (e.g., before
// first run, or on an older schema version) don't abort the entire
// refresh — the corresponding panel simply shows "no data".

void refresh_state(DashboardState& state, const DashboardConfig& config) {
    try {
        if (!std::filesystem::exists(config.db_path)) {
            state.error = "Database not found: " + config.db_path.string();
            state.daemon_running = false;
            return;
        }

        SQLite::Database db(config.db_path.string(),
                           SQLite::OPEN_READONLY);

        state.error.clear();
        state.config_path = config.config_path.string();

        // Database file size.
        std::error_code ec;
        auto sz = std::filesystem::file_size(config.db_path, ec);
        state.db_size_kb = ec ? 0 : static_cast<int64_t>(sz / 1024);

        // ── Active runs ──────────────────────────────────────────
        state.active_runs.clear();
        try {
            SQLite::Statement q(db,
                "SELECT run_id, target_name, status, start_ts, "
                "       CAST((julianday('now') - julianday(start_ts))"
                "            * 86400 AS INTEGER) AS elapsed_s "
                "FROM runs WHERE status = 'RUNNING' "
                "ORDER BY start_ts DESC LIMIT 20");
            while (q.executeStep()) {
                RunInfo ri;
                ri.run_id   = q.getColumn(0).getString();
                ri.workflow = q.getColumn(1).getString();
                ri.status   = q.getColumn(2).getString();
                ri.started  = q.getColumn(3).getString();
                ri.duration = helpers::format_duration(
                    q.getColumn(4).getDouble());
                state.active_runs.push_back(std::move(ri));
            }
        } catch (const std::exception&) {}

        // ── Daemon liveness via PID file ─────────────────────────
        // Use the same approach as `kairos status`: read kairos.lock
        // and verify the process is alive.
        auto data_dir = config.data_dir.empty()
            ? config.db_path.parent_path()
            : config.data_dir;
        auto [running, pid_str] = check_daemon_pid(data_dir);
        state.daemon_running = running;

        // ── Recent completed runs ────────────────────────────────
        state.recent_runs.clear();
        try {
            SQLite::Statement q(db,
                "SELECT run_id, target_name, status, start_ts, "
                "       duration_ms, exit_code "
                "FROM runs WHERE status != 'RUNNING' "
                "ORDER BY start_ts DESC LIMIT ?");
            q.bind(1, config.max_recent_runs);
            while (q.executeStep()) {
                RunInfo ri;
                ri.run_id    = q.getColumn(0).getString();
                ri.workflow  = q.getColumn(1).getString();
                ri.status    = q.getColumn(2).getString();
                ri.started   = platform::format_relative(
                    q.getColumn(3).getString());
                double ms    = q.getColumn(4).getDouble();
                ri.duration  = helpers::format_duration(ms / 1000.0);
                ri.exit_code = q.getColumn(5).getInt();
                state.recent_runs.push_back(std::move(ri));
            }
        } catch (const std::exception&) {}

        // ── Runs today / failures today ──────────────────────────
        try {
            SQLite::Statement q(db,
                "SELECT COUNT(*), "
                "  SUM(CASE WHEN status='FAILED' THEN 1 ELSE 0 END) "
                "FROM runs WHERE start_ts >= date('now')");
            if (q.executeStep()) {
                state.runs_today     = q.getColumn(0).getInt64();
                state.failures_today = q.getColumn(1).getInt64();
            }
        } catch (const std::exception&) {}

        // ── Watch groups ─────────────────────────────────────────
        // FIX: column is created_at, not timestamp.
        state.watch_groups.clear();
        try {
            SQLite::Statement q(db,
                "SELECT watch_group, COUNT(*), MAX(created_at) "
                "FROM watch_events "
                "GROUP BY watch_group "
                "ORDER BY MAX(created_at) DESC LIMIT ?");
            q.bind(1, config.max_watch_groups);
            while (q.executeStep()) {
                WatchGroupInfo wg;
                wg.name        = q.getColumn(0).getString();
                wg.event_count = q.getColumn(1).getInt();
                wg.last_scan   = platform::format_relative(
                    q.getColumn(2).getString());
                state.watch_groups.push_back(std::move(wg));
            }
        } catch (const std::exception&) {}

        // ── Recent watch events ──────────────────────────────────
        state.events.clear();
        try {
            SQLite::Statement q(db,
                "SELECT id, watch_group, rule_name, event_type, "
                "       file_path, created_at "
                "FROM watch_events "
                "ORDER BY created_at DESC LIMIT ?");
            q.bind(1, config.max_events);
            while (q.executeStep()) {
                EventInfo ev;
                ev.event_uid      = std::to_string(
                    q.getColumn(0).getInt64());
                ev.watch_group    = q.getColumn(1).getString();
                ev.rule_name      = q.getColumn(2).getString();
                ev.event_type     = q.getColumn(3).getString();
                ev.affected_files = q.getColumn(4).getString();
                ev.created_at     = platform::format_relative(
                    q.getColumn(5).getString());
                // Truncate long file paths for display.
                if (ev.affected_files.size() > 60) {
                    ev.affected_files = ev.affected_files.substr(0, 57) + "...";
                }
                state.events.push_back(std::move(ev));
            }
        } catch (const std::exception&) {}

        // ── Recent trigger fires ─────────────────────────────────
        // FIX: table is trigger_history (not trigger_state).
        // The scheduler's next-fire time is in-memory only (§10.5).
        state.trigger_fires.clear();
        try {
            SQLite::Statement q(db,
                "SELECT trigger_id, trigger_type, target_id, "
                "       fired_at, status "
                "FROM trigger_history "
                "ORDER BY fired_at DESC LIMIT ?");
            q.bind(1, config.max_timers);
            while (q.executeStep()) {
                TriggerFireInfo ti;
                ti.trigger_id   = q.getColumn(0).getString();
                ti.trigger_type = q.getColumn(1).getString();
                ti.target_id    = q.getColumn(2).getString();
                ti.fired_at     = platform::format_relative(
                    q.getColumn(3).getString());
                ti.status       = q.getColumn(4).getString();
                state.trigger_fires.push_back(std::move(ti));
            }
        } catch (const std::exception&) {}

        // ── Recent log entries ───────────────────────────────────
        // FIX: column is content (not data), no JOIN needed.
        state.log_entries.clear();
        try {
            SQLite::Statement q(db,
                "SELECT lc.content, lc.created_at, lc.stream "
                "FROM log_chunks lc "
                "ORDER BY lc.id DESC LIMIT ?");
            q.bind(1, config.max_log_lines);
            while (q.executeStep()) {
                LogEntry le;
                le.message   = q.getColumn(0).getString();
                le.timestamp = q.getColumn(1).getString();
                std::string stream = q.getColumn(2).getString();
                le.level = (stream == "stderr") ? "ERR" : "OUT";
                if (le.message.size() > 120) {
                    le.message = le.message.substr(0, 117) + "...";
                }
                state.log_entries.push_back(std::move(le));
            }
            std::reverse(state.log_entries.begin(),
                         state.log_entries.end());
        } catch (const std::exception&) {}

        // Update refresh timestamp (cross-platform).
        // Update refresh timestamp using configured timezone.
        auto tz = platform::TimezoneConfig::parse(config.timezone);
        state.last_refresh = platform::format_now(tz);

    } catch (const std::exception& e) {
        state.error = std::string("Database error: ") + e.what();
    }
}

}  // anonymous namespace

// ── Dashboard entry point ────────────────────────────────────────────────

int run_dashboard(const DashboardConfig& config) {
    auto screen = ScreenInteractive::Fullscreen();

    DashboardState state;
    state.config_path = config.config_path.string();
    std::mutex state_mutex;
    std::atomic<bool> should_quit{false};
    std::atomic<bool> force_refresh{false};
    bool show_logs = true;

    // Phase 8.5: Filter bar state (§4.1 TUI search/filter).
    std::string filter_text;
    bool filter_active = false;

    // Initial refresh.
    refresh_state(state, config);

    // ── Build the UI component tree ──────────────────────────────

    auto renderer = Renderer([&] {
        std::lock_guard<std::mutex> lock(state_mutex);

        // Phase 8.5: Case-insensitive substring filter matcher.
        auto matches_filter = [&](const std::string& text) -> bool {
            if (!filter_active || filter_text.empty()) return true;
            // Case-insensitive search.
            std::string lower_text = text;
            std::string lower_filter = filter_text;
            std::transform(lower_text.begin(), lower_text.end(),
                           lower_text.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            std::transform(lower_filter.begin(), lower_filter.end(),
                           lower_filter.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return lower_text.find(lower_filter) != std::string::npos;
        };

        // Helper: check if any field in a RunInfo matches.
        auto run_matches = [&](const RunInfo& r) -> bool {
            return matches_filter(r.run_id) ||
                   matches_filter(r.workflow) ||
                   matches_filter(r.status);
        };

        // ── Header bar ───────────────────────────────────────────
        auto status_indicator = state.daemon_running
            ? text("● RUNNING") | color(Color::Green) | bold
            : text("○ STOPPED") | color(Color::Red) | bold;

        auto header = hbox({
            text(" KAIROS ") | bold | color(Color::Cyan),
            text("v" + state.version) | color(Color::GrayLight),
            separator(),
            status_indicator,
            separator(),
            text("Runs: " + std::to_string(state.runs_today)),
            separator(),
            text("Fails: " + std::to_string(state.failures_today))
                | (state.failures_today > 0
                    ? color(Color::Red) : color(Color::White)),
            separator(),
            text("DB: " + std::to_string(state.db_size_kb) + " KB")
                | color(Color::GrayLight),
            filler(),
            text(state.last_refresh + " ") | color(Color::GrayDark),
            text("q=quit r=refresh l=logs /=filter") | dim,
        }) | borderLight;

        // ── Error banner ─────────────────────────────────────────
        Element error_banner = text("");
        if (!state.error.empty()) {
            error_banner = text(" ⚠  " + state.error + " ")
                | color(Color::Red) | bold | borderLight;
        }

        // ── Active runs panel ────────────────────────────────────
        Elements active_items;
        if (state.active_runs.empty()) {
            active_items.push_back(text("  No active runs") | dim);
        } else {
            for (const auto& r : state.active_runs) {
                if (!run_matches(r)) continue;
                active_items.push_back(hbox({
                    text("  ● ") | color(Color::Cyan),
                    text(r.workflow) | bold,
                    filler(),
                    text("(" + r.duration + ")") | dim,
                }));
            }
            if (active_items.empty())
                active_items.push_back(text("  (filtered)") | dim);
        }
        auto active_panel = vbox(std::move(active_items))
            | borderLight | size(HEIGHT, LESS_THAN, 8);
        active_panel = vbox({
            text(" Active Runs (" +
                 std::to_string(state.active_runs.size()) + ")")
                | bold | color(Color::Cyan),
            active_panel,
        });

        // ── Recent triggers panel ────────────────────────────────
        Elements trigger_items;
        if (state.trigger_fires.empty()) {
            trigger_items.push_back(
                text("  No trigger history") | dim);
        } else {
            for (const auto& t : state.trigger_fires) {
                if (!matches_filter(t.trigger_id) &&
                    !matches_filter(t.trigger_type) &&
                    !matches_filter(t.target_id))
                    continue;
                auto short_id = t.trigger_id.size() > 16
                    ? t.trigger_id.substr(0, 16) : t.trigger_id;
                trigger_items.push_back(hbox({
                    text("  " + short_id),
                    filler(),
                    text(t.trigger_type + " ") | dim,
                    text(t.fired_at) | dim,
                }));
            }
            if (trigger_items.empty())
                trigger_items.push_back(text("  (filtered)") | dim);
        }
        auto trigger_panel = vbox(std::move(trigger_items))
            | borderLight | size(HEIGHT, LESS_THAN, 8);
        trigger_panel = vbox({
            text(" Recent Triggers (" +
                 std::to_string(state.trigger_fires.size()) + ")")
                | bold | color(Color::Yellow),
            trigger_panel,
        });

        // ── Recent runs table ────────────────────────────────────
        Elements run_rows;
        run_rows.push_back(hbox({
            text("  RUN ID") | bold | size(WIDTH, EQUAL, 16),
            text("WORKFLOW") | bold | size(WIDTH, EQUAL, 20),
            text("STATUS")   | bold | size(WIDTH, EQUAL, 12),
            text("STARTED")  | bold | size(WIDTH, EQUAL, 12),
            text("DURATION") | bold | size(WIDTH, EQUAL, 10),
        }));
        run_rows.push_back(separator());

        if (state.recent_runs.empty()) {
            run_rows.push_back(text("  No completed runs") | dim);
        } else {
            for (const auto& r : state.recent_runs) {
                if (!run_matches(r)) continue;
                auto short_id = r.run_id.size() > 12
                    ? r.run_id.substr(0, 12) : r.run_id;
                auto short_wf = r.workflow.size() > 18
                    ? r.workflow.substr(0, 18) : r.workflow;
                run_rows.push_back(hbox({
                    text("  " + short_id) | size(WIDTH, EQUAL, 16),
                    text(short_wf) | size(WIDTH, EQUAL, 20),
                    text(helpers::status_icon(r.status) + " " + r.status)
                        | color(status_color(r.status))
                        | size(WIDTH, EQUAL, 12),
                    text(r.started) | size(WIDTH, EQUAL, 12),
                    text(r.duration) | size(WIDTH, EQUAL, 10),
                }));
            }
        }
        auto runs_panel = vbox({
            text(" Recent Runs") | bold | color(Color::Green),
            vbox(std::move(run_rows)) | borderLight | yflex,
        });

        // ── Watch groups panel ───────────────────────────────────
        Elements wg_items;
        if (state.watch_groups.empty()) {
            wg_items.push_back(text("  No watch groups") | dim);
        } else {
            for (const auto& wg : state.watch_groups) {
                if (!matches_filter(wg.name)) continue;
                wg_items.push_back(hbox({
                    text("  " + wg.name) | bold,
                    filler(),
                    text(std::to_string(wg.event_count) + " events")
                        | dim,
                    text("  " + wg.last_scan) | dim,
                }));
            }
            if (wg_items.empty())
                wg_items.push_back(text("  (filtered)") | dim);
        }
        auto watch_panel = vbox({
            text(" Watch Groups (" +
                 std::to_string(state.watch_groups.size()) + ")")
                | bold | color(Color::Magenta),
            vbox(std::move(wg_items)) | borderLight,
        });

        // ── Recent events panel ──────────────────────────────────
        Elements event_items;
        if (state.events.empty()) {
            event_items.push_back(text("  No events") | dim);
        } else {
            for (const auto& ev : state.events) {
                if (!matches_filter(ev.watch_group) &&
                    !matches_filter(ev.rule_name) &&
                    !matches_filter(ev.event_type) &&
                    !matches_filter(ev.affected_files))
                    continue;
                auto short_uid = ev.event_uid.size() > 8
                    ? ev.event_uid.substr(0, 8) : ev.event_uid;
                auto short_group = ev.watch_group.size() > 14
                    ? ev.watch_group.substr(0, 14) : ev.watch_group;
                event_items.push_back(hbox({
                    text("  " + short_uid) | dim
                        | size(WIDTH, EQUAL, 11),
                    text(short_group)
                        | size(WIDTH, EQUAL, 16),
                    text(ev.event_type)
                        | color(Color::Yellow)
                        | size(WIDTH, EQUAL, 10),
                    text(ev.affected_files) | dim | flex,
                    text("  " + ev.created_at) | dim,
                }));
            }
            if (event_items.empty())
                event_items.push_back(text("  (filtered)") | dim);
        }
        auto events_panel = vbox({
            text(" Events (" +
                 std::to_string(state.events.size()) + ")")
                | bold | color(Color::Yellow),
            vbox(std::move(event_items))
                | borderLight | size(HEIGHT, LESS_THAN, 10),
        });

        // ── Log tail panel ───────────────────────────────────────
        Element log_panel = text("");
        if (show_logs) {
            Elements log_items;
            if (state.log_entries.empty()) {
                log_items.push_back(text("  No log entries") | dim);
            } else {
                for (const auto& le : state.log_entries) {
                    auto ts = le.timestamp.size() > 8
                        ? le.timestamp.substr(le.timestamp.size() - 8)
                        : le.timestamp;
                    log_items.push_back(hbox({
                        text("  " + ts + " ") | dim,
                        text("[" + le.level + "] ")
                            | color(level_color(le.level)),
                        text(le.message) | flex,
                    }));
                }
            }
            log_panel = vbox({
                text(" Logs") | bold | color(Color::Blue),
                vbox(std::move(log_items))
                    | borderLight | size(HEIGHT, LESS_THAN, 12),
            });
        }

        // ── Filter bar (Phase 8.5) ────────────────────────────────
        Element filter_bar = text("");
        if (filter_active) {
            filter_bar = hbox({
                text(" / ") | bold | color(Color::Cyan),
                text(filter_text) | bold | color(Color::White),
                text("█") | blink | color(Color::Cyan),
                filler(),
                text("[Esc] clear  [Enter] keep") | dim,
            }) | borderLight | color(Color::Cyan);
        }

        // ── Compose the full layout ──────────────────────────────
        return vbox({
            header,
            error_banner,
            hbox({
                active_panel  | flex,
                trigger_panel | flex,
            }),
            runs_panel | flex,
            events_panel,
            hbox({
                watch_panel | flex,
                log_panel   | flex,
            }),
            filter_bar,
        });
    });

    // ── Keyboard handler ─────────────────────────────────────────
    auto component = CatchEvent(renderer, [&](Event event) {
        // Phase 8.5: Filter mode input handling.
        if (filter_active) {
            if (event == Event::Escape) {
                // Clear filter and exit filter mode.
                filter_text.clear();
                filter_active = false;
                return true;
            }
            if (event == Event::Return) {
                // Keep filter text, exit filter mode.
                filter_active = false;
                return true;
            }
            if (event == Event::Backspace) {
                if (!filter_text.empty()) {
                    filter_text.pop_back();
                }
                return true;
            }
            // Accept printable characters into the filter.
            if (event.is_character()) {
                filter_text += event.character();
                return true;
            }
            return false;
        }

        // Normal mode key handling.
        if (event == Event::Character('q') ||
            event == Event::Escape) {
            should_quit = true;
            screen.Exit();
            return true;
        }
        if (event == Event::Character('r')) {
            force_refresh = true;
            return true;
        }
        if (event == Event::Character('l')) {
            show_logs = !show_logs;
            return true;
        }
        if (event == Event::Character('/')) {
            filter_active = true;
            filter_text.clear();
            return true;
        }
        return false;
    });

    // ── Background refresh thread ────────────────────────────────
    std::jthread refresh_thread([&](std::stop_token stop) {
        while (!stop.stop_requested() && !should_quit) {
            for (int i = 0; i < config.refresh_ms / 100; ++i) {
                if (stop.stop_requested() || should_quit) return;
                if (force_refresh.exchange(false)) break;
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(100));
            }

            if (stop.stop_requested() || should_quit) return;

            DashboardState new_state;
            new_state.config_path = config.config_path.string();
            refresh_state(new_state, config);

            {
                std::lock_guard<std::mutex> lock(state_mutex);
                state = std::move(new_state);
            }

            screen.Post(Event::Custom);
        }
    });

    // ── Run the event loop ───────────────────────────────────────
    screen.Loop(component);

    should_quit = true;
    refresh_thread.request_stop();

    return 0;
}

}  // namespace kairos::tui
