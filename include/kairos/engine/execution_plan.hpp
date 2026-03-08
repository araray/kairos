/// include/kairos/engine/execution_plan.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/engine/execution_plan.hpp — Execution plan for explain/dry-run   ║
// ║                                                                           ║
// ║  An ExecutionPlan describes what would happen if a workflow ran now,      ║
// ║  without actually executing anything. Used by `kairos explain`, MCP      ║
// ║  `kairos.explainPlan`, and the pipeline's pre-execution analysis.        ║
// ║                                                                           ║
// ║  Spec reference: §11.7 (execution plan generation)                      ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <sstream>
#include <string>
#include <vector>

namespace kairos::engine {

/// The planned action for a single job.
enum class PlanAction {
    Run,              ///< Job will execute.
    Skip,             ///< Job will be skipped (condition was false).
    ConditionPending, ///< Condition depends on current run's results; unknown.
    DependencyFailed, ///< Upstream dependency failed or was skipped.
    Disabled,         ///< Job is explicitly disabled.
};

/// Convert PlanAction to display string.
[[nodiscard]] constexpr std::string_view plan_action_to_string(
    PlanAction action) noexcept
{
    switch (action) {
        case PlanAction::Run:              return "RUN";
        case PlanAction::Skip:             return "SKIP";
        case PlanAction::ConditionPending: return "PEND";
        case PlanAction::DependencyFailed: return "DEP_FAIL";
        case PlanAction::Disabled:         return "DISABLED";
    }
    return "UNKNOWN";
}

/// A single entry in the execution plan.
struct PlanEntry {
    std::string job_id;           ///< Content-addressable job ID.
    std::string job_name;         ///< Human-readable job name.
    int level = 0;                ///< Topological level in the DAG.
    PlanAction action = PlanAction::Run;
    std::string reason;           ///< Human-readable explanation of why.

    /// The condition expression (if any).
    std::string condition_expr;

    /// The evaluated result of the condition (if evaluated).
    std::string condition_result;

    /// Dependencies (needs) that this job waits for.
    std::vector<std::string> needs;
};

/// The complete execution plan for a workflow.
struct ExecutionPlan {
    std::string workflow_id;       ///< Content-addressable workflow ID.
    std::string workflow_name;     ///< Human-readable workflow name.
    std::string trigger_type;      ///< What triggered this plan.
    std::vector<PlanEntry> entries; ///< One entry per job, in topological order.

    // ── Aggregate statistics ────────────────────────────────────────

    /// How many jobs will run?
    [[nodiscard]] int jobs_to_run() const {
        int count = 0;
        for (const auto& e : entries) {
            if (e.action == PlanAction::Run) ++count;
        }
        return count;
    }

    /// How many jobs will be skipped?
    [[nodiscard]] int jobs_to_skip() const {
        int count = 0;
        for (const auto& e : entries) {
            if (e.action == PlanAction::Skip) ++count;
        }
        return count;
    }

    /// How many jobs are condition-pending (undecidable before run)?
    [[nodiscard]] int jobs_pending() const {
        int count = 0;
        for (const auto& e : entries) {
            if (e.action == PlanAction::ConditionPending) ++count;
        }
        return count;
    }

    /// Maximum parallelism at any level (max number of RUN entries at one level).
    [[nodiscard]] int max_parallelism() const {
        // Count RUN entries per level.
        if (entries.empty()) return 0;
        int max_level = 0;
        for (const auto& e : entries) {
            if (e.level > max_level) max_level = e.level;
        }

        int max_par = 0;
        for (int lvl = 0; lvl <= max_level; ++lvl) {
            int par = 0;
            for (const auto& e : entries) {
                if (e.level == lvl && e.action == PlanAction::Run) ++par;
            }
            if (par > max_par) max_par = par;
        }
        return max_par;
    }

    // ── Rendering ───────────────────────────────────────────────────

    /// Render as human-readable text (for CLI / logs).
    [[nodiscard]] std::string render_text() const {
        std::ostringstream oss;
        oss << "Workflow: " << workflow_name
            << " (" << workflow_id << ")\n";
        oss << "Trigger:  " << trigger_type << "\n\n";

        // Header
        oss << "  Level │ Job           │ Action │ Reason\n";
        oss << "  ──────┼───────────────┼────────┼"
            << "──────────────────────────────────\n";

        for (const auto& e : entries) {
            // Pad job name to 13 chars.
            std::string padded_name = e.job_name;
            if (padded_name.size() < 13) {
                padded_name.resize(13, ' ');
            }

            oss << "  " << e.level
                << "     │ " << padded_name
                << " │ " << plan_action_to_string(e.action);

            // Pad action to 6 chars.
            auto action_str = plan_action_to_string(e.action);
            for (size_t i = action_str.size(); i < 6; ++i) {
                oss << ' ';
            }

            oss << " │ " << e.reason << "\n";

            // Show condition expression if present and pending.
            if (!e.condition_expr.empty() &&
                e.action == PlanAction::ConditionPending) {
                oss << "        │               │        │   Condition: "
                    << e.condition_expr << "\n";
            }
        }

        oss << "\n  Summary: "
            << jobs_to_run() << " to run, "
            << jobs_to_skip() << " to skip, "
            << jobs_pending() << " condition-pending\n";
        oss << "  Max parallelism: " << max_parallelism() << "\n";

        return oss.str();
    }

    /// Render as JSON string (for MCP / --format json).
    [[nodiscard]] std::string render_json() const;
};

}  // namespace kairos::engine
