/// tests/unit/mcp/transport_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  StdioTransport unit tests — JSON-RPC 2.0 protocol compliance           ║
// ║                                                                          ║
// ║  Tests: valid requests, error handling, notifications, compact JSON,    ║
// ║  parse errors, invalid requests, and concurrent notification safety.    ║
// ║                                                                          ║
// ║  Spec reference: §22.3                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/mcp/transport.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <sstream>
#include <string>

using json = nlohmann::json;
using namespace kairos::mcp;

// ── Helpers ────────────────────────────────────────────────────────────────

/// Build a JSON-RPC 2.0 request as a newline-terminated string.
static std::string make_request(const std::string& method,
                                 const json& params = json::object(),
                                 const json& id = 1) {
    json req = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params},
        {"id", id}
    };
    return req.dump(-1) + "\n";
}

/// Build a JSON-RPC 2.0 notification (no "id" key).
static std::string make_notification(const std::string& method,
                                      const json& params = json::object()) {
    json req = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params}
    };
    return req.dump(-1) + "\n";
}

/// Parse all responses from an output stream (one per line).
static std::vector<json> parse_responses(const std::string& output) {
    std::vector<json> results;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty()) {
            results.push_back(json::parse(line));
        }
    }
    return results;
}

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(StdioTransportTest, ValidRequestReturnsSuccessResponse) {
    std::istringstream in(make_request("test_method", {{"key", "val"}}, 42));
    std::ostringstream out;

    StdioTransport transport(
        [](const std::string& method, const json& params,
           const json& /*id*/) -> json {
            return {{"method_echo", method}, {"got", params}};
        }, in, out);

    transport.run();

    auto responses = parse_responses(out.str());
    ASSERT_EQ(responses.size(), 1);

    const auto& resp = responses[0];
    EXPECT_EQ(resp["jsonrpc"], "2.0");
    EXPECT_EQ(resp["id"], 42);
    EXPECT_TRUE(resp.contains("result"));
    EXPECT_EQ(resp["result"]["method_echo"], "test_method");
    EXPECT_EQ(resp["result"]["got"]["key"], "val");
}

TEST(StdioTransportTest, ParseErrorReturnsErrorResponse) {
    std::istringstream in("this is not json\n");
    std::ostringstream out;

    StdioTransport transport(
        [](const std::string&, const json&, const json&) -> json {
            return {};
        }, in, out);

    transport.run();

    auto responses = parse_responses(out.str());
    ASSERT_EQ(responses.size(), 1);

    const auto& resp = responses[0];
    EXPECT_EQ(resp["jsonrpc"], "2.0");
    EXPECT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"],
              static_cast<int>(ErrorCode::ParseError));
}

TEST(StdioTransportTest, InvalidRequestMissingMethod) {
    json req = {{"jsonrpc", "2.0"}, {"id", 1}};
    std::istringstream in(req.dump(-1) + "\n");
    std::ostringstream out;

    StdioTransport transport(
        [](const std::string&, const json&, const json&) -> json {
            return {};
        }, in, out);

    transport.run();

    auto responses = parse_responses(out.str());
    ASSERT_EQ(responses.size(), 1);
    EXPECT_EQ(responses[0]["error"]["code"],
              static_cast<int>(ErrorCode::InvalidRequest));
}

TEST(StdioTransportTest, InvalidRequestWrongVersion) {
    json req = {{"jsonrpc", "1.0"}, {"method", "test"}, {"id", 1}};
    std::istringstream in(req.dump(-1) + "\n");
    std::ostringstream out;

    StdioTransport transport(
        [](const std::string&, const json&, const json&) -> json {
            return {};
        }, in, out);

    transport.run();

    auto responses = parse_responses(out.str());
    ASSERT_EQ(responses.size(), 1);
    EXPECT_EQ(responses[0]["error"]["code"],
              static_cast<int>(ErrorCode::InvalidRequest));
}

TEST(StdioTransportTest, NotificationProducesNoResponse) {
    std::istringstream in(make_notification("notify_me", {{"x", 1}}));
    std::ostringstream out;

    bool handler_called = false;
    StdioTransport transport(
        [&](const std::string& method, const json& params,
            const json& id) -> json {
            handler_called = true;
            EXPECT_EQ(method, "notify_me");
            EXPECT_EQ(params["x"], 1);
            EXPECT_TRUE(id.is_null());
            return {};
        }, in, out);

    transport.run();

    EXPECT_TRUE(handler_called);
    // Notifications must NOT produce output.
    EXPECT_TRUE(out.str().empty());
}

TEST(StdioTransportTest, HandlerExceptionBecomesInternalError) {
    std::istringstream in(make_request("fail", {}, 99));
    std::ostringstream out;

    StdioTransport transport(
        [](const std::string&, const json&, const json&) -> json {
            throw std::runtime_error("something broke");
        }, in, out);

    transport.run();

    auto responses = parse_responses(out.str());
    ASSERT_EQ(responses.size(), 1);
    EXPECT_EQ(responses[0]["error"]["code"],
              static_cast<int>(ErrorCode::InternalError));
    EXPECT_NE(responses[0]["error"]["message"].get<std::string>()
                  .find("something broke"), std::string::npos);
}

TEST(StdioTransportTest, InvalidArgumentBecomesInvalidParams) {
    std::istringstream in(make_request("bad_args", {}, 7));
    std::ostringstream out;

    StdioTransport transport(
        [](const std::string&, const json&, const json&) -> json {
            throw std::invalid_argument("missing required field");
        }, in, out);

    transport.run();

    auto responses = parse_responses(out.str());
    ASSERT_EQ(responses.size(), 1);
    EXPECT_EQ(responses[0]["error"]["code"],
              static_cast<int>(ErrorCode::InvalidParams));
}

TEST(StdioTransportTest, MultipleRequestsProcessedSequentially) {
    std::string input = make_request("m1", {}, 1) +
                        make_request("m2", {}, 2) +
                        make_request("m3", {}, 3);
    std::istringstream in(input);
    std::ostringstream out;

    int call_count = 0;
    StdioTransport transport(
        [&](const std::string& method, const json&,
            const json&) -> json {
            ++call_count;
            return {{"method", method}};
        }, in, out);

    transport.run();

    EXPECT_EQ(call_count, 3);

    auto responses = parse_responses(out.str());
    ASSERT_EQ(responses.size(), 3);
    EXPECT_EQ(responses[0]["id"], 1);
    EXPECT_EQ(responses[1]["id"], 2);
    EXPECT_EQ(responses[2]["id"], 3);
}

TEST(StdioTransportTest, EmptyLinesAreSkipped) {
    std::string input = "\n\n" + make_request("ok", {}, 1) + "\n\n";
    std::istringstream in(input);
    std::ostringstream out;

    StdioTransport transport(
        [](const std::string&, const json&, const json&) -> json {
            return {{"ok", true}};
        }, in, out);

    transport.run();

    auto responses = parse_responses(out.str());
    ASSERT_EQ(responses.size(), 1);
    EXPECT_TRUE(responses[0]["result"]["ok"]);
}

TEST(StdioTransportTest, NoEmbeddedNewlinesInOutput) {
    std::istringstream in(make_request("test", {}, 1));
    std::ostringstream out;

    StdioTransport transport(
        [](const std::string&, const json&, const json&) -> json {
            // Return something that could tempt pretty-printing.
            return {
                {"list", {1, 2, 3}},
                {"nested", {{"a", "b"}, {"c", "d"}}}
            };
        }, in, out);

    transport.run();

    // Each response should be a single line (no embedded newlines).
    auto output = out.str();
    // Count newlines — should be exactly 1 (the line terminator).
    auto newline_count = std::count(output.begin(), output.end(), '\n');
    EXPECT_EQ(newline_count, 1);
}

TEST(StdioTransportTest, SendNotificationWritesToOutput) {
    std::istringstream in("");  // No requests — just test notification.
    std::ostringstream out;

    StdioTransport transport(
        [](const std::string&, const json&, const json&) -> json {
            return {};
        }, in, out);

    transport.send_notification("log/chunk", {{"data", "SGVsbG8="}});

    auto output = out.str();
    EXPECT_FALSE(output.empty());

    auto responses = parse_responses(output);
    ASSERT_EQ(responses.size(), 1);

    const auto& notif = responses[0];
    EXPECT_EQ(notif["jsonrpc"], "2.0");
    EXPECT_EQ(notif["method"], "log/chunk");
    EXPECT_EQ(notif["params"]["data"], "SGVsbG8=");
    EXPECT_FALSE(notif.contains("id"));
}

TEST(StdioTransportTest, RequestCounterTracksProcessed) {
    std::string input = make_request("a", {}, 1) + make_request("b", {}, 2);
    std::istringstream in(input);
    std::ostringstream out;

    StdioTransport transport(
        [](const std::string&, const json&, const json&) -> json {
            return {};
        }, in, out);

    EXPECT_EQ(transport.requests_processed(), 0);
    transport.run();
    EXPECT_EQ(transport.requests_processed(), 2);
}

TEST(StdioTransportTest, ErrorCounterTracksErrors) {
    std::string input = "bad json\n" + make_request("ok", {}, 1);
    std::istringstream in(input);
    std::ostringstream out;

    StdioTransport transport(
        [](const std::string&, const json&, const json&) -> json {
            return {};
        }, in, out);

    transport.run();
    EXPECT_EQ(transport.errors(), 1);
    // The valid request still counted.
    EXPECT_EQ(transport.requests_processed(), 1);
}

TEST(StdioTransportTest, MakeSuccessResponseStructure) {
    auto resp = StdioTransport::make_success_response(42, {{"ok", true}});
    EXPECT_EQ(resp["jsonrpc"], "2.0");
    EXPECT_EQ(resp["id"], 42);
    EXPECT_TRUE(resp["result"]["ok"]);
    EXPECT_FALSE(resp.contains("error"));
}

TEST(StdioTransportTest, MakeErrorResponseStructure) {
    auto resp = StdioTransport::make_error_response(
        7, ErrorCode::MethodNotFound, "nope");
    EXPECT_EQ(resp["jsonrpc"], "2.0");
    EXPECT_EQ(resp["id"], 7);
    EXPECT_EQ(resp["error"]["code"],
              static_cast<int>(ErrorCode::MethodNotFound));
    EXPECT_EQ(resp["error"]["message"], "nope");
    EXPECT_FALSE(resp.contains("result"));
}

TEST(StdioTransportTest, RequestWithStringId) {
    json req = {
        {"jsonrpc", "2.0"},
        {"method", "test"},
        {"id", "abc-123"}
    };
    std::istringstream in(req.dump(-1) + "\n");
    std::ostringstream out;

    StdioTransport transport(
        [](const std::string&, const json&, const json&) -> json {
            return {{"ok", true}};
        }, in, out);

    transport.run();

    auto responses = parse_responses(out.str());
    ASSERT_EQ(responses.size(), 1);
    EXPECT_EQ(responses[0]["id"], "abc-123");
}

TEST(StdioTransportTest, RequestWithNullIdGetsResponse) {
    // A request with "id": null should get a response (unlike notification
    // where "id" key is absent entirely).
    json req = {
        {"jsonrpc", "2.0"},
        {"method", "test"},
        {"id", nullptr}
    };
    std::istringstream in(req.dump(-1) + "\n");
    std::ostringstream out;

    StdioTransport transport(
        [](const std::string&, const json&, const json&) -> json {
            return {{"ok", true}};
        }, in, out);

    transport.run();

    auto responses = parse_responses(out.str());
    ASSERT_EQ(responses.size(), 1);
    // Response should have id: null.
    EXPECT_TRUE(responses[0]["id"].is_null());
    EXPECT_TRUE(responses[0]["result"]["ok"]);
}
