/// include/kairos/mcp/handler.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/mcp/handler.hpp — MCP request handler                            ║
// ║                                                                          ║
// ║  Routes MCP JSON-RPC method calls to Kairos subsystem handlers.         ║
// ║  Implements the 14-tool schema defined in §22.4.                        ║
// ║                                                                          ║
// ║  Thread safety: all methods may be called from the MCP server thread.   ║
// ║  Access to shared state (WatchEngine, MetricsRegistry, QueryReader,     ║
// ║  WorkflowRegistry) is thread-safe by their respective designs.          ║
// ║                                                                          ║
// ║  Phase 4 Batch 3: all 10 stub tools now fully wired.                   ║
// ║                                                                          ║
// ║  Spec reference: §22.4–§22.6                                           ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/mcp/transport.hpp"
#include "kairos/engine/trigger_event.hpp"
#include "kairos/engine/workflow_registry.hpp"
#include "kairos/exec/output_sink.hpp"
#include "kairos/observability/metrics.hpp"
#include "kairos/persist/query_reader.hpp"
#include "kairos/watch/watch_engine.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

namespace kairos::mcp {

using json = nlohmann::json;

/// MCP server information returned in initialize response.
struct McpServerInfo {
    std::string name = "kairos";
    std::string version;
};

/// Submit-run callback type.
/// Takes (target_name_or_id, target_kind) → run_id (empty on failure).
using SubmitRunFn = std::function<std::string(
    const std::string&, engine::TriggerEvent::TargetKind)>;

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

        /// RunStream for live log following (§22.7).
        /// When follow=true, start_log_follow subscribes to this stream
        /// and emits base64-encoded chunks as notifications.
        exec::RunStream* run_stream = nullptr;

        /// Config reload callback. Returns true on success.
        /// On failure, populates the error vector.
        std::function<bool(std::vector<std::string>&)> reload_config;

        /// Server info for the initialize response.
        McpServerInfo server_info;

        // ── Phase 4 Batch 3: dependencies for full tool wiring ──────

        /// Workflow/job registry for listing and resolving targets.
        /// Thread-safe: immutable after construction; shared_ptr ensures
        /// the registry outlives any in-flight MCP calls during reload.
        std::shared_ptr<const engine::WorkflowRegistry> registry;

        /// Read-only database queries for runs, logs, step output.
        /// Thread-safe: SQLite WAL mode allows concurrent reads.
        persist::QueryReader* query_reader = nullptr;

        /// Submit a run via the trigger bus. Returns run_id on success,
        /// empty string on failure (queue full, target not found).
        SubmitRunFn submit_run;
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

    /// Start streaming log chunks for a run via MCP notifications.
    ///
    /// Called from tool_run_workflow when follow=true.  Subscribes
    /// to the run's log stream and emits base64-encoded chunks as
    /// JSON-RPC notifications ("notifications/log_chunk").
    ///
    /// When the run completes, emits "notifications/run_complete".
    ///
    /// This method launches a background thread that monitors the
    /// run and emits notifications via the transport.
    ///
    /// @param run_id   The run to follow.
    /// @param stop     Stop token from the daemon.
    /// Spec reference: §22.7
    void start_log_follow(const std::string& run_id,
                          std::stop_token stop = {});

    /// Update the registry pointer (called on config reload).
    /// Thread-safe: protected by registry_mu_.
    void update_registry(
        std::shared_ptr<const engine::WorkflowRegistry> new_registry);

    /// Set the transport pointer (called after transport creation).
    /// The handler is created before the transport (circular dep:
    /// transport needs handler's dispatch callback). This setter
    /// resolves the chicken-and-egg by wiring transport afterward.
    void set_transport(StdioTransport* transport) {
        deps_.transport = transport;
    }

private:
    // ── MCP protocol methods ────────────────────────────────────────
    json handle_initialize(const json& params);
    json handle_tools_list(const json& params);
    json handle_tools_call(const json& params);

    // ── Tool implementations: Watch ─────────────────────────────────

    /// kairos.listWatchGroups — List all configured watch groups.
    json tool_list_watch_groups(const json& args);

    /// kairos.getEvents — Get recent watch events.
    json tool_get_events(const json& args);

    /// kairos.watchScanOnce — Run a single scan cycle.
    json tool_watch_scan_once(const json& args);

    // ── Tool implementations: Config/Metrics ────────────────────────

    /// kairos.reloadConfig — Reload configuration.
    json tool_reload_config(const json& args);

    /// kairos.getMetrics — Get current metrics snapshot.
    json tool_get_metrics(const json& args);

    // ── Tool implementations: Workflows (Phase 4 Batch 3) ───────────

    /// kairos.listWorkflows — List all configured workflows.
    json tool_list_workflows(const json& args);

    /// kairos.getWorkflow — Get workflow details + DAG structure.
    json tool_get_workflow(const json& args);

    /// kairos.runWorkflow — Trigger a workflow execution.
    json tool_run_workflow(const json& args);

    /// kairos.explainPlan — Dry-run: explain what would happen.
    json tool_explain_plan(const json& args);

    // ── Tool implementations: Jobs (Phase 4 Batch 3) ────────────────

    /// kairos.listJobs — List all standalone jobs.
    json tool_list_jobs(const json& args);

    /// kairos.runJob — Trigger a standalone job execution.
    json tool_run_job(const json& args);

    // ── Tool implementations: Runs/Logs (Phase 4 Batch 3) ───────────

    /// kairos.queryRuns — Query run execution history.
    json tool_query_runs(const json& args);

    /// kairos.getRunDetail — Get full run detail with jobs and steps.
    json tool_get_run_detail(const json& args);

    /// kairos.getRunLogs — Get log chunks for a run.
    json tool_get_run_logs(const json& args);

    /// kairos.getStepOutput — Get stdout/stderr for a specific step.
    json tool_get_step_output(const json& args);

    // ── Internal ────────────────────────────────────────────────────

    /// Build the tools/list response with all 14 tool schemas.
    json build_tool_schemas() const;

    /// Wrap a tool result in the MCP content array format.
    static json wrap_tool_result(const json& result);

    /// Resolve a workflow by ID or name. Returns nullptr if not found.
    const engine::WorkflowDef* resolve_workflow(
        const std::string& id_or_name) const;

    /// Resolve a standalone job by ID or name. Returns nullptr if not found.
    const engine::JobDef* resolve_standalone_job(
        const std::string& id_or_name) const;

    /// Get a snapshot of the current registry (thread-safe).
    std::shared_ptr<const engine::WorkflowRegistry> get_registry() const;

public:
    // ── Log streaming (§22.7) ───────────────────────────────────────
    // Public: called by start_log_follow thread and testable.

    /// Emit a base64-encoded log chunk notification.
    /// @param run_id   Run identifier.
    /// @param job_id   Job identifier (empty for run-level logs).
    /// @param stream   "stdout" or "stderr".
    /// @param data     Raw log data (will be base64-encoded).
    void emit_log_chunk(const std::string& run_id,
                        const std::string& job_id,
                        const std::string& stream,
                        const std::string& data);

    /// Emit a run-complete notification.
    /// @param run_id      Run identifier.
    /// @param status      Final status ("success", "failure", "cancelled").
    /// @param duration_ms Run duration in milliseconds.
    void emit_run_complete(const std::string& run_id,
                           const std::string& status,
                           int64_t duration_ms);

private:
    Dependencies deps_;
    bool initialized_ = false;

    /// Mutex for registry updates during config reload.
    mutable std::mutex registry_mu_;
};

}  // namespace kairos::mcp
