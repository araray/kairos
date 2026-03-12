/// include/kairos/http/http_server.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/http/http_server.hpp — Web UI, REST API, SSE, and metrics       ║
// ║                                                                          ║
// ║  Optional component (KAIROS_HTTP=ON build flag).                        ║
// ║  Runs on Thread N+2, using cpp-httplib for the HTTP stack.              ║
// ║                                                                          ║
// ║  Provides:                                                               ║
// ║    - /health            → health check                                  ║
// ║    - /metrics           → Prometheus text exposition                    ║
// ║    - /api/v1/*          → JSON REST API (token-authenticated)          ║
// ║    - /sse/logs/:run_id  → SSE log stream                               ║
// ║    - /                  → Dashboard (server-rendered HTML)              ║
// ║                                                                          ║
// ║  Spec reference: §26.1–§26.8                                           ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#ifdef KAIROS_HTTP_ENABLED

#include "kairos/engine/pipeline.hpp"
#include "kairos/engine/workflow_registry.hpp"
#include "kairos/exec/output_sink.hpp"
#include "kairos/observability/metrics.hpp"
#include "kairos/persist/query_reader.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

namespace kairos::http {

// ── Configuration ─────────────────────────────────────────────────────────

/// HTTP server configuration (§26.7).
struct HttpConfig {
    /// Listen address. Default: "127.0.0.1" (localhost only).
    std::string listen_addr = "127.0.0.1";

    /// Listen port. Default: 8420.
    uint16_t listen_port = 8420;

    /// Optional API token for authenticated endpoints.
    /// If empty, authentication is disabled (suitable for local-only use).
    std::string api_token;

    /// Maximum request body size (for POST endpoints).
    std::size_t max_body_bytes = 1 * 1024 * 1024;  // 1 MiB

    /// Read timeout for client connections (seconds).
    int read_timeout_seconds = 30;

    /// Enable CORS headers. Default: false (localhost doesn't need CORS).
    bool enable_cors = false;

    /// CORS allowed origins (if enable_cors is true).
    std::string cors_origins = "*";

    /// Timezone for display in HTML pages (e.g., "UTC", "local", "+05:30").
    /// JSON API always returns UTC.
    std::string timezone = "local";
};

// ── Shared dependencies ───────────────────────────────────────────────────

/// Thread-safe references to shared daemon state.
struct HttpDependencies {
    metrics::MetricsRegistry* metrics = nullptr;
    persist::QueryReader* reader = nullptr;
    std::shared_ptr<const engine::WorkflowRegistry> registry;
    exec::RunStream* run_stream = nullptr;

    /// Submit a manual run by workflow name. Returns run_id or empty.
    std::function<std::string(const std::string& name)> submit_run;

    /// Config reload callback.
    std::function<bool()> reload_config;

    /// Daemon uptime (seconds).
    std::function<double()> get_uptime;
};

// ── HTTP Server ───────────────────────────────────────────────────────────

/// The Kairos HTTP server.
///
/// Thread model: runs on its own jthread (Thread N+2).
/// All access to HttpDependencies must be thread-safe.
///
/// Non-copyable, non-movable (owns the server thread and internal state).
class HttpServer {
public:
    HttpServer(HttpConfig config, HttpDependencies deps);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /// Start the HTTP server on its own thread.
    /// @param stop  Global stop token for cooperative shutdown.
    void start(std::stop_token stop);

    /// Block until the server thread exits.
    void join();

    /// Gracefully stop the server.
    void stop();

    /// @return The listen address (e.g., "127.0.0.1:8420").
    [[nodiscard]] std::string listen_address() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kairos::http

#endif  // KAIROS_HTTP_ENABLED
