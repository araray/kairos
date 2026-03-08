/// src/engine/pipeline.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Pipeline engine implementation                                           ║
// ║  Spec reference: §11.4–§11.12                                           ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/pipeline.hpp"
#include "kairos/core/id_generator.hpp"
#include "kairos/platform/threading.hpp"

#include <spdlog/spdlog.h>

#include <condition_variable>
#include <mutex>

namespace kairos::engine {

Pipeline::Pipeline(PipelineConfig config, Dependencies deps)
    : config_(std::move(config)), deps_(std::move(deps))
{
    if (!deps_.clock) {
        throw std::invalid_argument("Pipeline requires a non-null ClockSource");
    }
}

Pipeline::~Pipeline() { stop(); }

void Pipeline::start(std::stop_token stop) {
    thread_ = std::jthread([this, stop](std::stop_token) { run(stop); });
}

void Pipeline::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void Pipeline::run(std::stop_token stop) {
    platform::set_thread_name("kairos-pipe");
    spdlog::info("Pipeline thread starting");

    while (!stop.stop_requested()) {
        if (reload_requested_.exchange(false)) {
            std::lock_guard lock(reload_mu_);
            if (pending_registry_) deps_.registry = std::move(pending_registry_);
            spdlog::info("Pipeline reloaded workflow registry");
        }

        auto event_opt = deps_.trigger_bus->pop(std::chrono::milliseconds(500));
        if (!event_opt) continue;

        if (event_opt->type == TriggerType::ConfigReload) {
            spdlog::info("Pipeline received config reload event");
            continue;
        }

        process_event(*event_opt, stop);
    }

    spdlog::info("Pipeline thread stopping");
}

RunStatus Pipeline::process_event(const TriggerEvent& event,
                                   std::stop_token stop)
{
    spdlog::info("Pipeline processing {} event for target '{}' (correlation={})",
                 trigger_type_to_string(event.type), event.target_id,
                 event.correlation_id);

    if (!deps_.registry) {
        spdlog::error("Pipeline: no workflow registry loaded");
        return RunStatus::Failure;
    }

    auto dag_opt = deps_.registry->resolve_dag(event.target_id, event.target_kind);
    if (!dag_opt) {
        spdlog::error("Pipeline: target '{}' not found in registry", event.target_id);
        return RunStatus::Failure;
    }

    // Determine workflow name.
    std::string workflow_name;
    if (event.target_kind == TriggerEvent::TargetKind::Workflow) {
        auto wf = deps_.registry->workflow(event.target_id);
        workflow_name = wf ? wf->workflow_name : event.target_id;
    } else {
        auto sj = deps_.registry->standalone_job(event.target_id);
        workflow_name = sj ? sj->job_name : event.target_id;
    }

    // Create RunContext.
    RunContext ctx;
    ctx.run_id = core::generate_run_id();
    ctx.workflow_id = event.target_id;
    ctx.workflow_name = workflow_name;
    ctx.trigger_type = std::string(trigger_type_to_string(event.type));
    ctx.trigger_id = event.trigger_id;
    ctx.correlation_id = event.correlation_id;
    ctx.now = deps_.clock->now();

    if (deps_.active_runs) deps_.active_runs->increment(event.target_id);

    spdlog::info("Run {} starting for workflow '{}' (trigger: {})",
                 ctx.run_id, ctx.workflow_name, ctx.trigger_type);

    persist_run_start(ctx);

    auto status = execute_run(*dag_opt, ctx, stop);
    ctx.run_status = status;

    persist_run_complete(ctx);

    if (deps_.active_runs) deps_.active_runs->decrement(event.target_id);

    spdlog::info("Run {} completed: {} (workflow: '{}')",
                 ctx.run_id, run_status_to_string(status), ctx.workflow_name);
    return status;
}

RunStatus Pipeline::execute_run(const WorkflowDag& dag, RunContext& ctx,
                                 std::stop_token stop)
{
    ctx.run_status = RunStatus::Running;
    bool any_failed = false;

    for (int level = 0; level <= dag.max_level(); ++level) {
        if (stop.stop_requested()) return RunStatus::Cancelled;
        const auto& job_ids = dag.jobs_at_level(level);
        if (job_ids.empty()) continue;

        execute_level(job_ids, dag, ctx, stop);

        for (const auto& jid : job_ids) {
            auto it = ctx.job_statuses.find(jid);
            if (it != ctx.job_statuses.end() &&
                it->second.status == RunStatus::Failure) {
                any_failed = true;
            }
        }
    }

    if (stop.stop_requested()) return RunStatus::Cancelled;
    return any_failed ? RunStatus::Failure : RunStatus::Success;
}

void Pipeline::execute_level(const std::vector<std::string>& job_ids,
                              const WorkflowDag& dag, RunContext& ctx,
                              std::stop_token stop)
{
    // Execute jobs at this level sequentially for v1.
    // (Parallel dispatch within a level is a v2 enhancement.)
    for (const auto& job_id : job_ids) {
        if (stop.stop_requested()) {
            ctx.job_statuses[job_id] = RunContext::JobStatus{
                .status = RunStatus::Cancelled, .reason = "Shutdown requested",
                .start_time = deps_.clock->now(), .end_time = deps_.clock->now()};
            continue;
        }

        const auto& node = dag.node(job_id);

        // Check needs resolution.
        auto needs_dec = check_needs(node, ctx);
        if (needs_dec.action != ConditionDecision::Action::Run) {
            ctx.job_statuses[job_id] = RunContext::JobStatus{
                .status = RunStatus::Skipped, .exit_code = 0,
                .reason = needs_dec.reason,
                .start_time = deps_.clock->now(), .end_time = deps_.clock->now()};
            spdlog::info("Run {}: job '{}' skipped ({})",
                         ctx.run_id, node.job_name, needs_dec.reason);
            persist_job_start(ctx, job_id, node.job_name, RunStatus::Skipped, "");
            persist_job_complete(ctx, job_id, RunStatus::Skipped, 0);
            continue;
        }

        // Evaluate condition.
        auto cond_dec = evaluate_condition(node, ctx);
        if (cond_dec.action == ConditionDecision::Action::Skip) {
            ctx.job_statuses[job_id] = RunContext::JobStatus{
                .status = RunStatus::Skipped, .exit_code = 0,
                .reason = cond_dec.reason,
                .start_time = deps_.clock->now(), .end_time = deps_.clock->now()};
            spdlog::info("Run {}: job '{}' condition false: {}",
                         ctx.run_id, node.job_name, cond_dec.reason);
            persist_job_start(ctx, job_id, node.job_name, RunStatus::Skipped, "false");
            persist_job_complete(ctx, job_id, RunStatus::Skipped, 0);
            continue;
        }

        if (cond_dec.action == ConditionDecision::Action::Error) {
            ctx.job_statuses[job_id] = RunContext::JobStatus{
                .status = RunStatus::Failure, .exit_code = 201,
                .reason = cond_dec.reason,
                .start_time = deps_.clock->now(), .end_time = deps_.clock->now()};
            spdlog::error("Run {}: job '{}' condition error: {}",
                          ctx.run_id, node.job_name, cond_dec.reason);
            persist_job_start(ctx, job_id, node.job_name, RunStatus::Failure, "error");
            persist_job_complete(ctx, job_id, RunStatus::Failure, 201);
            continue;
        }

        // Execute job.
        ctx.job_statuses[job_id] = RunContext::JobStatus{
            .status = RunStatus::Running, .start_time = deps_.clock->now()};
        persist_job_start(ctx, job_id, node.job_name, RunStatus::Running, "true");

        auto job_status = execute_job(job_id, ctx, stop);

        ctx.job_statuses[job_id].status = job_status;
        ctx.job_statuses[job_id].end_time = deps_.clock->now();
        persist_job_complete(ctx, job_id, job_status,
                            ctx.job_statuses[job_id].exit_code);
    }
}

RunStatus Pipeline::execute_job(const std::string& job_id, RunContext& ctx,
                                 std::stop_token stop)
{
    auto job_def = deps_.registry->job(job_id);
    if (!job_def || job_def->steps.empty()) {
        spdlog::debug("Run {}: job '{}' has no steps, marking success",
                      ctx.run_id, job_id);
        return RunStatus::Success;
    }

    bool any_step_failed = false;

    for (const auto& step : job_def->steps) {
        if (stop.stop_requested()) return RunStatus::Cancelled;

        spdlog::debug("Run {}: executing step '{}' (job='{}')",
                      ctx.run_id, step.step_name, job_def->job_name);

        auto proc_spec = build_process_spec(step, *job_def, ctx);

        // Submit to runner pool and wait for completion callback.
        std::mutex step_mu;
        std::condition_variable step_cv;
        std::atomic<bool> step_done{false};
        exec::ProcessResult step_result;

        exec::WorkItem item;
        item.run_id = ctx.run_id;
        item.job_id = job_id;
        item.step_id = step.step_id;
        item.correlation_id = ctx.correlation_id;
        item.process_spec = std::move(proc_spec);
        item.on_complete = [&](const std::string&, exec::ProcessResult result) {
            step_result = std::move(result);
            step_done.store(true);
            step_cv.notify_one();
        };

        bool submitted = deps_.runner_pool ?
            deps_.runner_pool->submit(std::move(item)) : false;

        if (!submitted) {
            spdlog::error("Run {}: failed to submit step '{}' to runner pool",
                          ctx.run_id, step.step_name);
            ctx.job_statuses[job_id].exit_code = 202;
            return RunStatus::Failure;
        }

        {
            std::unique_lock lock(step_mu);
            step_cv.wait(lock, [&] {
                return step_done.load() || stop.stop_requested();
            });
        }

        if (stop.stop_requested() && !step_done.load()) return RunStatus::Cancelled;

        // Persist step.
        if (deps_.db_writer) {
            deps_.db_writer->enqueue(persist::InsertStepRun{
                .run_id = ctx.run_id, .job_id = job_id,
                .step_id = step.step_id, .step_name = step.step_name,
                .command = step.command,
                .status = step_result.success() ? "SUCCESS" : "FAILURE",
                .start_ts = format_iso8601(deps_.clock->now()),
            });
        }

        if (!step_result.success()) {
            spdlog::info("Run {}: step '{}' failed (exit {})",
                         ctx.run_id, step.step_name, step_result.exit_code);
            any_step_failed = true;
            ctx.job_statuses[job_id].exit_code = step_result.exit_code;
            if (!job_def->continue_on_error) return RunStatus::Failure;
        }
    }

    return any_step_failed ? RunStatus::Failure : RunStatus::Success;
}

ConditionDecision Pipeline::evaluate_condition(const DagNode& node,
                                                const RunContext& ctx)
{
    if (!node.condition_expr.has_value() || node.condition_expr->empty())
        return ConditionDecision::run();

    const auto& expr = *node.condition_expr;
    auto kel_ctx = kel::make_default_context();
    kel_ctx.variables["workflow"] = kel::KelValue(ctx.workflow_name);
    kel_ctx.variables["trigger"] = kel::KelValue(ctx.trigger_type);
    kel_ctx.variables["run_id"] = kel::KelValue(ctx.run_id);

    if (deps_.query_reader)
        deps_.query_reader->register_kel_bindings(kel_ctx, ctx.now);

    try {
        auto result = kel::eval_expression(expr, kel_ctx, config_.kel_limits);
        if (!result.is_bool())
            return ConditionDecision::error(
                "Condition must evaluate to bool, got " +
                std::string(result.type_name()));
        return result.as_bool() ? ConditionDecision::run()
                                : ConditionDecision::skip("Condition false: " + expr);
    } catch (const std::exception& e) {
        return ConditionDecision::error(e.what());
    }
}

ConditionDecision Pipeline::check_needs(const DagNode& node,
                                         const RunContext& ctx)
{
    for (const auto& need_id : node.needs) {
        auto it = ctx.job_statuses.find(need_id);
        if (it == ctx.job_statuses.end())
            return ConditionDecision::skip("Dependency '" + need_id + "' has not run");
        if (it->second.status != RunStatus::Success)
            return ConditionDecision::skip(
                "Dependency '" + need_id + "' was " +
                std::string(run_status_to_string(it->second.status)));
    }
    return ConditionDecision::run();
}

exec::ProcessSpec Pipeline::build_process_spec(
    const StepDef& step, const JobDef& job, const RunContext& ctx)
{
    exec::ProcessSpec spec;
    spec.command_line = step.command;
    spec.use_shell = step.use_shell;
    if (!step.working_dir.empty()) spec.working_dir = step.working_dir;
    else if (!job.working_dir.empty()) spec.working_dir = job.working_dir;
    spec.timeout = step.timeout;

    spec.environment = job.env;
    for (const auto& [k, v] : step.env) spec.environment[k] = v;
    spec.environment["KAIROS_RUN_ID"] = ctx.run_id;
    spec.environment["KAIROS_JOB_ID"] = job.job_id;
    spec.environment["KAIROS_STEP_ID"] = step.step_id;
    spec.environment["KAIROS_WORKFLOW"] = ctx.workflow_name;
    spec.environment["KAIROS_TRIGGER"] = ctx.trigger_type;
    spec.environment["KAIROS_CORRELATION_ID"] = ctx.correlation_id;
    return spec;
}

void Pipeline::request_reload(std::shared_ptr<const WorkflowRegistry> new_registry) {
    std::lock_guard lock(reload_mu_);
    pending_registry_ = std::move(new_registry);
    reload_requested_.store(true);
}

void Pipeline::persist_run_start(const RunContext& ctx) {
    if (!deps_.db_writer) return;
    deps_.db_writer->enqueue(persist::InsertRun{
        .run_id = ctx.run_id, .target_type = "workflow",
        .target_id = ctx.workflow_id, .target_name = ctx.workflow_name,
        .trigger_type = ctx.trigger_type, .trigger_id = ctx.trigger_id,
        .correlation_id = ctx.correlation_id, .status = "RUNNING",
        .start_ts = format_iso8601(ctx.now)});
}

void Pipeline::persist_run_complete(const RunContext& ctx) {
    if (!deps_.db_writer) return;
    auto end_time = deps_.clock->now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - ctx.now);
    int exit_code = 0;
    for (const auto& [jid, js] : ctx.job_statuses) {
        if (js.status == RunStatus::Failure && js.exit_code != 0) {
            exit_code = js.exit_code; break;
        }
    }
    deps_.db_writer->enqueue(persist::UpdateRunComplete{
        .run_id = ctx.run_id,
        .status = std::string(run_status_to_string(ctx.run_status)),
        .end_ts = format_iso8601(end_time),
        .exit_code = exit_code, .duration_ms = duration.count()});
}

void Pipeline::persist_job_start(const RunContext& ctx, const std::string& job_id,
                                  const std::string& job_name, RunStatus status,
                                  const std::string& condition_result) {
    if (!deps_.db_writer) return;
    deps_.db_writer->enqueue(persist::InsertJobRun{
        .run_id = ctx.run_id, .job_id = job_id, .job_name = job_name,
        .status = std::string(run_status_to_string(status)),
        .start_ts = format_iso8601(deps_.clock->now()),
        .condition_result = condition_result});
}

void Pipeline::persist_job_complete(const RunContext& ctx, const std::string& job_id,
                                     RunStatus status, int exit_code) {
    if (!deps_.db_writer) return;
    auto end_time = deps_.clock->now();
    auto it = ctx.job_statuses.find(job_id);
    int64_t duration_ms = 0;
    if (it != ctx.job_statuses.end())
        duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - it->second.start_time).count();
    deps_.db_writer->enqueue(persist::UpdateJobRunComplete{
        .run_id = ctx.run_id, .job_id = job_id,
        .status = std::string(run_status_to_string(status)),
        .end_ts = format_iso8601(end_time),
        .exit_code = exit_code, .duration_ms = duration_ms});
}

std::string Pipeline::format_iso8601(std::chrono::system_clock::time_point tp) {
    auto time_t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &time_t);
#else
    gmtime_r(&time_t, &tm_buf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return std::string(buf);
}

}  // namespace kairos::engine
