/// src/mcp/transport.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  transport.cpp — MCP stdio transport: JSON-RPC 2.0 over stdin/stdout    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/mcp/transport.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace kairos::mcp {

// ── Public API ─────────────────────────────────────────────────────────────

void StdioTransport::run() {
    std::string line;
    while (!stopped_.load(std::memory_order_relaxed)) {
        // Read one line from input.
        if (!std::getline(in_, line)) {
            // EOF or error — exit the loop.
            break;
        }

        // Skip empty lines (robustness).
        if (line.empty()) continue;

        // Parse JSON.
        json request;
        try {
            request = json::parse(line);
        } catch (const json::parse_error& e) {
            errors_.fetch_add(1, std::memory_order_relaxed);
            auto error_response = make_error_response(
                json(nullptr), ErrorCode::ParseError,
                std::string("JSON parse error: ") + e.what());
            write_response(error_response);
            continue;
        }

        // Process and optionally respond.
        auto response = process_request(request);

        // json(nullptr) signals: this was a notification, no response.
        if (!response.is_null()) {
            write_response(response);
        }

        requests_processed_.fetch_add(1, std::memory_order_relaxed);
    }
}

void StdioTransport::send_notification(
    const std::string& method, const json& params)
{
    json notification = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params}
    };
    write_response(notification);
}

// ── Static helpers ─────────────────────────────────────────────────────────

json StdioTransport::make_success_response(
    const json& id, const json& result)
{
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", result}
    };
}

json StdioTransport::make_error_response(
    const json& id, ErrorCode code, const std::string& message)
{
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {
            {"code", static_cast<int>(code)},
            {"message", message}
        }}
    };
}

// ── Private ────────────────────────────────────────────────────────────────

json StdioTransport::process_request(const json& request) {
    // Validate JSON-RPC 2.0 structure.
    if (!request.contains("jsonrpc") ||
        request["jsonrpc"] != "2.0" ||
        !request.contains("method"))
    {
        errors_.fetch_add(1, std::memory_order_relaxed);
        return make_error_response(
            request.value("id", json(nullptr)),
            ErrorCode::InvalidRequest,
            "Invalid JSON-RPC 2.0 request");
    }

    auto method = request["method"].get<std::string>();
    auto params = request.value("params", json::object());
    auto id = request.value("id", json(nullptr));

    // Notifications (no "id" key at all) don't get responses per spec.
    if (id.is_null() && !request.contains("id")) {
        try {
            handler_(method, params, json(nullptr));
        } catch (...) {
            // Swallow errors for notifications per JSON-RPC 2.0 spec.
        }
        return json(nullptr);  // Signal: do not send response.
    }

    // Regular request: dispatch and wrap result.
    try {
        auto result = handler_(method, params, id);
        return make_success_response(id, result);
    } catch (const std::invalid_argument& e) {
        errors_.fetch_add(1, std::memory_order_relaxed);
        return make_error_response(
            id, ErrorCode::InvalidParams, e.what());
    } catch (const std::exception& e) {
        errors_.fetch_add(1, std::memory_order_relaxed);
        return make_error_response(
            id, ErrorCode::InternalError, e.what());
    }
}

void StdioTransport::write_response(const json& response) {
    // Compact JSON, no embedded newlines (§22.3 constraint).
    std::string serialized = response.dump(-1);

    // Extra safety: strip any accidental newlines.
    std::erase(serialized, '\n');
    std::erase(serialized, '\r');

    std::lock_guard lock(write_mutex_);
    out_ << serialized << '\n';
    out_.flush();
}

}  // namespace kairos::mcp
