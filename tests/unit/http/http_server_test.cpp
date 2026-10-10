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
#include "kairos/config/yaml_loader.hpp"
#include "kairos/engine/trigger_types.hpp"
#include "kairos/observability/metrics.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/persist/query_reader.hpp"

#include "test_helpers.hpp"

#include <httplib.h>
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <set>
#include <stop_token>
#include <thread>

namespace kairos::http {
namespace {

using json = nlohmann::json;

class HttpServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create in-memory SQLite database and initialize the schema
        // (runs all migrations, incl. v2 tags_json).
        db_ = persist::open_database(":memory:");
        persist::get_schema_version(*db_);

        reader_ = std::make_unique<persist::QueryReader>(*db_);

        // Empty workflow registry (Phase 9 tests override via
        // set_registry / update_registry).
        registry_ = std::make_shared<engine::WorkflowRegistry>(
            std::vector<engine::WorkflowDef>{},
            std::vector<engine::TimerEntry>{});

        start_ = std::chrono::steady_clock::now();
    }

    /// Build a registry from a workflow YAML string (real parse path).
    void set_registry_from_yaml(const std::string& yaml) {
        auto r = config::load_workflow_string(yaml);
        ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "?" : r.errors[0].message);
        registry_ = std::make_shared<engine::WorkflowRegistry>(
            std::move(r.workflows), std::move(r.triggers),
            std::move(r.standalone_jobs), std::move(r.watch_groups));
    }

    /// Seed one completed run row directly into the DB.
    void seed_run(const std::string& run_id,
                  const std::string& target_name,
                  const std::string& status,
                  const std::string& tags_json = "[]")
    {
        SQLite::Statement insert(*db_,
            "INSERT INTO runs (run_id, target_type, target_id, target_name,"
            " trigger_type, trigger_id, correlation_id, status, exit_code,"
            " start_ts, end_ts, duration_ms, tags_json)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)");
        insert.bind(1, run_id);
        insert.bind(2, "workflow");
        insert.bind(3, "wfl-x");
        insert.bind(4, target_name);
        insert.bind(5, "manual");
        insert.bind(6, "trg-x");
        insert.bind(7, run_id);
        insert.bind(8, status);
        insert.bind(9, 0);
        insert.bind(10, "2026-03-16T10:00:00Z");
        insert.bind(11, "2026-03-16T10:00:01Z");
        insert.bind(12, 1000);
        insert.bind(13, tags_json);
        insert.exec();
    }

    /// Seed one watch event row; returns the event uid (row id as string).
    std::string seed_event(const std::string& group,
                           const std::string& files_json)
    {
        SQLite::Statement insert(*db_,
            "INSERT INTO watch_events (watch_group, event_type, file_path,"
            " rule_name, action_taken, details_json)"
            " VALUES (?,?,?,?,?,?)");
        insert.bind(1, group);
        insert.bind(2, "content_changed");
        insert.bind(3, files_json);
        insert.bind(4, "rule-1");
        insert.bind(5, "warning");
        insert.bind(6, "{\"k\":\"v\"}");
        insert.exec();

        SQLite::Statement q(*db_,
            "SELECT MAX(id) FROM watch_events");
        q.executeStep();
        return std::to_string(q.getColumn(0).getInt64());
    }

    /// Create an HttpServer with default test configuration.
    std::unique_ptr<HttpServer> make_server(
        uint16_t port, const std::string& token = "",
        const std::string& web_root = "")
    {
        HttpConfig cfg;
        cfg.listen_addr = "127.0.0.1";
        cfg.listen_port = port;
        cfg.api_token = token;
        cfg.web_root = web_root;

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

    /// Create a server with the write API enabled: YAML dirs under
    /// `wf_dir`/`wg_dir`, a DB writer for audit entries, a reload
    /// flag, and a controllable cancel callback.
    std::unique_ptr<HttpServer> make_write_server(
        uint16_t port,
        const std::filesystem::path& wf_dir,
        const std::filesystem::path& wg_dir)
    {
        HttpConfig cfg;
        cfg.listen_addr = "127.0.0.1";
        cfg.listen_port = port;

        writer_ = std::make_unique<persist::DBWriter>(*db_);
        writer_stop_ = std::stop_source{};
        writer_->start(writer_stop_.get_token());

        HttpDependencies deps;
        deps.metrics = &metrics_;
        deps.reader = reader_.get();
        deps.registry = registry_;
        deps.get_uptime = [this]() -> double {
            return std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start_).count();
        };
        deps.workflows_dir = wf_dir;
        deps.watch_groups_dir = wg_dir;
        deps.db_writer = writer_.get();
        deps.reload_config = [this]() -> bool {
            ++reload_calls_;
            return true;
        };
        deps.cancel_run = [this](const std::string& run_id) -> bool {
            last_cancelled_ = run_id;
            return cancel_result_;
        };
        deps.submit_run = [this](const std::string& name) -> std::string {
            if (submit_targets_.count(name) == 0) return {};
            ++submit_calls_;
            return "run-fixedcorrelationid0000000000000";
        };

        return std::make_unique<HttpServer>(std::move(cfg),
                                             std::move(deps));
    }

    void TearDown() override {
        if (writer_) {
            writer_stop_.request_stop();
            writer_->flush();
            writer_.reset();
        }
        reader_.reset();
        db_.reset();
    }

    // Write-API test state.
    std::unique_ptr<persist::DBWriter> writer_;
    std::stop_source writer_stop_;
    int reload_calls_ = 0;
    int submit_calls_ = 0;
    std::string last_cancelled_;
    bool cancel_result_ = true;
    std::set<std::string> submit_targets_;

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

// ═══════════════════════════════════════════════════════════════════════════
//  Phase 9 — Web UI foundation (Roadmap §3.1–§3.4, §3.6, §3.8.1)
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(HttpServerTest, DashboardUsesBaseLayout) {
    auto server = make_server(18440);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18440");
    auto res = cli.Get("/");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    // Base layout markers: theme attr, search bar, footer version.
    EXPECT_NE(res->body.find("data-theme="), std::string::npos);
    EXPECT_NE(res->body.find("kairos-search-input"), std::string::npos);
    EXPECT_NE(res->body.find("theme-toggle"), std::string::npos);
    EXPECT_NE(res->body.find("static/css/kairos.css"), std::string::npos);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, StaticAssetServedFromEmbedded) {
    auto server = make_server(18441);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18441");

    auto css = cli.Get("/static/css/kairos.css");
    ASSERT_TRUE(css);
    EXPECT_EQ(css->status, 200);
    EXPECT_TRUE(css->get_header_value("Content-Type").find("text/css")
                != std::string::npos);
    EXPECT_NE(css->body.find("[data-theme=\"light\"]"), std::string::npos);

    auto js = cli.Get("/static/js/kairos.js");
    ASSERT_TRUE(js);
    EXPECT_EQ(js->status, 200);

    auto missing = cli.Get("/static/css/nope.css");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, FaviconServed) {
    auto server = make_server(18442);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18442");
    auto res = cli.Get("/favicon.ico");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(res->get_header_value("Content-Type"), "image/svg+xml");

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, ExternalWebRootOverridesEmbedded) {
    kairos::testing::TempDir dir;
    dir.create_file("templates/base.html",
                    "OVERRODE-BASE[{{ title }}]{{ content }}END-OVERRIDE");

    auto server = make_server(18443, "", dir.path().string());
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18443");
    auto res = cli.Get("/");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_NE(res->body.find("OVERRODE-BASE"), std::string::npos);
    EXPECT_NE(res->body.find("END-OVERRIDE"), std::string::npos);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, NewPagesRender) {
    set_registry_from_yaml(R"(
name: "ci-pipeline"
tags: ["ci"]
jobs:
  build:
    steps:
      - name: "compile"
        run: "make all"
  test:
    needs: [build]
    steps:
      - name: "unit"
        run: "ctest"
)");

    auto server = make_server(18444);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18444");

    // Jobs page lists workflow jobs (by job name).
    auto jobs = cli.Get("/jobs");
    ASSERT_TRUE(jobs);
    EXPECT_EQ(jobs->status, 200);
    EXPECT_NE(jobs->body.find(">build<"), std::string::npos);

    // Triggers page renders (empty trigger set).
    auto triggers = cli.Get("/triggers");
    ASSERT_TRUE(triggers);
    EXPECT_EQ(triggers->status, 200);
    EXPECT_NE(triggers->body.find("Schedule Triggers"),
              std::string::npos);

    // Watches page renders (empty).
    auto watches = cli.Get("/watches");
    ASSERT_TRUE(watches);
    EXPECT_EQ(watches->status, 200);
    EXPECT_NE(watches->body.find("Watch Groups"), std::string::npos);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, WorkflowDetailPage) {
    set_registry_from_yaml(R"(
name: "ci-pipeline"
jobs:
  build:
    steps:
      - name: "compile"
        run: "make all"
  test:
    needs: [build]
    steps:
      - name: "unit"
        run: "ctest"
)");

    auto server = make_server(18445);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18445");

    auto res = cli.Get("/workflows/ci-pipeline");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    // Detail page shows jobs, the dry-run plan, and the YAML panel.
    EXPECT_NE(res->body.find("ci-pipeline"), std::string::npos);
    EXPECT_NE(res->body.find("Execution Plan"), std::string::npos);
    EXPECT_NE(res->body.find("compile"), std::string::npos);

    auto missing = cli.Get("/workflows/does-not-exist");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, ApiWorkflowDetailAndYaml) {
    set_registry_from_yaml(R"(
name: "ci-pipeline"
tags: ["ci", "build"]
jobs:
  build:
    steps:
      - name: "compile"
        run: "make all"
)");

    auto server = make_server(18446);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18446");

    auto detail = cli.Get("/api/v1/workflows/ci-pipeline");
    ASSERT_TRUE(detail);
    EXPECT_EQ(detail->status, 200);
    auto j = json::parse(detail->body);
    EXPECT_EQ(j["name"], "ci-pipeline");
    EXPECT_TRUE(j["workflow_id"].get<std::string>()
                    .starts_with("wfl-"));
    ASSERT_EQ(j["jobs"].size(), 1u);

    auto yaml = cli.Get("/api/v1/workflows/ci-pipeline/yaml");
    ASSERT_TRUE(yaml);
    EXPECT_EQ(yaml->status, 200);
    // Inline definitions have no source file — placeholder YAML.
    EXPECT_NE(yaml->body.find("inline definition"),
              std::string::npos);

    auto missing = cli.Get("/api/v1/workflows/zzz");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, ApiJobsEndpoints) {
    set_registry_from_yaml(R"(
name: "ci-pipeline"
jobs:
  build:
    steps:
      - name: "compile"
        run: "make all"
)");

    auto server = make_server(18447);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18447");

    auto list = cli.Get("/api/v1/jobs");
    ASSERT_TRUE(list);
    EXPECT_EQ(list->status, 200);
    auto arr = json::parse(list->body);
    ASSERT_TRUE(arr.is_array());
    ASSERT_EQ(arr.size(), 1u);
    EXPECT_EQ(arr[0]["name"], "build");
    EXPECT_EQ(arr[0]["scope"], "ci-pipeline");
    EXPECT_EQ(arr[0]["scope_kind"], "workflow");

    // Detail by name.
    auto detail = cli.Get("/api/v1/jobs/build");
    ASSERT_TRUE(detail);
    EXPECT_EQ(detail->status, 200);
    auto j = json::parse(detail->body);
    EXPECT_EQ(j["step_count"], 1);

    auto missing = cli.Get("/api/v1/jobs/nope");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, ApiTriggersEndpoint) {
    set_registry_from_yaml(R"(
name: "nightly"
triggers:
  - type: cron
    cron: "0 2 * * *"
jobs:
  backup:
    steps:
      - name: "dump"
        run: "echo backup"
)");

    auto server = make_server(18448);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18448");

    auto list = cli.Get("/api/v1/triggers");
    ASSERT_TRUE(list);
    EXPECT_EQ(list->status, 200);
    auto arr = json::parse(list->body);
    ASSERT_TRUE(arr.is_array());
    ASSERT_GE(arr.size(), 1u);
    EXPECT_EQ(arr[0]["type"], "cron");
    EXPECT_EQ(arr[0]["schedule"], "0 2 * * *");
    EXPECT_EQ(arr[0]["target_name"], "nightly");
    EXPECT_FALSE(arr[0]["next_fire"].get<std::string>().empty());

    // Triggers HTML page shows the cron schedule.
    auto page = cli.Get("/triggers");
    ASSERT_TRUE(page);
    EXPECT_NE(page->body.find("0 2 * * *"), std::string::npos);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, ApiExplainEndpoint) {
    set_registry_from_yaml(R"(
name: "ci-pipeline"
jobs:
  build:
    steps:
      - name: "compile"
        run: "make all"
  test:
    needs: [build]
    steps:
      - name: "unit"
        run: "ctest"
)");

    auto server = make_server(18449);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18449");

    auto res = cli.Get("/api/v1/explain/ci-pipeline");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto j = json::parse(res->body);
    EXPECT_EQ(j["workflow_name"], "ci-pipeline");
    EXPECT_EQ(j["entries"].size(), 2u);
    EXPECT_TRUE(j.contains("summary"));
    // Both jobs run: no conditions, build's needs are met.
    EXPECT_EQ(j["summary"]["jobs_to_run"], 2);

    auto missing = cli.Get("/api/v1/explain/zzz");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, ApiWatchesEndpoints) {
    auto r = config::load_watch_groups_string(R"(
watch_groups:
  - name: "logs"
    watch_items:
      - "/var/log/app/*.log"
    rules:
      - name: "grew"
        condition: "event.type == 'content_changed'"
        severity: "warning"
)");
    ASSERT_TRUE(r.ok());
    registry_ = std::make_shared<engine::WorkflowRegistry>(
        std::vector<engine::WorkflowDef>{},
        std::vector<engine::TimerEntry>{},
        std::vector<engine::JobDef>{}, std::move(r.watch_groups));

    auto server = make_server(18450);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18450");

    auto list = cli.Get("/api/v1/watches");
    ASSERT_TRUE(list);
    EXPECT_EQ(list->status, 200);
    auto arr = json::parse(list->body);
    ASSERT_EQ(arr.size(), 1u);
    EXPECT_EQ(arr[0]["name"], "logs");
    EXPECT_EQ(arr[0]["rules"].size(), 1u);

    auto detail = cli.Get("/api/v1/watches/logs");
    ASSERT_TRUE(detail);
    EXPECT_EQ(detail->status, 200);
    auto j = json::parse(detail->body);
    EXPECT_EQ(j["name"], "logs");
    EXPECT_EQ(j["watch_items"].size(), 1u);

    // HTML pages render.
    auto page = cli.Get("/watches/logs");
    ASSERT_TRUE(page);
    EXPECT_EQ(page->status, 200);
    EXPECT_NE(page->body.find("grew"), std::string::npos);

    auto missing = cli.Get("/api/v1/watches/zzz");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, RunIdPrefixResolution) {
    seed_run("run-aaaa11112222333344445555666677788899",
             "ci-pipeline", "SUCCESS");

    auto server = make_server(18451);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18451");

    // Prefix on the API detail endpoint.
    auto api = cli.Get("/api/v1/runs/run-aaaa1111");
    ASSERT_TRUE(api);
    EXPECT_EQ(api->status, 200);
    auto j = json::parse(api->body);
    EXPECT_EQ(j["run_id"], "run-aaaa11112222333344445555666677788899");

    // Prefix on the HTML page.
    auto page = cli.Get("/runs/run-aaaa1111");
    ASSERT_TRUE(page);
    EXPECT_EQ(page->status, 200);
    EXPECT_NE(page->body.find("ci-pipeline"), std::string::npos);

    // Unknown prefix → 404.
    auto missing = cli.Get("/api/v1/runs/run-zzzz");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, RunPageShowsTagsAndLogs) {
    seed_run("run-bbbb11112222333344445555666677788899",
             "ci-pipeline", "SUCCESS", R"(["deploy","prod"])");

    SQLite::Statement log(*db_,
        "INSERT INTO log_chunks (run_id, job_id, step_id, stream,"
        " chunk_index, content) VALUES (?,?,?,?,?,?)");
    log.bind(1, "run-bbbb11112222333344445555666677788899");
    log.bind(2, "job-build");
    log.bind(3, "stp-1");
    log.bind(4, "stdout");
    log.bind(5, 0);
    log.bind(6, "hello from step");
    log.exec();

    auto server = make_server(18452);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18452");

    // Runs list carries tags (§5.2).
    auto runs = cli.Get("/api/v1/runs");
    ASSERT_TRUE(runs);
    auto arr = json::parse(runs->body);
    ASSERT_EQ(arr.size(), 1u);
    EXPECT_EQ(arr[0]["tags"].size(), 2u);

    // Tag filter (comma-separated, AND semantics).
    auto filtered = cli.Get("/api/v1/runs?tag=deploy");
    ASSERT_TRUE(filtered);
    auto farr = json::parse(filtered->body);
    EXPECT_EQ(farr.size(), 1u);

    auto both = cli.Get("/api/v1/runs?tag=deploy,prod");
    ASSERT_TRUE(both);
    EXPECT_EQ(json::parse(both->body).size(), 1u);

    auto noMatch = cli.Get("/api/v1/runs?tag=missing");
    ASSERT_TRUE(noMatch);
    EXPECT_EQ(json::parse(noMatch->body).size(), 0u);

    // Detail page includes the log snapshot and tags.
    auto page = cli.Get("/runs/run-bbbb1111");
    ASSERT_TRUE(page);
    EXPECT_EQ(page->status, 200);
    EXPECT_NE(page->body.find("hello from step"), std::string::npos);
    EXPECT_NE(page->body.find("deploy"), std::string::npos);

    // Logs endpoint (prefix-resolved).
    auto logs = cli.Get("/api/v1/runs/run-bbbb/logs");
    ASSERT_TRUE(logs);
    EXPECT_EQ(logs->status, 200);
    auto larr = json::parse(logs->body);
    ASSERT_EQ(larr.size(), 1u);
    EXPECT_EQ(larr[0]["content"], "hello from step");

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, EventDetailEndpoints) {
    auto uid = seed_event("logs", R"(["/var/log/app/a.log"])");

    auto server = make_server(18453);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18453");

    auto api = cli.Get("/api/v1/events/" + uid);
    ASSERT_TRUE(api);
    EXPECT_EQ(api->status, 200);
    auto j = json::parse(api->body);
    EXPECT_EQ(j["watch_group"], "logs");
    ASSERT_EQ(j["affected_files"].size(), 1u);
    EXPECT_EQ(j["affected_files"][0], "/var/log/app/a.log");

    // Events list includes parsed affected files (§3.4).
    auto list = cli.Get("/api/v1/events");
    ASSERT_TRUE(list);
    auto arr = json::parse(list->body);
    ASSERT_EQ(arr.size(), 1u);
    EXPECT_EQ(arr[0]["affected_files"][0], "/var/log/app/a.log");

    // HTML detail page.
    auto page = cli.Get("/events/" + uid);
    ASSERT_TRUE(page);
    EXPECT_EQ(page->status, 200);
    EXPECT_NE(page->body.find("/var/log/app/a.log"), std::string::npos);

    // Unknown event → 404.
    auto missing = cli.Get("/api/v1/events/99999");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, UpdateRegistryPropagatesToRequests) {
    auto server = make_server(18454);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18454");

    // Initially empty.
    auto before = cli.Get("/api/v1/workflows");
    ASSERT_TRUE(before);
    EXPECT_EQ(json::parse(before->body).size(), 0u);

    // Hot registry swap (config reload path).
    set_registry_from_yaml(R"(
name: "hot-reload-wf"
jobs:
  only:
    steps:
      - name: "step"
        run: "true"
)");
    server->update_registry(registry_);

    auto after = cli.Get("/api/v1/workflows");
    ASSERT_TRUE(after);
    auto arr = json::parse(after->body);
    ASSERT_EQ(arr.size(), 1u);
    EXPECT_EQ(arr[0]["name"], "hot-reload-wf");

    ss.request_stop();
    server->stop();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Phase 10 — Write API (Roadmap §3.8.1/§3.8.2)
// ═══════════════════════════════════════════════════════════════════════════

namespace {
const char* kValidWfYaml = R"(name: api-made
jobs:
  build:
    steps:
      - name: compile
        run: make all
)";
const char* kValidWatchYaml = R"(watch_groups:
  - name: api-watch
    watch_items:
      - "/tmp/**/*.log"
    rules:
      - name: grew
        condition: "event.type == 'created'"
)";
}  // namespace

TEST_F(HttpServerTest, WriteApi_CreateWorkflowWritesFileAndReloads) {
    kairos::testing::TempDir wf_dir, wg_dir;
    auto server = make_write_server(18460, wf_dir.path(), wg_dir.path());
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18460");
    auto res = cli.Post("/api/v1/workflows", kValidWfYaml,
                        "text/yaml");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201);
    auto j = json::parse(res->body);
    EXPECT_EQ(j["name"], "api-made");
    EXPECT_TRUE(j["reloaded"]);

    auto file = wf_dir.path() / "api-made.yaml";
    ASSERT_TRUE(std::filesystem::exists(file));
    EXPECT_GT(reload_calls_, 0);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, WriteApi_CreateWorkflowRejectsInvalidYaml) {
    kairos::testing::TempDir wf_dir, wg_dir;
    auto server = make_write_server(18461, wf_dir.path(), wg_dir.path());
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18461");
    auto res = cli.Post("/api/v1/workflows",
                        "name: broken\njobs: 42\n", "text/yaml");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto j = json::parse(res->body);
    EXPECT_EQ(j["error"], "validation failed");
    EXPECT_FALSE(j["errors"].empty());
    EXPECT_TRUE(j["errors"][0].contains("message"));

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, WriteApi_CreateWorkflowConflict) {
    kairos::testing::TempDir wf_dir, wg_dir;
    auto server = make_write_server(18462, wf_dir.path(), wg_dir.path());
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18462");
    ASSERT_TRUE(cli.Post("/api/v1/workflows", kValidWfYaml, "text/yaml"));
    auto res = cli.Post("/api/v1/workflows", kValidWfYaml, "text/yaml");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 409);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, WriteApi_UpdateWorkflowBacksUp) {
    kairos::testing::TempDir wf_dir, wg_dir;
    auto server = make_write_server(18463, wf_dir.path(), wg_dir.path());
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18463");
    ASSERT_TRUE(cli.Post("/api/v1/workflows", kValidWfYaml, "text/yaml"));

    const char* updated = R"(name: api-made
jobs:
  build:
    steps:
      - name: compile2
        run: make -j8
)";
    auto res = cli.Put("/api/v1/workflows/api-made", updated, "text/yaml");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_TRUE(json::parse(res->body)["backup"]);

    auto bak = wf_dir.path() / "api-made.yaml.bak";
    ASSERT_TRUE(std::filesystem::exists(bak));
    // Backup holds the ORIGINAL content.
    std::ifstream in(bak);
    std::string bak_content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    EXPECT_NE(bak_content.find("compile\n"), std::string::npos);

    std::ifstream cur(wf_dir.path() / "api-made.yaml");
    std::string cur_content((std::istreambuf_iterator<char>(cur)),
                            std::istreambuf_iterator<char>());
    EXPECT_NE(cur_content.find("compile2"), std::string::npos);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, WriteApi_UpdateWorkflowRequiresNameMatch) {
    kairos::testing::TempDir wf_dir, wg_dir;
    auto server = make_write_server(18464, wf_dir.path(), wg_dir.path());
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18464");
    ASSERT_TRUE(cli.Post("/api/v1/workflows", kValidWfYaml, "text/yaml"));

    auto res = cli.Put("/api/v1/workflows/other-name", kValidWfYaml,
                       "text/yaml");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, WriteApi_DeleteWorkflowBacksUp) {
    kairos::testing::TempDir wf_dir, wg_dir;
    auto server = make_write_server(18465, wf_dir.path(), wg_dir.path());
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18465");
    ASSERT_TRUE(cli.Post("/api/v1/workflows", kValidWfYaml, "text/yaml"));

    auto res = cli.Delete("/api/v1/workflows/api-made");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_FALSE(std::filesystem::exists(wf_dir.path() / "api-made.yaml"));
    EXPECT_TRUE(std::filesystem::exists(wf_dir.path() / "api-made.yaml.bak"));

    auto missing = cli.Delete("/api/v1/workflows/api-made");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, WriteApi_WatchCrud) {
    kairos::testing::TempDir wf_dir, wg_dir;
    auto server = make_write_server(18466, wf_dir.path(), wg_dir.path());
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18466");

    // Create.
    auto create = cli.Post("/api/v1/watches", kValidWatchYaml,
                           "text/yaml");
    ASSERT_TRUE(create);
    EXPECT_EQ(create->status, 201);
    EXPECT_TRUE(std::filesystem::exists(wg_dir.path() / "api-watch.yaml"));

    // Update.
    auto update = cli.Put("/api/v1/watches/api-watch", kValidWatchYaml,
                          "text/yaml");
    ASSERT_TRUE(update);
    EXPECT_EQ(update->status, 200);
    EXPECT_TRUE(std::filesystem::exists(wg_dir.path() / "api-watch.yaml.bak"));

    // Delete.
    auto del = cli.Delete("/api/v1/watches/api-watch");
    ASSERT_TRUE(del);
    EXPECT_EQ(del->status, 200);
    EXPECT_FALSE(std::filesystem::exists(wg_dir.path() / "api-watch.yaml"));

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, WriteApi_ValidateEndpoint) {
    auto server = make_server(18467);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18467");

    auto ok = cli.Post("/api/v1/config/validate?kind=workflow",
                       kValidWfYaml, "text/yaml");
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->status, 200);
    EXPECT_TRUE(json::parse(ok->body)["valid"]);

    auto bad = cli.Post("/api/v1/config/validate?kind=workflow",
                        ":::not yaml:::", "text/yaml");
    ASSERT_TRUE(bad);
    EXPECT_EQ(bad->status, 400);

    auto watch_ok = cli.Post("/api/v1/config/validate?kind=watch",
                             kValidWatchYaml, "text/yaml");
    ASSERT_TRUE(watch_ok);
    EXPECT_EQ(watch_ok->status, 200);
    EXPECT_TRUE(json::parse(watch_ok->body)["valid"]);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, WriteApi_JobRunEndpoint) {
    // A standalone job file (top-level steps + standalone: true).
    set_registry_from_yaml(R"(
name: nightly-job
standalone: true
steps:
  - name: work
    run: "true"
)");
    submit_targets_ = {"nightly-job"};

    kairos::testing::TempDir wf_dir, wg_dir;
    auto server = make_write_server(18468, wf_dir.path(), wg_dir.path());
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18468");

    auto ok = cli.Post("/api/v1/jobs/nightly-job/run", "", "");
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->status, 200);
    auto j = json::parse(ok->body);
    EXPECT_EQ(j["correlation_id"],
              "run-fixedcorrelationid0000000000000");
    // No pipeline in tests → run_id stays null, status pending.
    EXPECT_TRUE(j["run_id"].is_null());
    EXPECT_EQ(j["status"], "pending");
    EXPECT_EQ(submit_calls_, 1);

    auto missing = cli.Post("/api/v1/jobs/unknown/run", "", "");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, WriteApi_CancelRunEndpoint) {
    seed_run("run-cccc11112222333344445555666677788899",
             "ci-pipeline", "RUNNING");

    kairos::testing::TempDir wf_dir, wg_dir;
    auto server = make_write_server(18469, wf_dir.path(), wg_dir.path());
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18469");

    // Prefix-resolved cancel of a running run.
    auto res = cli.Post("/api/v1/runs/run-cccc/cancel", "", "");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto j = json::parse(res->body);
    EXPECT_TRUE(j["cancelled"]);
    EXPECT_EQ(j["run_id"],
              "run-cccc11112222333344445555666677788899");
    EXPECT_EQ(last_cancelled_,
              "run-cccc11112222333344445555666677788899");

    // Not-active run reports gracefully.
    cancel_result_ = false;
    auto inactive = cli.Post("/api/v1/runs/run-cccc/cancel", "", "");
    ASSERT_TRUE(inactive);
    EXPECT_FALSE(json::parse(inactive->body)["cancelled"]);

    auto missing = cli.Post("/api/v1/runs/run-zzzz/cancel", "", "");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, WriteApi_AuditTrailRecordsMutations) {
    kairos::testing::TempDir wf_dir, wg_dir;
    auto server = make_write_server(18470, wf_dir.path(), wg_dir.path());
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18470");
    ASSERT_TRUE(cli.Post("/api/v1/workflows", kValidWfYaml, "text/yaml"));

    // The audit entry is written asynchronously — poll briefly.
    json arr = json::array();
    for (int i = 0; i < 20; ++i) {
        auto res = cli.Get("/api/v1/audit");
        ASSERT_TRUE(res);
        arr = json::parse(res->body);
        if (!arr.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_FALSE(arr.empty());
    EXPECT_EQ(arr[0]["actor"], "http");
    EXPECT_EQ(arr[0]["action"], "create_workflow");
    EXPECT_EQ(arr[0]["target"], "api-made");

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, WriteApi_DisabledWithoutDirs) {
    auto server = make_server(18471);  // No dirs wired.
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18471");
    auto res = cli.Post("/api/v1/workflows", kValidWfYaml, "text/yaml");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 503);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, EditorPagesRender) {
    set_registry_from_yaml(kValidWfYaml);

    auto server = make_server(18472);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18472");

    // New-workflow editor: starter template + textarea.
    auto wf_new = cli.Get("/workflows/new");
    ASSERT_TRUE(wf_new);
    EXPECT_EQ(wf_new->status, 200);
    EXPECT_NE(wf_new->body.find("yaml-editor"), std::string::npos);
    EXPECT_NE(wf_new->body.find("my-workflow"), std::string::npos);
    EXPECT_NE(wf_new->body.find("btn-save"), std::string::npos);

    // Edit page for the loaded workflow.
    auto wf_edit = cli.Get("/workflows/api-made/edit");
    ASSERT_TRUE(wf_edit);
    EXPECT_EQ(wf_edit->status, 200);
    EXPECT_NE(wf_edit->body.find("api-made"), std::string::npos);

    // New-watch editor.
    auto wg_new = cli.Get("/watches/new");
    ASSERT_TRUE(wg_new);
    EXPECT_EQ(wg_new->status, 200);
    EXPECT_NE(wg_new->body.find("watch_groups:"), std::string::npos);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, ActionButtonsPresent) {
    set_registry_from_yaml(kValidWfYaml);
    seed_run("run-dddd11112222333344445555666677788899",
             "api-made", "RUNNING");

    auto server = make_server(18473);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli("http://127.0.0.1:18473");

    // Workflow detail: Run / Edit / Delete buttons.
    auto wf = cli.Get("/workflows/api-made");
    ASSERT_TRUE(wf);
    EXPECT_NE(wf->body.find("run-workflow"), std::string::npos);
    EXPECT_NE(wf->body.find("delete-entity"), std::string::npos);
    EXPECT_NE(wf->body.find("/workflows/api-made/edit"),
              std::string::npos);

    // Run detail: Cancel button for a RUNNING run.
    auto run = cli.Get("/runs/run-dddd");
    ASSERT_TRUE(run);
    EXPECT_NE(run->body.find("cancel-run"), std::string::npos);

    ss.request_stop();
    server->stop();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Phase 11 — SSE streams (Roadmap §3.8.6)
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(HttpServerTest, SseRunsStreamEmitsNewAndChangedRuns) {
    seed_run("run-eeee11112222333344445555666677788899",
             "ci-pipeline", "RUNNING");

    auto server = make_server(18474);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Client consumes the stream in the background; we mutate the DB
    // mid-stream and then check what arrived.
    std::string received;
    auto consumer = std::async(std::launch::async, [&received]() {
        httplib::Client cli("http://127.0.0.1:18474");
        cli.set_read_timeout(6, 0);
        cli.Get("/sse/runs",
                [&received](const char* data, size_t len) -> bool {
                    received.append(data, len);
                    // Stop after the completion event arrives.
                    return received.find("run_finished") ==
                           std::string::npos;
                });
    });

    // Wait for the stream to pick up the RUNNING run, then complete it.
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    {
        SQLite::Statement up(*db_,
            "UPDATE runs SET status='SUCCESS', end_ts='2026-03-16T10:00:05Z'"
            " WHERE run_id='run-eeee11112222333344445555666677788899'");
        up.exec();
    }
    consumer.wait();

    EXPECT_NE(received.find("run_started"), std::string::npos);
    EXPECT_NE(received.find("RUNNING"), std::string::npos);
    EXPECT_NE(received.find("run_finished"), std::string::npos);
    EXPECT_NE(received.find("SUCCESS"), std::string::npos);

    ss.request_stop();
    server->stop();
}

TEST_F(HttpServerTest, SseEventsStreamEmitsNewEvents) {
    auto server = make_server(18475);
    std::stop_source ss;
    server->start(ss.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::string received;
    auto consumer = std::async(std::launch::async, [&received]() {
        httplib::Client cli("http://127.0.0.1:18475");
        cli.set_read_timeout(6, 0);
        cli.Get("/sse/events",
                [&received](const char* data, size_t len) -> bool {
                    received.append(data, len);
                    return received.find("watch_event") ==
                           std::string::npos;
                });
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    seed_event("live-group", R"(["/var/log/live.log"])");
    consumer.wait();

    EXPECT_NE(received.find("watch_event"), std::string::npos);
    EXPECT_NE(received.find("live-group"), std::string::npos);
    EXPECT_NE(received.find("/var/log/live.log"), std::string::npos);

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
