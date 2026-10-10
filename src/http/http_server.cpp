/// src/http/http_server.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  HTTP server implementation — cpp-httplib backend                         ║
// ║                                                                          ║
// ║  HTML pages (server-rendered via the web/ template catalog):             ║
// ║    GET  /                         Dashboard                              ║
// ║    GET  /workflows                Workflow list                          ║
// ║    GET  /workflows/:name          Workflow detail (+ plan, YAML)         ║
// ║    GET  /jobs                     Job list (workflow + standalone)       ║
// ║    GET  /jobs/:id                 Job detail                             ║
// ║    GET  /runs                     Run history (+ filters)                ║
// ║    GET  /runs/:id                 Run detail (+ SSE live logs)           ║
// ║    GET  /events                   Event list (affected files §3.4)       ║
// ║    GET  /events/:uid              Event detail                           ║
// ║    GET  /watches                  Watch group list                       ║
// ║    GET  /watches/:name            Watch group detail (+ YAML)            ║
// ║    GET  /triggers                 Trigger list (next fire times)         ║
// ║    GET  /static/*                 Static assets (external → embedded)    ║
// ║                                                                          ║
// ║  REST API (token-authenticated, JSON):                                   ║
// ║    GET  /health                   Health check                           ║
// ║    GET  /metrics                  Prometheus text exposition             ║
// ║    GET  /api/v1/workflows         List workflows                         ║
// ║    GET  /api/v1/workflows/:n      Workflow detail                        ║
// ║    GET  /api/v1/workflows/:n/yaml Raw YAML source                        ║
// ║    POST /api/v1/workflows/:n/run  Trigger workflow run                   ║
// ║    GET  /api/v1/jobs              List all jobs                          ║
// ║    GET  /api/v1/jobs/:id          Job detail                             ║
// ║    GET  /api/v1/runs              Query runs (status/workflow/since/tag) ║
// ║    GET  /api/v1/runs/:id          Run detail (prefix-resolved)           ║
// ║    GET  /api/v1/runs/:id/logs     Run logs (cursor-based)                ║
// ║    GET  /api/v1/events            Query events                           ║
// ║    GET  /api/v1/events/:uid       Event detail                           ║
// ║    GET  /api/v1/watches           List watch groups                      ║
// ║    GET  /api/v1/watches/:name     Watch group detail                     ║
// ║    GET  /api/v1/watches/:n/yaml   Raw YAML source                        ║
// ║    GET  /api/v1/triggers          List triggers                          ║
// ║    GET  /api/v1/triggers/:id      Trigger detail                         ║
// ║    GET  /api/v1/explain/:wf       Dry-run execution plan (§11.7)         ║
// ║    POST /api/v1/config/reload     Reload configuration                  ║
// ║    GET  /sse/logs/:run_id         SSE log stream                        ║
// ║                                                                          ║
// ║  Spec reference: §26.2–§26.7 (Roadmap §3.1, §3.3, §3.4, §3.6, §3.8.1)  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef KAIROS_HTTP_ENABLED

#include "kairos/http/http_server.hpp"
#include "kairos/http/dag_svg.hpp"
#include "kairos/http/web_catalog.hpp"

#include "kairos/config/yaml_loader.hpp"
#include "kairos/core/version.hpp"
#include "kairos/engine/explain.hpp"
#include "kairos/kel/errors.hpp"
#include "kairos/kel/evaluator.hpp"
#include "kairos/platform/timezone.hpp"

// cpp-httplib — header-only HTTP server.
// Must define implementation once.
#ifndef CPPHTTPLIB_HTTPLIB_H
#include <httplib.h>
#endif

#include <inja/inja.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <thread>

namespace kairos::http {

using json = nlohmann::json;

// ── Shared helpers ────────────────────────────────────────────────────────

namespace {

/// Map run status to Bootstrap badge class suffix.
std::string status_to_badge(const std::string& status) {
    if (status == "SUCCESS") return "success";
    if (status == "FAILURE") return "danger";
    if (status == "RUNNING") return "primary";
    if (status == "CANCELLED") return "warning";
    return "secondary";
}

/// Truncate a string for display.
std::string truncate(const std::string& s, size_t max_len = 12) {
    if (s.size() <= max_len) return s;
    return s.substr(0, max_len);
}

/// Parse a runs.tags_json value (JSON array) into a string list.
std::vector<std::string> parse_tags_json(const std::string& tags_json) {
    std::vector<std::string> tags;
    if (tags_json.empty()) return tags;
    try {
        auto parsed = json::parse(tags_json);
        if (parsed.is_array()) {
            for (const auto& t : parsed) {
                if (t.is_string()) tags.push_back(t.get<std::string>());
            }
        }
    } catch (...) { /* malformed — treat as untagged */ }
    return tags;
}

/// Parse affected_files_json into {summary, list} for display (§1.2, §3.4).
struct FilesDisplay {
    std::string summary;
    json list = json::array();
};

FilesDisplay files_display(const std::string& affected_files_json) {
    FilesDisplay out;
    if (affected_files_json.empty() || affected_files_json == "[]") {
        return out;
    }
    try {
        auto parsed = json::parse(affected_files_json);
        if (!parsed.is_array()) {
            out.summary = affected_files_json;
            return out;
        }
        std::string summary;
        for (size_t i = 0; i < parsed.size() && i < 5; ++i) {
            if (!parsed[i].is_string()) continue;
            std::string path = parsed[i].get<std::string>();
            out.list.push_back(path);
            if (!summary.empty()) summary += ", ";
            auto pos = path.find_last_of("/\\");
            summary += (pos != std::string::npos)
                ? path.substr(pos + 1) : path;
        }
        if (parsed.size() > 5) {
            summary += " (+" + std::to_string(parsed.size() - 5) + " more)";
        }
        out.summary = std::move(summary);
    } catch (...) {
        out.summary = affected_files_json;
    }
    return out;
}

/// Read a whole file (YAML sources). Returns nullopt on failure.
std::optional<std::string> read_text_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    return content;
}

/// Next-fire + schedule info for a trigger (mirrors the CLI helper).
struct TriggerInfo {
    std::string type;
    std::string schedule_expr;
    std::string next_fire;   ///< Display-formatted, "--" when unknown.
    std::string relative;    ///< "in 2h 30m"
};

TriggerInfo trigger_info(const engine::TimerEntry& t,
                         const platform::TimezoneConfig& tz) {
    using namespace std::chrono;
    TriggerInfo info;
    info.type = engine::trigger_spec_type_name(t.spec);
    auto now = system_clock::now();

    info.schedule_expr = std::visit([](const auto& s) -> std::string {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, engine::CronTrigger>)
            return s.expression;
        if constexpr (std::is_same_v<T, engine::IntervalTrigger>) {
            auto secs = s.interval.count() / 1000;
            if (secs < 60) return "every " + std::to_string(secs) + "s";
            if (secs < 3600) return "every " + std::to_string(secs / 60) + "m";
            return "every " + std::to_string(secs / 3600) + "h";
        }
        if constexpr (std::is_same_v<T, engine::DateTrigger>) {
            if (s.fired) return "(exhausted)";
            return platform::format_display_time(s.fire_at,
                                                 platform::TimezoneConfig{});
        }
        return {};
    }, t.spec);

    auto next = std::visit([&](const auto& s) -> system_clock::time_point {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, engine::CronTrigger>) {
            try { return s.next_fire_after(now); }
            catch (...) { return {}; }
        }
        if constexpr (std::is_same_v<T, engine::IntervalTrigger>) {
            return now + s.interval;
        }
        if constexpr (std::is_same_v<T, engine::DateTrigger>) {
            return s.fired ? system_clock::time_point{} : s.fire_at;
        }
        return system_clock::time_point{};
    }, t.spec);

    if (next.time_since_epoch().count() == 0) {
        info.next_fire = "--";
        info.relative = "--";
        return info;
    }
    info.next_fire = platform::format_display_time(next, tz);
    info.relative = platform::format_relative(next);
    return info;
}

/// All triggers targeting a workflow (by id or name).
std::vector<const engine::TimerEntry*> triggers_for_target(
    const engine::WorkflowRegistry& reg,
    const std::string& target_id,
    const std::string& target_name)
{
    std::vector<const engine::TimerEntry*> result;
    for (const auto& t : reg.triggers()) {
        if (t.target_id == target_id || t.target_name == target_name)
            result.push_back(&t);
    }
    return result;
}

/// Reject entity names that would escape the YAML directory or create
/// surprising files: allow [A-Za-z0-9._-]+ only, no leading dot.
bool is_safe_entity_name(const std::string& name) {
    if (name.empty() || name.front() == '.') return false;
    for (char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) ||
              c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return name.find("..") == std::string::npos;
}

/// Write a file atomically: temp file + rename (§3.8.2 backups/writes).
bool write_file_atomic(const std::filesystem::path& path,
                       const std::string& content) {
    std::error_code ec;
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(content.data(),
                  static_cast<std::streamsize>(content.size()));
        if (!out) return false;
    }
#ifdef _WIN32
    std::filesystem::remove(path, ec);  // rename fails if target exists
#endif
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

/// Copy path → path + ".bak" (overwriting any previous backup).
bool backup_file(const std::filesystem::path& path) {
    std::error_code ec;
    auto bak = path;
    bak += ".bak";
    std::filesystem::remove(bak, ec);
    if (!std::filesystem::exists(path, ec)) return false;
    return std::filesystem::copy_file(path, bak, ec) && !ec;
}

}  // namespace

// ── Server implementation ─────────────────────────────────────────────────

struct HttpServer::Impl {
    HttpConfig config;
    HttpDependencies deps;
    httplib::Server svr;
    std::jthread server_thread;
    platform::TimezoneConfig tz;  // Parsed from config.timezone.

    /// Asset resolver: external web root → embedded fallback (§3.1).
    web::WebCatalog catalog;

    /// Current registry snapshot (swapped on config reload).
    std::mutex registry_mu;
    std::shared_ptr<const engine::WorkflowRegistry> current_registry;

    Impl(HttpConfig cfg, HttpDependencies d)
        : config(std::move(cfg))
        , deps(std::move(d))
        , tz(platform::TimezoneConfig::parse(config.timezone))
        , catalog(config.web_root.empty()
              ? std::filesystem::path{}
              : std::filesystem::path(config.web_root))
    {
        current_registry = deps.registry;
    }

    /// Thread-safe registry snapshot.
    std::shared_ptr<const engine::WorkflowRegistry> registry() {
        std::lock_guard lock(registry_mu);
        return current_registry;
    }

    // ── Auth / CORS ────────────────────────────────────────────────

    bool check_auth(const httplib::Request& req,
                    httplib::Response& res) {
        if (config.api_token.empty()) return true;  // No auth required.

        auto auth = req.get_header_value("Authorization");
        std::string expected = "Bearer " + config.api_token;
        if (auth != expected) {
            res.status = 401;
            res.set_content(R"({"error":"Unauthorized"})",
                            "application/json");
            return false;
        }
        return true;
    }

    void set_cors(httplib::Response& res) {
        if (config.enable_cors) {
            res.set_header("Access-Control-Allow-Origin",
                           config.cors_origins);
        }
    }

    // ── Page rendering (two-pass: page → base wrapper) ─────────────

    /// Render a page template wrapped in templates/base.html.
    /// Returns nullopt on any load/parse failure (caller sends 500).
    std::optional<std::string> render_page(
        const std::string& template_path,
        json data,
        const std::string& title,
        const std::string& active)
    {
        auto page = catalog.load(template_path);
        auto base = catalog.load("templates/base.html");
        if (!page || !base) {
            spdlog::error("Web UI: missing template {}", template_path);
            return std::nullopt;
        }

        try {
            inja::Environment env;

            // severity_class(sev) → CSS class for the sev-* color hints.
            env.add_callback("severity_class", 1,
                [](inja::Arguments& args) -> json {
                    if (args.empty() || !args.at(0)->is_string())
                        return "info";
                    auto s = args.at(0)->get<std::string>();
                    if (s == "warning") return "warning";
                    if (s == "critical") return "critical";
                    return "info";
                });

            std::string content = env.render(
                page->content, data);

            json base_data;
            base_data["title"] = title;
            base_data["active"] = active;
            base_data["content"] = std::move(content);
            base_data["version"] = std::string(kairos::kVersion);
            return env.render(base->content, base_data);
        } catch (const std::exception& e) {
            spdlog::error("Web UI: render {} failed: {}",
                          template_path, e.what());
            return std::nullopt;
        }
    }

    /// Send a rendered page, or a minimal fallback on render failure.
    void send_page(httplib::Response& res,
                   const std::string& template_path,
                   json data,
                   const std::string& title,
                   const std::string& active)
    {
        if (auto html = render_page(template_path, std::move(data),
                                    title, active)) {
            res.set_content(*html, "text/html; charset=utf-8");
        } else {
            res.status = 500;
            res.set_content("Kairos web UI: template error (see daemon log)",
                            "text/plain; charset=utf-8");
        }
        set_cors(res);
    }

    /// Error page (404/500 …).
    void send_error_page(httplib::Response& res, int status,
                         const std::string& title,
                         const std::string& message) {
        json data;
        data["error_title"] = title;
        data["error_message"] = message;
        data["error_code"] = status;
        res.status = status;
        if (auto html = render_page("templates/error.html",
                                    std::move(data), "Error", "")) {
            res.set_content(*html, "text/html; charset=utf-8");
        } else {
            res.set_content(title + ": " + message,
                            "text/plain; charset=utf-8");
        }
        set_cors(res);
    }

    // ── Data builders ──────────────────────────────────────────────

    /// JSON for a run summary row (used by pages and API).
    json run_json(const persist::QueryReader::RunSummary& r) {
        json j;
        j["run_id"] = r.run_id;
        j["run_id_short"] = truncate(r.run_id);
        j["target_name"] = r.target_name;
        j["target_type"] = r.target_type;
        j["trigger_type"] = r.trigger_type;
        j["status"] = r.status;
        j["badge"] = status_to_badge(r.status);
        j["exit_code"] = r.exit_code;
        j["start_ts"] = platform::format_display_time(r.start_ts, tz);
        j["end_ts"] = platform::format_display_time(r.end_ts, tz);
        j["duration_ms"] = r.duration_ms;
        json tags = json::array();
        for (const auto& t : parse_tags_json(r.tags_json)) {
            tags.push_back(t);
        }
        j["tags"] = std::move(tags);
        return j;
    }

    /// JSON for a workflow definition.
    json workflow_json(const engine::WorkflowDef& wf,
                       const engine::WorkflowRegistry& reg) {
        json j;
        j["workflow_id"] = wf.workflow_id;
        j["name"] = wf.workflow_name;
        j["job_count"] = wf.jobs.size();
        json tags = json::array();
        for (const auto& t : wf.tags) tags.push_back(t);
        j["tags"] = std::move(tags);
        j["source_path"] = wf.source_path;

        json jobs = json::array();
        for (const auto& job : wf.jobs) {
            jobs.push_back({
                {"job_id", job.job_id},
                {"name", job.job_name},
                {"step_count", job.steps.size()},
                {"condition", job.condition_expr.value_or("")},
                {"needs", job.needs},
                {"tags", job.tags},
            });
        }
        j["jobs"] = std::move(jobs);

        json trig = json::array();
        for (const auto* t : triggers_for_target(
                 reg, wf.workflow_id, wf.workflow_name)) {
            auto info = trigger_info(*t, tz);
            trig.push_back({
                {"id", t->trigger_id},
                {"type", info.type},
                {"schedule", info.schedule_expr},
                {"next_fire", info.next_fire},
                {"enabled", t->enabled},
            });
        }
        j["triggers"] = std::move(trig);
        return j;
    }

    /// JSON for a job definition with scope information.
    json job_json(const engine::JobDef& job,
                  const std::string& scope,
                  const std::string& scope_kind) {
        json j;
        j["id"] = job.job_id;
        j["name"] = job.job_name;
        j["scope"] = scope;
        j["scope_kind"] = scope_kind;
        j["step_count"] = job.steps.size();
        j["condition"] = job.condition_expr.value_or("");
        json needs = json::array();
        for (const auto& n : job.needs) needs.push_back(n);
        j["needs"] = std::move(needs);
        json tags = json::array();
        for (const auto& t : job.tags) tags.push_back(t);
        j["tags"] = std::move(tags);

        json steps = json::array();
        for (const auto& s : job.steps) {
            steps.push_back({
                {"name", s.step_name},
                {"command", s.command},
                {"working_dir", s.working_dir.string()},
                {"timeout_s", s.timeout
                    ? static_cast<int64_t>(s.timeout->count()) : -1},
            });
        }
        j["steps"] = std::move(steps);
        return j;
    }

    /// JSON for a watch group definition.
    json watch_json(const watch::WatchGroupDef& g) {
        json j;
        j["id"] = g.group_id;
        j["name"] = g.group_name;
        j["mode"] = g.mode == watch::WatchMode::Native ? "native"
                  : g.mode == watch::WatchMode::Sample ? "sample" : "hybrid";
        j["enabled"] = g.enabled;
        j["source_path"] = g.source_path;
        j["max_depth"] = g.max_depth;
        j["sample_rate_s"] = g.sample_rate.count();
        j["hash_policy"] =
            g.hash_policy == watch::HashPolicy::Full ? "full" : "size+mtime";

        json items = json::array();
        for (const auto& i : g.watch_items) items.push_back(i);
        j["watch_items"] = std::move(items);
        json excludes = json::array();
        for (const auto& e : g.exclude_globs) excludes.push_back(e);
        j["exclude_globs"] = std::move(excludes);
        json tags = json::array();
        for (const auto& t : g.tags) tags.push_back(t);
        j["tags"] = std::move(tags);

        json rules = json::array();
        for (const auto& r : g.rules) {
            rules.push_back({
                {"name", r.rule_name},
                {"condition", r.condition},
                {"severity", r.severity},
                {"description", r.description},
                {"trigger_target", r.trigger_target.value_or("")},
                {"trigger_is_workflow", r.trigger_is_workflow},
            });
        }
        j["rules"] = std::move(rules);
        return j;
    }

    /// YAML source text for a definition (reads the source file).
    std::string yaml_source(const std::string& source_path) {
        if (source_path.empty()) {
            return "# (inline definition — no source file)";
        }
        auto content = read_text_file(source_path);
        if (!content) {
            return "# (source file not readable: " + source_path + ")";
        }
        return *content;
    }

    /// Resolve a possibly-truncated run id (§1.1). Returns full id or "".
    std::string resolve_run_id(const std::string& input,
                               std::vector<std::string>& candidates) {
        if (!deps.reader) return "";
        auto pr = deps.reader->resolve_run_id_prefix(input);
        if (pr.status == persist::QueryReader::PrefixResult::kExact ||
            pr.status == persist::QueryReader::PrefixResult::kUnique) {
            return pr.resolved_id;
        }
        candidates = std::move(pr.candidates);
        return "";
    }

    // ── Write API helpers (§3.8.2) ──────────────────────────────────

    /// Append an audit entry (no-op without a DB writer).
    void audit(const std::string& action, const std::string& target,
               const std::string& detail_json = "{}") {
        if (deps.db_writer) {
            deps.db_writer->enqueue(persist::InsertAuditEntry{
                .actor = "http",
                .action = action,
                .target = target,
                .detail_json = detail_json,
            });
        }
    }

    /// Structured validation-error body (§3.8.2: errors with locations).
    static json validation_error_body(
        const std::vector<config::YamlError>& errors) {
        json arr = json::array();
        for (const auto& e : errors) {
            arr.push_back({
                {"file", e.file},
                {"path", e.path},
                {"message", e.message},
            });
        }
        return {
            {"error", "validation failed"},
            {"errors", std::move(arr)},
        };
    }

    /// Reload after a successful write; logs and reports failures
    /// without failing the request (the file is already durable).
    bool reload_after_write() {
        if (!deps.reload_config) return false;
        return deps.reload_config();
    }

    /// Resolve the YAML file backing a workflow by name: registry
    /// source_path first, then the conventional <dir>/<name>.yaml.
    std::optional<std::filesystem::path> resolve_workflow_file(
        const std::string& name) {
        if (auto reg = registry()) {
            for (const auto* wf : reg->workflows()) {
                if (wf->workflow_name == name && !wf->source_path.empty())
                    return std::filesystem::path(wf->source_path);
            }
        }
        auto candidate = deps.workflows_dir / (name + ".yaml");
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) return candidate;
        return std::nullopt;
    }

    /// Resolve the YAML file backing a watch group by name.
    std::optional<std::filesystem::path> resolve_watch_file(
        const std::string& name) {
        if (auto reg = registry()) {
            for (const auto& g : reg->watch_groups()) {
                if (g.group_name == name && !g.source_path.empty())
                    return std::filesystem::path(g.source_path);
            }
        }
        auto candidate = deps.watch_groups_dir / (name + ".yaml");
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) return candidate;
        return std::nullopt;
    }

    // ── Routes ─────────────────────────────────────────────────────

    void register_routes() {
        register_health_and_metrics();
        register_static_routes();
        register_page_routes();
        register_api_routes();
        register_write_api_routes();
        register_sse_routes();

        // ── CORS preflight ─────────────────────────────────────────
        if (config.enable_cors) {
            svr.Options(".*", [this](const httplib::Request&,
                                      httplib::Response& res) {
                res.set_header("Access-Control-Allow-Origin",
                               config.cors_origins);
                res.set_header("Access-Control-Allow-Methods",
                               "GET, POST, PUT, DELETE, OPTIONS");
                res.set_header("Access-Control-Allow-Headers",
                               "Authorization, Content-Type");
                res.status = 204;
            });
        }
    }

    void register_health_and_metrics() {
        svr.Get("/health", [this](const httplib::Request&,
                                   httplib::Response& res) {
            json j;
            j["status"] = "ok";
            j["uptime_s"] = deps.get_uptime ? deps.get_uptime() : 0.0;
            res.set_content(j.dump(), "application/json");
            set_cors(res);
        });

        svr.Get("/metrics", [this](const httplib::Request&,
                                    httplib::Response& res) {
            if (deps.metrics) {
                res.set_content(deps.metrics->to_prometheus(),
                                "text/plain; version=0.0.4; charset=utf-8");
            } else {
                res.set_content("# no metrics\n", "text/plain");
            }
        });
    }

    void register_static_routes() {
        // Static assets: external web root first, embedded fallback (§3.1).
        svr.Get(R"(/static/(.*))",
            [this](const httplib::Request& req, httplib::Response& res) {
                auto path = req.matches[1].str();
                auto asset = catalog.load("static/" + path);
                if (!asset) {
                    res.status = 404;
                    res.set_content("not found", "text/plain");
                    return;
                }
                res.set_content(asset->content,
                                web::WebCatalog::content_type(path));
                if (asset->source == web::AssetSource::Embedded) {
                    res.set_header("Cache-Control", "max-age=3600");
                } else {
                    // Hot-editable override — always revalidate.
                    res.set_header("Cache-Control", "no-cache");
                }
            });

        svr.Get("/favicon.ico",
            [this](const httplib::Request&, httplib::Response& res) {
                auto asset = catalog.load("static/img/kairos-logo.svg");
                if (!asset) {
                    res.status = 404;
                    return;
                }
                res.set_content(asset->content, "image/svg+xml");
                res.set_header("Cache-Control", "max-age=86400");
            });
    }

    void register_page_routes() {
        // ── Dashboard ─────────────────────────────────────────────
        svr.Get("/", [this](const httplib::Request&,
                             httplib::Response& res) {
            try {
                double uptime = deps.get_uptime ? deps.get_uptime() : 0.0;

                auto runs = deps.reader
                    ? deps.reader->query_recent_runs(10)
                    : std::vector<persist::QueryReader::RunSummary>{};

                auto stats = deps.reader
                    ? deps.reader->query_run_stats()
                    : persist::QueryReader::RunStats{};

                json data;
                data["total_runs"] = stats.total_runs;
                data["active_runs"] = stats.active_runs;
                data["runs_today"] = stats.runs_today;
                data["failures_today"] = stats.failures_today;
                int up_h = static_cast<int>(uptime / 3600);
                int up_m = static_cast<int>(
                    std::fmod(uptime, 3600.0) / 60.0);
                data["uptime"] = std::to_string(up_h) + "h " +
                                 std::to_string(up_m) + "m";

                json runs_arr = json::array();
                for (const auto& r : runs) {
                    runs_arr.push_back(run_json(r));
                }
                data["recent_runs"] = std::move(runs_arr);

                json events_arr = json::array();
                if (deps.reader) {
                    auto events = deps.reader->query_watch_events(10);
                    for (const auto& e : events) {
                        json j;
                        j["event_uid"] = e.event_uid;
                        j["event_uid_short"] = truncate(e.event_uid);
                        j["watch_group"] = e.watch_group;
                        j["rule_name"] = e.rule_name;
                        j["event_type"] = e.event_type;
                        auto files = files_display(e.affected_files_json);
                        j["files_summary"] = std::move(files.summary);
                        j["created_at"] =
                            platform::format_display_time(e.created_at, tz);
                        events_arr.push_back(std::move(j));
                    }
                }
                data["recent_events"] = std::move(events_arr);

                send_page(res, "templates/dashboard.html", std::move(data),
                          "Dashboard", "dashboard");
            } catch (const std::exception& e) {
                spdlog::error("Dashboard render error: {}", e.what());
                send_error_page(res, 500, "Render Error", e.what());
            }
        });

        // ── Workflows list ────────────────────────────────────────
        svr.Get("/workflows", [this](const httplib::Request&,
                                      httplib::Response& res) {
            try {
                auto reg = registry();
                json arr = json::array();
                if (reg) {
                    for (const auto* wf : reg->workflows()) {
                        json j;
                        j["name"] = wf->workflow_name;
                        j["id"] = wf->workflow_id;
                        j["job_count"] = wf->jobs.size();
                        json tags = json::array();
                        for (const auto& t : wf->tags) tags.push_back(t);
                        j["tags"] = std::move(tags);

                        // Trigger summary line.
                        auto trig = triggers_for_target(
                            *reg, wf->workflow_id, wf->workflow_name);
                        std::string summary;
                        for (const auto* t : trig) {
                            auto info = trigger_info(*t, tz);
                            if (!summary.empty()) summary += "; ";
                            summary += info.type + " " + info.schedule_expr;
                        }
                        j["trigger_summary"] =
                            summary.empty() ? "manual" : summary;

                        // Last run.
                        j["last_status"] = "";
                        j["last_badge"] = "secondary";
                        j["last_when"] = "";
                        if (deps.reader) {
                            auto last = deps.reader->query_recent_runs(
                                1, "", wf->workflow_name);
                            if (!last.empty()) {
                                j["last_status"] = last[0].status;
                                j["last_badge"] =
                                    status_to_badge(last[0].status);
                                j["last_when"] = platform::format_relative(
                                    last[0].start_ts);
                            }
                        }
                        arr.push_back(std::move(j));
                    }
                }
                json data;
                data["workflows"] = std::move(arr);
                send_page(res, "templates/workflows/list.html",
                          std::move(data), "Workflows", "workflows");
            } catch (const std::exception& e) {
                spdlog::error("Workflows page error: {}", e.what());
                send_error_page(res, 500, "Render Error", e.what());
            }
        });

        // ── Workflow YAML editor (§3.8.3/§3.8.4) ──────────────────
        svr.Get("/workflows/new",
            [this](const httplib::Request&, httplib::Response& res) {
                json data;
                data["mode"] = "new";
                data["name"] = "";
                data["kind"] = "workflow";
                data["yaml"] =
                    "name: my-workflow\n"
                    "tags: []\n"
                    "jobs:\n"
                    "  build:\n"
                    "    steps:\n"
                    "      - name: do-it\n"
                    "        run: echo hello\n";
                data["save_url"] = "/api/v1/workflows";
                data["save_method"] = "POST";
                send_page(res, "templates/workflows/edit.html",
                          std::move(data), "New Workflow", "workflows");
            });

        svr.Get(R"(/workflows/([^/]+)/edit)",
            [this](const httplib::Request& req, httplib::Response& res) {
                auto name = req.matches[1].str();
                auto reg = registry();
                if (!reg) {
                    send_error_page(res, 503, "No Registry",
                                    "Workflow registry not available.");
                    return;
                }
                const engine::WorkflowDef* wf = reg->workflow_by_name(name);
                if (!wf) wf = reg->workflow(name);
                if (!wf) {
                    send_error_page(res, 404, "Not Found",
                                    "Workflow '" + name + "' not found.");
                    return;
                }
                json data;
                data["mode"] = "edit";
                data["name"] = wf->workflow_name;
                data["kind"] = "workflow";
                data["yaml"] = yaml_source(wf->source_path);
                data["save_url"] = "/api/v1/workflows/" + wf->workflow_name;
                data["save_method"] = "PUT";
                send_page(res, "templates/workflows/edit.html",
                          std::move(data),
                          "Edit " + wf->workflow_name, "workflows");
            });

        // ── Workflow detail ───────────────────────────────────────
        svr.Get(R"(/workflows/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                auto name = req.matches[1].str();
                try {
                    auto reg = registry();
                    if (!reg) {
                        send_error_page(res, 503, "No Registry",
                                        "Workflow registry not available.");
                        return;
                    }
                    const engine::WorkflowDef* wf = nullptr;
                    for (const auto* w : reg->workflows()) {
                        if (w->workflow_name == name ||
                            w->workflow_id == name) {
                            wf = w;
                            break;
                        }
                    }
                    if (!wf) {
                        send_error_page(res, 404, "Not Found",
                                        "Workflow '" + name + "' not found.");
                        return;
                    }

                    json data;
                    data["wf"] = workflow_json(*wf, *reg);
                    data["wf"]["yaml"] = yaml_source(wf->source_path);

                    // Jobs with steps and level info.
                    json jobs = json::array();
                    for (int lvl = 0; lvl < wf->dag.level_count(); ++lvl) {
                        for (const auto& job_id :
                             wf->dag.jobs_at_level(lvl)) {
                            const auto& node = wf->dag.node(job_id);
                            const auto* def = reg->job(job_id);

                            json jj;
                            jj["id"] = job_id;
                            jj["name"] = node.job_name;
                            jj["level"] = lvl;
                            std::string needs;
                            for (const auto& n : node.needs) {
                                if (!needs.empty()) needs += ", ";
                                try {
                                    needs += wf->dag.node(n).job_name;
                                } catch (...) {
                                    needs += n;
                                }
                            }
                            jj["needs_names"] = needs;
                            jj["condition"] =
                                node.condition_expr.value_or("");
                            json tags = json::array();
                            if (def) {
                                for (const auto& t : def->tags)
                                    tags.push_back(t);
                            }
                            jj["tags"] = std::move(tags);

                            json steps = json::array();
                            if (def) {
                                for (const auto& s : def->steps) {
                                    steps.push_back({
                                        {"name", s.step_name},
                                        {"command", s.command},
                                    });
                                }
                            }
                            jj["steps"] = std::move(steps);
                            jobs.push_back(std::move(jj));
                        }
                    }
                    data["jobs"] = std::move(jobs);

                    // Dry-run plan (§11.7).
                    auto plan = engine::build_explain_plan(
                        *wf, deps.reader);
                    json plan_arr = json::array();
                    for (const auto& p : plan.entries) {
                        plan_arr.push_back({
                            {"job_name", p.job_name},
                            {"level", p.level},
                            {"action", std::string(
                                engine::plan_action_to_string(p.action))},
                            {"reason", p.reason},
                        });
                    }
                    data["plan"] = std::move(plan_arr);

                    // Recent runs.
                    json last_runs = json::array();
                    if (deps.reader) {
                        auto runs = deps.reader->query_recent_runs(
                            5, "", wf->workflow_name);
                        for (const auto& r : runs) {
                            last_runs.push_back(run_json(r));
                        }
                    }
                    data["last_runs"] = std::move(last_runs);

                    // DAG visualization (§3.7): definition view, nodes
                    // link to their job pages.
                    data["dag_svg"] = dag::render_dag_svg(
                        wf->dag, "/jobs");

                    send_page(res, "templates/workflows/detail.html",
                              std::move(data),
                              "Workflow " + wf->workflow_name,
                              "workflows");
                } catch (const std::exception& e) {
                    spdlog::error("Workflow detail error: {}", e.what());
                    send_error_page(res, 500, "Render Error", e.what());
                }
            });

        // ── Jobs list ─────────────────────────────────────────────
        svr.Get("/jobs", [this](const httplib::Request&,
                                 httplib::Response& res) {
            try {
                auto reg = registry();
                json arr = json::array();
                if (reg) {
                    for (const auto* wf : reg->workflows()) {
                        for (const auto& j : wf->jobs) {
                            arr.push_back(job_json(
                                j, wf->workflow_name, "workflow"));
                        }
                    }
                    for (const auto* j : reg->standalone_jobs()) {
                        arr.push_back(job_json(*j, j->job_name,
                                               "standalone"));
                    }
                }
                json data;
                data["jobs"] = std::move(arr);
                send_page(res, "templates/jobs/list.html", std::move(data),
                          "Jobs", "jobs");
            } catch (const std::exception& e) {
                spdlog::error("Jobs page error: {}", e.what());
                send_error_page(res, 500, "Render Error", e.what());
            }
        });

        // ── Job detail ────────────────────────────────────────────
        svr.Get(R"(/jobs/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                auto id = req.matches[1].str();
                try {
                    auto reg = registry();
                    if (!reg) {
                        send_error_page(res, 503, "No Registry",
                                        "Workflow registry not available.");
                        return;
                    }

                    // Find the job and its scope.
                    const engine::JobDef* job = nullptr;
                    std::string scope, scope_kind;
                    for (const auto* wf : reg->workflows()) {
                        for (const auto& j : wf->jobs) {
                            if (j.job_id == id || j.job_name == id) {
                                job = &j;
                                scope = wf->workflow_name;
                                scope_kind = "workflow";
                                break;
                            }
                        }
                        if (job) break;
                    }
                    if (!job) {
                        if (auto* sj = reg->standalone_job_by_name(id)) {
                            job = sj;
                            scope = sj->job_name;
                            scope_kind = "standalone";
                        }
                    }
                    if (!job) {
                        send_error_page(res, 404, "Not Found",
                                        "Job '" + id + "' not found.");
                        return;
                    }

                    json data;
                    data["job"] = job_json(*job, scope, scope_kind);
                    data["job"]["needs"] = json::array();
                    for (const auto& n : job->needs) {
                        data["job"]["needs"].push_back(n);
                    }

                    json steps = json::array();
                    for (size_t i = 0; i < job->steps.size(); ++i) {
                        const auto& s = job->steps[i];
                        steps.push_back({
                            {"index", i + 1},
                            {"name", s.step_name},
                            {"command", s.command},
                            {"working_dir", s.working_dir.string()},
                            {"timeout_s", s.timeout
                                ? static_cast<int64_t>(s.timeout->count())
                                : -1},
                        });
                    }
                    data["steps"] = std::move(steps);

                    json runs = json::array();
                    if (deps.reader) {
                        auto rr = deps.reader->query_recent_runs(
                            10, "", job->job_name);
                        for (const auto& r : rr) {
                            runs.push_back(run_json(r));
                        }
                    }
                    data["recent_runs"] = std::move(runs);

                    send_page(res, "templates/jobs/detail.html",
                              std::move(data), "Job " + job->job_name,
                              "jobs");
                } catch (const std::exception& e) {
                    spdlog::error("Job detail error: {}", e.what());
                    send_error_page(res, 500, "Render Error", e.what());
                }
            });

        // ── Runs list ─────────────────────────────────────────────
        svr.Get("/runs", [this](const httplib::Request& req,
                                 httplib::Response& res) {
            try {
                int limit = 50;
                std::string status_filter, wf_filter, tag_filter;
                if (req.has_param("limit")) {
                    try {
                        limit = std::stoi(req.get_param_value("limit"));
                    } catch (...) { /* keep default */ }
                }
                if (req.has_param("status"))
                    status_filter = req.get_param_value("status");
                if (req.has_param("workflow"))
                    wf_filter = req.get_param_value("workflow");
                if (req.has_param("tag"))
                    tag_filter = req.get_param_value("tag");

                json arr = json::array();
                if (deps.reader) {
                    auto runs = deps.reader->query_recent_runs(
                        limit, status_filter, wf_filter);
                    for (const auto& r : runs) {
                        auto j = run_json(r);
                        if (!tag_filter.empty()) {
                            // AND semantics over comma-separated tags (§5.2).
                            auto tags = parse_tags_json(r.tags_json);
                            bool keep = true;
                            std::string tag;
                            std::istringstream ss(tag_filter);
                            while (std::getline(ss, tag, ',')) {
                                if (std::find(tags.begin(), tags.end(),
                                              tag) == tags.end()) {
                                    keep = false;
                                    break;
                                }
                            }
                            if (!keep) continue;
                        }
                        arr.push_back(std::move(j));
                    }
                }

                json data;
                data["runs"] = std::move(arr);
                data["statuses"] = {"SUCCESS", "FAILURE", "RUNNING",
                                    "CANCELLED"};
                data["filters"]["status"] = status_filter;
                data["filters"]["workflow"] = wf_filter;
                data["filters"]["limit"] = limit;
                send_page(res, "templates/runs/list.html", std::move(data),
                          "Runs", "runs");
            } catch (const std::exception& e) {
                spdlog::error("Runs page error: {}", e.what());
                send_error_page(res, 500, "Render Error", e.what());
            }
        });

        // ── Run detail (prefix-resolved) ──────────────────────────
        svr.Get(R"(/runs/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                auto input = req.matches[1].str();

                // Avoid matching /runs/xxx when it's /api/... or /sse/...
                if (input == "api" || input == "sse") return;

                try {
                    if (!deps.reader) {
                        send_error_page(res, 503, "No Database",
                                        "Run history is not available.");
                        return;
                    }

                    std::vector<std::string> candidates;
                    auto run_id = resolve_run_id(input, candidates);
                    if (run_id.empty()) {
                        send_error_page(
                            res, 404, "Not Found",
                            candidates.empty()
                                ? "Run '" + input + "' not found."
                                : "Ambiguous run ID '" + input + "'.");
                        return;
                    }

                    auto detail = deps.reader->get_run_detail(run_id);
                    if (!detail) {
                        send_error_page(res, 404, "Not Found",
                                        "Run '" + run_id + "' not found.");
                        return;
                    }

                    json data = run_json(detail->run);
                    data["is_running"] = detail->run.status == "RUNNING";

                    json jobs = json::array();
                    for (const auto& jd : detail->jobs) {
                        json jj;
                        jj["job_name"] = jd.job_name;
                        jj["status"] = jd.status;
                        jj["badge"] = status_to_badge(jd.status);
                        jj["exit_code"] = jd.exit_code;
                        jj["duration_ms"] = jd.duration_ms;
                        jj["condition"] = jd.condition_result;

                        json steps = json::array();
                        for (const auto& s : jd.steps) {
                            steps.push_back({
                                {"step_name", s.step_name},
                                {"status", s.status},
                                {"badge", status_to_badge(s.status)},
                                {"exit_code", s.exit_code},
                                {"command", s.command},
                            });
                        }
                        jj["steps"] = std::move(steps);
                        jobs.push_back(std::move(jj));
                    }
                    data["jobs"] = std::move(jobs);

                    // Initial log snapshot; live runs stream via SSE (§3.6).
                    json logs = json::array();
                    auto chunks = deps.reader->get_log_chunks(run_id, 0, 200);
                    for (const auto& c : chunks) {
                        logs.push_back({
                            {"job_id", c.job_id},
                            {"stream", c.stream},
                            {"content", c.content},
                        });
                    }
                    data["logs"] = std::move(logs);

                    // DAG visualization (§3.7): run view, nodes colored
                    // by per-job status when the target is a workflow.
                    data["dag_svg"] = "";
                    if (auto reg = registry()) {
                        if (const auto* wf = reg->workflow(
                                detail->run.target_id)) {
                            std::map<std::string, dag::NodeStatus> sts;
                            for (const auto& jd : detail->jobs) {
                                sts[jd.job_id] = dag::NodeStatus{
                                    jd.status, jd.duration_ms};
                            }
                            data["dag_svg"] = dag::render_dag_svg(
                                wf->dag, "/jobs", sts);
                        }
                    }

                    send_page(res, "templates/runs/detail.html",
                              std::move(data),
                              "Run " + truncate(run_id), "runs");
                } catch (const std::exception& e) {
                    spdlog::error("Run detail error: {}", e.what());
                    send_error_page(res, 500, "Render Error", e.what());
                }
            });

        // ── Events list ───────────────────────────────────────────
        svr.Get("/events", [this](const httplib::Request& req,
                                   httplib::Response& res) {
            try {
                int limit = 50;
                std::string group, severity;
                if (req.has_param("limit")) {
                    try {
                        limit = std::stoi(req.get_param_value("limit"));
                    } catch (...) { /* keep default */ }
                }
                if (req.has_param("watch_group"))
                    group = req.get_param_value("watch_group");
                if (req.has_param("severity"))
                    severity = req.get_param_value("severity");

                json arr = json::array();
                if (deps.reader) {
                    auto events = deps.reader->query_watch_events(
                        limit, group);
                    for (const auto& e : events) {
                        if (!severity.empty() && e.severity != severity)
                            continue;
                        json j;
                        j["event_uid"] = e.event_uid;
                        j["event_uid_short"] = truncate(e.event_uid);
                        j["watch_group"] = e.watch_group;
                        j["rule_name"] = e.rule_name;
                        j["event_type"] = e.event_type;
                        j["severity"] = e.severity;
                        auto files = files_display(e.affected_files_json);
                        j["files_summary"] = std::move(files.summary);
                        j["files_list"] = std::move(files.list);
                        j["created_at"] =
                            platform::format_display_time(e.created_at, tz);
                        arr.push_back(std::move(j));
                    }
                }
                json data;
                data["events"] = std::move(arr);
                send_page(res, "templates/events/list.html", std::move(data),
                          "Events", "events");
            } catch (const std::exception& e) {
                spdlog::error("Events page error: {}", e.what());
                send_error_page(res, 500, "Render Error", e.what());
            }
        });

        // ── Event detail ──────────────────────────────────────────
        svr.Get(R"(/events/([0-9]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                auto uid = req.matches[1].str();
                try {
                    if (!deps.reader) {
                        send_error_page(res, 503, "No Database",
                                        "Event history is not available.");
                        return;
                    }
                    auto e = deps.reader->get_watch_event(uid);
                    if (!e) {
                        send_error_page(res, 404, "Not Found",
                                        "Event '" + uid + "' not found.");
                        return;
                    }
                    json data;
                    data["event_uid_short"] = truncate(e->event_uid);
                    json ev;
                    ev["event_uid"] = e->event_uid;
                    ev["watch_group"] = e->watch_group;
                    ev["rule_name"] = e->rule_name;
                    ev["event_type"] = e->event_type;
                    ev["severity"] = e->severity;
                    ev["sample_epoch"] = e->sample_epoch;
                    ev["created_at"] =
                        platform::format_display_time(e->created_at, tz);
                    auto files = files_display(e->affected_files_json);
                    ev["files_list"] = std::move(files.list);
                    data["event"] = std::move(ev);

                    // Pretty-print details JSON if present.
                    std::string pretty;
                    if (!e->details_json.empty()) {
                        try {
                            pretty = json::parse(e->details_json).dump(2);
                        } catch (...) {
                            pretty = e->details_json;
                        }
                    }
                    data["event"]["details_pretty"] = pretty;

                    send_page(res, "templates/events/detail.html",
                              std::move(data), "Event " + uid, "events");
                } catch (const std::exception& ex) {
                    spdlog::error("Event detail error: {}", ex.what());
                    send_error_page(res, 500, "Render Error", ex.what());
                }
            });

        // ── Watches list ──────────────────────────────────────────
        svr.Get("/watches", [this](const httplib::Request&,
                                    httplib::Response& res) {
            try {
                auto reg = registry();
                json arr = json::array();
                if (reg) {
                    for (const auto& g : reg->watch_groups()) {
                        json j;
                        j["name"] = g.group_name;
                        j["id"] = g.group_id;
                        j["mode"] = g.mode == watch::WatchMode::Native
                            ? "native"
                            : g.mode == watch::WatchMode::Sample
                                ? "sample" : "hybrid";
                        j["items_count"] = g.watch_items.size();
                        j["rules_count"] = g.rules.size();
                        j["enabled"] = g.enabled;
                        json tags = json::array();
                        for (const auto& t : g.tags) tags.push_back(t);
                        j["tags"] = std::move(tags);
                        arr.push_back(std::move(j));
                    }
                }
                json data;
                data["groups"] = std::move(arr);
                send_page(res, "templates/watches/list.html",
                          std::move(data), "Watches", "watches");
            } catch (const std::exception& e) {
                spdlog::error("Watches page error: {}", e.what());
                send_error_page(res, 500, "Render Error", e.what());
            }
        });

        // ── Watch group YAML editor (§3.8.3/§3.8.4) ───────────────
        svr.Get("/watches/new",
            [this](const httplib::Request&, httplib::Response& res) {
                json data;
                data["mode"] = "new";
                data["name"] = "";
                data["kind"] = "watch";
                data["yaml"] =
                    "watch_groups:\n"
                    "  - name: my-watch\n"
                    "    watch_items:\n"
                    "      - \"/tmp/**/*.log\"\n"
                    "    rules:\n"
                    "      - name: created\n"
                    "        condition: \"event.type == 'created'\"\n"
                    "        severity: info\n";
                data["save_url"] = "/api/v1/watches";
                data["save_method"] = "POST";
                send_page(res, "templates/watches/edit.html",
                          std::move(data), "New Watch Group", "watches");
            });

        svr.Get(R"(/watches/([^/]+)/edit)",
            [this](const httplib::Request& req, httplib::Response& res) {
                auto name = req.matches[1].str();
                auto reg = registry();
                if (!reg) {
                    send_error_page(res, 503, "No Registry",
                                    "Workflow registry not available.");
                    return;
                }
                const watch::WatchGroupDef* group = nullptr;
                for (const auto& g : reg->watch_groups()) {
                    if (g.group_name == name || g.group_id == name) {
                        group = &g;
                        break;
                    }
                }
                if (!group) {
                    send_error_page(res, 404, "Not Found",
                                    "Watch group '" + name +
                                    "' not found.");
                    return;
                }
                json data;
                data["mode"] = "edit";
                data["name"] = group->group_name;
                data["kind"] = "watch";
                data["yaml"] = yaml_source(group->source_path);
                data["save_url"] = "/api/v1/watches/" + group->group_name;
                data["save_method"] = "PUT";
                send_page(res, "templates/watches/edit.html",
                          std::move(data),
                          "Edit " + group->group_name, "watches");
            });

        // ── Watch group detail ────────────────────────────────────
        svr.Get(R"(/watches/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                auto name = req.matches[1].str();
                try {
                    auto reg = registry();
                    if (!reg) {
                        send_error_page(res, 503, "No Registry",
                                        "Workflow registry not available.");
                        return;
                    }
                    const watch::WatchGroupDef* group = nullptr;
                    for (const auto& g : reg->watch_groups()) {
                        if (g.group_name == name || g.group_id == name) {
                            group = &g;
                            break;
                        }
                    }
                    if (!group) {
                        send_error_page(res, 404, "Not Found",
                                        "Watch group '" + name +
                                        "' not found.");
                        return;
                    }

                    json data;
                    data["group"] = watch_json(*group);
                    data["group"]["yaml"] = yaml_source(group->source_path);

                    json events = json::array();
                    if (deps.reader) {
                        auto evs = deps.reader->query_watch_events(
                            10, group->group_name);
                        for (const auto& e : evs) {
                            events.push_back({
                                {"event_uid", e.event_uid},
                                {"event_uid_short", truncate(e.event_uid)},
                                {"rule_name", e.rule_name},
                                {"event_type", e.event_type},
                                {"created_at", platform::format_display_time(
                                                   e.created_at, tz)},
                            });
                        }
                    }
                    data["recent_events"] = std::move(events);

                    send_page(res, "templates/watches/detail.html",
                              std::move(data),
                              "Watch " + group->group_name, "watches");
                } catch (const std::exception& e) {
                    spdlog::error("Watch detail error: {}", e.what());
                    send_error_page(res, 500, "Render Error", e.what());
                }
            });

        // ── KEL playground (§3.8.7) ────────────────────────────────
        svr.Get("/kel",
            [this](const httplib::Request&, httplib::Response& res) {
                json data;
                send_page(res, "templates/kel.html", std::move(data),
                          "KEL Playground", "kel");
            });

        // ── Run comparison (§3.8.6) ────────────────────────────────
        svr.Get(R"(/runs/([^/]+)/vs/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                auto a_in = req.matches[1].str();
                auto b_in = req.matches[2].str();
                try {
                    if (!deps.reader) {
                        send_error_page(res, 503, "No Database",
                                        "Run history is not available.");
                        return;
                    }
                    std::vector<std::string> candidates;
                    auto a_id = resolve_run_id(a_in, candidates);
                    if (a_id.empty()) {
                        send_error_page(res, 404, "Not Found",
                                        "Run '" + a_in + "' not found.");
                        return;
                    }
                    auto b_id = resolve_run_id(b_in, candidates);
                    if (b_id.empty()) {
                        send_error_page(res, 404, "Not Found",
                                        "Run '" + b_in + "' not found.");
                        return;
                    }

                    auto da = deps.reader->get_run_detail(a_id);
                    auto db_ = deps.reader->get_run_detail(b_id);
                    if (!da || !db_) {
                        send_error_page(res, 404, "Not Found",
                                        "Run detail unavailable.");
                        return;
                    }

                    // Union of job names across both runs, in the
                    // order they appear in A then B.
                    std::vector<std::string> job_order;
                    std::map<std::string, bool> seen;
                    for (const auto& j : da->jobs)
                        if (seen.emplace(j.job_name, true).second)
                            job_order.push_back(j.job_name);
                    for (const auto& j : db_->jobs)
                        if (seen.emplace(j.job_name, true).second)
                            job_order.push_back(j.job_name);

                    auto find_job =
                        [](const persist::QueryReader::RunDetail& d,
                           const std::string& name)
                        -> const persist::QueryReader::JobDetail* {
                        for (const auto& j : d.jobs)
                            if (j.job_name == name) return &j;
                        return nullptr;
                    };

                    json data;
                    data["a"] = run_json(da->run);
                    data["b"] = run_json(db_->run);
                    data["a_id"] = a_id;
                    data["b_id"] = b_id;

                    json rows = json::array();
                    for (const auto& name : job_order) {
                        const auto* ja = find_job(*da, name);
                        const auto* jb = find_job(*db_, name);
                        json row;
                        row["job"] = name;
                        row["a_status"] = ja ? ja->status : "—";
                        row["a_badge"] =
                            ja ? status_to_badge(ja->status) : "secondary";
                        row["a_exit"] = ja ? std::to_string(ja->exit_code)
                                           : "—";
                        row["a_dur"] =
                            ja ? std::to_string(ja->duration_ms) + "ms"
                               : "—";
                        row["b_status"] = jb ? jb->status : "—";
                        row["b_badge"] =
                            jb ? status_to_badge(jb->status) : "secondary";
                        row["b_exit"] = jb ? std::to_string(jb->exit_code)
                                           : "—";
                        row["b_dur"] =
                            jb ? std::to_string(jb->duration_ms) + "ms"
                               : "—";
                        row["same"] =
                            ja && jb && ja->status == jb->status &&
                            ja->exit_code == jb->exit_code;
                        rows.push_back(std::move(row));
                    }
                    data["jobs"] = std::move(rows);

                    send_page(res, "templates/runs/compare.html",
                              std::move(data),
                              "Compare runs", "runs");
                } catch (const std::exception& e) {
                    spdlog::error("Run compare error: {}", e.what());
                    send_error_page(res, 500, "Render Error", e.what());
                }
            });
        // ── Triggers list ─────────────────────────────────────────
        svr.Get("/triggers", [this](const httplib::Request&,
                                     httplib::Response& res) {
            try {
                auto reg = registry();
                json arr = json::array();
                if (reg) {
                    for (const auto& t : reg->triggers()) {
                        auto info = trigger_info(t, tz);
                        arr.push_back({
                            {"id", truncate(t.trigger_id, 16)},
                            {"target_name", t.target_name},
                            {"target_kind", t.target_kind ==
                                engine::TriggerEvent::TargetKind::Workflow
                                ? "workflow" : "job"},
                            {"type", info.type},
                            {"expression", info.schedule_expr},
                            {"next_fire", info.next_fire},
                            {"when", info.relative},
                        });
                    }
                }
                json data;
                data["triggers"] = std::move(arr);
                send_page(res, "templates/triggers/list.html",
                          std::move(data), "Triggers", "triggers");
            } catch (const std::exception& e) {
                spdlog::error("Triggers page error: {}", e.what());
                send_error_page(res, 500, "Render Error", e.what());
            }
        });
    }

    void register_api_routes() {
        using persist::QueryReader;

        // ── Workflows ─────────────────────────────────────────────
        svr.Get("/api/v1/workflows",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                auto reg = registry();
                json arr = json::array();
                if (reg) {
                    for (const auto* wf : reg->workflows()) {
                        arr.push_back(workflow_json(*wf, *reg));
                    }
                }
                res.set_content(arr.dump(), "application/json");
                set_cors(res);
            });

        svr.Get(R"(/api/v1/workflows/([^/]+)/yaml)",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                auto reg = registry();
                if (!reg) {
                    res.status = 503;
                    res.set_content(R"({"error":"No registry"})",
                                    "application/json");
                    return;
                }
                auto name = req.matches[1].str();
                const engine::WorkflowDef* wf =
                    reg->workflow_by_name(name);
                if (!wf) wf = reg->workflow(name);
                if (!wf) {
                    res.status = 404;
                    res.set_content(R"({"error":"Workflow not found"})",
                                    "application/json");
                    return;
                }
                res.set_content(yaml_source(wf->source_path),
                                "text/yaml; charset=utf-8");
                set_cors(res);
            });

        svr.Get(R"(/api/v1/workflows/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                auto reg = registry();
                if (!reg) {
                    res.status = 503;
                    res.set_content(R"({"error":"No registry"})",
                                    "application/json");
                    return;
                }
                auto name = req.matches[1].str();
                const engine::WorkflowDef* wf = reg->workflow_by_name(name);
                if (!wf) wf = reg->workflow(name);
                if (!wf) {
                    res.status = 404;
                    res.set_content(R"({"error":"Workflow not found"})",
                                    "application/json");
                    return;
                }
                json j = workflow_json(*wf, *reg);
                j["yaml"] = yaml_source(wf->source_path);
                res.set_content(j.dump(2), "application/json");
                set_cors(res);
            });

        svr.Post(R"(/api/v1/workflows/([^/]+)/run)",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;

                auto name = req.matches[1].str();
                if (!deps.submit_run) {
                    res.status = 501;
                    res.set_content(R"({"error":"Run submission not configured"})",
                                    "application/json");
                    return;
                }

                // submit_run returns a correlation id; the pipeline mints
                // the actual run_id asynchronously. Resolve it with a
                // short bounded poll so callers can follow the run.
                auto correlation = deps.submit_run(name);
                if (correlation.empty()) {
                    res.status = 404;
                    res.set_content(R"({"error":"Workflow not found"})",
                                    "application/json");
                    return;
                }

                std::string run_id;
                for (int i = 0; i < 20 && run_id.empty(); ++i) {
                    if (deps.reader) {
                        if (auto id = deps.reader
                                ->get_run_id_by_correlation(correlation)) {
                            run_id = *id;
                            break;
                        }
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(100));
                }

                json j;
                j["correlation_id"] = correlation;
                j["run_id"] = run_id.empty() ? json(nullptr)
                                             : json(run_id);
                j["status"] = run_id.empty() ? "pending" : "submitted";
                res.set_content(j.dump(), "application/json");
                set_cors(res);
            });

        // ── Jobs ──────────────────────────────────────────────────
        svr.Get("/api/v1/jobs",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                auto reg = registry();
                json arr = json::array();
                if (reg) {
                    for (const auto* wf : reg->workflows()) {
                        for (const auto& j : wf->jobs) {
                            arr.push_back(job_json(
                                j, wf->workflow_name, "workflow"));
                        }
                    }
                    for (const auto* j : reg->standalone_jobs()) {
                        arr.push_back(job_json(*j, j->job_name,
                                               "standalone"));
                    }
                }
                res.set_content(arr.dump(), "application/json");
                set_cors(res);
            });

        svr.Get(R"(/api/v1/jobs/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                auto reg = registry();
                if (!reg) {
                    res.status = 503;
                    res.set_content(R"({"error":"No registry"})",
                                    "application/json");
                    return;
                }
                auto id = req.matches[1].str();

                const engine::JobDef* job = nullptr;
                std::string scope, scope_kind;
                for (const auto* wf : reg->workflows()) {
                    for (const auto& j : wf->jobs) {
                        if (j.job_id == id || j.job_name == id) {
                            job = &j;
                            scope = wf->workflow_name;
                            scope_kind = "workflow";
                            break;
                        }
                    }
                    if (job) break;
                }
                if (!job) {
                    if (auto* sj = reg->standalone_job(id)) {
                        job = sj;
                        scope = sj->job_name;
                        scope_kind = "standalone";
                    } else if (auto* sj = reg->standalone_job_by_name(id)) {
                        job = sj;
                        scope = sj->job_name;
                        scope_kind = "standalone";
                    }
                }
                if (!job) {
                    res.status = 404;
                    res.set_content(R"({"error":"Job not found"})",
                                    "application/json");
                    return;
                }

                json j = job_json(*job, scope, scope_kind);

                // Recent runs for this job target.
                if (deps.reader) {
                    auto runs = deps.reader->query_recent_runs(
                        10, "", job->job_name);
                    json arr = json::array();
                    for (const auto& r : runs) arr.push_back(run_json(r));
                    j["recent_runs"] = std::move(arr);
                }
                res.set_content(j.dump(2), "application/json");
                set_cors(res);
            });

        // ── Runs ──────────────────────────────────────────────────
        svr.Get("/api/v1/runs",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;

                int limit = 50;
                std::string status, workflow, since, tag;
                if (req.has_param("limit")) {
                    try {
                        limit = std::stoi(req.get_param_value("limit"));
                    } catch (...) { /* keep default */ }
                }
                if (req.has_param("status"))
                    status = req.get_param_value("status");
                if (req.has_param("workflow"))
                    workflow = req.get_param_value("workflow");
                if (req.has_param("since"))
                    since = req.get_param_value("since");
                if (req.has_param("tag"))
                    tag = req.get_param_value("tag");

                json arr = json::array();
                if (deps.reader) {
                    auto runs = deps.reader->query_recent_runs(
                        limit, status, workflow, since);
                    for (const auto& r : runs) {
                        if (!tag.empty()) {
                            auto tags = parse_tags_json(r.tags_json);
                            bool keep = true;
                            std::string one;
                            std::istringstream ss(tag);
                            while (std::getline(ss, one, ',')) {
                                if (std::find(tags.begin(), tags.end(), one)
                                        == tags.end()) {
                                    keep = false;
                                    break;
                                }
                            }
                            if (!keep) continue;
                        }
                        arr.push_back(run_json(r));
                    }
                }
                res.set_content(arr.dump(), "application/json");
                set_cors(res);
            });

        svr.Get(R"(/api/v1/runs/([^/]+)/logs)",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                if (!deps.reader) {
                    res.status = 503;
                    res.set_content(R"({"error":"No database reader"})",
                                    "application/json");
                    return;
                }
                auto input = req.matches[1].str();
                std::vector<std::string> candidates;
                auto run_id = resolve_run_id(input, candidates);
                if (run_id.empty()) {
                    res.status = 404;
                    json err = {{"error", "Run not found"},
                                {"run_id", input}};
                    if (!candidates.empty()) err["candidates"] = candidates;
                    res.set_content(err.dump(), "application/json");
                    return;
                }

                int64_t after_id = 0;
                int limit = 500;
                if (req.has_param("after")) {
                    try {
                        after_id = std::stoll(
                            req.get_param_value("after"));
                    } catch (...) { /* keep default */ }
                }
                if (req.has_param("limit")) {
                    try {
                        limit = std::stoi(req.get_param_value("limit"));
                    } catch (...) { /* keep default */ }
                }

                json arr = json::array();
                auto chunks = deps.reader->get_log_chunks(
                    run_id, after_id, limit);
                for (const auto& c : chunks) {
                    arr.push_back({
                        {"id", c.id},
                        {"run_id", c.run_id},
                        {"job_id", c.job_id},
                        {"step_id", c.step_id},
                        {"stream", c.stream},
                        {"content", c.content},
                        {"created_at", platform::format_display_time(
                                           c.created_at, tz)},
                    });
                }
                res.set_content(arr.dump(), "application/json");
                set_cors(res);
            });

        svr.Get(R"(/api/v1/runs/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                if (!deps.reader) {
                    res.status = 503;
                    res.set_content(R"({"error":"No database reader"})",
                                    "application/json");
                    return;
                }
                auto input = req.matches[1].str();
                std::vector<std::string> candidates;
                auto run_id = resolve_run_id(input, candidates);
                if (run_id.empty()) {
                    res.status = 404;
                    json err = {{"error", "Run not found"},
                                {"run_id", input}};
                    if (!candidates.empty()) err["candidates"] = candidates;
                    res.set_content(err.dump(), "application/json");
                    return;
                }

                auto detail = deps.reader->get_run_detail(run_id);
                if (!detail) {
                    res.status = 404;
                    res.set_content(R"({"error":"Run not found"})",
                                    "application/json");
                    return;
                }

                json j = run_json(detail->run);
                j["target_id"] = detail->run.target_id;

                json jobs = json::array();
                for (const auto& jd : detail->jobs) {
                    json jj;
                    jj["job_id"] = jd.job_id;
                    jj["job_name"] = jd.job_name;
                    jj["status"] = jd.status;
                    jj["exit_code"] = jd.exit_code;
                    jj["duration_ms"] = jd.duration_ms;
                    jj["condition_result"] = jd.condition_result;
                    jj["start_ts"] = platform::format_display_time(
                        jd.start_ts, tz);
                    jj["end_ts"] = platform::format_display_time(
                        jd.end_ts, tz);

                    json steps = json::array();
                    for (const auto& s : jd.steps) {
                        steps.push_back({
                            {"step_id", s.step_id},
                            {"step_name", s.step_name},
                            {"status", s.status},
                            {"exit_code", s.exit_code},
                            {"command", s.command},
                            {"duration_ms", s.duration_ms},
                        });
                    }
                    jj["steps"] = std::move(steps);
                    jobs.push_back(std::move(jj));
                }
                j["jobs"] = std::move(jobs);

                res.set_content(j.dump(2), "application/json");
                set_cors(res);
            });

        // ── Events ────────────────────────────────────────────────
        svr.Get("/api/v1/events",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;

                int limit = 50;
                std::string group, severity;
                if (req.has_param("limit")) {
                    try {
                        limit = std::stoi(req.get_param_value("limit"));
                    } catch (...) { /* keep default */ }
                }
                if (req.has_param("watch_group"))
                    group = req.get_param_value("watch_group");
                if (req.has_param("severity"))
                    severity = req.get_param_value("severity");

                json arr = json::array();
                if (deps.reader) {
                    auto events = deps.reader->query_watch_events(
                        limit, group);
                    for (const auto& e : events) {
                        if (!severity.empty() && e.severity != severity)
                            continue;
                        auto files = files_display(e.affected_files_json);
                        arr.push_back({
                            {"event_uid", e.event_uid},
                            {"watch_group", e.watch_group},
                            {"rule_name", e.rule_name},
                            {"event_type", e.event_type},
                            {"severity", e.severity},
                            {"affected_files", files.list},
                            {"details", e.details_json},
                            {"created_at", platform::format_display_time(
                                               e.created_at, tz)},
                        });
                    }
                }
                res.set_content(arr.dump(), "application/json");
                set_cors(res);
            });

        svr.Get(R"(/api/v1/events/([0-9]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                if (!deps.reader) {
                    res.status = 503;
                    res.set_content(R"({"error":"No database reader"})",
                                    "application/json");
                    return;
                }
                auto uid = req.matches[1].str();
                auto e = deps.reader->get_watch_event(uid);
                if (!e) {
                    res.status = 404;
                    res.set_content(R"({"error":"Event not found"})",
                                    "application/json");
                    return;
                }
                auto files = files_display(e->affected_files_json);
                json details;
                if (!e->details_json.empty()) {
                    try {
                        details = json::parse(e->details_json);
                    } catch (...) {
                        details = e->details_json;
                    }
                }
                res.set_content(json{
                    {"event_uid", e->event_uid},
                    {"watch_group", e->watch_group},
                    {"rule_name", e->rule_name},
                    {"event_type", e->event_type},
                    {"severity", e->severity},
                    {"affected_files", files.list},
                    {"details", details},
                    {"created_at", platform::format_display_time(
                                       e->created_at, tz)},
                }.dump(2), "application/json");
                set_cors(res);
            });

        // ── Watches ───────────────────────────────────────────────
        svr.Get("/api/v1/watches",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                auto reg = registry();
                json arr = json::array();
                if (reg) {
                    for (const auto& g : reg->watch_groups()) {
                        arr.push_back(watch_json(g));
                    }
                }
                res.set_content(arr.dump(), "application/json");
                set_cors(res);
            });

        svr.Get(R"(/api/v1/watches/([^/]+)/yaml)",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                auto reg = registry();
                if (!reg) {
                    res.status = 503;
                    res.set_content(R"({"error":"No registry"})",
                                    "application/json");
                    return;
                }
                auto name = req.matches[1].str();
                const watch::WatchGroupDef* group = nullptr;
                for (const auto& g : reg->watch_groups()) {
                    if (g.group_name == name || g.group_id == name) {
                        group = &g;
                        break;
                    }
                }
                if (!group) {
                    res.status = 404;
                    res.set_content(R"({"error":"Watch group not found"})",
                                    "application/json");
                    return;
                }
                res.set_content(yaml_source(group->source_path),
                                "text/yaml; charset=utf-8");
                set_cors(res);
            });

        svr.Get(R"(/api/v1/watches/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                auto reg = registry();
                if (!reg) {
                    res.status = 503;
                    res.set_content(R"({"error":"No registry"})",
                                    "application/json");
                    return;
                }
                auto name = req.matches[1].str();
                const watch::WatchGroupDef* group = nullptr;
                for (const auto& g : reg->watch_groups()) {
                    if (g.group_name == name || g.group_id == name) {
                        group = &g;
                        break;
                    }
                }
                if (!group) {
                    res.status = 404;
                    res.set_content(R"({"error":"Watch group not found"})",
                                    "application/json");
                    return;
                }
                json j = watch_json(*group);
                j["yaml"] = yaml_source(group->source_path);
                res.set_content(j.dump(2), "application/json");
                set_cors(res);
            });

        // ── Triggers ──────────────────────────────────────────────
        svr.Get("/api/v1/triggers",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                auto reg = registry();
                json arr = json::array();
                if (reg) {
                    for (const auto& t : reg->triggers()) {
                        auto info = trigger_info(t, tz);
                        arr.push_back({
                            {"trigger_id", t.trigger_id},
                            {"target_id", t.target_id},
                            {"target_name", t.target_name},
                            {"target_kind", t.target_kind ==
                                engine::TriggerEvent::TargetKind::Workflow
                                ? "workflow" : "job"},
                            {"type", info.type},
                            {"schedule", info.schedule_expr},
                            {"next_fire", info.next_fire},
                            {"enabled", t.enabled},
                            {"max_instances", t.max_instances},
                        });
                    }
                }
                res.set_content(arr.dump(), "application/json");
                set_cors(res);
            });

        svr.Get(R"(/api/v1/triggers/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                auto reg = registry();
                if (!reg) {
                    res.status = 503;
                    res.set_content(R"({"error":"No registry"})",
                                    "application/json");
                    return;
                }
                auto id = req.matches[1].str();
                for (const auto& t : reg->triggers()) {
                    if (t.trigger_id == id) {
                        auto info = trigger_info(t, tz);
                        res.set_content(json{
                            {"trigger_id", t.trigger_id},
                            {"target_id", t.target_id},
                            {"target_name", t.target_name},
                            {"target_kind", t.target_kind ==
                                engine::TriggerEvent::TargetKind::Workflow
                                ? "workflow" : "job"},
                            {"type", info.type},
                            {"schedule", info.schedule_expr},
                            {"next_fire", info.next_fire},
                            {"enabled", t.enabled},
                            {"max_instances", t.max_instances},
                            {"misfire_policy", static_cast<int>(
                                t.misfire_policy)},
                        }.dump(2), "application/json");
                        set_cors(res);
                        return;
                    }
                }
                res.status = 404;
                res.set_content(R"({"error":"Trigger not found"})",
                                "application/json");
            });

        // ── Explain (dry-run plan, §11.7) ─────────────────────────
        svr.Get(R"(/api/v1/explain/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                auto reg = registry();
                if (!reg) {
                    res.status = 503;
                    res.set_content(R"({"error":"No registry"})",
                                    "application/json");
                    return;
                }
                auto name = req.matches[1].str();
                const engine::WorkflowDef* wf = reg->workflow_by_name(name);
                if (!wf) wf = reg->workflow(name);
                if (!wf) {
                    res.status = 404;
                    res.set_content(R"({"error":"Workflow not found"})",
                                    "application/json");
                    return;
                }
                auto plan = engine::build_explain_plan(*wf, deps.reader);
                res.set_content(plan.render_json(), "application/json");
                set_cors(res);
            });

        // ── Config reload ─────────────────────────────────────────
        svr.Post("/api/v1/config/reload",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;

                if (!deps.reload_config) {
                    res.status = 501;
                    res.set_content(R"({"error":"Reload not configured"})",
                                    "application/json");
                    return;
                }

                bool ok = deps.reload_config();
                json j;
                j["status"] = ok ? "reloaded" : "failed";
                res.status = ok ? 200 : 500;
                res.set_content(j.dump(), "application/json");
                set_cors(res);
            });
    }

    // ── Write API routes (§3.8.1/§3.8.2) ────────────────────────────
    //
    // Write safety contract for every mutating endpoint:
    //   1. Validate the YAML against the Kairos schema (loaders).
    //   2. Back up the target file to <name>.yaml.bak before touching it.
    //   3. Write atomically (temp file + rename).
    //   4. Trigger a config reload so the running daemon picks it up.
    //   5. Append an audit_log entry (actor "http").

    void register_write_api_routes() {
        // ── Workflows: create ──────────────────────────────────────
        svr.Post("/api/v1/workflows",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                if (deps.workflows_dir.empty()) {
                    res.status = 503;
                    res.set_content(R"({"error":"Workflow directory not configured"})",
                                    "application/json");
                    return;
                }

                auto parsed = config::load_workflow_string(req.body);
                if (!parsed.ok()) {
                    res.status = 400;
                    res.set_content(
                        validation_error_body(parsed.errors).dump(2),
                        "application/json");
                    return;
                }
                if (parsed.workflows.size() != 1u) {
                    res.status = 400;
                    res.set_content(R"({"error":"Body must define exactly one workflow"})",
                                    "application/json");
                    return;
                }
                const auto& name =
                    parsed.workflows[0].workflow_name;
                if (!is_safe_entity_name(name)) {
                    res.status = 400;
                    res.set_content(R"({"error":"Invalid workflow name: allowed chars are letters, digits, dot, underscore, dash"})",
                                    "application/json");
                    return;
                }

                auto target = deps.workflows_dir / (name + ".yaml");
                std::error_code ec;
                if (std::filesystem::exists(target, ec)) {
                    res.status = 409;
                    res.set_content(R"({"error":"Workflow file already exists"})",
                                    "application/json");
                    return;
                }

                if (!write_file_atomic(target, req.body)) {
                    res.status = 500;
                    res.set_content(R"({"error":"Failed to write file"})",
                                    "application/json");
                    return;
                }
                bool reloaded = reload_after_write();
                audit("create_workflow", name);
                res.status = 201;
                res.set_content(json{
                    {"name", name},
                    {"file", target.string()},
                    {"reloaded", reloaded},
                }.dump(), "application/json");
                set_cors(res);
            });

        // ── Workflows: update ──────────────────────────────────────
        svr.Put(R"(/api/v1/workflows/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                if (deps.workflows_dir.empty()) {
                    res.status = 503;
                    res.set_content(R"({"error":"Workflow directory not configured"})",
                                    "application/json");
                    return;
                }
                auto url_name = req.matches[1].str();

                auto parsed = config::load_workflow_string(req.body);
                if (!parsed.ok()) {
                    res.status = 400;
                    res.set_content(
                        validation_error_body(parsed.errors).dump(2),
                        "application/json");
                    return;
                }
                if (parsed.workflows.size() != 1u ||
                    parsed.workflows[0].workflow_name != url_name) {
                    res.status = 400;
                    res.set_content(R"({"error":"Body workflow name must match the URL"})",
                                    "application/json");
                    return;
                }

                auto target = resolve_workflow_file(url_name);
                if (!target) {
                    res.status = 404;
                    res.set_content(R"({"error":"Workflow not found"})",
                                    "application/json");
                    return;
                }

                bool backed_up = backup_file(*target);
                if (!write_file_atomic(*target, req.body)) {
                    res.status = 500;
                    res.set_content(R"({"error":"Failed to write file"})",
                                    "application/json");
                    return;
                }
                bool reloaded = reload_after_write();
                audit("update_workflow", url_name,
                      json{{"backup", backed_up}}.dump());
                res.set_content(json{
                    {"name", url_name},
                    {"file", target->string()},
                    {"backup", backed_up},
                    {"reloaded", reloaded},
                }.dump(), "application/json");
                set_cors(res);
            });

        // ── Workflows: delete ──────────────────────────────────────
        svr.Delete(R"(/api/v1/workflows/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                if (deps.workflows_dir.empty()) {
                    res.status = 503;
                    res.set_content(R"({"error":"Workflow directory not configured"})",
                                    "application/json");
                    return;
                }
                auto name = req.matches[1].str();

                auto target = resolve_workflow_file(name);
                if (!target) {
                    res.status = 404;
                    res.set_content(R"({"error":"Workflow not found"})",
                                    "application/json");
                    return;
                }

                bool backed_up = backup_file(*target);
                std::error_code ec;
                std::filesystem::remove(*target, ec);
                if (ec) {
                    res.status = 500;
                    res.set_content(R"({"error":"Failed to delete file"})",
                                    "application/json");
                    return;
                }
                bool reloaded = reload_after_write();
                audit("delete_workflow", name,
                      json{{"backup", backed_up}}.dump());
                res.set_content(json{
                    {"name", name},
                    {"file", target->string()},
                    {"backup", backed_up},
                    {"reloaded", reloaded},
                }.dump(), "application/json");
                set_cors(res);
            });

        // ── Watches: create (single group per file by convention) ──
        svr.Post("/api/v1/watches",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                if (deps.watch_groups_dir.empty()) {
                    res.status = 503;
                    res.set_content(R"({"error":"Watch directory not configured"})",
                                    "application/json");
                    return;
                }

                auto parsed = config::load_watch_groups_string(req.body);
                if (!parsed.ok()) {
                    res.status = 400;
                    res.set_content(
                        validation_error_body(parsed.errors).dump(2),
                        "application/json");
                    return;
                }
                if (parsed.watch_groups.size() != 1u) {
                    res.status = 400;
                    res.set_content(R"({"error":"Body must define exactly one watch group - multi-group files are managed directly"})",
                                    "application/json");
                    return;
                }
                const auto& name = parsed.watch_groups[0].group_name;
                if (!is_safe_entity_name(name)) {
                    res.status = 400;
                    res.set_content(R"({"error":"Invalid watch group name: allowed chars are letters, digits, dot, underscore, dash"})",
                                    "application/json");
                    return;
                }

                auto target = deps.watch_groups_dir / (name + ".yaml");
                std::error_code ec;
                if (std::filesystem::exists(target, ec)) {
                    res.status = 409;
                    res.set_content(R"({"error":"Watch group file already exists"})",
                                    "application/json");
                    return;
                }

                if (!write_file_atomic(target, req.body)) {
                    res.status = 500;
                    res.set_content(R"({"error":"Failed to write file"})",
                                    "application/json");
                    return;
                }
                bool reloaded = reload_after_write();
                audit("create_watch_group", name);
                res.status = 201;
                res.set_content(json{
                    {"name", name},
                    {"file", target.string()},
                    {"reloaded", reloaded},
                }.dump(), "application/json");
                set_cors(res);
            });

        // ── Watches: update ────────────────────────────────────────
        svr.Put(R"(/api/v1/watches/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                if (deps.watch_groups_dir.empty()) {
                    res.status = 503;
                    res.set_content(R"({"error":"Watch directory not configured"})",
                                    "application/json");
                    return;
                }
                auto url_name = req.matches[1].str();

                auto parsed = config::load_watch_groups_string(req.body);
                if (!parsed.ok()) {
                    res.status = 400;
                    res.set_content(
                        validation_error_body(parsed.errors).dump(2),
                        "application/json");
                    return;
                }
                if (parsed.watch_groups.size() != 1u ||
                    parsed.watch_groups[0].group_name != url_name) {
                    res.status = 400;
                    res.set_content(R"({"error":"Body watch group name must match the URL"})",
                                    "application/json");
                    return;
                }

                auto target = resolve_watch_file(url_name);
                if (!target) {
                    res.status = 404;
                    res.set_content(R"({"error":"Watch group not found"})",
                                    "application/json");
                    return;
                }

                bool backed_up = backup_file(*target);
                if (!write_file_atomic(*target, req.body)) {
                    res.status = 500;
                    res.set_content(R"({"error":"Failed to write file"})",
                                    "application/json");
                    return;
                }
                bool reloaded = reload_after_write();
                audit("update_watch_group", url_name,
                      json{{"backup", backed_up}}.dump());
                res.set_content(json{
                    {"name", url_name},
                    {"file", target->string()},
                    {"backup", backed_up},
                    {"reloaded", reloaded},
                }.dump(), "application/json");
                set_cors(res);
            });

        // ── Watches: delete ────────────────────────────────────────
        svr.Delete(R"(/api/v1/watches/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                if (deps.watch_groups_dir.empty()) {
                    res.status = 503;
                    res.set_content(R"({"error":"Watch directory not configured"})",
                                    "application/json");
                    return;
                }
                auto name = req.matches[1].str();

                auto target = resolve_watch_file(name);
                if (!target) {
                    res.status = 404;
                    res.set_content(R"({"error":"Watch group not found"})",
                                    "application/json");
                    return;
                }

                bool backed_up = backup_file(*target);
                std::error_code ec;
                std::filesystem::remove(*target, ec);
                if (ec) {
                    res.status = 500;
                    res.set_content(R"({"error":"Failed to delete file"})",
                                    "application/json");
                    return;
                }
                bool reloaded = reload_after_write();
                audit("delete_watch_group", name,
                      json{{"backup", backed_up}}.dump());
                res.set_content(json{
                    {"name", name},
                    {"file", target->string()},
                    {"backup", backed_up},
                    {"reloaded", reloaded},
                }.dump(), "application/json");
                set_cors(res);
            });

        // ── Validate YAML without applying (§3.8.1) ────────────────
        // POST /api/v1/config/validate?kind=workflow|watch
        svr.Post("/api/v1/config/validate",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                auto kind = req.has_param("kind")
                    ? req.get_param_value("kind") : "workflow";

                if (kind == "workflow") {
                    auto parsed = config::load_workflow_string(req.body);
                    if (parsed.ok() && parsed.workflows.size() == 1u) {
                        res.set_content(json{
                            {"valid", true},
                            {"name",
                             parsed.workflows[0].workflow_name},
                        }.dump(), "application/json");
                    } else if (parsed.ok()) {
                        res.status = 400;
                        res.set_content(R"({"valid":false,"error":"Body must define exactly one workflow"})",
                                        "application/json");
                    } else {
                        res.status = 400;
                        res.set_content(
                            validation_error_body(parsed.errors).dump(2),
                            "application/json");
                    }
                } else if (kind == "watch") {
                    auto parsed =
                        config::load_watch_groups_string(req.body);
                    if (parsed.ok() && !parsed.watch_groups.empty()) {
                        res.set_content(json{
                            {"valid", true},
                            {"count", parsed.watch_groups.size()},
                        }.dump(), "application/json");
                    } else if (parsed.ok()) {
                        res.status = 400;
                        res.set_content(R"({"valid":false,"error":"No watch groups defined"})",
                                        "application/json");
                    } else {
                        res.status = 400;
                        res.set_content(
                            validation_error_body(parsed.errors).dump(2),
                            "application/json");
                    }
                } else {
                    res.status = 400;
                    res.set_content(R"({"error":"kind must be 'workflow' or 'watch'"})",
                                    "application/json");
                }
                set_cors(res);
            });

        // ── Trigger a standalone job ───────────────────────────────
        svr.Post(R"(/api/v1/jobs/([^/]+)/run)",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;

                auto name = req.matches[1].str();
                auto reg = registry();
                if (!reg || (!reg->standalone_job(name) &&
                             !reg->standalone_job_by_name(name))) {
                    res.status = 404;
                    res.set_content(R"({"error":"Standalone job not found"})",
                                    "application/json");
                    return;
                }
                if (!deps.submit_run) {
                    res.status = 501;
                    res.set_content(R"({"error":"Run submission not configured"})",
                                    "application/json");
                    return;
                }

                auto correlation = deps.submit_run(name);
                if (correlation.empty()) {
                    res.status = 500;
                    res.set_content(R"({"error":"Failed to submit run"})",
                                    "application/json");
                    return;
                }

                std::string run_id;
                for (int i = 0; i < 20 && run_id.empty(); ++i) {
                    if (deps.reader) {
                        if (auto id = deps.reader
                                ->get_run_id_by_correlation(correlation)) {
                            run_id = *id;
                            break;
                        }
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(100));
                }

                audit("run_job", name);
                res.set_content(json{
                    {"correlation_id", correlation},
                    {"run_id", run_id.empty() ? json(nullptr)
                                              : json(run_id)},
                    {"status", run_id.empty() ? "pending" : "submitted"},
                }.dump(), "application/json");
                set_cors(res);
            });

        // ── Cancel a run (§3.8.5) ──────────────────────────────────
        svr.Post(R"(/api/v1/runs/([^/]+)/cancel)",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;

                auto input = req.matches[1].str();
                std::vector<std::string> candidates;
                auto run_id = resolve_run_id(input, candidates);
                if (run_id.empty()) {
                    res.status = 404;
                    json err = {{"error", "Run not found"},
                                {"run_id", input}};
                    if (!candidates.empty()) err["candidates"] = candidates;
                    res.set_content(err.dump(), "application/json");
                    return;
                }

                if (!deps.cancel_run) {
                    res.status = 501;
                    res.set_content(R"({"error":"Cancellation not configured"})",
                                    "application/json");
                    return;
                }

                bool cancelled = deps.cancel_run(run_id);
                audit("cancel_run", run_id,
                      json{{"was_active", cancelled}}.dump());
                res.set_content(json{
                    {"run_id", run_id},
                    {"cancelled", cancelled},
                    {"status", cancelled ? "cancelling" : "not_active"},
                }.dump(), "application/json");
                set_cors(res);
            });

        // ── KEL playground eval (§3.8.7) ───────────────────────────
        // Body: {"expr": "<expression>"} (JSON).
        svr.Post("/api/v1/kel/eval",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;

                std::string expr;
                try {
                    auto body = json::parse(req.body);
                    expr = body.value("expr", std::string{});
                } catch (...) {
                    res.status = 400;
                    res.set_content(R"({"error":"Body must be JSON: {\"expr\": \"...\"}"})",
                                    "application/json");
                    return;
                }
                if (expr.empty()) {
                    res.status = 400;
                    res.set_content(R"({"error":"Missing expr"})",
                                    "application/json");
                    return;
                }

                try {
                    kel::EvalContext ctx = kel::make_default_context();
                    if (deps.reader) {
                        deps.reader->register_kel_bindings(ctx);
                        deps.reader
                            ->register_watch_kel_bindings(ctx);
                    }
                    if (deps.tag_store) {
                        auto* ts = deps.tag_store;
                        ctx.functions["has_tag"] =
                            [ts](const std::vector<kel::KelValue>& args)
                                -> kel::KelValue {
                            if (args.size() != 2 ||
                                !args[0].is_string() ||
                                !args[1].is_string()) {
                                throw kel::KelEvalError(
                                    "has_tag() requires two string "
                                    "arguments (entity_id, tag)");
                            }
                            const auto& eid = args[0].as_string();
                            const auto& tag = args[1].as_string();
                            static const std::string types[] = {
                                "workflow", "job", "watch_group",
                                "trigger"};
                            for (const auto& t : types) {
                                if (ts->has_tag(t, eid, tag))
                                    return kel::KelValue(true);
                            }
                            return kel::KelValue(false);
                        };
                    }

                    kel::EvalLimits limits;
                    auto result =
                        kel::eval_expression(expr, ctx, limits);

                    json j;
                    j["expression"] = expr;
                    j["type"] = result.type_name();
                    if (result.is_bool()) {
                        j["value"] = result.as_bool();
                    } else if (result.is_int()) {
                        j["value"] = result.as_int();
                    } else if (result.is_float()) {
                        j["value"] = result.as_float();
                    } else if (result.is_string()) {
                        j["value"] = result.as_string();
                    } else {
                        j["value"] = result.to_display_string();
                    }
                    res.set_content(j.dump(2), "application/json");
                } catch (const std::exception& e) {
                    res.status = 400;
                    res.set_content(json{
                        {"expression", expr},
                        {"error", e.what()},
                    }.dump(2), "application/json");
                }
                set_cors(res);
            });

        // ── Audit trail (read; §15.14) ─────────────────────────────
        svr.Get("/api/v1/audit",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;
                int limit = 50;
                if (req.has_param("limit")) {
                    try {
                        limit = std::stoi(req.get_param_value("limit"));
                    } catch (...) { /* keep default */ }
                }
                json arr = json::array();
                if (deps.reader) {
                    for (const auto& row :
                         deps.reader->query_audit_log(limit)) {
                        arr.push_back({
                            {"id", row.id},
                            {"timestamp",
                             platform::format_display_time(
                                 row.timestamp, tz)},
                            {"actor", row.actor},
                            {"action", row.action},
                            {"target", row.target},
                            {"detail", row.detail_json},
                        });
                    }
                }
                res.set_content(arr.dump(), "application/json");
                set_cors(res);
            });
    }

    void register_sse_routes() {
        // ── SSE: Run status stream (§3.8.6) ────────────────────────
        // Poll-diff loop: emits `run` events for new runs and status
        // transitions of recent runs. Browsers auto-reconnect when the
        // stream closes at the 30-minute cap.
        svr.Get("/sse/runs",
            [this](const httplib::Request&, httplib::Response& res) {
                res.set_header("Cache-Control", "no-cache");
                res.set_header("Connection", "keep-alive");
                if (config.enable_cors) {
                    res.set_header("Access-Control-Allow-Origin",
                                   config.cors_origins);
                }

                res.set_chunked_content_provider(
                    "text/event-stream",
                    [this](std::size_t, httplib::DataSink& sink) -> bool {
                        std::map<std::string, std::string> last_status;
                        const auto deadline =
                            std::chrono::steady_clock::now() +
                            std::chrono::minutes(30);
                        auto last_ping =
                            std::chrono::steady_clock::now();

                        while (std::chrono::steady_clock::now() <
                                   deadline) {
                            if (deps.reader) {
                                auto runs =
                                    deps.reader->query_recent_runs(50);
                                for (const auto& r : runs) {
                                    auto it =
                                        last_status.find(r.run_id);
                                    bool is_new =
                                        it == last_status.end();
                                    bool changed =
                                        !is_new &&
                                        it->second != r.status;
                                    if (is_new || changed) {
                                        last_status[r.run_id] =
                                            r.status;
                                        json ev = run_json(r);
                                        ev["kind"] = is_new
                                            ? "run_started"
                                            : "run_finished";
                                        std::string msg =
                                            "data: " + ev.dump() +
                                            "\n\n";
                                        if (!sink.write(msg.data(),
                                                        msg.size()))
                                            return false;
                                    }
                                }
                            }

                            // Keep-alive comment every ~15s.
                            if (std::chrono::steady_clock::now() -
                                    last_ping >
                                std::chrono::seconds(15)) {
                                if (!sink.write(": ping\n\n", 8))
                                    return false;
                                last_ping =
                                    std::chrono::steady_clock::now();
                            }

                            std::this_thread::sleep_for(
                                std::chrono::seconds(2));
                        }
                        return false;
                    });
            });

        // ── SSE: Watch event stream (§3.8.6) ───────────────────────
        svr.Get("/sse/events",
            [this](const httplib::Request&, httplib::Response& res) {
                res.set_header("Cache-Control", "no-cache");
                res.set_header("Connection", "keep-alive");
                if (config.enable_cors) {
                    res.set_header("Access-Control-Allow-Origin",
                                   config.cors_origins);
                }

                res.set_chunked_content_provider(
                    "text/event-stream",
                    [this](std::size_t, httplib::DataSink& sink) -> bool {
                        int64_t cursor = deps.reader
                            ? deps.reader->query_max_event_id()
                            : 0;
                        const auto deadline =
                            std::chrono::steady_clock::now() +
                            std::chrono::minutes(30);
                        auto last_ping =
                            std::chrono::steady_clock::now();

                        while (std::chrono::steady_clock::now() <
                                   deadline) {
                            if (deps.reader) {
                                auto events =
                                    deps.reader
                                        ->query_watch_events_since(
                                            cursor, 100);
                                for (const auto& e : events) {
                                    auto files =
                                        files_display(
                                            e.affected_files_json);
                                    json ev{
                                        {"kind", "watch_event"},
                                        {"event_uid", e.event_uid},
                                        {"watch_group", e.watch_group},
                                        {"rule_name", e.rule_name},
                                        {"event_type", e.event_type},
                                        {"severity", e.severity},
                                        {"affected_files", files.list},
                                        {"created_at",
                                         platform::format_display_time(
                                             e.created_at, tz)},
                                    };
                                    std::string msg =
                                        "data: " + ev.dump() + "\n\n";
                                    if (!sink.write(msg.data(),
                                                    msg.size()))
                                        return false;
                                }
                                if (!events.empty()) {
                                    cursor = deps.reader
                                        ->query_max_event_id();
                                }
                            }

                            if (std::chrono::steady_clock::now() -
                                    last_ping >
                                std::chrono::seconds(15)) {
                                if (!sink.write(": ping\n\n", 8))
                                    return false;
                                last_ping =
                                    std::chrono::steady_clock::now();
                            }

                            std::this_thread::sleep_for(
                                std::chrono::seconds(2));
                        }
                        return false;
                    });
            });

        // ── SSE: Log stream (push-based via RunStream) ────────────
        svr.Get(R"(/sse/logs/([^/]+))",
            [this](const httplib::Request& req,
                   httplib::Response& res) {
                auto run_id = req.matches[1].str();

                // Resolve truncated ids for convenience.
                std::vector<std::string> candidates;
                auto resolved = resolve_run_id(run_id, candidates);
                if (!resolved.empty()) run_id = resolved;

                res.set_header("Cache-Control", "no-cache");
                res.set_header("Connection", "keep-alive");
                if (config.enable_cors) {
                    res.set_header("Access-Control-Allow-Origin",
                                   config.cors_origins);
                }

                res.set_chunked_content_provider(
                    "text/event-stream",
                    [this, run_id](
                        std::size_t /*offset*/,
                        httplib::DataSink& sink) -> bool
                    {
                        // ── Push-based SSE via RunStream (§26.4) ──
                        struct SharedState {
                            std::mutex mu;
                            std::condition_variable cv;
                            std::vector<std::string> pending;
                            bool closed = false;
                        };
                        auto state = std::make_shared<SharedState>();

                        exec::RunStream::SubscriberId sub_id = 0;
                        exec::RunStream::SubscriberId close_id = 0;

                        if (deps.run_stream) {
                            sub_id = deps.run_stream->subscribe(run_id,
                                [state](const std::string& /*rid*/,
                                        const std::string& job_id,
                                        const std::string& /*step_id*/,
                                        std::string_view chunk,
                                        bool is_stderr) {
                                    nlohmann::json j;
                                    j["job_id"] = job_id;
                                    j["stream"] = is_stderr ? "stderr"
                                                            : "stdout";
                                    j["content"] = std::string(chunk);
                                    std::string event =
                                        "data: " + j.dump() + "\n\n";

                                    std::lock_guard lock(state->mu);
                                    state->pending.push_back(
                                        std::move(event));
                                    state->cv.notify_one();
                                });

                            close_id = deps.run_stream->on_close(run_id,
                                [state](const std::string& /*rid*/) {
                                    std::lock_guard lock(state->mu);
                                    state->closed = true;
                                    state->cv.notify_one();
                                });
                        }

                        // If RunStream is not available, fall back to
                        // poll-based mode (same as v1 behavior).
                        if (!deps.run_stream) {
                            int64_t last_id = 0;
                            int idle_count = 0;
                            constexpr int max_idle = 600;

                            while (idle_count < max_idle) {
                                if (!deps.reader) break;
                                auto chunks =
                                    deps.reader->get_log_chunks(
                                        run_id, last_id, 100);
                                if (chunks.empty()) {
                                    auto run =
                                        deps.reader->get_run_summary(
                                            run_id);
                                    if (run &&
                                        run->status != "RUNNING") {
                                        std::string event =
                                            "event: complete\n"
                                            "data: {\"status\":\"" +
                                            run->status + "\"}\n\n";
                                        sink.write(event.data(),
                                                   event.size());
                                        return false;
                                    }
                                    ++idle_count;
                                } else {
                                    idle_count = 0;
                                }
                                for (const auto& c : chunks) {
                                    nlohmann::json j;
                                    j["id"] = c.id;
                                    j["job_id"] = c.job_id;
                                    j["stream"] = c.stream;
                                    j["content"] = c.content;
                                    std::string ev =
                                        "data: " + j.dump() + "\n\n";
                                    if (!sink.write(ev.data(),
                                                    ev.size()))
                                        return false;
                                    last_id = std::max(last_id, c.id);
                                }
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(500));
                            }
                            return false;
                        }

                        // Push-based main loop: wait for chunks from
                        // RunStream or close signal.
                        constexpr auto timeout = std::chrono::minutes(5);
                        auto deadline =
                            std::chrono::steady_clock::now() + timeout;

                        while (true) {
                            std::vector<std::string> batch;
                            bool done = false;

                            {
                                std::unique_lock lock(state->mu);
                                state->cv.wait_for(lock,
                                    std::chrono::milliseconds(500),
                                    [&state] {
                                        return !state->pending.empty()
                                            || state->closed;
                                    });

                                batch = std::move(state->pending);
                                state->pending.clear();
                                done = state->closed;
                            }

                            for (const auto& event : batch) {
                                if (!sink.write(event.data(),
                                                event.size())) {
                                    deps.run_stream->unsubscribe(sub_id);
                                    deps.run_stream->unsubscribe(
                                        close_id);
                                    return false;
                                }
                            }

                            if (done) {
                                std::string status = "completed";
                                if (deps.reader) {
                                    auto run =
                                        deps.reader->get_run_summary(
                                            run_id);
                                    if (run) status = run->status;
                                }
                                std::string event =
                                    "event: complete\n"
                                    "data: {\"status\":\"" +
                                    status + "\"}\n\n";
                                sink.write(event.data(), event.size());
                                return false;
                            }

                            if (std::chrono::steady_clock::now() >
                                deadline) {
                                deps.run_stream->unsubscribe(sub_id);
                                deps.run_stream->unsubscribe(close_id);
                                return false;
                            }
                        }

                        return false;
                    }
                );
            });
    }
};

// ── Public API ────────────────────────────────────────────────────────────

HttpServer::HttpServer(HttpConfig config, HttpDependencies deps)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(deps)))
{
    impl_->register_routes();
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::start(std::stop_token stop) {
    impl_->svr.set_read_timeout(impl_->config.read_timeout_seconds, 0);
    impl_->svr.set_payload_max_length(
        static_cast<size_t>(impl_->config.max_body_bytes));

    auto addr = impl_->config.listen_addr;
    auto port = impl_->config.listen_port;

    impl_->server_thread = std::jthread(
        [this, addr, port](std::stop_token) {
            spdlog::info("HTTP server listening on {}:{}", addr, port);
            impl_->svr.listen(addr, port);
            spdlog::info("HTTP server stopped");
        });

    // Register stop callback for cooperative shutdown.
    std::stop_callback stop_cb(stop, [this]() {
        impl_->svr.stop();
    });

    // Note: stop_cb is destroyed here, but the server thread
    // is already running. The daemon's main loop will call stop()
    // or the destructor will handle it.
}

void HttpServer::join() {
    if (impl_->server_thread.joinable()) {
        impl_->server_thread.join();
    }
}

void HttpServer::stop() {
    impl_->svr.stop();
    if (impl_->server_thread.joinable()) {
        impl_->server_thread.join();
    }
}

void HttpServer::update_registry(
    std::shared_ptr<const engine::WorkflowRegistry> registry)
{
    if (!registry) return;
    std::lock_guard lock(impl_->registry_mu);
    impl_->current_registry = std::move(registry);
}

std::string HttpServer::listen_address() const {
    return impl_->config.listen_addr + ":" +
           std::to_string(impl_->config.listen_port);
}

}  // namespace kairos::http

#endif  // KAIROS_HTTP_ENABLED
