/// tests/unit/mcp/handler_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  McpHandler unit tests — tool dispatch, watch tools, protocol methods   ║
// ║                                                                          ║
// ║  Tests: initialize, tools/list, tool routing, listWatchGroups,          ║
// ║  getEvents, watchScanOnce, reloadConfig, getMetrics, stubs, errors.     ║
// ║                                                                          ║
// ║  Spec reference: §22.4–§22.6                                           ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/mcp/handler.hpp"
#include "kairos/observability/metrics.hpp"
#include "kairos/testing/fake_clock.hpp"
#include "kairos/testing/fake_filesystem.hpp"
#include "kairos/testing/fake_fs_scanner.hpp"
#include "kairos/watch/watch_engine.hpp"

#include <gtest/gtest.h>

using json = nlohmann::json;
using namespace kairos::mcp;
using namespace kairos::watch;
using namespace kairos::engine;
using namespace kairos::testing;

// ── Test fixture ───────────────────────────────────────────────────────────

class McpHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a minimal watch engine for testing.
        WatchEngineConfig watch_cfg;
        watch_cfg.enabled = true;

        // Create a simple watch group.
        WatchGroupDef group;
        group.group_name = "test_group";
        group.watch_items = {"/tmp/test_watch"};
        group.sample_rate = std::chrono::seconds(5);
        group.mode = WatchMode::Sample;

        fake_fs_ = std::make_unique<FakeFilesystem>();
        fake_scanner_ = std::make_unique<FakeFilesystemScanner>(*fake_fs_);
        fake_clock_ = std::make_unique<FakeClock>();

        watch_engine_ = std::make_unique<WatchEngine>(
            watch_cfg,
            WatchEngine::Dependencies{
                .clock = fake_clock_.get(),
                .scanner = fake_scanner_.get(),
            },
            std::vector<WatchGroupDef>{group});
    }

    /// Create a handler with full dependencies.
    McpHandler make_handler() {
        McpHandler::Dependencies deps;
        deps.watch_engine = watch_engine_.get();
        deps.metrics = &metrics_;
        deps.server_info.name = "kairos-test";
        deps.server_info.version = "0.1.0-test";
        deps.reload_config = [this](std::vector<std::string>& errors) {
            if (reload_should_fail_) {
                errors.push_back("test reload error");
                return false;
            }
            return true;
        };
        return McpHandler(deps);
    }

    /// Create a handler with no watch engine (null deps).
    McpHandler make_handler_no_watch() {
        McpHandler::Dependencies deps;
        deps.metrics = &metrics_;
        deps.server_info.name = "kairos-test";
        return McpHandler(deps);
    }

    std::unique_ptr<FakeFilesystem> fake_fs_;
    std::unique_ptr<FakeFilesystemScanner> fake_scanner_;
    std::unique_ptr<FakeClock> fake_clock_;
    std::unique_ptr<WatchEngine> watch_engine_;
    kairos::metrics::MetricsRegistry metrics_;
    bool reload_should_fail_ = false;
};

// ── Protocol method tests ──────────────────────────────────────────────────

TEST_F(McpHandlerTest, InitializeReturnsServerInfo) {
    auto handler = make_handler();
    auto result = handler.dispatch("initialize", {}, 1);

    EXPECT_EQ(result["protocolVersion"], "2024-11-05");
    EXPECT_EQ(result["serverInfo"]["name"], "kairos-test");
    EXPECT_EQ(result["serverInfo"]["version"], "0.1.0-test");
    EXPECT_TRUE(result.contains("capabilities"));
    EXPECT_TRUE(result["capabilities"].contains("tools"));
}

TEST_F(McpHandlerTest, ToolsListReturns14Tools) {
    auto handler = make_handler();
    auto result = handler.dispatch("tools/list", {}, 1);

    EXPECT_TRUE(result.contains("tools"));
    auto& tools = result["tools"];
    // 14 tools from spec §22.4 + 1 additional (watchScanOnce) = 15.
    EXPECT_EQ(tools.size(), 15);

    // Verify all expected tool names are present.
    std::vector<std::string> expected_names = {
        "kairos.listWorkflows", "kairos.getWorkflow",
        "kairos.runWorkflow",   "kairos.listJobs",
        "kairos.runJob",        "kairos.queryRuns",
        "kairos.getRunDetail",  "kairos.getRunLogs",
        "kairos.getStepOutput", "kairos.listWatchGroups",
        "kairos.getEvents",     "kairos.watchScanOnce",
        "kairos.reloadConfig",  "kairos.explainPlan",
        "kairos.getMetrics",
    };
    EXPECT_EQ(tools.size(), expected_names.size());

    // Verify each tool has name, description, inputSchema.
    for (const auto& tool : tools) {
        EXPECT_TRUE(tool.contains("name"))
            << "Tool missing 'name'";
        EXPECT_TRUE(tool.contains("description"))
            << "Tool " << tool.value("name", "???") << " missing 'description'";
        EXPECT_TRUE(tool.contains("inputSchema"))
            << "Tool " << tool.value("name", "???") << " missing 'inputSchema'";
    }
}

TEST_F(McpHandlerTest, UnknownMethodThrows) {
    auto handler = make_handler();
    EXPECT_THROW(
        handler.dispatch("nonexistent/method", {}, 1),
        std::invalid_argument);
}

// ── Tool call: listWatchGroups ──────────────────────────────────────────────

TEST_F(McpHandlerTest, ListWatchGroupsReturnsStatus) {
    auto handler = make_handler();

    json params = {{"name", "kairos.listWatchGroups"}};
    auto result = handler.dispatch("tools/call", params, 1);

    // Result is wrapped in content array.
    EXPECT_TRUE(result.contains("content"));
    EXPECT_EQ(result["content"].size(), 1);
    EXPECT_EQ(result["content"][0]["type"], "text");

    // Parse the inner JSON.
    auto inner = json::parse(result["content"][0]["text"].get<std::string>());
    EXPECT_TRUE(inner.contains("watch_groups"));
    EXPECT_TRUE(inner.contains("count"));
    EXPECT_EQ(inner["count"], 1);
    EXPECT_EQ(inner["watch_groups"][0]["name"], "test_group");
}

TEST_F(McpHandlerTest, ListWatchGroupsNoEngine) {
    auto handler = make_handler_no_watch();

    json params = {{"name", "kairos.listWatchGroups"}};
    auto result = handler.dispatch("tools/call", params, 1);

    auto inner = json::parse(result["content"][0]["text"].get<std::string>());
    EXPECT_TRUE(inner.contains("error"));
}

// ── Tool call: getEvents ────────────────────────────────────────────────────

TEST_F(McpHandlerTest, GetEventsReturnsEmptyInitially) {
    auto handler = make_handler();

    json params = {
        {"name", "kairos.getEvents"},
        {"arguments", {{"limit", 10}}}
    };
    auto result = handler.dispatch("tools/call", params, 1);

    auto inner = json::parse(result["content"][0]["text"].get<std::string>());
    EXPECT_TRUE(inner.contains("events"));
    EXPECT_EQ(inner["count"], 0);
}

TEST_F(McpHandlerTest, GetEventsWithGroupFilter) {
    auto handler = make_handler();

    json params = {
        {"name", "kairos.getEvents"},
        {"arguments", {
            {"watch_group", "test_group"},
            {"limit", 5}
        }}
    };
    auto result = handler.dispatch("tools/call", params, 1);

    auto inner = json::parse(result["content"][0]["text"].get<std::string>());
    EXPECT_EQ(inner["count"], 0);
}

// ── Tool call: watchScanOnce ────────────────────────────────────────────────

TEST_F(McpHandlerTest, WatchScanOnceAllGroups) {
    auto handler = make_handler();

    json params = {{"name", "kairos.watchScanOnce"}};
    auto result = handler.dispatch("tools/call", params, 1);

    auto inner = json::parse(result["content"][0]["text"].get<std::string>());
    EXPECT_TRUE(inner.contains("scanned_groups"));
    EXPECT_EQ(inner["scanned_groups"], 1);
    EXPECT_TRUE(inner.contains("results"));
}

TEST_F(McpHandlerTest, WatchScanOnceSpecificGroup) {
    auto handler = make_handler();

    json params = {
        {"name", "kairos.watchScanOnce"},
        {"arguments", {{"watch_group", "test_group"}}}
    };
    auto result = handler.dispatch("tools/call", params, 1);

    auto inner = json::parse(result["content"][0]["text"].get<std::string>());
    EXPECT_EQ(inner["watch_group"], "test_group");
    EXPECT_TRUE(inner.contains("files_scanned"));
    EXPECT_TRUE(inner.contains("changes"));
    EXPECT_TRUE(inner.contains("scan_duration_ms"));
}

// ── Tool call: reloadConfig ─────────────────────────────────────────────────

TEST_F(McpHandlerTest, ReloadConfigSuccess) {
    auto handler = make_handler();

    json params = {{"name", "kairos.reloadConfig"}};
    auto result = handler.dispatch("tools/call", params, 1);

    auto inner = json::parse(result["content"][0]["text"].get<std::string>());
    EXPECT_TRUE(inner["success"]);
    EXPECT_TRUE(inner["errors"].empty());
}

TEST_F(McpHandlerTest, ReloadConfigFailure) {
    reload_should_fail_ = true;
    auto handler = make_handler();

    json params = {{"name", "kairos.reloadConfig"}};
    auto result = handler.dispatch("tools/call", params, 1);

    auto inner = json::parse(result["content"][0]["text"].get<std::string>());
    EXPECT_FALSE(inner["success"]);
    EXPECT_FALSE(inner["errors"].empty());
}

// ── Tool call: getMetrics ───────────────────────────────────────────────────

TEST_F(McpHandlerTest, GetMetricsReturnsJson) {
    // Register a counter so there's something to return.
    metrics_.register_counter("test_counter", "A test counter");

    auto handler = make_handler();

    json params = {{"name", "kairos.getMetrics"}};
    auto result = handler.dispatch("tools/call", params, 1);

    auto inner = json::parse(result["content"][0]["text"].get<std::string>());
    // Should be valid JSON (not an error).
    EXPECT_FALSE(inner.contains("error"));
}

// ── Stub tools ──────────────────────────────────────────────────────────────

TEST_F(McpHandlerTest, StubToolsReturnNotImplemented) {
    auto handler = make_handler();

    std::vector<std::string> stub_tools = {
        "kairos.listWorkflows", "kairos.getWorkflow",
        "kairos.runWorkflow",   "kairos.listJobs",
        "kairos.runJob",        "kairos.queryRuns",
        "kairos.getRunDetail",  "kairos.getRunLogs",
        "kairos.getStepOutput", "kairos.explainPlan",
    };

    for (const auto& tool_name : stub_tools) {
        json params = {{"name", tool_name}};
        auto result = handler.dispatch("tools/call", params, 1);

        auto inner = json::parse(
            result["content"][0]["text"].get<std::string>());
        EXPECT_EQ(inner["status"], "not_implemented")
            << "Tool " << tool_name << " should be stub";
    }
}

// ── Unknown tool ────────────────────────────────────────────────────────────

TEST_F(McpHandlerTest, UnknownToolThrows) {
    auto handler = make_handler();

    json params = {{"name", "kairos.nonexistent"}};
    EXPECT_THROW(
        handler.dispatch("tools/call", params, 1),
        std::invalid_argument);
}

// ── Missing tool name ───────────────────────────────────────────────────────

TEST_F(McpHandlerTest, MissingToolNameThrows) {
    auto handler = make_handler();

    EXPECT_THROW(
        handler.dispatch("tools/call", json::object(), 1),
        std::invalid_argument);
}

// ── Content wrapping ────────────────────────────────────────────────────────

TEST_F(McpHandlerTest, ToolResultWrappedInContentArray) {
    auto handler = make_handler();

    json params = {{"name", "kairos.getMetrics"}};
    auto result = handler.dispatch("tools/call", params, 1);

    EXPECT_TRUE(result.contains("content"));
    EXPECT_TRUE(result["content"].is_array());
    EXPECT_EQ(result["content"].size(), 1);
    EXPECT_EQ(result["content"][0]["type"], "text");
    EXPECT_TRUE(result["content"][0].contains("text"));
}
