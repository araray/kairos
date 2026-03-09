/// src/http/http_server.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  HTTP server implementation — cpp-httplib backend                         ║
// ║                                                                          ║
// ║  Route table (§26.3):                                                   ║
// ║    GET  /                         Dashboard (HTML)                       ║
// ║    GET  /health                   Health check                           ║
// ║    GET  /metrics                  Prometheus text exposition             ║
// ║    GET  /api/v1/workflows         List workflows                        ║
// ║    POST /api/v1/workflows/:n/run  Trigger workflow run                  ║
// ║    GET  /api/v1/runs              Query runs                            ║
// ║    GET  /api/v1/runs/:id          Run detail                            ║
// ║    GET  /api/v1/runs/:id/logs     Run logs                              ║
// ║    GET  /api/v1/events            Query events                          ║
// ║    POST /api/v1/config/reload     Reload configuration                  ║
// ║    GET  /sse/logs/:run_id         SSE log stream                        ║
// ║                                                                          ║
// ║  Spec reference: §26.2–§26.7                                           ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef KAIROS_HTTP_ENABLED

#include "kairos/http/http_server.hpp"

// cpp-httplib — header-only HTTP server.
// Must define implementation once.
#ifndef CPPHTTPLIB_HTTPLIB_H
#include <httplib.h>
#endif

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

namespace kairos::http {

using json = nlohmann::json;

// ── Dashboard HTML template (embedded) ────────────────────────────────────

/// Minimal MPA dashboard — Bootstrap 5 CDN, server-rendered.
/// In a real build, this would be generated from inja templates
/// compiled into the binary (§26.6). For v1, inline HTML is adequate.
static std::string render_dashboard(const HttpDependencies& deps) {
    // Gather data.
    double uptime = deps.get_uptime ? deps.get_uptime() : 0.0;
    int hours = static_cast<int>(uptime / 3600);
    int mins = static_cast<int>((uptime - hours * 3600) / 60);

    auto runs = deps.reader ? deps.reader->query_recent_runs(10) :
        std::vector<persist::QueryReader::RunSummary>{};

    auto stats = deps.reader ? deps.reader->query_run_stats() :
        persist::QueryReader::RunStats{};

    // Build runs table rows.
    std::string rows;
    for (const auto& r : runs) {
        std::string badge = "secondary";
        if (r.status == "SUCCESS") badge = "success";
        else if (r.status == "FAILURE") badge = "danger";
        else if (r.status == "RUNNING") badge = "primary";
        else if (r.status == "CANCELLED") badge = "warning";

        rows += "<tr>"
            "<td><code>" + r.run_id.substr(0, 12) + "</code></td>"
            "<td>" + r.target_name + "</td>"
            "<td><span class='badge bg-" + badge + "'>" +
                r.status + "</span></td>"
            "<td>" + r.trigger_type + "</td>"
            "<td>" + r.start_ts + "</td>"
            "<td>" + std::to_string(r.duration_ms) + "ms</td>"
            "</tr>\n";
    }

    return R"html(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Kairos Dashboard</title>
  <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.2/dist/css/bootstrap.min.css"
        rel="stylesheet">
  <style>
    body { background: #1a1a2e; color: #e0e0e0; }
    .card { background: #16213e; border: 1px solid #0f3460; }
    .table { color: #e0e0e0; }
    .navbar { background: #0f3460 !important; }
    code { color: #00d4ff; }
    .stat-value { font-size: 2rem; font-weight: bold; color: #00d4ff; }
    .stat-label { font-size: 0.85rem; color: #888; text-transform: uppercase; }
  </style>
</head>
<body>
  <nav class="navbar navbar-dark mb-4">
    <div class="container-fluid">
      <span class="navbar-brand">⏱ Kairos</span>
      <span class="text-light">Orchestration Dashboard</span>
    </div>
  </nav>
  <div class="container-fluid">
    <div class="row mb-4">
      <div class="col-md-3">
        <div class="card p-3 text-center">
          <div class="stat-value">)html" + std::to_string(stats.total_runs) + R"html(</div>
          <div class="stat-label">Total Runs</div>
        </div>
      </div>
      <div class="col-md-3">
        <div class="card p-3 text-center">
          <div class="stat-value">)html" + std::to_string(stats.active_runs) + R"html(</div>
          <div class="stat-label">Active Runs</div>
        </div>
      </div>
      <div class="col-md-3">
        <div class="card p-3 text-center">
          <div class="stat-value">)html" + std::to_string(stats.runs_today) + R"html(</div>
          <div class="stat-label">Runs Today</div>
        </div>
      </div>
      <div class="col-md-3">
        <div class="card p-3 text-center">
          <div class="stat-value" style="color:)html" +
            (stats.failures_today > 0 ? "#ff6b6b" : "#00d4ff") + R"html(">)html" +
            std::to_string(stats.failures_today) + R"html(</div>
          <div class="stat-label">Failures Today</div>
        </div>
      </div>
    </div>
    <div class="row mb-4">
      <div class="col-md-6">
        <div class="card p-3">
          <h5>Uptime</h5>
          <p>)html" + std::to_string(hours) + "h " + std::to_string(mins) + R"html(m</p>
        </div>
      </div>
      <div class="col-md-6">
        <div class="card p-3">
          <h5>Quick Actions</h5>
          <a href="/api/v1/workflows" class="btn btn-outline-info btn-sm me-2">Workflows API</a>
          <a href="/api/v1/runs" class="btn btn-outline-info btn-sm me-2">Runs API</a>
          <a href="/metrics" class="btn btn-outline-info btn-sm">Metrics</a>
        </div>
      </div>
    </div>
    <div class="card p-3">
      <h5>Recent Runs</h5>
      <table class="table table-sm table-hover">
        <thead>
          <tr>
            <th>Run ID</th><th>Target</th><th>Status</th>
            <th>Trigger</th><th>Started</th><th>Duration</th>
          </tr>
        </thead>
        <tbody>
          )html" + rows + R"html(
        </tbody>
      </table>
    </div>
  </div>
</body>
</html>)html";
}

// ── Server implementation ─────────────────────────────────────────────────

struct HttpServer::Impl {
    HttpConfig config;
    HttpDependencies deps;
    httplib::Server svr;
    std::jthread server_thread;

    /// Check Bearer token auth for API endpoints.
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

    /// Set CORS headers if enabled.
    void set_cors(httplib::Response& res) {
        if (config.enable_cors) {
            res.set_header("Access-Control-Allow-Origin",
                           config.cors_origins);
        }
    }

    /// Register all routes.
    void register_routes() {
        // ── Health check (unauthenticated) ─────────────────────
        svr.Get("/health", [this](const httplib::Request&,
                                   httplib::Response& res) {
            json j;
            j["status"] = "ok";
            j["uptime_s"] = deps.get_uptime ? deps.get_uptime() : 0.0;
            res.set_content(j.dump(), "application/json");
            set_cors(res);
        });

        // ── Prometheus metrics (unauthenticated) ───────────────
        svr.Get("/metrics", [this](const httplib::Request&,
                                    httplib::Response& res) {
            if (deps.metrics) {
                res.set_content(deps.metrics->to_prometheus(),
                                "text/plain; version=0.0.4; charset=utf-8");
            } else {
                res.set_content("# no metrics\n", "text/plain");
            }
        });

        // ── Dashboard (unauthenticated) ────────────────────────
        svr.Get("/", [this](const httplib::Request&,
                             httplib::Response& res) {
            res.set_content(render_dashboard(deps), "text/html");
            set_cors(res);
        });

        // ── REST API: List workflows ───────────────────────────
        svr.Get("/api/v1/workflows",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;

                json arr = json::array();
                if (deps.registry) {
                    for (const auto* wf : deps.registry->workflows()) {
                        json j;
                        j["name"] = wf->workflow_name;
                        j["job_count"] = wf->jobs.size();
                        arr.push_back(std::move(j));
                    }
                }
                res.set_content(arr.dump(), "application/json");
                set_cors(res);
            });

        // ── REST API: Trigger workflow run ──────────────────────
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

                auto run_id = deps.submit_run(name);
                if (run_id.empty()) {
                    res.status = 404;
                    res.set_content(R"({"error":"Workflow not found"})",
                                    "application/json");
                    return;
                }

                json j;
                j["run_id"] = run_id;
                j["status"] = "submitted";
                res.set_content(j.dump(), "application/json");
                set_cors(res);
            });

        // ── REST API: Query runs ───────────────────────────────
        svr.Get("/api/v1/runs",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;

                int limit = 50;
                std::string status, workflow, since;
                if (req.has_param("limit"))
                    limit = std::stoi(req.get_param_value("limit"));
                if (req.has_param("status"))
                    status = req.get_param_value("status");
                if (req.has_param("workflow"))
                    workflow = req.get_param_value("workflow");
                if (req.has_param("since"))
                    since = req.get_param_value("since");

                json arr = json::array();
                if (deps.reader) {
                    auto runs = deps.reader->query_recent_runs(
                        limit, status, workflow, since);
                    for (const auto& r : runs) {
                        json j;
                        j["run_id"] = r.run_id;
                        j["target_name"] = r.target_name;
                        j["target_type"] = r.target_type;
                        j["trigger_type"] = r.trigger_type;
                        j["status"] = r.status;
                        j["exit_code"] = r.exit_code;
                        j["start_ts"] = r.start_ts;
                        j["end_ts"] = r.end_ts;
                        j["duration_ms"] = r.duration_ms;
                        arr.push_back(std::move(j));
                    }
                }
                res.set_content(arr.dump(), "application/json");
                set_cors(res);
            });

        // ── REST API: Run detail ───────────────────────────────
        svr.Get(R"(/api/v1/runs/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;

                auto run_id = req.matches[1].str();
                if (!deps.reader) {
                    res.status = 503;
                    res.set_content(R"({"error":"No database reader"})",
                                    "application/json");
                    return;
                }

                auto detail = deps.reader->get_run_detail(run_id);
                if (!detail) {
                    res.status = 404;
                    res.set_content(R"({"error":"Run not found"})",
                                    "application/json");
                    return;
                }

                json j;
                j["run_id"] = detail->run.run_id;
                j["target_name"] = detail->run.target_name;
                j["status"] = detail->run.status;
                j["start_ts"] = detail->run.start_ts;
                j["end_ts"] = detail->run.end_ts;
                j["duration_ms"] = detail->run.duration_ms;

                json jobs_arr = json::array();
                for (const auto& jd : detail->jobs) {
                    json jj;
                    jj["job_id"] = jd.job_id;
                    jj["job_name"] = jd.job_name;
                    jj["status"] = jd.status;
                    jj["exit_code"] = jd.exit_code;
                    jj["duration_ms"] = jd.duration_ms;

                    json steps_arr = json::array();
                    for (const auto& s : jd.steps) {
                        json sj;
                        sj["step_id"] = s.step_id;
                        sj["step_name"] = s.step_name;
                        sj["status"] = s.status;
                        sj["exit_code"] = s.exit_code;
                        sj["command"] = s.command;
                        steps_arr.push_back(std::move(sj));
                    }
                    jj["steps"] = std::move(steps_arr);
                    jobs_arr.push_back(std::move(jj));
                }
                j["jobs"] = std::move(jobs_arr);

                res.set_content(j.dump(2), "application/json");
                set_cors(res);
            });

        // ── REST API: Run logs ─────────────────────────────────
        svr.Get(R"(/api/v1/runs/([^/]+)/logs)",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;

                auto run_id = req.matches[1].str();
                int64_t after_id = 0;
                int limit = 500;
                if (req.has_param("after"))
                    after_id = std::stoll(req.get_param_value("after"));
                if (req.has_param("limit"))
                    limit = std::stoi(req.get_param_value("limit"));

                json arr = json::array();
                if (deps.reader) {
                    auto chunks = deps.reader->get_log_chunks(
                        run_id, after_id, limit);
                    for (const auto& c : chunks) {
                        json j;
                        j["id"] = c.id;
                        j["job_id"] = c.job_id;
                        j["step_id"] = c.step_id;
                        j["stream"] = c.stream;
                        j["content"] = c.content;
                        j["created_at"] = c.created_at;
                        arr.push_back(std::move(j));
                    }
                }
                res.set_content(arr.dump(), "application/json");
                set_cors(res);
            });

        // ── REST API: Query events ─────────────────────────────
        svr.Get("/api/v1/events",
            [this](const httplib::Request& req, httplib::Response& res) {
                if (!check_auth(req, res)) return;

                int limit = 50;
                std::string group;
                if (req.has_param("limit"))
                    limit = std::stoi(req.get_param_value("limit"));
                if (req.has_param("watch_group"))
                    group = req.get_param_value("watch_group");

                json arr = json::array();
                if (deps.reader) {
                    auto events = deps.reader->query_watch_events(
                        limit, group);
                    for (const auto& e : events) {
                        json j;
                        j["event_uid"] = e.event_uid;
                        j["watch_group"] = e.watch_group;
                        j["rule_name"] = e.rule_name;
                        j["event_type"] = e.event_type;
                        j["severity"] = e.severity;
                        j["created_at"] = e.created_at;
                        arr.push_back(std::move(j));
                    }
                }
                res.set_content(arr.dump(), "application/json");
                set_cors(res);
            });

        // ── REST API: Reload config ────────────────────────────
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

        // ── SSE: Log stream (push-based via RunStream) ────────────
        svr.Get(R"(/sse/logs/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                auto run_id = req.matches[1].str();

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
                        // ── Push-based SSE via RunStream (§26.4) ──────
                        // Subscribe to RunStream for live output.
                        // Output chunks are written to the SSE sink
                        // as they arrive from the ProcessHandle.
                        //
                        // Synchronization: the subscriber callback runs
                        // on the ProcessHandle reader thread. We use a
                        // mutex + condition_variable to safely pass
                        // chunks to the SSE thread (this lambda).

                        struct SharedState {
                            std::mutex mu;
                            std::condition_variable cv;
                            std::vector<std::string> pending;
                            bool closed = false;
                        };
                        auto state = std::make_shared<SharedState>();

                        // Subscribe to output chunks.
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

                            // Subscribe to run completion.
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

                            // Write buffered chunks.
                            for (const auto& event : batch) {
                                if (!sink.write(event.data(),
                                                event.size())) {
                                    // Client disconnected.
                                    deps.run_stream->unsubscribe(sub_id);
                                    deps.run_stream->unsubscribe(
                                        close_id);
                                    return false;
                                }
                            }

                            if (done) {
                                // Emit completion event.
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

                            // Timeout check.
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

        // ── CORS preflight ─────────────────────────────────────
        if (config.enable_cors) {
            svr.Options(".*", [this](const httplib::Request&,
                                      httplib::Response& res) {
                res.set_header("Access-Control-Allow-Origin",
                               config.cors_origins);
                res.set_header("Access-Control-Allow-Methods",
                               "GET, POST, OPTIONS");
                res.set_header("Access-Control-Allow-Headers",
                               "Authorization, Content-Type");
                res.status = 204;
            });
        }
    }
};

// ── Public API ────────────────────────────────────────────────────────────

HttpServer::HttpServer(HttpConfig config, HttpDependencies deps)
    : impl_(std::make_unique<Impl>())
{
    impl_->config = std::move(config);
    impl_->deps = std::move(deps);
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
            spdlog::info("HTTP server listening on {}:{}",
                         addr, port);
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

std::string HttpServer::listen_address() const {
    return impl_->config.listen_addr + ":" +
           std::to_string(impl_->config.listen_port);
}

}  // namespace kairos::http

#endif  // KAIROS_HTTP_ENABLED
