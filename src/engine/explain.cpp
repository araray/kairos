/// src/engine/explain.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Dry-run execution plan builder — see include/kairos/engine/explain.hpp  ║
// ║                                                                          ║
// ║  Extracted from the CLI `workflows explain` implementation (Roadmap      ║
// ║  Phase 9) so CLI, HTTP, and MCP share one source of truth.               ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/explain.hpp"

#include "kairos/kel/evaluator.hpp"
#include "kairos/persist/query_reader.hpp"

namespace kairos::engine {

ExecutionPlan build_explain_plan(
    const WorkflowDef& wf,
    const persist::QueryReader* reader,
    std::chrono::system_clock::time_point now)
{
    ExecutionPlan plan;
    plan.workflow_id = wf.workflow_id;
    plan.workflow_name = wf.workflow_name;
    plan.trigger_type = "explain";

    for (int lvl = 0; lvl < wf.dag.level_count(); ++lvl) {
        for (const auto& job_id : wf.dag.jobs_at_level(lvl)) {
            const auto& dag_node = wf.dag.node(job_id);

            PlanEntry entry;
            entry.job_id = dag_node.job_id;
            entry.job_name = dag_node.job_name;
            entry.level = dag_node.topo_level;
            entry.needs = dag_node.needs;
            entry.condition_expr = dag_node.condition_expr.value_or("");

            // Check if any predecessor would not run.
            bool all_needs_will_run = true;
            for (const auto& need_id : dag_node.needs) {
                for (const auto& prev : plan.entries) {
                    if (prev.job_id == need_id &&
                        prev.action != PlanAction::Run &&
                        prev.action != PlanAction::ConditionPending) {
                        all_needs_will_run = false;
                        break;
                    }
                }
            }

            if (!all_needs_will_run) {
                entry.action = PlanAction::DependencyFailed;
                entry.reason = "Upstream dependency will not run";
                plan.entries.push_back(std::move(entry));
                continue;
            }

            // Evaluate condition expression if present.
            if (dag_node.condition_expr.has_value() &&
                !dag_node.condition_expr->empty()) {
                // A condition referencing a job in THIS workflow is
                // undecidable before execution.
                bool references_self_jobs = false;
                for (const auto& other_id : wf.dag.all_job_ids()) {
                    const auto& other_node = wf.dag.node(other_id);
                    if (dag_node.condition_expr->find(
                            "\"" + other_node.job_name + "\"") !=
                        std::string::npos) {
                        references_self_jobs = true;
                        break;
                    }
                }

                if (references_self_jobs) {
                    entry.action = PlanAction::ConditionPending;
                    entry.reason = "Condition references same-workflow job "
                                   "(undecidable before execution)";
                    entry.condition_result = "pending";
                    plan.entries.push_back(std::move(entry));
                    continue;
                }

                // Try to evaluate the condition against history.
                if (reader) {
                    try {
                        kel::EvalContext ctx;
                        ctx.variables["workflow"] =
                            kel::KelValue(wf.workflow_name);
                        ctx.variables["trigger"] =
                            kel::KelValue(std::string("explain"));
                        ctx.variables["run_id"] =
                            kel::KelValue(std::string("(dry-run)"));

                        reader->register_kel_bindings(ctx, now);

                        kel::EvalLimits limits;
                        auto result = kel::eval_expression(
                            *dag_node.condition_expr, ctx, limits);

                        if (result.is_bool()) {
                            if (result.as_bool()) {
                                entry.action = PlanAction::Run;
                                entry.reason = "Condition: " +
                                    *dag_node.condition_expr + " \xe2\x86\x92 true";
                                entry.condition_result = "true";
                            } else {
                                entry.action = PlanAction::Skip;
                                entry.reason = "Condition: " +
                                    *dag_node.condition_expr + " \xe2\x86\x92 false";
                                entry.condition_result = "false";
                            }
                        } else {
                            entry.action = PlanAction::ConditionPending;
                            entry.reason = "Condition evaluated to "
                                           "non-boolean result";
                            entry.condition_result = "error";
                        }
                    } catch (const std::exception& e) {
                        entry.action = PlanAction::ConditionPending;
                        entry.reason = std::string(
                            "Condition eval error: ") + e.what();
                        entry.condition_result = "error";
                    }
                } else {
                    // No DB available — mark as pending.
                    entry.action = PlanAction::ConditionPending;
                    entry.reason = "No database available for "
                                   "condition evaluation";
                    entry.condition_result = "pending";
                }
            } else {
                // No condition — will run.
                if (dag_node.needs.empty()) {
                    entry.action = PlanAction::Run;
                    entry.reason = "No dependencies, no condition";
                } else {
                    std::string needs_str;
                    for (size_t i = 0; i < dag_node.needs.size(); ++i) {
                        if (i > 0) needs_str += ", ";
                        try {
                            needs_str += wf.dag.node(
                                dag_node.needs[i]).job_name;
                        } catch (...) {
                            needs_str += dag_node.needs[i];
                        }
                    }
                    entry.action = PlanAction::Run;
                    entry.reason = "Needs [" + needs_str +
                        "] \xe2\x86\x92 will be met";
                }
            }

            plan.entries.push_back(std::move(entry));
        }
    }

    return plan;
}

}  // namespace kairos::engine
