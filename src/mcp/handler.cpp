/// src/mcp/handler.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  handler.cpp — MCP handler: dispatch + all 14 tool implementations       ║
// ║                                                                          ║
// ║  Phase 4 Batch 3: all 10 previously-stub tools are now fully wired.     ║
// ║                                                                          ║
// ║  Tool categories:                                                        ║
// ║    Watch:     listWatchGroups, getEvents, watchScanOnce                  ║
// ║    Config:    reloadConfig                                               ║
// ║    Metrics:   getMetrics                                                ║
// ║    Workflows: listWorkflows, getWorkflow, runWorkflow, explainPlan      ║
// ║    Jobs:      listJobs, runJob                                          ║
// ║    Runs/Logs: queryRuns, getRunDetail, getRunLogs, getStepOutput        ║
// ║                                                                          ║
// ║  Spec reference: §22.4–§22.8                                           ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/mcp/handler.hpp"
#include "kairos/core/id_generator.hpp"
#include "kairos/core/version.hpp"
#include "kairos/engine/dag.hpp"
#include "kairos/engine/execution_plan.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

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
    // Guard: if arguments was explicitly null, default to empty object.
    if (arguments.is_null()) {
        arguments = json::object();
    }

    // Tool dispatch table — all 14 tools fully wired.
    static const std::unordered_map<
        std::string,
        std::function<json(McpHandler*, const json&)>
    > tool_map = {
        // Watch tools
        {"kairos.listWatchGroups", &McpHandler::tool_list_watch_groups},
        {"kairos.getEvents",       &McpHandler::tool_get_events},
        {"kairos.watchScanOnce",   &McpHandler::tool_watch_scan_once},
        // Config/Metrics
        {"kairos.reloadConfig",    &McpHandler::tool_reload_config},
        {"kairos.getMetrics",      &McpHandler::tool_get_metrics},
        // Workflows (Phase 4 Batch 3)
        {"kairos.listWorkflows",   &McpHandler::tool_list_workflows},
        {"kairos.getWorkflow",     &McpHandler::tool_get_workflow},
        {"kairos.runWorkflow",     &McpHandler::tool_run_workflow},
        {"kairos.explainPlan",     &McpHandler::tool_explain_plan},
        // Jobs (Phase 4 Batch 3)
        {"kairos.listJobs",        &McpHandler::tool_list_jobs},
        {"kairos.runJob",          &McpHandler::tool_run_job},
        // Runs/Logs (Phase 4 Batch 3)
        {"kairos.queryRuns",       &McpHandler::tool_query_runs},
        {"kairos.getRunDetail",    &McpHandler::tool_get_run_detail},
        {"kairos.getRunLogs",      &McpHandler::tool_get_run_logs},
        {"kairos.getStepOutput",   &McpHandler::tool_get_step_output},
    };

    auto it = tool_map.find(tool_name);
    if (it != tool_map.end()) {
        auto result = it->second(this, arguments);
        return wrap_tool_result(result);
    }

    throw std::invalid_argument("Unknown tool: " + tool_name);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Tool implementations: Watch
// ═══════════════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════════════
//  Tool implementations: Config
// ═══════════════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════════════
//  Tool implementations: Metrics
// ═══════════════════════════════════════════════════════════════════════════

json McpHandler::tool_get_metrics(const json& /*args*/) {
    if (!deps_.metrics) {
        return {{"error", "Metrics not available"}};
    }

    try {
        auto metrics_str = deps_.metrics->to_json();
        return json::parse(metrics_str);
    } catch (const std::exception& e) {
        return {{"error", std::string("Failed to get metrics: ") + e.what()}};
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Tool implementations: Workflows (Phase 4 Batch 3)
// ═══════════════════════════════════════════════════════════════════════════

json McpHandler::tool_list_workflows(const json& /*args*/) {
    auto reg = get_registry();
    if (!reg) {
        return {{"error", "Workflow registry not available"}};
    }

    json workflows = json::array();
    for (const auto* wf : reg->workflows()) {
        workflows.push_back({
            {"id",            wf->workflow_id},
            {"name",          wf->workflow_name},
            {"job_count",     static_cast<int>(wf->jobs.size())},
        });
    }

    return {
        {"workflows", workflows},
        {"count",     static_cast<int>(workflows.size())}
    };
}

json McpHandler::tool_get_workflow(const json& args) {
    auto reg = get_registry();
    if (!reg) {
        return {{"error", "Workflow registry not available"}};
    }

    auto workflow_id = args.value("workflow_id", std::string{});
    if (workflow_id.empty()) {
        return {{"error", "Missing required parameter: workflow_id"}};
    }

    const auto* wf = resolve_workflow(workflow_id);
    if (!wf) {
        return {{"error", "Workflow not found: " + workflow_id}};
    }

    // Build jobs array with step details.
    json jobs = json::array();
    for (const auto& job : wf->jobs) {
        json steps = json::array();
        for (const auto& step : job.steps) {
            json step_j = {
                {"step_id",   step.step_id},
                {"step_name", step.step_name},
                {"command",   step.command},
                {"use_shell", step.use_shell},
            };
            if (!step.working_dir.empty()) {
                step_j["working_dir"] = step.working_dir.string();
            }
            if (step.timeout.has_value()) {
                step_j["timeout_seconds"] = step.timeout->count();
            }
            steps.push_back(std::move(step_j));
        }

        json job_j = {
            {"job_id",    job.job_id},
            {"job_name",  job.job_name},
            {"steps",     steps},
            {"needs",     json(job.needs)},
            {"continue_on_error", job.continue_on_error},
        };
        if (job.condition_expr.has_value()) {
            job_j["condition"] = *job.condition_expr;
        }
        if (!job.working_dir.empty()) {
            job_j["working_dir"] = job.working_dir.string();
        }
        jobs.push_back(std::move(job_j));
    }

    // Build DAG structure: levels with job IDs.
    json dag_levels = json::array();
    for (int lvl = 0; lvl < wf->dag.level_count(); ++lvl) {
        dag_levels.push_back(json(wf->dag.jobs_at_level(lvl)));
    }

    return {
        {"workflow_id",   wf->workflow_id},
        {"workflow_name", wf->workflow_name},
        {"jobs",          jobs},
        {"dag_levels",    dag_levels},
        {"level_count",   wf->dag.level_count()},
    };
}

json McpHandler::tool_run_workflow(const json& args) {
    auto workflow_id = args.value("workflow_id", std::string{});
    if (workflow_id.empty()) {
        return {{"error", "Missing required parameter: workflow_id"}};
    }

    // Resolve name to ID if needed.
    auto reg = get_registry();
    if (reg) {
        const auto* wf = resolve_workflow(workflow_id);
        if (!wf) {
            return {{"error", "Workflow not found: " + workflow_id}};
        }
        workflow_id = wf->workflow_id;
    }

    if (!deps_.submit_run) {
        return {{"error", "Run submission not available"}};
    }

    auto run_id = deps_.submit_run(
        workflow_id, engine::TriggerEvent::TargetKind::Workflow);

    if (run_id.empty()) {
        return {{"error", "Failed to submit workflow run (queue full?)"}};
    }

    bool follow = args.value("follow", false);
    if (follow) {
        start_log_follow(run_id);
    }

    return {
        {"run_id",  run_id},
        {"status",  "running"},
        {"follow",  follow},
    };
}

json McpHandler::tool_explain_plan(const json& args) {
    auto reg = get_registry();
    if (!reg) {
        return {{"error", "Workflow registry not available"}};
    }

    auto workflow_id = args.value("workflow_id", std::string{});
    if (workflow_id.empty()) {
        return {{"error", "Missing required parameter: workflow_id"}};
    }

    const auto* wf = resolve_workflow(workflow_id);
    if (!wf) {
        return {{"error", "Workflow not found: " + workflow_id}};
    }

    // Build execution plan from the DAG structure.
    // Walk the DAG level-by-level, marking conditions as pending
    // if they reference same-workflow jobs (can't evaluate until runtime).
    engine::ExecutionPlan plan;
    plan.workflow_id = wf->workflow_id;
    plan.workflow_name = wf->workflow_name;
    plan.trigger_type = "explain";

    // Collect all job names in this workflow for self-reference detection.
    std::unordered_set<std::string> wf_job_names;
    for (const auto& job : wf->jobs) {
        wf_job_names.insert(job.job_name);
    }

    for (int lvl = 0; lvl < wf->dag.level_count(); ++lvl) {
        for (const auto& job_id : wf->dag.jobs_at_level(lvl)) {
            const auto& node = wf->dag.node(job_id);
            engine::PlanEntry entry;
            entry.job_id = node.job_id;
            entry.job_name = node.job_name;
            entry.level = node.topo_level;
            entry.needs = node.needs;
            entry.condition_expr =
                node.condition_expr.value_or("");

            if (node.condition_expr.has_value() &&
                !node.condition_expr->empty()) {
                // Check if the condition references a job in the same
                // workflow — if so, mark as ConditionPending because
                // we can't evaluate it before the run starts.
                bool refs_self = false;
                for (const auto& jn : wf_job_names) {
                    if (node.condition_expr->find(
                            "\"" + jn + "\"") != std::string::npos) {
                        refs_self = true;
                        break;
                    }
                }

                if (refs_self) {
                    entry.action = engine::PlanAction::ConditionPending;
                    entry.reason = "Condition references same-workflow "
                                   "job — will be evaluated at runtime";
                    entry.condition_result = "pending";
                } else {
                    // External condition — could potentially evaluate
                    // against the database, but for safety we mark
                    // as Run (the condition will be evaluated at runtime).
                    entry.action = engine::PlanAction::Run;
                    entry.reason = "External condition (evaluated at "
                                   "runtime)";
                }
            } else if (node.needs.empty()) {
                entry.action = engine::PlanAction::Run;
                entry.reason = "No dependencies, no condition";
            } else {
                entry.action = engine::PlanAction::Run;
                entry.reason = "Dependencies will be met";
            }

            plan.entries.push_back(std::move(entry));
        }
    }

    // Return as parsed JSON (from render_json).
    auto json_str = plan.render_json();
    return json::parse(json_str);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Tool implementations: Jobs (Phase 4 Batch 3)
// ═══════════════════════════════════════════════════════════════════════════

json McpHandler::tool_list_jobs(const json& /*args*/) {
    auto reg = get_registry();
    if (!reg) {
        return {{"error", "Workflow registry not available"}};
    }

    json jobs = json::array();
    for (const auto* job : reg->standalone_jobs()) {
        json job_j = {
            {"id",         job->job_id},
            {"name",       job->job_name},
            {"step_count", static_cast<int>(job->steps.size())},
        };
        if (job->condition_expr.has_value()) {
            job_j["condition"] = *job->condition_expr;
        }
        jobs.push_back(std::move(job_j));
    }

    return {
        {"jobs",  jobs},
        {"count", static_cast<int>(jobs.size())}
    };
}

json McpHandler::tool_run_job(const json& args) {
    auto job_id = args.value("job_id", std::string{});
    if (job_id.empty()) {
        return {{"error", "Missing required parameter: job_id"}};
    }

    // Resolve name to ID if needed.
    auto reg = get_registry();
    if (reg) {
        const auto* job = resolve_standalone_job(job_id);
        if (!job) {
            return {{"error", "Standalone job not found: " + job_id}};
        }
        job_id = job->job_id;
    }

    if (!deps_.submit_run) {
        return {{"error", "Run submission not available"}};
    }

    auto run_id = deps_.submit_run(
        job_id, engine::TriggerEvent::TargetKind::StandaloneJob);

    if (run_id.empty()) {
        return {{"error", "Failed to submit job run (queue full?)"}};
    }

    bool follow = args.value("follow", false);
    if (follow) {
        start_log_follow(run_id);
    }

    return {
        {"run_id",  run_id},
        {"status",  "running"},
        {"follow",  follow},
    };
}

// ═══════════════════════════════════════════════════════════════════════════
//  Tool implementations: Runs/Logs (Phase 4 Batch 3)
// ═══════════════════════════════════════════════════════════════════════════

json McpHandler::tool_query_runs(const json& args) {
    if (!deps_.query_reader) {
        return {{"error", "Query reader not available"}};
    }

    int limit = args.value("limit", 20);
    if (limit < 1) limit = 1;
    if (limit > 100) limit = 100;

    auto status_filter = args.value("status", std::string{});
    auto workflow_filter = args.value("workflow_id", std::string{});
    auto since = args.value("since", std::string{});

    // Map MCP status names to internal status names.
    // MCP uses lowercase; internal uses uppercase.
    if (!status_filter.empty()) {
        // Convert to uppercase for the DB query.
        std::string upper;
        upper.reserve(status_filter.size());
        for (char c : status_filter) {
            upper += static_cast<char>(std::toupper(
                static_cast<unsigned char>(c)));
        }
        status_filter = upper;
    }

    auto runs = deps_.query_reader->query_recent_runs(
        limit, status_filter, workflow_filter, since);

    json results = json::array();
    for (const auto& r : runs) {
        results.push_back({
            {"run_id",        r.run_id},
            {"target_type",   r.target_type},
            {"target_id",     r.target_id},
            {"target_name",   r.target_name},
            {"trigger_type",  r.trigger_type},
            {"status",        r.status},
            {"exit_code",     r.exit_code},
            {"start_ts",      r.start_ts},
            {"end_ts",        r.end_ts},
            {"duration_ms",   r.duration_ms},
        });
    }

    return {
        {"runs",  results},
        {"count", static_cast<int>(results.size())}
    };
}

json McpHandler::tool_get_run_detail(const json& args) {
    if (!deps_.query_reader) {
        return {{"error", "Query reader not available"}};
    }

    auto run_id = args.value("run_id", std::string{});
    if (run_id.empty()) {
        return {{"error", "Missing required parameter: run_id"}};
    }

    auto detail = deps_.query_reader->get_run_detail(run_id);
    if (!detail.has_value()) {
        return {{"error", "Run not found: " + run_id}};
    }

    // Build jobs array.
    json jobs = json::array();
    for (const auto& j : detail->jobs) {
        json steps = json::array();
        for (const auto& s : j.steps) {
            steps.push_back({
                {"step_id",       s.step_id},
                {"step_name",     s.step_name},
                {"status",        s.status},
                {"exit_code",     s.exit_code},
                {"start_ts",      s.start_ts},
                {"end_ts",        s.end_ts},
                {"duration_ms",   s.duration_ms},
                {"command",       s.command},
            });
        }

        jobs.push_back({
            {"job_id",            j.job_id},
            {"job_name",          j.job_name},
            {"status",            j.status},
            {"exit_code",         j.exit_code},
            {"start_ts",          j.start_ts},
            {"end_ts",            j.end_ts},
            {"duration_ms",       j.duration_ms},
            {"condition_result",  j.condition_result},
            {"steps",             steps},
        });
    }

    return {
        {"run_id",        detail->run.run_id},
        {"target_type",   detail->run.target_type},
        {"target_id",     detail->run.target_id},
        {"target_name",   detail->run.target_name},
        {"trigger_type",  detail->run.trigger_type},
        {"status",        detail->run.status},
        {"exit_code",     detail->run.exit_code},
        {"start_ts",      detail->run.start_ts},
        {"end_ts",        detail->run.end_ts},
        {"duration_ms",   detail->run.duration_ms},
        {"jobs",          jobs},
    };
}

json McpHandler::tool_get_run_logs(const json& args) {
    if (!deps_.query_reader) {
        return {{"error", "Query reader not available"}};
    }

    auto run_id = args.value("run_id", std::string{});
    if (run_id.empty()) {
        return {{"error", "Missing required parameter: run_id"}};
    }

    // Cursor-based pagination: cursor is a stringified row ID.
    int64_t after_id = 0;
    if (args.contains("cursor") && !args.at("cursor").is_null()) {
        auto cursor_str = args.at("cursor").get<std::string>();
        if (!cursor_str.empty()) {
            try {
                after_id = std::stoll(cursor_str);
            } catch (...) {
                // Invalid cursor — start from beginning.
            }
        }
    }

    int limit = args.value("limit", 100);
    if (limit < 1) limit = 1;
    if (limit > 1000) limit = 1000;

    auto chunks = deps_.query_reader->get_log_chunks(
        run_id, after_id, limit);

    json results = json::array();
    int64_t last_id = after_id;
    for (const auto& c : chunks) {
        results.push_back({
            {"job_id",      c.job_id},
            {"step_id",     c.step_id},
            {"stream",      c.stream},
            {"content",     c.content},
            {"created_at",  c.created_at},
        });
        if (c.id > last_id) {
            last_id = c.id;
        }
    }

    // Build next_cursor: if we got exactly `limit` results,
    // there might be more.
    json next_cursor = json(nullptr);
    if (static_cast<int>(chunks.size()) == limit) {
        next_cursor = std::to_string(last_id);
    }

    return {
        {"chunks",      results},
        {"count",       static_cast<int>(results.size())},
        {"next_cursor", next_cursor},
    };
}

json McpHandler::tool_get_step_output(const json& args) {
    if (!deps_.query_reader) {
        return {{"error", "Query reader not available"}};
    }

    auto run_id = args.value("run_id", std::string{});
    auto step_id = args.value("step_id", std::string{});
    if (run_id.empty() || step_id.empty()) {
        return {{"error",
                 "Missing required parameters: run_id, step_id"}};
    }

    // Get all log chunks for this run, then filter by step_id.
    // For large runs this is sub-optimal; a future QueryReader method
    // could accept step_id directly. For now, this is correct.
    auto all_chunks = deps_.query_reader->get_log_chunks(
        run_id, 0, 10000);

    std::string stdout_content;
    std::string stderr_content;
    int exit_code = -1;  // Unknown unless we can find it in run detail.

    for (const auto& c : all_chunks) {
        if (c.step_id == step_id) {
            if (c.stream == "stdout") {
                stdout_content += c.content;
            } else if (c.stream == "stderr") {
                stderr_content += c.content;
            }
        }
    }

    // Try to get exit code from run detail.
    auto detail = deps_.query_reader->get_run_detail(run_id);
    if (detail.has_value()) {
        for (const auto& job : detail->jobs) {
            for (const auto& step : job.steps) {
                if (step.step_id == step_id) {
                    exit_code = step.exit_code;
                    break;
                }
            }
        }
    }

    return {
        {"run_id",    run_id},
        {"step_id",   step_id},
        {"stdout",    stdout_content},
        {"stderr",    stderr_content},
        {"exit_code", exit_code},
    };
}

// ═══════════════════════════════════════════════════════════════════════════
//  Log streaming (§22.7)
// ═══════════════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════════════
//  Registry management
// ═══════════════════════════════════════════════════════════════════════════

void McpHandler::update_registry(
    std::shared_ptr<const engine::WorkflowRegistry> new_registry) {
    std::lock_guard lock(registry_mu_);
    deps_.registry = std::move(new_registry);
}

std::shared_ptr<const engine::WorkflowRegistry>
McpHandler::get_registry() const {
    std::lock_guard lock(registry_mu_);
    return deps_.registry;
}

const engine::WorkflowDef* McpHandler::resolve_workflow(
    const std::string& id_or_name) const {
    auto reg = get_registry();
    if (!reg) return nullptr;

    // Try by ID first.
    const auto* wf = reg->workflow(id_or_name);
    if (wf) return wf;

    // Try by name.
    return reg->workflow_by_name(id_or_name);
}

const engine::JobDef* McpHandler::resolve_standalone_job(
    const std::string& id_or_name) const {
    auto reg = get_registry();
    if (!reg) return nullptr;

    // Try by ID first.
    const auto* job = reg->standalone_job(id_or_name);
    if (job) return job;

    // Try by name.
    return reg->standalone_job_by_name(id_or_name);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Helper: wrap tool result
// ═══════════════════════════════════════════════════════════════════════════

json McpHandler::wrap_tool_result(const json& result) {
    return {
        {"content", json::array({
            {{"type", "text"},
             {"text", result.dump(2)}}
        })}
    };
}

// ═══════════════════════════════════════════════════════════════════════════
//  Tool schemas
// ═══════════════════════════════════════════════════════════════════════════

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
