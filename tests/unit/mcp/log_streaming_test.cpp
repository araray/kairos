/// tests/unit/mcp/log_streaming_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  MCP log streaming unit tests (§22.7)                                    ║
// ║                                                                          ║
// ║  Tests:                                                                  ║
// ║    - Base64 encoding correctness (RFC 4648 vectors)                     ║
// ║    - Log chunk notification emission                                     ║
// ║    - Run complete notification emission                                  ║
// ║    - Initialize response includes notification capabilities              ║
// ║    - start_log_follow method exists and is callable                      ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/mcp/handler.hpp"
#include "kairos/mcp/transport.hpp"
#include "kairos/observability/metrics.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <sstream>
#include <string>

using json = nlohmann::json;

namespace {

// ── Helper: parse the first notification line from output ────────────────

/// StdioTransport writes compact JSON + '\n' to the output stream.
/// This helper extracts the first complete JSON line from an ostringstream.
json parse_first_notification(std::ostringstream& out) {
    std::istringstream iss(out.str());
    std::string line;
    if (std::getline(iss, line) && !line.empty()) {
        return json::parse(line);
    }
    return json{};
}

/// Count and collect all notifications from the output stream.
std::vector<json> parse_all_notifications(std::ostringstream& out) {
    std::vector<json> result;
    std::istringstream iss(out.str());
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty()) {
            result.push_back(json::parse(line));
        }
    }
    return result;
}

// ── Test fixture ─────────────────────────────────────────────────────────

class LogStreamingTest : public ::testing::Test {
protected:
    void SetUp() override {
        out_.str("");
        out_.clear();
    }

    /// Create a handler with a transport wired to capture output.
    std::pair<
        std::unique_ptr<kairos::mcp::McpHandler>,
        std::unique_ptr<kairos::mcp::StdioTransport>
    > make_handler() {
        auto transport = std::make_unique<kairos::mcp::StdioTransport>(
            [](const std::string&, const json&, const json&) -> json {
                return json::object();
            },
            in_, out_);

        kairos::mcp::McpHandler::Dependencies deps;
        deps.transport = transport.get();
        deps.metrics = &metrics_;
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

// ── Base64 encoding tests (RFC 4648 test vectors) ────────────────────────

TEST_F(LogStreamingTest, EmitLogChunkEncodesBase64) {
    auto [handler, transport] = make_handler();

    handler->emit_log_chunk("run-abc", "build", "stdout", "Hello\nWorld");

    auto notification = parse_first_notification(out_);

    EXPECT_EQ(notification["jsonrpc"], "2.0");
    EXPECT_EQ(notification["method"], "notifications/log_chunk");
    EXPECT_EQ(notification["params"]["run_id"], "run-abc");
    EXPECT_EQ(notification["params"]["job_id"], "build");
    EXPECT_EQ(notification["params"]["stream"], "stdout");
    // "Hello\nWorld" base64 = "SGVsbG8KV29ybGQ="
    EXPECT_EQ(notification["params"]["data"], "SGVsbG8KV29ybGQ=");
}

TEST_F(LogStreamingTest, EmitLogChunkEmptyData) {
    auto [handler, transport] = make_handler();

    handler->emit_log_chunk("run-xyz", "", "stderr", "");

    auto notification = parse_first_notification(out_);

    EXPECT_EQ(notification["params"]["run_id"], "run-xyz");
    EXPECT_EQ(notification["params"]["stream"], "stderr");
    EXPECT_EQ(notification["params"]["data"], "");
    EXPECT_FALSE(notification["params"].contains("job_id"));
}

TEST_F(LogStreamingTest, EmitLogChunkBase64RFC4648Vector_f) {
    auto [handler, transport] = make_handler();
    handler->emit_log_chunk("r", "", "stdout", "f");
    EXPECT_EQ(parse_first_notification(out_)["params"]["data"], "Zg==");
}

TEST_F(LogStreamingTest, EmitLogChunkBase64RFC4648Vector_fo) {
    auto [handler, transport] = make_handler();
    handler->emit_log_chunk("r", "", "stdout", "fo");
    EXPECT_EQ(parse_first_notification(out_)["params"]["data"], "Zm8=");
}

TEST_F(LogStreamingTest, EmitLogChunkBase64RFC4648Vector_foo) {
    auto [handler, transport] = make_handler();
    handler->emit_log_chunk("r", "", "stdout", "foo");
    EXPECT_EQ(parse_first_notification(out_)["params"]["data"], "Zm9v");
}

TEST_F(LogStreamingTest, EmitLogChunkBase64RFC4648Vector_foob) {
    auto [handler, transport] = make_handler();
    handler->emit_log_chunk("r", "", "stdout", "foob");
    EXPECT_EQ(parse_first_notification(out_)["params"]["data"], "Zm9vYg==");
}

TEST_F(LogStreamingTest, EmitLogChunkBase64RFC4648Vector_fooba) {
    auto [handler, transport] = make_handler();
    handler->emit_log_chunk("r", "", "stdout", "fooba");
    EXPECT_EQ(parse_first_notification(out_)["params"]["data"], "Zm9vYmE=");
}

TEST_F(LogStreamingTest, EmitLogChunkBase64RFC4648Vector_foobar) {
    auto [handler, transport] = make_handler();
    handler->emit_log_chunk("r", "", "stdout", "foobar");
    EXPECT_EQ(parse_first_notification(out_)["params"]["data"], "Zm9vYmFy");
}

// ── Run complete notification ────────────────────────────────────────────

TEST_F(LogStreamingTest, EmitRunComplete) {
    auto [handler, transport] = make_handler();

    handler->emit_run_complete("run-abc", "success", 4523);

    auto notification = parse_first_notification(out_);

    EXPECT_EQ(notification["jsonrpc"], "2.0");
    EXPECT_EQ(notification["method"], "notifications/run_complete");
    EXPECT_EQ(notification["params"]["run_id"], "run-abc");
    EXPECT_EQ(notification["params"]["status"], "success");
    EXPECT_EQ(notification["params"]["duration_ms"], 4523);
    EXPECT_FALSE(notification.contains("id"));
}

TEST_F(LogStreamingTest, EmitRunCompleteFailure) {
    auto [handler, transport] = make_handler();

    handler->emit_run_complete("run-xyz", "failure", 125);

    auto notification = parse_first_notification(out_);
    EXPECT_EQ(notification["params"]["status"], "failure");
    EXPECT_EQ(notification["params"]["duration_ms"], 125);
}

// ── Initialize response capabilities ─────────────────────────────────────

TEST_F(LogStreamingTest, InitializeIncludesNotificationCapabilities) {
    auto [handler, transport] = make_handler();

    auto result = handler->dispatch("initialize", json::object(), 1);

    ASSERT_TRUE(result.contains("capabilities"));
    auto caps = result["capabilities"];

    EXPECT_TRUE(caps.contains("logging"));
    EXPECT_TRUE(caps.contains("notifications"));
    EXPECT_TRUE(caps["notifications"]["log_chunk"]);
    EXPECT_TRUE(caps["notifications"]["run_complete"]);
}

// ── start_log_follow ─────────────────────────────────────────────────────

TEST_F(LogStreamingTest, StartLogFollowIsCallable) {
    auto [handler, transport] = make_handler();
    EXPECT_NO_THROW(handler->start_log_follow("run-123"));
}

// ── No transport: emit is safe no-op ─────────────────────────────────────

TEST_F(LogStreamingTest, EmitWithNullTransportIsNoop) {
    kairos::mcp::McpHandler::Dependencies deps;
    deps.transport = nullptr;
    deps.server_info.name = "test";

    kairos::mcp::McpHandler handler(std::move(deps));

    EXPECT_NO_THROW(handler.emit_log_chunk("r", "j", "stdout", "data"));
    EXPECT_NO_THROW(handler.emit_run_complete("r", "success", 100));
}

// ── Multiple chunks ──────────────────────────────────────────────────────

TEST_F(LogStreamingTest, MultipleChunksEmitSeparateNotifications) {
    auto [handler, transport] = make_handler();

    handler->emit_log_chunk("r1", "build", "stdout", "line1");
    handler->emit_log_chunk("r1", "build", "stdout", "line2");
    handler->emit_log_chunk("r1", "build", "stderr", "error1");
    handler->emit_run_complete("r1", "failure", 1000);

    auto notifications = parse_all_notifications(out_);
    EXPECT_EQ(notifications.size(), 4u);
    for (const auto& n : notifications) {
        EXPECT_EQ(n["jsonrpc"], "2.0");
    }
}

// ── No embedded newlines in notification frame ───────────────────────────

TEST_F(LogStreamingTest, NotificationHasNoEmbeddedNewlines) {
    auto [handler, transport] = make_handler();

    std::string multiline = "line1\nline2\nline3\n\nline5\n";
    handler->emit_log_chunk("r", "j", "stdout", multiline);

    std::string raw = out_.str();

    // One trailing newline only (the frame delimiter).
    auto newline_count = std::count(raw.begin(), raw.end(), '\n');
    EXPECT_EQ(newline_count, 1)
        << "Notification frame must be a single line (no embedded newlines)";
}

}  // anonymous namespace
