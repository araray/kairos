/// include/kairos/engine/explain.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/engine/explain.hpp — dry-run execution plan builder               ║
// ║                                                                          ║
// ║  Builds an ExecutionPlan (§11.7) for a workflow WITHOUT executing it:    ║
//  walks the DAG level-by-level, evaluates conditions that are decidable     ║
// ║ from run history (via QueryReader KEL bindings), and marks same-workflow ║
// ║  references as pending (undecidable before the run starts).              ║
// ║                                                                          ║
// ║  Shared by `kairos workflows explain` (CLI), the HTTP explain endpoint   ║
// ║  (GET /api/v1/explain/{name}), and MCP kairos.explainPlan.               ║
// ║                                                                          ║
// ║  Spec reference: §11.7                                                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/engine/execution_plan.hpp"
#include "kairos/engine/workflow_registry.hpp"

#include <chrono>

namespace kairos::persist {
class QueryReader;
}

namespace kairos::engine {

/// Build a dry-run execution plan for `wf`.
///
/// @param wf       The workflow definition (with pre-built DAG).
/// @param reader   Run-history reader for condition evaluation. May be null —
///                 conditions that need history are then marked pending.
/// @param now      Reference time for finished_within() style conditions.
/// @return The plan. Never throws for evaluation problems — condition
///         failures surface as ConditionPending entries.
[[nodiscard]] ExecutionPlan build_explain_plan(
    const WorkflowDef& wf,
    const persist::QueryReader* reader,
    std::chrono::system_clock::time_point now
        = std::chrono::system_clock::now());

}  // namespace kairos::engine
