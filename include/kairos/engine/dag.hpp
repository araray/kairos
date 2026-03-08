/// include/kairos/engine/dag.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/engine/dag.hpp — Workflow DAG construction and analysis           ║
// ║                                                                           ║
// ║  Defines DagNode (a job in the workflow) and WorkflowDag (the complete   ║
// ║  directed acyclic graph). Provides Kahn's topological sort with          ║
// ║  level-based grouping for parallel execution.                             ║
// ║                                                                           ║
// ║  Spec reference: §11.2 (DAG construction), §11.3 (topological sort)     ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <algorithm>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kairos::engine {

/// A node in the workflow DAG. Each node is a job definition.
struct DagNode {
    std::string job_id;         ///< Content-addressable ID (job-xxx)
    std::string job_name;       ///< Human-readable name
    std::vector<std::string> needs;  ///< IDs of predecessor jobs

    /// KEL expression for conditional execution (raw string).
    /// If empty/nullopt, job always runs.
    std::optional<std::string> condition_expr;

    /// If true, step failures don't fail the job (skipped steps
    /// are recorded but don't block dependents).
    bool continue_on_error = false;

    // ── Computed during DAG analysis ─────────────────────────────────

    /// Depth in the DAG (0 = root / no deps). Set by topological_sort().
    int topo_level = -1;

    /// Number of unresolved predecessors. Used during Kahn's algorithm.
    int in_degree = 0;
};

/// The complete DAG for a workflow.
///
/// Built from a list of DagNode definitions. Validates that all `needs`
/// references exist and that no duplicates are present. Cycle detection
/// is performed by topological_sort() (which is called during build).
///
/// Thread safety: immutable after construction. Safe to share across
/// threads without synchronization.
class WorkflowDag {
public:
    /// Build a DAG from a list of node definitions.
    ///
    /// Validates:
    ///   - No duplicate job IDs.
    ///   - All `needs` references resolve to existing nodes.
    ///   - The graph is acyclic (via Kahn's algorithm).
    ///
    /// @throws std::runtime_error on validation failure.
    static WorkflowDag build(std::vector<DagNode> nodes);

    /// Get the topological order (computed during build).
    [[nodiscard]] const std::vector<std::string>& topological_order() const {
        return topo_order_;
    }

    /// Get all jobs at a given topological level (for parallel execution).
    /// Level 0 = root nodes (no dependencies).
    [[nodiscard]] const std::vector<std::string>& jobs_at_level(
        int level) const
    {
        static const std::vector<std::string> empty;
        if (level < 0 || level >= static_cast<int>(levels_.size())) {
            return empty;
        }
        return levels_[static_cast<size_t>(level)];
    }

    /// Maximum topological level (depth of the DAG).
    [[nodiscard]] int max_level() const {
        return static_cast<int>(levels_.size()) - 1;
    }

    /// Number of levels.
    [[nodiscard]] int level_count() const {
        return static_cast<int>(levels_.size());
    }

    /// Get a node by ID.
    /// @throws std::out_of_range if ID not found.
    [[nodiscard]] const DagNode& node(const std::string& id) const {
        return nodes_.at(id);
    }

    /// Check if a node exists.
    [[nodiscard]] bool has_node(const std::string& id) const {
        return nodes_.count(id) > 0;
    }

    /// All node IDs in topological order.
    [[nodiscard]] std::vector<std::string> all_job_ids() const {
        return topo_order_;
    }

    /// Number of nodes.
    [[nodiscard]] size_t size() const { return nodes_.size(); }

    /// Is this a single-node DAG (standalone job)?
    [[nodiscard]] bool is_standalone() const { return nodes_.size() == 1; }

    /// Get direct successors (dependents) of a node.
    [[nodiscard]] const std::vector<std::string>& successors(
        const std::string& id) const
    {
        static const std::vector<std::string> empty;
        auto it = adj_forward_.find(id);
        return it != adj_forward_.end() ? it->second : empty;
    }

    /// Get direct predecessors (needs) of a node.
    [[nodiscard]] const std::vector<std::string>& predecessors(
        const std::string& id) const
    {
        static const std::vector<std::string> empty;
        auto it = adj_reverse_.find(id);
        return it != adj_reverse_.end() ? it->second : empty;
    }

    /// Validate that the graph is acyclic.
    /// Returns the cycle path if one is found.
    [[nodiscard]] std::optional<std::vector<std::string>> find_cycle() const;

private:
    WorkflowDag() = default;

    /// Perform Kahn's topological sort, populating topo_order_ and levels_.
    /// Returns false if a cycle is detected.
    bool compute_topological_order();

    std::unordered_map<std::string, DagNode> nodes_;
    std::unordered_map<std::string, std::vector<std::string>> adj_forward_;
    std::unordered_map<std::string, std::vector<std::string>> adj_reverse_;
    std::vector<std::vector<std::string>> levels_;
    std::vector<std::string> topo_order_;
};

/// Create a standalone job DAG (single node, no dependencies).
/// Used to unify standalone jobs with workflow jobs in the pipeline.
[[nodiscard]] inline WorkflowDag make_standalone_dag(
    std::string job_id, std::string job_name,
    std::optional<std::string> condition = std::nullopt)
{
    DagNode node;
    node.job_id = std::move(job_id);
    node.job_name = std::move(job_name);
    node.condition_expr = std::move(condition);
    return WorkflowDag::build({std::move(node)});
}

}  // namespace kairos::engine
