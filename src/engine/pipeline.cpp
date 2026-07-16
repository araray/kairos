/// src/engine/pipeline.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Pipeline engine implementation                                           ║
// ║  Spec reference: §11.4–§11.12                                           ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/pipeline.hpp"
#include "kairos/core/id_generator.hpp"
#include "kairos/exec/output_sink.hpp"
#include "kairos/kel/errors.hpp"

#include <spdlog/spdlog.h>

#include <condition_variable>
#include <future>
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

    // ── Per-run cancellation (§23.10) ─────────────────────────────
    // Register this run with the CancelRegistry to get a per-run
    // stop_token. Create a CombinedStopToken that fires when either
    // the global stop OR the per-run cancel fires.
    std::stop_token run_cancel_token;
    if (deps_.cancel_registry) {
        run_cancel_token = deps_.cancel_registry->register_run(ctx.run_id);
    }

    // Build combined stop token: fires on global shutdown OR per-run cancel.
    CombinedStopToken combined;
    std::stop_token effective_stop = stop;
    if (deps_.cancel_registry) {
        combined.arm(stop, run_cancel_token);
        effective_stop = combined.token();
    }

    // Store cancel token for WorkItems (runner pool uses it too).
    ctx.cancel_token = deps_.cancel_registry ? run_cancel_token
                                              : std::stop_token{};

    spdlog::info("Run {} starting for workflow '{}' (trigger: {})",
                 ctx.run_id, ctx.workflow_name, ctx.trigger_type);

    persist_run_start(ctx);

    auto status = execute_run(*dag_opt, ctx, effective_stop);
    ctx.run_status = status;

    persist_run_complete(ctx);

    // Close RunStream subscribers for this run (§14.7).
    // Any live followers (CLI --follow, MCP, SSE) will see
    // the stream end and can check the final run status.
    if (deps_.run_stream) deps_.run_stream->close_run(ctx.run_id);

    // Unregister from CancelRegistry (cleanup).
    if (deps_.cancel_registry) {
        deps_.cancel_registry->unregister_run(ctx.run_id);
    }

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
    if (job_ids.empty()) return;

    // ── Single job at this level: no parallelism overhead ────────────
    if (job_ids.size() == 1) {
        const auto& job_id = job_ids[0];
        execute_single_job_in_level(job_id, dag, ctx, stop);
        return;
    }

    // ── Multiple jobs at this level: parallel dispatch ───────────────
    // Per spec §11.4: evaluate conditions on the pipeline thread (they
    // may read RunContext::job_statuses from prior levels), then dispatch
    // ready jobs to run concurrently via std::jthread.
    //
    // We use an atomic counter + condition_variable to wait for all
    // jobs at this level to complete before proceeding.

    std::atomic<int> remaining{0};
    std::mutex level_mu;
    std::condition_variable level_cv;

    // Pre-evaluate needs and conditions (on pipeline thread).
    // This produces a list of jobs to actually execute.
    struct JobDecision {
        std::string job_id;
        bool should_run = false;
    };
    std::vector<JobDecision> decisions;
    decisions.reserve(job_ids.size());

    for (const auto& job_id : job_ids) {
        JobDecision dec;
        dec.job_id = job_id;

        if (stop.stop_requested()) {
            std::lock_guard lock(level_mu);
            ctx.job_statuses[job_id] = RunContext::JobStatus{
                .status = RunStatus::Cancelled, .reason = "Shutdown requested",
                .start_time = deps_.clock->now(), .end_time = deps_.clock->now()};
            decisions.push_back(std::move(dec));
            continue;
        }

        const auto& node = dag.node(job_id);

        // Check needs resolution.
        auto needs_dec = check_needs(node, ctx);
        if (needs_dec.action != ConditionDecision::Action::Run) {
            std::lock_guard lock(level_mu);
            ctx.job_statuses[job_id] = RunContext::JobStatus{
                .status = RunStatus::Skipped, .exit_code = 0,
                .reason = needs_dec.reason,
                .start_time = deps_.clock->now(), .end_time = deps_.clock->now()};
            spdlog::info("Run {}: job '{}' skipped ({})",
                         ctx.run_id, node.job_name, needs_dec.reason);
            persist_job_start(ctx, job_id, node.job_name, RunStatus::Skipped, "");
            persist_job_complete(ctx, job_id, RunStatus::Skipped, 0);
            decisions.push_back(std::move(dec));
            continue;
        }

        // Evaluate KEL condition.
        auto cond_dec = evaluate_condition(node, ctx);
        if (cond_dec.action == ConditionDecision::Action::Skip) {
            std::lock_guard lock(level_mu);
            ctx.job_statuses[job_id] = RunContext::JobStatus{
                .status = RunStatus::Skipped, .exit_code = 0,
                .reason = cond_dec.reason,
                .start_time = deps_.clock->now(), .end_time = deps_.clock->now()};
            spdlog::info("Run {}: job '{}' condition false: {}",
                         ctx.run_id, node.job_name, cond_dec.reason);
            persist_job_start(ctx, job_id, node.job_name, RunStatus::Skipped, "false");
            persist_job_complete(ctx, job_id, RunStatus::Skipped, 0);
            decisions.push_back(std::move(dec));
            continue;
        }

        if (cond_dec.action == ConditionDecision::Action::Error) {
            std::lock_guard lock(level_mu);
            ctx.job_statuses[job_id] = RunContext::JobStatus{
                .status = RunStatus::Failure, .exit_code = 201,
                .reason = cond_dec.reason,
                .start_time = deps_.clock->now(), .end_time = deps_.clock->now()};
            spdlog::error("Run {}: job '{}' condition error: {}",
                          ctx.run_id, node.job_name, cond_dec.reason);
            persist_job_start(ctx, job_id, node.job_name, RunStatus::Failure, "error");
            persist_job_complete(ctx, job_id, RunStatus::Failure, 201);
            decisions.push_back(std::move(dec));
            continue;
        }

        // This job will run.
        dec.should_run = true;
        remaining.fetch_add(1, std::memory_order_relaxed);

        {
            std::lock_guard lock(level_mu);
            ctx.job_statuses[job_id] = RunContext::JobStatus{
                .status = RunStatus::Running,
                .start_time = deps_.clock->now()};
        }
        const auto& node_name = dag.node(job_id).job_name;
        persist_job_start(ctx, job_id, node_name, RunStatus::Running, "true");

        decisions.push_back(std::move(dec));
    }

    // If nothing to actually run, return immediately.
    if (remaining.load(std::memory_order_relaxed) == 0) return;

    // Dispatch runnable jobs as parallel jthreads.
    std::vector<std::jthread> job_threads;
    job_threads.reserve(remaining.load());

    for (const auto& dec : decisions) {
        if (!dec.should_run) continue;

        job_threads.emplace_back([this, &dec, &ctx, &dag, &remaining,
                                   &level_mu, &level_cv, stop]() {
            auto job_status = execute_job(dec.job_id, ctx, stop);

            {
                std::lock_guard lock(level_mu);
                ctx.job_statuses[dec.job_id].status = job_status;
                ctx.job_statuses[dec.job_id].end_time = deps_.clock->now();
            }

            persist_job_complete(ctx, dec.job_id, job_status,
                                ctx.job_statuses[dec.job_id].exit_code);

            if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                level_cv.notify_one();
            }
        });
    }

    // Wait for all parallel jobs at this level to complete.
    {
        std::unique_lock lock(level_mu);
        level_cv.wait(lock, [&] {
            return remaining.load(std::memory_order_acquire) == 0
                   || stop.stop_requested();
        });
    }

    // Join all threads (destructors would do this, but explicit is clearer).
    for (auto& t : job_threads) {
        if (t.joinable()) t.join();
    }
}

/// Helper: execute a single job within a level (no parallelism overhead).
/// Handles needs checking, condition evaluation, and execution sequentially.
void Pipeline::execute_single_job_in_level(
    const std::string& job_id,
    const WorkflowDag& dag,
    RunContext& ctx,
    std::stop_token stop)
{
    if (stop.stop_requested()) {
        ctx.job_statuses[job_id] = RunContext::JobStatus{
            .status = RunStatus::Cancelled, .reason = "Shutdown requested",
            .start_time = deps_.clock->now(), .end_time = deps_.clock->now()};
        return;
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
        return;
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
        return;
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
        return;
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

        // ── Wire output streaming (§14.7) ────────────────────────
        // Create a per-step OutputMultiplexer that fans out to:
        //   1. DB writer (log_chunks table, batched async)
        //   2. RunStream pub-sub (live followers: CLI, MCP, SSE)
        exec::OutputMultiplexer output_mux;

        // Apply secret masking (§17.3): register secret values
        // so all output chunks are redacted before reaching any sink.
        if (!deps_.secret_values.empty()) {
            output_mux.set_secret_values(deps_.secret_values);
        }

        std::atomic<int64_t> chunk_index{0};

        // Sink 1: DB writer — persist log chunks.
        if (deps_.db_writer) {
            output_mux.add_sink(
                [this, &ctx, &job_id, &step, &chunk_index]
                (std::string_view chunk, bool is_stderr) {
                    deps_.db_writer->enqueue(persist::InsertLogChunk{
                        .run_id = ctx.run_id,
                        .job_id = job_id,
                        .step_id = step.step_id,
                        .chunk_index = chunk_index.fetch_add(1),
                        .stream = is_stderr ? "stderr" : "stdout",
                        .content = std::string(chunk),
                    });
                });
        }

        // Sink 2: RunStream pub-sub — live followers.
        if (deps_.run_stream) {
            output_mux.add_sink(
                [this, &ctx, &job_id, &step]
                (std::string_view chunk, bool is_stderr) {
                    deps_.run_stream->publish(
                        ctx.run_id, job_id, step.step_id,
                        chunk, is_stderr);
                });
        }

        // Submit to runner pool and wait for completion.
        // Uses promise/future instead of CV+atomic: the shared state is
        // heap-allocated, so it's safe even if the worker thread is still
        // unwinding from set_value() when we destroy locals here.
        // Wrapped in shared_ptr because std::function requires copyable callables.
        auto step_promise = std::make_shared<std::promise<exec::ProcessResult>>();
        auto step_future = step_promise->get_future();

        exec::WorkItem item;
        item.run_id = ctx.run_id;
        item.job_id = job_id;
        item.step_id = step.step_id;
        item.correlation_id = ctx.correlation_id;
        item.process_spec = std::move(proc_spec);
        item.output_callback = exec::make_output_callback(output_mux);
        // Per-run cancel token (§23.10): the runner pool will merge
        // this with the global stop token so that cancelling a single
        // run kills only that run's processes.
        if (ctx.cancel_token.stop_possible()) {
            item.cancel_token = ctx.cancel_token;
        }
        item.on_complete = [step_promise](
            const std::string&, exec::ProcessResult result) {
            step_promise->set_value(std::move(result));
        };

        bool submitted = deps_.runner_pool ?
            deps_.runner_pool->submit(std::move(item)) : false;

        if (!submitted) {
            spdlog::error("Run {}: failed to submit step '{}' to runner pool",
                          ctx.run_id, step.step_name);
            ctx.job_statuses[job_id].exit_code = 202;
            return RunStatus::Failure;
        }

        // Block until the worker completes the step.
        auto step_result = step_future.get();

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

    // Phase 8.5: Register has_tag(entity_id, tag) → bool.
    // Checks all entity types (workflow, job, watch_group, trigger).
    if (deps_.tag_store) {
        auto* ts = deps_.tag_store;
        kel_ctx.functions["has_tag"] = [ts](
            const std::vector<kel::KelValue>& args) -> kel::KelValue
        {
            if (args.size() != 2 || !args[0].is_string() || !args[1].is_string())
                throw kel::KelEvalError(
                    "has_tag() requires two string arguments (entity_id, tag)");
            const auto& entity_id = args[0].as_string();
            const auto& tag = args[1].as_string();
            // Check all entity types sequentially.
            static const std::string types[] = {
                "workflow", "job", "watch_group", "trigger"};
            for (const auto& t : types) {
                if (ts->has_tag(t, entity_id, tag))
                    return kel::KelValue(true);
            }
            return kel::KelValue(false);
        };
    }

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

    // Build environment via the 7-layer EnvBuilder (§14.6).
    exec::EnvBuilder builder;
    builder.inherit_parent()
           .add_job(job.env)
           .add_step(step.env);

    // Layer 6: Resolve ${{ secrets.key }} references (§17.1).
    if (deps_.secret_resolver) {
        builder.resolve_secrets(deps_.secret_resolver);
    }

    // Layer 7: Inject Kairos metadata variables.
    builder.add_kairos_vars(
        ctx.run_id, job.job_id, step.step_id,
        ctx.workflow_id, ctx.trigger_type,
        ctx.correlation_id,
        "", "");  // data_dir and db_path filled by config

    spec.environment = builder.build();
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
