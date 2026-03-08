/// include/kairos/mcp/handler.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/mcp/handler.hpp — MCP request handler                            ║
// ║                                                                          ║
// ║  Routes MCP JSON-RPC method calls to Kairos subsystem handlers.         ║
// ║  Implements the 14-tool schema defined in §22.4.                        ║
// ║                                                                          ║
// ║  Thread safety: all methods may be called from the MCP server thread.   ║
// ║  Access to shared state (WatchEngine, MetricsRegistry) is thread-safe  ║
// ║  by their respective designs.                                           ║
// ║                                                                          ║
// ║  Spec reference: §22.4–§22.6                                           ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/mcp/transport.hpp"
#include "kairos/observability/metrics.hpp"
#include "kairos/watch/watch_engine.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace kairos::mcp {

using json = nlohmann::json;

/// MCP server information returned in initialize response.
struct McpServerInfo {
    std::string name = "kairos";
    std::string version;
};

/// MCP handler.  Routes protocol messages and tool calls to
/// the appropriate Kairos subsystem.
///
/// The handler is designed as a dispatch table: each MCP method
/// maps to a private handler function. Tool calls are further
/// dispatched via a tool name → handler map.
///
/// Dependencies are injected at construction time. Not all
/// dependencies are required — null/nullptr means "not available
/// yet" (the handler returns a graceful error).
class McpHandler {
public:
    /// Dependencies injected from the daemon.
    struct Dependencies {
        watch::WatchEngine* watch_engine = nullptr;
        metrics::MetricsRegistry* metrics = nullptr;
        StdioTransport* transport = nullptr;

        /// Config reload callback. Returns true on success.
        /// On failure, populates the error vector.
        std::function<bool(std::vector<std::string>&)> reload_config;

        /// Server info for the initialize response.
        McpServerInfo server_info;
    };

    explicit McpHandler(Dependencies deps);

    /// Main dispatch function. Called by StdioTransport for each
    /// incoming JSON-RPC request.
    ///
    /// Routes to the appropriate handler based on the method name.
    ///
    /// @param method  The JSON-RPC method name.
    /// @param params  The request parameters.
    /// @param id      The request id (null for notifications).
    /// @return        The result JSON (wrapped in content array for tool calls).
    json dispatch(const std::string& method,
                  const json& params, const json& id);

private:
    // ── MCP protocol methods ────────────────────────────────────────
    json handle_initialize(const json& params);
    json handle_tools_list(const json& params);
    json handle_tools_call(const json& params);

    // ── Tool implementations ────────────────────────────────────────

    /// kairos.listWatchGroups — List all configured watch groups.
    json tool_list_watch_groups(const json& args);

    /// kairos.getEvents — Get recent watch events.
    json tool_get_events(const json& args);

    /// kairos.watchScanOnce — Run a single scan cycle.
    json tool_watch_scan_once(const json& args);

    /// kairos.reloadConfig — Reload configuration.
    json tool_reload_config(const json& args);

    /// kairos.getMetrics — Get current metrics snapshot.
    json tool_get_metrics(const json& args);

    // ── Stub tools (not yet wired) ──────────────────────────────────
    json tool_stub(const std::string& name);

    // ── Internal ────────────────────────────────────────────────────

    /// Build the tools/list response with all 14 tool schemas.
    json build_tool_schemas() const;

    /// Wrap a tool result in the MCP content array format.
    static json wrap_tool_result(const json& result);

    Dependencies deps_;
    bool initialized_ = false;
};

}  // namespace kairos::mcp
