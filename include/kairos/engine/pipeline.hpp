/// include/kairos/engine/pipeline.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/engine/pipeline.hpp — DAG execution pipeline                     ║
// ║                                                                           ║
// ║  Consumes TriggerEvents from the trigger bus, resolves targets to        ║
// ║  WorkflowDags, evaluates KEL conditions level-by-level, submits          ║
// ║  WorkItems to the RunnerPool, and persists results via DBWriter.         ║
// ║                                                                           ║
// ║  Spec reference: §11.4–§11.12                                           ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/engine/cancel_registry.hpp"
#include "kairos/engine/dag.hpp"
#include "kairos/engine/execution_plan.hpp"
#include "kairos/engine/trigger_event.hpp"
#include "kairos/engine/trigger_types.hpp"
#include "kairos/engine/workflow_registry.hpp"
#include "kairos/exec/output_sink.hpp"
#include "kairos/exec/runner_pool.hpp"
#include "kairos/kel/evaluator.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/query_reader.hpp"
#include "kairos/testing/fake_clock.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>

namespace kairos::engine {

// ── Run status ──────────────────────────────────────────────────────────

/// Status of a job or run.
enum class RunStatus {
    Evaluating,  ///< Pre-execution (building plan).
    Running,     ///< In progress.
    Success,     ///< Completed successfully.
    Failure,     ///< One or more jobs failed.
    Skipped,     ///< Condition was false.
    Cancelled,   ///< Shutdown or manual cancellation.
    TimedOut,    ///< Timeout exceeded.
};

/// Convert RunStatus to a string.
[[nodiscard]] constexpr std::string_view run_status_to_string(
    RunStatus s) noexcept
{
    switch (s) {
        case RunStatus::Evaluating: return "EVALUATING";
        case RunStatus::Running:    return "RUNNING";
        case RunStatus::Success:    return "SUCCESS";
        case RunStatus::Failure:    return "FAILURE";
        case RunStatus::Skipped:    return "SKIPPED";
        case RunStatus::Cancelled:  return "CANCELLED";
        case RunStatus::TimedOut:   return "TIMED_OUT";
    }
    return "UNKNOWN";
}

// ── Run context ─────────────────────────────────────────────────────────

/// Per-run context, threaded through the entire execution.
struct RunContext {
    std::string run_id;
    std::string workflow_id;
    std::string workflow_name;
    std::string trigger_type;
    std::string trigger_id;
    std::string correlation_id;

    /// Captured once at run start for deterministic condition evaluation.
    std::chrono::system_clock::time_point now;

    /// Per-run cancel token (from CancelRegistry, §23.10).
    /// Stored here so execute_job can set it on each WorkItem.
    std::stop_token cancel_token;

    /// Status of each job in this run.
    struct JobStatus {
        RunStatus status = RunStatus::Evaluating;
        int exit_code = -1;
        std::string reason;
        std::chrono::system_clock::time_point start_time;
        std::chrono::system_clock::time_point end_time;
    };
    std::unordered_map<std::string, JobStatus> job_statuses;

    /// Overall run status.
    RunStatus run_status = RunStatus::Evaluating;
};

// ── Condition evaluation decision ───────────────────────────────────────

/// Result of evaluating a job's condition.
struct ConditionDecision {
    enum class Action { Run, Skip, Error };
    Action action = Action::Run;
    std::string reason;

    static ConditionDecision run() {
        return {Action::Run, "No condition or condition true"};
    }
    static ConditionDecision skip(std::string reason) {
        return {Action::Skip, std::move(reason)};
    }
    static ConditionDecision error(std::string reason) {
        return {Action::Error, std::move(reason)};
    }
};

// ── Pipeline engine ─────────────────────────────────────────────────────

/// Pipeline configuration.
struct PipelineConfig {
    /// Timeout for draining the trigger queue during shutdown.
    std::chrono::seconds drain_timeout{30};

    /// KEL evaluation limits.
    kel::EvalLimits kel_limits;
};

/// The Pipeline engine.
///
/// Thread model: runs on a single jthread (Thread 3: Pipeline).
/// Consumes from trigger bus, dispatches to runner pool.
class Pipeline {
public:
    /// Dependencies needed by the pipeline.
    struct Dependencies {
        ClockSource* clock = nullptr;
        TriggerBus* trigger_bus = nullptr;
        exec::RunnerPool* runner_pool = nullptr;
        std::shared_ptr<const WorkflowRegistry> registry;
        ActiveRunTracker* active_runs = nullptr;
        persist::DBWriter* db_writer = nullptr;
        persist::QueryReader* query_reader = nullptr;
        exec::RunStream* run_stream = nullptr;
        CancelRegistry* cancel_registry = nullptr;
    };

    explicit Pipeline(PipelineConfig config, Dependencies deps);
    ~Pipeline();

    /// Start the pipeline thread.
    void start(std::stop_token stop);

    /// Run the pipeline synchronously (for testing).
    void run(std::stop_token stop);

    /// Process a single trigger event (for testing).
    /// Returns the final RunStatus.
    RunStatus process_event(const TriggerEvent& event,
                            std::stop_token stop);

    /// Request a configuration reload (new registry).
    void request_reload(
        std::shared_ptr<const WorkflowRegistry> new_registry);

    /// Stop the pipeline (joins thread if running).
    void stop();

private:
    /// Execute a complete workflow run.
    RunStatus execute_run(const WorkflowDag& dag,
                          RunContext& ctx,
                          std::stop_token stop);

    /// Execute one level of the DAG (parallel when multiple jobs).
    void execute_level(const std::vector<std::string>& job_ids,
                       const WorkflowDag& dag,
                       RunContext& ctx,
                       std::stop_token stop);

    /// Execute a single job within a level (no parallelism overhead).
    void execute_single_job_in_level(const std::string& job_id,
                                     const WorkflowDag& dag,
                                     RunContext& ctx,
                                     std::stop_token stop);

    /// Execute a single job (all steps).
    RunStatus execute_job(const std::string& job_id,
                          RunContext& ctx,
                          std::stop_token stop);

    /// Evaluate a job's KEL condition.
    ConditionDecision evaluate_condition(
        const DagNode& node,
        const RunContext& ctx);

    /// Check if a job's dependencies are satisfied.
    ConditionDecision check_needs(const DagNode& node,
                                  const RunContext& ctx);

    /// Build a ProcessSpec from a StepDef.
    exec::ProcessSpec build_process_spec(
        const StepDef& step,
        const JobDef& job,
        const RunContext& ctx);

    /// Persist run/job/step lifecycle events.
    void persist_run_start(const RunContext& ctx);
    void persist_run_complete(const RunContext& ctx);
    void persist_job_start(const RunContext& ctx,
                           const std::string& job_id,
                           const std::string& job_name,
                           RunStatus status,
                           const std::string& condition_result = "");
    void persist_job_complete(const RunContext& ctx,
                              const std::string& job_id,
                              RunStatus status, int exit_code);

    /// Format a time_point as ISO 8601.
    static std::string format_iso8601(
        std::chrono::system_clock::time_point tp);

    PipelineConfig config_;
    Dependencies deps_;

    // Reload coordination.
    std::atomic<bool> reload_requested_{false};
    std::shared_ptr<const WorkflowRegistry> pending_registry_;
    std::mutex reload_mu_;

    // Thread.
    std::jthread thread_;
};

}  // namespace kairos::engine
