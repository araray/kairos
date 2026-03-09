/// src/mcp/handler.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  handler.cpp — MCP handler: dispatch + tool implementations             ║
// ║                                                                          ║
// ║  Implements the 14-tool MCP schema from §22.4.                          ║
// ║  Watch tools (listWatchGroups, getEvents, watchScanOnce) are fully      ║
// ║  wired. Workflow/run/log tools are stubs pending Phase 4.               ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/mcp/handler.hpp"
#include "kairos/core/version.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace kairos::mcp {

// ── Base64 encoder (RFC 4648) ──────────────────────────────────────────────
// Needed for §22.7: log chunks must be base64-encoded to prevent embedded
// newlines from breaking the JSON-RPC stdio frame boundary.

namespace {

constexpr std::array<char, 64> kBase64Alphabet = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    'a','b','c','d','e','f','g','h','i','j','k','l','m',
    'n','o','p','q','r','s','t','u','v','w','x','y','z',
    '0','1','2','3','4','5','6','7','8','9','+','/'
};

/// Encode raw bytes as base64.
/// No line wrapping — suitable for JSON embedding.
std::string base64_encode(const std::string& input) {
    std::string result;
    result.reserve(((input.size() + 2) / 3) * 4);

    const auto* data = reinterpret_cast<const unsigned char*>(input.data());
    std::size_t len = input.size();

    for (std::size_t i = 0; i < len; i += 3) {
        uint32_t octet_a = data[i];
        uint32_t octet_b = (i + 1 < len) ? data[i + 1] : 0;
        uint32_t octet_c = (i + 2 < len) ? data[i + 2] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        result += kBase64Alphabet[(triple >> 18) & 0x3F];
        result += kBase64Alphabet[(triple >> 12) & 0x3F];
        result += (i + 1 < len) ? kBase64Alphabet[(triple >> 6) & 0x3F] : '=';
        result += (i + 2 < len) ? kBase64Alphabet[triple & 0x3F] : '=';
    }

    return result;
}

}  // anonymous namespace

// ── Constructor ────────────────────────────────────────────────────────────

McpHandler::McpHandler(Dependencies deps)
    : deps_(std::move(deps)) {}

// ── Main dispatch ──────────────────────────────────────────────────────────

json McpHandler::dispatch(const std::string& method,
                          const json& params, const json& id) {
    // MCP protocol methods.
    if (method == "initialize") {
        return handle_initialize(params);
    }
    if (method == "tools/list") {
        return handle_tools_list(params);
    }
    if (method == "tools/call") {
        return handle_tools_call(params);
    }

    // Unknown method.
    throw std::invalid_argument("Unknown method: " + method);
}

// ── MCP protocol methods ───────────────────────────────────────────────────

json McpHandler::handle_initialize(const json& params) {
    initialized_ = true;

    return {
        {"protocolVersion", "2024-11-05"},
        {"capabilities", {
            {"tools", {{"listChanged", false}}},
            {"logging", json::object()},
            {"notifications", {{"log_chunk", true}, {"run_complete", true}}}
        }},
        {"serverInfo", {
            {"name", deps_.server_info.name},
            {"version",
                deps_.server_info.version.empty()
                    ? std::string(kairos::kVersion)
                    : deps_.server_info.version}
        }}
    };
}

json McpHandler::handle_tools_list(const json& /*params*/) {
    return build_tool_schemas();
}

json McpHandler::handle_tools_call(const json& params) {
    if (!params.contains("name")) {
        throw std::invalid_argument("Missing 'name' in tools/call");
    }
    auto tool_name = params.at("name").get<std::string>();
    auto arguments = params.value("arguments", json::object());

    // Tool dispatch table.
    static const std::unordered_map<
        std::string,
        std::function<json(McpHandler*, const json&)>
    > tool_map = {
        {"kairos.listWatchGroups", &McpHandler::tool_list_watch_groups},
        {"kairos.getEvents",       &McpHandler::tool_get_events},
        {"kairos.watchScanOnce",   &McpHandler::tool_watch_scan_once},
        {"kairos.reloadConfig",    &McpHandler::tool_reload_config},
        {"kairos.getMetrics",      &McpHandler::tool_get_metrics},
    };

    auto it = tool_map.find(tool_name);
    if (it != tool_map.end()) {
        auto result = it->second(this, arguments);
        return wrap_tool_result(result);
    }

    // Check if it's a known-but-unimplemented tool.
    static const std::vector<std::string> known_stubs = {
        "kairos.listWorkflows", "kairos.getWorkflow",
        "kairos.runWorkflow",   "kairos.listJobs",
        "kairos.runJob",        "kairos.queryRuns",
        "kairos.getRunDetail",  "kairos.getRunLogs",
        "kairos.getStepOutput", "kairos.explainPlan",
    };
    for (const auto& name : known_stubs) {
        if (tool_name == name) {
            return wrap_tool_result(tool_stub(tool_name));
        }
    }

    throw std::invalid_argument("Unknown tool: " + tool_name);
}

// ── Tool implementations: Watch ────────────────────────────────────────────

json McpHandler::tool_list_watch_groups(const json& /*args*/) {
    if (!deps_.watch_engine) {
        return {{"error", "Watch engine not available"}};
    }

    auto statuses = deps_.watch_engine->get_status();

    json groups = json::array();
    for (const auto& s : statuses) {
        groups.push_back({
            {"name",               s.group_name},
            {"mode",               s.mode},
            {"watched_paths",      s.watched_paths},
            {"files_in_last_sample", s.files_in_last_sample},
            {"last_scan_time",     s.last_scan_time},
            {"next_scan_time",     s.next_scan_time},
            {"events_last_hour",   s.events_last_hour},
            {"status",             s.status}
        });
    }

    return {
        {"watch_groups", groups},
        {"count", static_cast<int>(statuses.size())}
    };
}

json McpHandler::tool_get_events(const json& args) {
    if (!deps_.watch_engine) {
        return {{"error", "Watch engine not available"}};
    }

    auto group_name = args.value("watch_group", std::string{});
    int limit = args.value("limit", 50);

    // Clamp limit.
    if (limit < 1) limit = 1;
    if (limit > 200) limit = 200;

    std::vector<watch::WatchTriggerResult> events;
    if (group_name.empty()) {
        events = deps_.watch_engine->get_recent_events(limit);
    } else {
        events = deps_.watch_engine->get_recent_events(group_name, limit);
    }

    json result = json::array();
    for (const auto& e : events) {
        json paths = json::array();
        for (const auto& p : e.affected_paths) {
            paths.push_back(p);
        }

        result.push_back({
            {"rule_name",     e.rule_name},
            {"watch_group",   e.watch_group_name},
            {"event_type",    e.event_type},
            {"severity",      e.severity},
            {"affected_paths", paths},
            {"has_trigger",   e.trigger_target.has_value()},
        });
    }

    return {
        {"events", result},
        {"count",  static_cast<int>(events.size())}
    };
}

json McpHandler::tool_watch_scan_once(const json& args) {
    if (!deps_.watch_engine) {
        return {{"error", "Watch engine not available"}};
    }

    auto group_name = args.value("watch_group", std::string{});

    // Null sink — scan_once for diagnostics, don't push to trigger bus.
    engine::TriggerSink null_sink = [](engine::TriggerEvent) {
        return true;
    };

    if (group_name.empty()) {
        // Scan all groups.
        auto results = deps_.watch_engine->scan_once(null_sink);

        json groups = json::array();
        for (const auto& r : results) {
            json triggered = json::array();
            for (const auto& t : r.triggered) {
                triggered.push_back({
                    {"rule_name", t.rule_name},
                    {"event_type", t.event_type},
                    {"affected_count",
                     static_cast<int>(t.affected_paths.size())}
                });
            }
            groups.push_back({
                {"files_scanned",
                 static_cast<int>(r.sample.entries.size())},
                {"changes", {
                    {"created",
                     static_cast<int>(r.diff.created.size())},
                    {"modified",
                     static_cast<int>(r.diff.modified.size())},
                    {"deleted",
                     static_cast<int>(r.diff.deleted.size())},
                }},
                {"triggered", triggered},
                {"scan_duration_ms", r.scan_duration.count()},
                {"incomplete", r.incomplete}
            });
        }

        return {
            {"scanned_groups", static_cast<int>(results.size())},
            {"results", groups}
        };
    }

    // Scan a specific group.
    auto result = deps_.watch_engine->scan_group(group_name, null_sink);

    json triggered = json::array();
    for (const auto& t : result.triggered) {
        json paths = json::array();
        for (const auto& p : t.affected_paths) {
            paths.push_back(p);
        }
        triggered.push_back({
            {"rule_name", t.rule_name},
            {"event_type", t.event_type},
            {"affected_paths", paths}
        });
    }

    return {
        {"watch_group",    group_name},
        {"files_scanned",
         static_cast<int>(result.sample.entries.size())},
        {"changes", {
            {"created",
             static_cast<int>(result.diff.created.size())},
            {"modified",
             static_cast<int>(result.diff.modified.size())},
            {"deleted",
             static_cast<int>(result.diff.deleted.size())},
        }},
        {"triggered", triggered},
        {"scan_duration_ms", result.scan_duration.count()},
        {"incomplete", result.incomplete}
    };
}

// ── Tool implementations: Config ───────────────────────────────────────────

json McpHandler::tool_reload_config(const json& /*args*/) {
    if (!deps_.reload_config) {
        return {{"success", false},
                {"errors", json::array({"Config reload not available"})}};
    }

    std::vector<std::string> errors;
    bool ok = deps_.reload_config(errors);

    json err_arr = json::array();
    for (const auto& e : errors) {
        err_arr.push_back(e);
    }

    return {
        {"success", ok},
        {"errors", err_arr}
    };
}

// ── Tool implementations: Metrics ──────────────────────────────────────────

json McpHandler::tool_get_metrics(const json& /*args*/) {
    if (!deps_.metrics) {
        return {{"error", "Metrics not available"}};
    }

    // Parse the JSON string from MetricsRegistry::to_json().
    try {
        auto metrics_str = deps_.metrics->to_json();
        return json::parse(metrics_str);
    } catch (const std::exception& e) {
        return {{"error", std::string("Failed to get metrics: ") + e.what()}};
    }
}

// ── Stub tool ──────────────────────────────────────────────────────────────

json McpHandler::tool_stub(const std::string& name) {
    return {
        {"status", "not_implemented"},
        {"tool", name},
        {"message", "This tool will be available in a future release"}
    };
}

// ── Log streaming (§22.7) ─────────────────────────────────────────────────

void McpHandler::emit_log_chunk(
    const std::string& run_id,
    const std::string& job_id,
    const std::string& stream,
    const std::string& data)
{
    if (!deps_.transport) return;

    // Base64-encode to prevent embedded newlines from breaking
    // the JSON-RPC stdio frame boundary.
    std::string encoded = base64_encode(data);

    json params = {
        {"run_id", run_id},
        {"stream", stream},
        {"data", encoded}
    };
    if (!job_id.empty()) {
        params["job_id"] = job_id;
    }

    deps_.transport->send_notification("notifications/log_chunk", params);
}

void McpHandler::emit_run_complete(
    const std::string& run_id,
    const std::string& status,
    int64_t duration_ms)
{
    if (!deps_.transport) return;

    deps_.transport->send_notification("notifications/run_complete", {
        {"run_id", run_id},
        {"status", status},
        {"duration_ms", duration_ms}
    });
}

void McpHandler::start_log_follow(
    const std::string& run_id,
    std::stop_token stop)
{
    // ── RunStream-based log following (§22.7) ─────────────────────
    // Subscribe to the RunStream for this run_id. Each output chunk
    // is base64-encoded and emitted as a JSON-RPC notification.
    // When the run completes (close_run), emit run_complete.

    if (!deps_.run_stream) {
        spdlog::debug("MCP log follow: RunStream not available for "
                      "run '{}'", run_id);
        return;
    }

    if (!deps_.transport) {
        spdlog::debug("MCP log follow: no transport for run '{}'",
                      run_id);
        return;
    }

    spdlog::info("MCP log follow: subscribing to run '{}'", run_id);

    // Subscribe to output chunks.
    // The callback runs on the ProcessHandle's reader thread.
    deps_.run_stream->subscribe(run_id,
        [this](const std::string& rid,
               const std::string& job_id,
               const std::string& /*step_id*/,
               std::string_view chunk,
               bool is_stderr) {
            emit_log_chunk(rid, job_id,
                           is_stderr ? "stderr" : "stdout",
                           std::string(chunk));
        });

    // Subscribe to run completion.
    // Invoked by Pipeline when the run finishes (via close_run).
    deps_.run_stream->on_close(run_id,
        [this](const std::string& rid) {
            spdlog::debug("MCP log follow: run '{}' completed", rid);
            // We don't have the exact status and duration here.
            // Emit with "completed" — the client can query the
            // actual status via kairos.getRunDetail if needed.
            emit_run_complete(rid, "completed", 0);
        });
}

// ── Helper: wrap tool result ───────────────────────────────────────────────

json McpHandler::wrap_tool_result(const json& result) {
    return {
        {"content", json::array({
            {{"type", "text"},
             {"text", result.dump(2)}}
        })}
    };
}

// ── Tool schemas ───────────────────────────────────────────────────────────

json McpHandler::build_tool_schemas() const {
    json tools = json::array();

    // kairos.listWorkflows
    tools.push_back({
        {"name", "kairos.listWorkflows"},
        {"description", "List all configured workflows"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // kairos.getWorkflow
    tools.push_back({
        {"name", "kairos.getWorkflow"},
        {"description", "Get workflow details including jobs and DAG structure"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"workflow_id", {
                    {"type", "string"},
                    {"description", "Workflow ID or name"}
                }}
            }},
            {"required", json::array({"workflow_id"})}
        }}
    });

    // kairos.runWorkflow
    tools.push_back({
        {"name", "kairos.runWorkflow"},
        {"description", "Trigger a workflow execution"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"workflow_id", {
                    {"type", "string"},
                    {"description", "Workflow content-addressable ID or name"}
                }},
                {"follow", {
                    {"type", "boolean"},
                    {"default", false},
                    {"description", "Stream log chunks as notifications"}
                }}
            }},
            {"required", json::array({"workflow_id"})}
        }}
    });

    // kairos.listJobs
    tools.push_back({
        {"name", "kairos.listJobs"},
        {"description", "List all standalone jobs"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // kairos.runJob
    tools.push_back({
        {"name", "kairos.runJob"},
        {"description", "Trigger a standalone job execution"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"job_id", {
                    {"type", "string"},
                    {"description", "Job ID or name"}
                }},
                {"follow", {
                    {"type", "boolean"},
                    {"default", false}
                }}
            }},
            {"required", json::array({"job_id"})}
        }}
    });

    // kairos.queryRuns
    tools.push_back({
        {"name", "kairos.queryRuns"},
        {"description", "Query run execution history with filters"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"limit", {
                    {"type", "integer"},
                    {"default", 20},
                    {"minimum", 1},
                    {"maximum", 100}
                }},
                {"status", {
                    {"type", "string"},
                    {"enum", json::array(
                        {"success", "failure", "running", "cancelled"})}
                }},
                {"workflow_id", {{"type", "string"}}},
                {"since", {
                    {"type", "string"},
                    {"format", "date-time"}
                }}
            }}
        }}
    });

    // kairos.getRunDetail
    tools.push_back({
        {"name", "kairos.getRunDetail"},
        {"description", "Get full run detail with jobs and steps"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"run_id", {{"type", "string"}}}
            }},
            {"required", json::array({"run_id"})}
        }}
    });

    // kairos.getRunLogs
    tools.push_back({
        {"name", "kairos.getRunLogs"},
        {"description", "Get logs for a run"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"run_id", {{"type", "string"}}},
                {"cursor", {{"type", "string"}}},
                {"limit", {
                    {"type", "integer"},
                    {"default", 100}
                }}
            }},
            {"required", json::array({"run_id"})}
        }}
    });

    // kairos.getStepOutput
    tools.push_back({
        {"name", "kairos.getStepOutput"},
        {"description", "Get stdout/stderr for a specific step"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"run_id", {{"type", "string"}}},
                {"step_id", {{"type", "string"}}}
            }},
            {"required", json::array({"run_id", "step_id"})}
        }}
    });

    // kairos.listWatchGroups
    tools.push_back({
        {"name", "kairos.listWatchGroups"},
        {"description", "List all configured watch groups with status"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // kairos.getEvents
    tools.push_back({
        {"name", "kairos.getEvents"},
        {"description", "Get recent watch events"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"watch_group", {
                    {"type", "string"},
                    {"description", "Filter by watch group name"}
                }},
                {"limit", {
                    {"type", "integer"},
                    {"default", 50},
                    {"minimum", 1},
                    {"maximum", 200}
                }}
            }}
        }}
    });

    // kairos.watchScanOnce
    tools.push_back({
        {"name", "kairos.watchScanOnce"},
        {"description",
         "Run a single watch scan cycle for diagnostics"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"watch_group", {
                    {"type", "string"},
                    {"description",
                     "Specific watch group to scan (all if omitted)"}
                }}
            }}
        }}
    });

    // kairos.reloadConfig
    tools.push_back({
        {"name", "kairos.reloadConfig"},
        {"description", "Reload configuration from disk"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // kairos.explainPlan
    tools.push_back({
        {"name", "kairos.explainPlan"},
        {"description",
         "Dry-run: explain what would happen if a workflow ran now"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"workflow_id", {
                    {"type", "string"},
                    {"description", "Workflow ID or name to explain"}
                }}
            }},
            {"required", json::array({"workflow_id"})}
        }}
    });

    // kairos.getMetrics
    tools.push_back({
        {"name", "kairos.getMetrics"},
        {"description", "Get current metrics snapshot as JSON"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    return {{"tools", tools}};
}

}  // namespace kairos::mcp
