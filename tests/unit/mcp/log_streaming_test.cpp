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

// ── Test fixture ─────────────────────────────────────────────────────────

/// Capture output from StdioTransport into a stringstream.
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
        // Create transport with fake I/O.
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

// We test base64 indirectly via emit_log_chunk which encodes the data.
// The base64 encoder is internal to handler.cpp, so we verify through
// the notification output.

TEST_F(LogStreamingTest, EmitLogChunkEncodesBase64) {
    auto [handler, transport] = make_handler();

    // Emit a log chunk. The data "Hello\nWorld" contains a newline
    // which is why base64 encoding is needed (§22.7).
    handler->emit_log_chunk("run-abc", "build", "stdout", "Hello\nWorld");

    // Parse the notification from output.
    std::string line;
    std::getline(out_, line);
    auto notification = json::parse(line);

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

    std::string line;
    std::getline(out_, line);
    auto notification = json::parse(line);

    EXPECT_EQ(notification["params"]["run_id"], "run-xyz");
    EXPECT_EQ(notification["params"]["stream"], "stderr");
    EXPECT_EQ(notification["params"]["data"], "");  // Empty → empty base64.

    // job_id should not be present when empty.
    EXPECT_FALSE(notification["params"].contains("job_id"));
}

TEST_F(LogStreamingTest, EmitLogChunkBase64RFC4648Vector_f) {
    // RFC 4648 § 10: "f" → "Zg=="
    auto [handler, transport] = make_handler();
    handler->emit_log_chunk("r", "", "stdout", "f");

    std::string line;
    std::getline(out_, line);
    auto notification = json::parse(line);
    EXPECT_EQ(notification["params"]["data"], "Zg==");
}

TEST_F(LogStreamingTest, EmitLogChunkBase64RFC4648Vector_fo) {
    // "fo" → "Zm8="
    auto [handler, transport] = make_handler();
    handler->emit_log_chunk("r", "", "stdout", "fo");

    std::string line;
    std::getline(out_, line);
    auto notification = json::parse(line);
    EXPECT_EQ(notification["params"]["data"], "Zm8=");
}

TEST_F(LogStreamingTest, EmitLogChunkBase64RFC4648Vector_foo) {
    // "foo" → "Zm9v"
    auto [handler, transport] = make_handler();
    handler->emit_log_chunk("r", "", "stdout", "foo");

    std::string line;
    std::getline(out_, line);
    auto notification = json::parse(line);
    EXPECT_EQ(notification["params"]["data"], "Zm9v");
}

TEST_F(LogStreamingTest, EmitLogChunkBase64RFC4648Vector_foob) {
    // "foob" → "Zm9vYg=="
    auto [handler, transport] = make_handler();
    handler->emit_log_chunk("r", "", "stdout", "foob");

    std::string line;
    std::getline(out_, line);
    auto notification = json::parse(line);
    EXPECT_EQ(notification["params"]["data"], "Zm9vYg==");
}

TEST_F(LogStreamingTest, EmitLogChunkBase64RFC4648Vector_fooba) {
    // "fooba" → "Zm9vYmE="
    auto [handler, transport] = make_handler();
    handler->emit_log_chunk("r", "", "stdout", "fooba");

    std::string line;
    std::getline(out_, line);
    auto notification = json::parse(line);
    EXPECT_EQ(notification["params"]["data"], "Zm9vYmE=");
}

TEST_F(LogStreamingTest, EmitLogChunkBase64RFC4648Vector_foobar) {
    // "foobar" → "Zm9vYmFy"
    auto [handler, transport] = make_handler();
    handler->emit_log_chunk("r", "", "stdout", "foobar");

    std::string line;
    std::getline(out_, line);
    auto notification = json::parse(line);
    EXPECT_EQ(notification["params"]["data"], "Zm9vYmFy");
}

// ── Run complete notification ────────────────────────────────────────────

TEST_F(LogStreamingTest, EmitRunComplete) {
    auto [handler, transport] = make_handler();

    handler->emit_run_complete("run-abc", "success", 4523);

    std::string line;
    std::getline(out_, line);
    auto notification = json::parse(line);

    EXPECT_EQ(notification["jsonrpc"], "2.0");
    EXPECT_EQ(notification["method"], "notifications/run_complete");
    EXPECT_EQ(notification["params"]["run_id"], "run-abc");
    EXPECT_EQ(notification["params"]["status"], "success");
    EXPECT_EQ(notification["params"]["duration_ms"], 4523);

    // Notifications have no "id" field.
    EXPECT_FALSE(notification.contains("id"));
}

TEST_F(LogStreamingTest, EmitRunCompleteFailure) {
    auto [handler, transport] = make_handler();

    handler->emit_run_complete("run-xyz", "failure", 125);

    std::string line;
    std::getline(out_, line);
    auto notification = json::parse(line);

    EXPECT_EQ(notification["params"]["status"], "failure");
    EXPECT_EQ(notification["params"]["duration_ms"], 125);
}

// ── Initialize response capabilities ─────────────────────────────────────

TEST_F(LogStreamingTest, InitializeIncludesNotificationCapabilities) {
    auto [handler, transport] = make_handler();

    auto result = handler->dispatch("initialize", json::object(), 1);

    ASSERT_TRUE(result.contains("capabilities"));
    auto caps = result["capabilities"];

    // Logging capability per §22.7.
    EXPECT_TRUE(caps.contains("logging"));

    // Notification types supported.
    EXPECT_TRUE(caps.contains("notifications"));
    EXPECT_TRUE(caps["notifications"]["log_chunk"]);
    EXPECT_TRUE(caps["notifications"]["run_complete"]);
}

// ── start_log_follow ─────────────────────────────────────────────────────

TEST_F(LogStreamingTest, StartLogFollowIsCallable) {
    auto [handler, transport] = make_handler();

    // start_log_follow should not throw in v1 (it's a stub that
    // logs a diagnostic). Just verify it doesn't crash.
    EXPECT_NO_THROW(handler->start_log_follow("run-123"));
}

// ── No transport: emit is safe no-op ─────────────────────────────────────

TEST_F(LogStreamingTest, EmitWithNullTransportIsNoop) {
    kairos::mcp::McpHandler::Dependencies deps;
    deps.transport = nullptr;  // No transport.
    deps.server_info.name = "test";

    kairos::mcp::McpHandler handler(std::move(deps));

    // Should not crash or throw.
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

    // Read all 4 notifications.
    std::string line;
    int count = 0;
    while (std::getline(out_, line)) {
        auto n = json::parse(line);
        EXPECT_EQ(n["jsonrpc"], "2.0");
        ++count;
    }
    EXPECT_EQ(count, 4);
}

// ── No embedded newlines in notification frame ───────────────────────────

TEST_F(LogStreamingTest, NotificationHasNoEmbeddedNewlines) {
    auto [handler, transport] = make_handler();

    // Emit data with lots of newlines.
    std::string multiline = "line1\nline2\nline3\n\nline5\n";
    handler->emit_log_chunk("r", "j", "stdout", multiline);

    std::string raw = out_.str();

    // The entire notification should be one line.
    auto newline_count = std::count(raw.begin(), raw.end(), '\n');
    EXPECT_EQ(newline_count, 1)
        << "Notification frame must be a single line (no embedded newlines)";
}

}  // anonymous namespace
