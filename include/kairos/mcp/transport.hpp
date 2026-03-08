/// include/kairos/mcp/transport.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/mcp/transport.hpp — MCP stdio transport layer                     ║
// ║                                                                          ║
// ║  Reads newline-delimited JSON-RPC 2.0 from stdin, dispatches to a       ║
// ║  handler callback, and writes JSON-RPC 2.0 responses to stdout.         ║
// ║                                                                          ║
// ║  Critical constraint: no embedded newlines in a single JSON-RPC frame.  ║
// ║  All JSON is serialized in compact format.                              ║
// ║                                                                          ║
// ║  Spec reference: §22.3                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>

namespace kairos::mcp {

using json = nlohmann::json;

/// JSON-RPC 2.0 error codes per spec.
enum class ErrorCode : int {
    ParseError      = -32700,
    InvalidRequest  = -32600,
    MethodNotFound  = -32601,
    InvalidParams   = -32602,
    InternalError   = -32603,
};

/// Handler function signature.
/// Takes method, params, and id. Returns the "result" JSON for the response.
/// If id is null (notification), the return value is ignored.
using RequestHandler = std::function<json(
    const std::string& method,
    const json& params,
    const json& id)>;

/// MCP stdio transport.
///
/// Reads newline-delimited JSON from an input stream, dispatches to the
/// handler, and writes compact JSON-RPC 2.0 responses to an output stream.
///
/// Thread safety:
///   - run() blocks on the reader stream (call from a dedicated thread).
///   - send_notification() can be called from any thread.
///   - write_response() serializes writes via write_mutex_.
class StdioTransport {
public:
    /// Construct with I/O streams (defaults to stdin/stdout).
    explicit StdioTransport(
        RequestHandler handler,
        std::istream& in = std::cin,
        std::ostream& out = std::cout)
        : handler_(std::move(handler))
        , in_(in)
        , out_(out) {}

    /// Run the read-dispatch-write loop until EOF or stop().
    /// Blocks on input reads.
    void run();

    /// Signal the transport to stop.
    void stop() {
        stopped_.store(true, std::memory_order_relaxed);
    }

    /// Check if the transport has been stopped.
    [[nodiscard]] bool is_stopped() const {
        return stopped_.load(std::memory_order_relaxed);
    }

    /// Send a JSON-RPC notification (no id, no response expected).
    /// Used for log streaming and event notifications.
    void send_notification(
        const std::string& method, const json& params);

    /// Number of requests processed.
    [[nodiscard]] int64_t requests_processed() const {
        return requests_processed_.load(std::memory_order_relaxed);
    }

    /// Number of errors encountered.
    [[nodiscard]] int64_t errors() const {
        return errors_.load(std::memory_order_relaxed);
    }

    // ── Static helpers (public for testing) ─────────────────────────

    /// Build a JSON-RPC 2.0 success response.
    static json make_success_response(const json& id, const json& result);

    /// Build a JSON-RPC 2.0 error response.
    static json make_error_response(
        const json& id, ErrorCode code, const std::string& message);

private:
    /// Process a single JSON-RPC request and return the response.
    /// Returns json(nullptr) for notifications (no response needed).
    json process_request(const json& request);

    /// Write a JSON response to output (thread-safe).
    void write_response(const json& response);

    RequestHandler handler_;
    std::istream& in_;
    std::ostream& out_;
    std::atomic<bool> stopped_{false};
    std::mutex write_mutex_;
    std::atomic<int64_t> requests_processed_{0};
    std::atomic<int64_t> errors_{0};
};

}  // namespace kairos::mcp
