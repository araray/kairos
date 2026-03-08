/// src/engine/dag.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  dag.cpp — Workflow DAG construction and Kahn's topological sort          ║
// ║                                                                           ║
// ║  Determinism guarantee: zero-in-degree nodes are sorted lexicographically ║
// ║  by job_id before processing, ensuring the same DAG always produces the  ║
// ║  same plan regardless of insertion order.                                 ║
// ║                                                                           ║
// ║  Spec reference: §11.2–§11.3                                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/dag.hpp"

#include <algorithm>
#include <functional>
#include <sstream>
#include <unordered_set>

namespace kairos::engine {

WorkflowDag WorkflowDag::build(std::vector<DagNode> node_defs) {
    WorkflowDag dag;

    // ── Step 1: Insert nodes, check for duplicates ──────────────────
    for (auto& node : node_defs) {
        if (dag.nodes_.count(node.job_id)) {
            throw std::runtime_error(
                "Duplicate job ID in workflow DAG: " + node.job_id);
        }
        const auto& id = node.job_id;
        dag.nodes_.emplace(id, std::move(node));
    }

    // ── Step 2: Build adjacency lists, validate needs ───────────────
    for (const auto& [id, node] : dag.nodes_) {
        for (const auto& dep : node.needs) {
            if (!dag.nodes_.count(dep)) {
                throw std::runtime_error(
                    "Job '" + id + "' has unresolved dependency: " + dep);
            }
            dag.adj_forward_[dep].push_back(id);
            dag.adj_reverse_[id].push_back(dep);
        }
    }

    // ── Step 3: Compute topological order ────────────────────────────
    if (!dag.compute_topological_order()) {
        // Cycle detected — find and report it.
        auto cycle = dag.find_cycle();
        std::string cycle_str = "unknown";
        if (cycle) {
            std::ostringstream oss;
            for (size_t i = 0; i < cycle->size(); ++i) {
                if (i > 0) oss << " → ";
                oss << (*cycle)[i];
            }
            cycle_str = oss.str();
        }
        throw std::runtime_error(
            "Cycle detected in workflow DAG: " + cycle_str);
    }

    return dag;
}

bool WorkflowDag::compute_topological_order() {
    // Kahn's algorithm with level tracking.
    // Working copy of in-degrees.
    std::unordered_map<std::string, int> in_degree;
    for (const auto& [id, node] : nodes_) {
        in_degree[id] = static_cast<int>(node.needs.size());
    }

    // Collect zero-in-degree nodes, sorted for determinism.
    std::vector<std::string> ready;
    for (const auto& [id, deg] : in_degree) {
        if (deg == 0) {
            ready.push_back(id);
        }
    }
    // Sort lexicographically by job_id for deterministic ordering.
    std::sort(ready.begin(), ready.end());

    topo_order_.clear();
    topo_order_.reserve(nodes_.size());
    levels_.clear();

    // Use a deque as a FIFO queue, but re-sort each level.
    while (!ready.empty()) {
        int current_level = static_cast<int>(levels_.size());
        std::vector<std::string> level_nodes = std::move(ready);
        ready.clear();

        // Record this level.
        for (const auto& id : level_nodes) {
            topo_order_.push_back(id);
            nodes_.at(id).topo_level = current_level;
            nodes_.at(id).in_degree = 0;

            // Decrement in-degree of all successors.
            auto fwd_it = adj_forward_.find(id);
            if (fwd_it != adj_forward_.end()) {
                for (const auto& succ : fwd_it->second) {
                    if (--in_degree[succ] == 0) {
                        ready.push_back(succ);
                    }
                }
            }
        }

        // Sort next level for determinism.
        std::sort(ready.begin(), ready.end());

        levels_.push_back(std::move(level_nodes));
    }

    // If we didn't visit all nodes, there's a cycle.
    return topo_order_.size() == nodes_.size();
}

std::optional<std::vector<std::string>> WorkflowDag::find_cycle() const {
    // DFS-based cycle detection with path tracking.
    enum class Color { White, Gray, Black };
    std::unordered_map<std::string, Color> color;
    for (const auto& [id, _] : nodes_) {
        color[id] = Color::White;
    }

    std::vector<std::string> path;

    std::function<bool(const std::string&)> dfs =
        [&](const std::string& u) -> bool
    {
        color[u] = Color::Gray;
        path.push_back(u);

        auto fwd_it = adj_forward_.find(u);
        if (fwd_it != adj_forward_.end()) {
            for (const auto& v : fwd_it->second) {
                if (color[v] == Color::Gray) {
                    // Found cycle: extract the cycle portion of path.
                    auto it = std::find(path.begin(), path.end(), v);
                    std::vector<std::string> cycle(it, path.end());
                    cycle.push_back(v);  // Close the cycle.
                    path = std::move(cycle);
                    return true;
                }
                if (color[v] == Color::White && dfs(v)) {
                    return true;
                }
            }
        }

        path.pop_back();
        color[u] = Color::Black;
        return false;
    };

    for (const auto& [id, _] : nodes_) {
        if (color[id] == Color::White) {
            if (dfs(id)) {
                return path;
            }
        }
    }

    return std::nullopt;  // No cycle.
}

}  // namespace kairos::engine
