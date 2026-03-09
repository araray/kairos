/// tests/unit/mcp/log_follow_wiring_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  MCP → RunStream wiring tests (§22.7)                                   ║
// ║                                                                          ║
// ║  Tests:                                                                  ║
// ║    1. start_log_follow subscribes to RunStream                          ║
// ║    2. Published chunks appear as base64 notifications                    ║
// ║    3. close_run emits run_complete notification                         ║
// ║    4. stderr chunks emitted with correct stream label                   ║
// ║    5. No RunStream → start_log_follow is safe no-op                    ║
// ║    6. Multiple chunks produce multiple notifications                    ║
// ║    7. No transport → safe no-op                                        ║
// ║    8. Multiline output is base64-encoded without embedded newlines      ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/mcp/handler.hpp"
#include "kairos/mcp/transport.hpp"
#include "kairos/exec/output_sink.hpp"
#include "kairos/observability/metrics.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <sstream>
#include <string>

using json = nlohmann::json;

namespace {

/// Collect all JSON lines from the output stream.
std::vector<json> parse_all(std::ostringstream& out) {
    std::vector<json> result;
    std::istringstream iss(out.str());
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty()) result.push_back(json::parse(line));
    }
    return result;
}

class McpLogFollowWiringTest : public ::testing::Test {
protected:
    void SetUp() override {
        out_.str("");
        out_.clear();
    }

    struct Handles {
        std::unique_ptr<kairos::mcp::McpHandler> handler;
        std::unique_ptr<kairos::mcp::StdioTransport> transport;
    };

    /// Create a handler wired to RunStream + capture transport.
    Handles make_handler(kairos::exec::RunStream* rs = nullptr) {
        auto transport = std::make_unique<kairos::mcp::StdioTransport>(
            [](const std::string&, const json&, const json&) -> json {
                return json::object();
            },
            in_, out_);

        kairos::mcp::McpHandler::Dependencies deps;
        deps.transport = transport.get();
        deps.metrics = &metrics_;
        deps.run_stream = rs;
        deps.server_info.name = "kairos-test";
        deps.server_info.version = "1.0.0";

        auto handler = std::make_unique<kairos::mcp::McpHandler>(
            std::move(deps));

        return {std::move(handler), std::move(transport)};
    }

    std::istringstream in_;
    std::ostringstream out_;
    kairos::metrics::MetricsRegistry metrics_;
};

// ── start_log_follow subscribes to RunStream ─────────────────────────────

TEST_F(McpLogFollowWiringTest, SubscribesToRunStream) {
    kairos::exec::RunStream rs;
    auto [handler, transport] = make_handler(&rs);

    EXPECT_EQ(rs.subscriber_count("run-abc"), 0u);

    handler->start_log_follow("run-abc");

    // Should have 1 output subscriber + 1 close subscriber.
    // subscriber_count only counts output subscribers.
    EXPECT_GE(rs.subscriber_count("run-abc"), 1u);
}

// ── Published chunks appear as base64 notifications ─────────────────────

TEST_F(McpLogFollowWiringTest, PublishedChunkEmitsNotification) {
    kairos::exec::RunStream rs;
    auto [handler, transport] = make_handler(&rs);

    handler->start_log_follow("run-abc");

    // Publish a chunk to the RunStream.
    rs.publish("run-abc", "build-job", "step-1", "Hello World", false);

    auto notifications = parse_all(out_);
    ASSERT_GE(notifications.size(), 1u);

    auto& n = notifications[0];
    EXPECT_EQ(n["method"], "notifications/log_chunk");
    EXPECT_EQ(n["params"]["run_id"], "run-abc");
    EXPECT_EQ(n["params"]["job_id"], "build-job");
    EXPECT_EQ(n["params"]["stream"], "stdout");
    // "Hello World" in base64 = "SGVsbG8gV29ybGQ="
    EXPECT_EQ(n["params"]["data"], "SGVsbG8gV29ybGQ=");
}

// ── close_run emits run_complete notification ───────────────────────────

TEST_F(McpLogFollowWiringTest, CloseRunEmitsRunComplete) {
    kairos::exec::RunStream rs;
    auto [handler, transport] = make_handler(&rs);

    handler->start_log_follow("run-xyz");

    // Publish some output, then close.
    rs.publish("run-xyz", "job", "step", "data", false);
    rs.close_run("run-xyz");

    auto notifications = parse_all(out_);
    ASSERT_GE(notifications.size(), 2u);

    // Last notification should be run_complete.
    auto& last = notifications.back();
    EXPECT_EQ(last["method"], "notifications/run_complete");
    EXPECT_EQ(last["params"]["run_id"], "run-xyz");
}

// ── stderr chunks use correct stream label ──────────────────────────────

TEST_F(McpLogFollowWiringTest, StderrChunkUsesCorrectStreamLabel) {
    kairos::exec::RunStream rs;
    auto [handler, transport] = make_handler(&rs);

    handler->start_log_follow("run-err");

    rs.publish("run-err", "job", "step", "error msg", true);

    auto notifications = parse_all(out_);
    ASSERT_GE(notifications.size(), 1u);
    EXPECT_EQ(notifications[0]["params"]["stream"], "stderr");
}

// ── No RunStream → safe no-op ────────────────────────────────────────────

TEST_F(McpLogFollowWiringTest, NoRunStreamIsSafeNoop) {
    auto [handler, transport] = make_handler(nullptr);

    EXPECT_NO_THROW(handler->start_log_follow("run-noop"));

    auto notifications = parse_all(out_);
    EXPECT_TRUE(notifications.empty());
}

// ── Multiple chunks produce multiple notifications ──────────────────────

TEST_F(McpLogFollowWiringTest, MultipleChunksMultipleNotifications) {
    kairos::exec::RunStream rs;
    auto [handler, transport] = make_handler(&rs);

    handler->start_log_follow("run-multi");

    rs.publish("run-multi", "j", "s", "line1", false);
    rs.publish("run-multi", "j", "s", "line2", false);
    rs.publish("run-multi", "j", "s", "line3", false);

    auto notifications = parse_all(out_);
    EXPECT_EQ(notifications.size(), 3u);
    for (const auto& n : notifications) {
        EXPECT_EQ(n["method"], "notifications/log_chunk");
    }
}

// ── No transport → safe no-op ───────────────────────────────────────────

TEST_F(McpLogFollowWiringTest, NoTransportSafeNoop) {
    kairos::exec::RunStream rs;

    kairos::mcp::McpHandler::Dependencies deps;
    deps.transport = nullptr;
    deps.run_stream = &rs;
    deps.server_info.name = "test";

    kairos::mcp::McpHandler handler(std::move(deps));

    // Should not crash even though we subscribe.
    EXPECT_NO_THROW(handler.start_log_follow("run-notrans"));
}

// ── Multiline output is base64-encoded ──────────────────────────────────

TEST_F(McpLogFollowWiringTest, MultilineOutputBase64Encoded) {
    kairos::exec::RunStream rs;
    auto [handler, transport] = make_handler(&rs);

    handler->start_log_follow("run-ml");

    std::string multiline = "line1\nline2\nline3";
    rs.publish("run-ml", "j", "s", multiline, false);

    auto notifications = parse_all(out_);
    ASSERT_GE(notifications.size(), 1u);

    // The notification frame must be a single line.
    std::string raw = out_.str();
    auto newline_count = std::count(raw.begin(), raw.end(), '\n');
    EXPECT_EQ(newline_count, static_cast<long>(notifications.size()));
}

}  // anonymous namespace
