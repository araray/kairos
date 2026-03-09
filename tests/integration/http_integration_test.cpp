/// tests/integration/http_integration_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  HTTP server integration test                                            ║
// ║                                                                          ║
// ║  Tests the HTTP dashboard and REST API by spinning up a real             ║
// ║  HttpServer on a random port with in-memory dependencies, then          ║
// ║  making HTTP requests against it using cpp-httplib's Client.            ║
// ║                                                                          ║
// ║  Covers (§30.6):                                                        ║
// ║    1. GET /health — 200 + JSON with status: "ok"                        ║
// ║    2. GET /metrics — 200 + Prometheus text                              ║
// ║    3. GET / — 200 + HTML dashboard with inja rendering                  ║
// ║    4. GET /workflows — 200 + HTML workflows page                        ║
// ║    5. GET /runs — 200 + HTML runs page                                  ║
// ║    6. GET /events — 200 + HTML events page                              ║
// ║    7. GET /api/v1/workflows — 200 + JSON (auth bypass)                  ║
// ║    8. GET /api/v1/runs — 200 + JSON                                     ║
// ║    9. POST /api/v1/workflows/:name/run — 501 (no submit)               ║
// ║   10. GET /api/v1/runs/nonexistent — 404                                ║
// ║   11. Auth: token-protected endpoints reject missing/bad tokens         ║
// ║   12. CORS headers when enabled                                         ║
// ║                                                                          ║
// ║  Spec reference: §26.3, §26.4, §26.8, §30.6                           ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef KAIROS_HTTP_ENABLED

#include <gtest/gtest.h>

#include "kairos/http/http_server.hpp"
#include "kairos/observability/metrics.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/persist/query_reader.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/engine/workflow_registry.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <stop_source>
#include <thread>

namespace kairos::http::test {

using json = nlohmann::json;

// ── Test fixture ─────────────────────────────────────────────────────────

/// Fixture that creates an in-memory SQLite DB, a MetricsRegistry,
/// a WorkflowRegistry, and an HttpServer on a random port.
class HttpIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // In-memory SQLite for isolation.
        db_ = persist::open_database(":memory:");
        ASSERT_NE(db_, nullptr);

        // Initialize schema.
        persist::get_schema_version(*db_);

        // Create subsystems.
        query_reader_ = std::make_unique<persist::QueryReader>(*db_);

        // Populate a test workflow in the registry.
        std::vector<engine::WorkflowDef> workflows;
        engine::WorkflowDef wf;
        wf.workflow_name = "test-deploy";
        wf.trigger_type = "manual";
        engine::JobDef job;
        job.job_name = "build";
        engine::StepDef step;
        step.step_name = "compile";
        step.command = "make all";
        job.steps.push_back(std::move(step));
        wf.jobs.push_back(std::move(job));
        workflows.push_back(std::move(wf));

        registry_ = std::make_shared<engine::WorkflowRegistry>(
            std::move(workflows),
            std::vector<engine::TriggerDef>{},
            std::vector<engine::StandaloneJobDef>{},
            std::vector<watch::WatchGroupDef>{});

        // Create metrics.
        auto* uptime_gauge = metrics_.register_gauge(
            "kairos_uptime_seconds", "Uptime");
        uptime_gauge->set(42.0);

        start_time_ = std::chrono::steady_clock::now();
    }

    void TearDown() override {
        stop_server();
    }

    /// Start the HTTP server with the given config.
    void start_server(HttpConfig cfg = {}) {
        cfg.listen_addr = "127.0.0.1";
        // Use port 0 for OS-assigned random port... but cpp-httplib
        // doesn't directly support port 0 introspection, so we pick
        // a high random port.
        cfg.listen_port = static_cast<uint16_t>(18000 +
            (std::chrono::steady_clock::now().time_since_epoch().count() % 1000));

        HttpDependencies deps;
        deps.metrics = &metrics_;
        deps.reader = query_reader_.get();
        deps.registry = registry_;
        deps.get_uptime = [this]() -> double {
            return std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start_time_).count();
        };

        server_ = std::make_unique<HttpServer>(
            std::move(cfg), std::move(deps));

        stop_source_ = std::stop_source{};
        server_->start(stop_source_.get_token());

        // Brief delay for server to bind.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Create client.
        client_ = std::make_unique<httplib::Client>(
            "127.0.0.1", server_port());
        client_->set_read_timeout(5, 0);
    }

    void stop_server() {
        if (server_) {
            stop_source_.request_stop();
            server_->stop();
            server_.reset();
        }
    }

    uint16_t server_port() const {
        // Extract port from the server's listen address.
        auto addr = server_->listen_address();
        auto pos = addr.find_last_of(':');
        return static_cast<uint16_t>(
            std::stoi(addr.substr(pos + 1)));
    }

    // ── Shared state ─────────────────────────────────────────────────
    std::unique_ptr<SQLite::Database> db_;
    std::unique_ptr<persist::QueryReader> query_reader_;
    std::shared_ptr<engine::WorkflowRegistry> registry_;
    metrics::MetricsRegistry metrics_;
    std::chrono::steady_clock::time_point start_time_;

    std::unique_ptr<HttpServer> server_;
    std::stop_source stop_source_;
    std::unique_ptr<httplib::Client> client_;
};

// ── Health check ─────────────────────────────────────────────────────────

TEST_F(HttpIntegrationTest, HealthEndpointReturnsOk) {
    start_server();

    auto res = client_->Get("/health");
    ASSERT_TRUE(res) << "HTTP request failed";
    EXPECT_EQ(res->status, 200);

    auto body = json::parse(res->body);
    EXPECT_EQ(body["status"], "ok");
    EXPECT_GT(body["uptime_s"].get<double>(), 0.0);
}

// ── Prometheus metrics ──────────────────────────────────────────────────

TEST_F(HttpIntegrationTest, MetricsEndpointReturnsPrometheus) {
    start_server();

    auto res = client_->Get("/metrics");
    ASSERT_TRUE(res) << "HTTP request failed";
    EXPECT_EQ(res->status, 200);
    EXPECT_TRUE(res->get_header_value("Content-Type").find(
        "text/plain") != std::string::npos);
    // Should contain our registered gauge.
    EXPECT_TRUE(res->body.find("kairos_uptime_seconds") !=
        std::string::npos);
}

// ── Dashboard page ──────────────────────────────────────────────────────

TEST_F(HttpIntegrationTest, DashboardReturnsHtml) {
    start_server();

    auto res = client_->Get("/");
    ASSERT_TRUE(res) << "HTTP request failed";
    EXPECT_EQ(res->status, 200);
    EXPECT_TRUE(res->get_header_value("Content-Type").find(
        "text/html") != std::string::npos);

    // Verify inja template rendered — look for key elements.
    EXPECT_TRUE(res->body.find("Kairos") != std::string::npos)
        << "Dashboard should contain Kairos branding";
    EXPECT_TRUE(res->body.find("Total Runs") != std::string::npos)
        << "Dashboard should contain stat cards";
    EXPECT_TRUE(res->body.find("Recent Runs") != std::string::npos)
        << "Dashboard should contain recent runs section";
    EXPECT_TRUE(res->body.find("bootstrap") != std::string::npos)
        << "Dashboard should reference Bootstrap CSS";
}

// ── Workflows page ──────────────────────────────────────────────────────

TEST_F(HttpIntegrationTest, WorkflowsPageListsWorkflows) {
    start_server();

    auto res = client_->Get("/workflows");
    ASSERT_TRUE(res) << "HTTP request failed";
    EXPECT_EQ(res->status, 200);

    // Should contain the test workflow we registered.
    EXPECT_TRUE(res->body.find("test-deploy") != std::string::npos)
        << "Workflows page should list test-deploy workflow";
}

// ── Runs page ───────────────────────────────────────────────────────────

TEST_F(HttpIntegrationTest, RunsPageReturnsHtml) {
    start_server();

    auto res = client_->Get("/runs");
    ASSERT_TRUE(res) << "HTTP request failed";
    EXPECT_EQ(res->status, 200);
    EXPECT_TRUE(res->body.find("Run History") != std::string::npos);
}

// ── Events page ─────────────────────────────────────────────────────────

TEST_F(HttpIntegrationTest, EventsPageReturnsHtml) {
    start_server();

    auto res = client_->Get("/events");
    ASSERT_TRUE(res) << "HTTP request failed";
    EXPECT_EQ(res->status, 200);
    EXPECT_TRUE(res->body.find("Watch Events") != std::string::npos);
}

// ── REST API: List workflows ────────────────────────────────────────────

TEST_F(HttpIntegrationTest, ApiListWorkflowsReturnsJson) {
    start_server();  // No auth token → all endpoints open.

    auto res = client_->Get("/api/v1/workflows");
    ASSERT_TRUE(res) << "HTTP request failed";
    EXPECT_EQ(res->status, 200);

    auto body = json::parse(res->body);
    ASSERT_TRUE(body.is_array());
    ASSERT_GE(body.size(), 1u);
    EXPECT_EQ(body[0]["name"], "test-deploy");
}

// ── REST API: Query runs (empty) ────────────────────────────────────────

TEST_F(HttpIntegrationTest, ApiQueryRunsReturnsEmptyArray) {
    start_server();

    auto res = client_->Get("/api/v1/runs");
    ASSERT_TRUE(res) << "HTTP request failed";
    EXPECT_EQ(res->status, 200);

    auto body = json::parse(res->body);
    ASSERT_TRUE(body.is_array());
    EXPECT_EQ(body.size(), 0u);
}

// ── REST API: Run detail (not found) ────────────────────────────────────

TEST_F(HttpIntegrationTest, ApiRunDetailReturns404ForUnknownRun) {
    start_server();

    auto res = client_->Get("/api/v1/runs/nonexistent-run-id");
    ASSERT_TRUE(res) << "HTTP request failed";
    EXPECT_EQ(res->status, 404);
}

// ── REST API: Trigger run (no submit configured) ────────────────────────

TEST_F(HttpIntegrationTest, ApiTriggerRunReturns501WhenNoSubmit) {
    start_server();

    auto res = client_->Post("/api/v1/workflows/test-deploy/run", "", "");
    ASSERT_TRUE(res) << "HTTP request failed";
    EXPECT_EQ(res->status, 501);
}

// ── Auth: Token enforcement ─────────────────────────────────────────────

TEST_F(HttpIntegrationTest, ApiRejectsRequestsWithoutToken) {
    HttpConfig cfg;
    cfg.api_token = "secret-test-token-42";
    start_server(std::move(cfg));

    // Missing auth header → 401.
    auto res = client_->Get("/api/v1/workflows");
    ASSERT_TRUE(res) << "HTTP request failed";
    EXPECT_EQ(res->status, 401);
}

TEST_F(HttpIntegrationTest, ApiRejectsWrongToken) {
    HttpConfig cfg;
    cfg.api_token = "correct-token";
    start_server(std::move(cfg));

    httplib::Headers headers{
        {"Authorization", "Bearer wrong-token"}
    };
    auto res = client_->Get("/api/v1/workflows", headers);
    ASSERT_TRUE(res) << "HTTP request failed";
    EXPECT_EQ(res->status, 401);
}

TEST_F(HttpIntegrationTest, ApiAcceptsCorrectToken) {
    HttpConfig cfg;
    cfg.api_token = "correct-token";
    start_server(std::move(cfg));

    httplib::Headers headers{
        {"Authorization", "Bearer correct-token"}
    };
    auto res = client_->Get("/api/v1/workflows", headers);
    ASSERT_TRUE(res) << "HTTP request failed";
    EXPECT_EQ(res->status, 200);
}

// ── CORS headers ────────────────────────────────────────────────────────

TEST_F(HttpIntegrationTest, CorsHeadersIncludedWhenEnabled) {
    HttpConfig cfg;
    cfg.enable_cors = true;
    cfg.cors_origins = "http://localhost:3000";
    start_server(std::move(cfg));

    auto res = client_->Get("/health");
    ASSERT_TRUE(res) << "HTTP request failed";
    EXPECT_EQ(res->status, 200);

    auto cors = res->get_header_value("Access-Control-Allow-Origin");
    EXPECT_EQ(cors, "http://localhost:3000");
}

// ── REST API: Query events ──────────────────────────────────────────────

TEST_F(HttpIntegrationTest, ApiQueryEventsReturnsEmptyArray) {
    start_server();

    auto res = client_->Get("/api/v1/events");
    ASSERT_TRUE(res) << "HTTP request failed";
    EXPECT_EQ(res->status, 200);

    auto body = json::parse(res->body);
    ASSERT_TRUE(body.is_array());
    EXPECT_EQ(body.size(), 0u);
}

// ── REST API: Config reload (no callback) ───────────────────────────────

TEST_F(HttpIntegrationTest, ApiConfigReloadReturns501WhenNoCallback) {
    start_server();

    auto res = client_->Post("/api/v1/config/reload", "", "");
    ASSERT_TRUE(res) << "HTTP request failed";
    EXPECT_EQ(res->status, 501);
}

// ── Health check pages return without auth ──────────────────────────────

TEST_F(HttpIntegrationTest, HealthAndMetricsBypassAuth) {
    HttpConfig cfg;
    cfg.api_token = "secret-token";
    start_server(std::move(cfg));

    // /health should work without auth (it's not an API endpoint).
    auto health = client_->Get("/health");
    ASSERT_TRUE(health);
    EXPECT_EQ(health->status, 200);

    // /metrics should work without auth (Prometheus scraper compat).
    auto metrics = client_->Get("/metrics");
    ASSERT_TRUE(metrics);
    EXPECT_EQ(metrics->status, 200);

    // Dashboard pages should also work without auth.
    auto dash = client_->Get("/");
    ASSERT_TRUE(dash);
    EXPECT_EQ(dash->status, 200);
}

}  // namespace kairos::http::test

#else  // !KAIROS_HTTP_ENABLED

// When HTTP is not compiled, provide a dummy test to avoid
// "no tests in this file" warnings.
#include <gtest/gtest.h>
TEST(HttpIntegrationTest, HttpNotCompiled) {
    GTEST_SKIP() << "HTTP server not compiled (KAIROS_HTTP=OFF)";
}

#endif  // KAIROS_HTTP_ENABLED
