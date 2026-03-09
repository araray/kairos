/// tests/unit/http/http_server_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  HTTP server tests — config, route registration, auth checking          ║
// ║                                                                          ║
// ║  These tests are only compiled when KAIROS_HTTP=ON.                     ║
// ║  They start the HTTP server on a random port and make real HTTP         ║
// ║  requests using cpp-httplib's client.                                   ║
// ║                                                                          ║
// ║  Spec reference: §26.2–§26.7                                           ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef KAIROS_HTTP_ENABLED

#include "kairos/http/http_server.hpp"
#include "kairos/engine/trigger_types.hpp"
#include "kairos/observability/metrics.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/persist/query_reader.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <stop_token>
#include <thread>

namespace kairos::http {
namespace {

using json = nlohmann::json;

class HttpServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create in-memory SQLite database.
        db_ = persist::open_database(":memory:");

        reader_ = std::make_unique<persist::QueryReader>(*db_);

        // Empty workflow registry.
        registry_ = std::make_shared<engine::WorkflowRegistry>(
            std::vector<engine::WorkflowDef>{},
            std::vector<engine::TimerEntry>{});

        start_ = std::chrono::steady_clock::now();
    }

    void TearDown() override {
        reader_.reset();
        db_.reset();
    }

    /// Create an HttpServer with default test configuration.
    std::unique_ptr<HttpServer> make_server(
        uint16_t port, const std::string& token = "")
    {
        HttpConfig cfg;
        cfg.listen_addr = "127.0.0.1";
        cfg.listen_port = port;
        cfg.api_token = token;

        HttpDependencies deps;
        deps.metrics = &metrics_;
        deps.reader = reader_.get();
        deps.registry = registry_;
        deps.get_uptime = [this]() -> double {
            return std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start_).count();
        };

        return std::make_unique<HttpServer>(std::move(cfg),
                                             std::move(deps));
    }

    std::unique_ptr<SQLite::Database> db_;
    std::unique_ptr<persist::QueryReader> reader_;
    std::shared_ptr<engine::WorkflowRegistry> registry_;
    metrics::MetricsRegistry metrics_;
    std::chrono::steady_clock::time_point start_;
};

TEST_F(HttpServerTest, HealthEndpoint) {
    auto server = make_server(18421);
    std::stop_source ss;
    server->start(ss.get_token());

    // Brief delay for server to start.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18421");
    auto res = cli.Get("/health");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto j = json::parse(res->body);
    EXPECT_EQ(j["status"], "ok");
    EXPECT_TRUE(j.contains("uptime_s"));

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, MetricsEndpoint) {
    auto server = make_server(18422);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18422");
    auto res = cli.Get("/metrics");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    // Prometheus format starts with # or metric name.
    // At minimum, the response should be non-empty text.

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, DashboardReturnsHtml) {
    auto server = make_server(18423);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18423");
    auto res = cli.Get("/");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_NE(res->body.find("Kairos"), std::string::npos);
    EXPECT_NE(res->body.find("<!DOCTYPE html>"), std::string::npos);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, ApiWorkflowsNoAuth) {
    auto server = make_server(18424);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18424");
    auto res = cli.Get("/api/v1/workflows");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto j = json::parse(res->body);
    EXPECT_TRUE(j.is_array());

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, ApiAuthRequired) {
    auto server = make_server(18425, "secret-token");
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18425");

    // Without token → 401.
    auto res1 = cli.Get("/api/v1/workflows");
    ASSERT_TRUE(res1);
    EXPECT_EQ(res1->status, 401);

    // With wrong token → 401.
    httplib::Headers bad_headers = {
        {"Authorization", "Bearer wrong-token"}
    };
    auto res2 = cli.Get("/api/v1/workflows", bad_headers);
    ASSERT_TRUE(res2);
    EXPECT_EQ(res2->status, 401);

    // With correct token → 200.
    httplib::Headers good_headers = {
        {"Authorization", "Bearer secret-token"}
    };
    auto res3 = cli.Get("/api/v1/workflows", good_headers);
    ASSERT_TRUE(res3);
    EXPECT_EQ(res3->status, 200);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, ApiRunsReturnsJson) {
    auto server = make_server(18426);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18426");
    auto res = cli.Get("/api/v1/runs");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto j = json::parse(res->body);
    EXPECT_TRUE(j.is_array());

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, ApiRunNotFound) {
    auto server = make_server(18427);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18427");
    auto res = cli.Get("/api/v1/runs/nonexistent-run-id");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, ApiEventsReturnsJson) {
    auto server = make_server(18428);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18428");
    auto res = cli.Get("/api/v1/events");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto j = json::parse(res->body);
    EXPECT_TRUE(j.is_array());

    ss.request_stop();
    server->stop();
}

}  // namespace
}  // namespace kairos::http

#else

// When KAIROS_HTTP is not enabled, provide a placeholder test.
#include <gtest/gtest.h>
TEST(HttpServerTest, HttpDisabled) {
    GTEST_SKIP() << "HTTP server tests require KAIROS_HTTP=ON";
}

#endif  // KAIROS_HTTP_ENABLED
