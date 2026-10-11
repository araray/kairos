/// include/kairos/cli/dag_graph.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/cli/dag_graph.hpp — ASCII DAG graph rendering                    ║
// ║                                                                          ║
// ║  Renders a workflow's job dependency graph as an ASCII tree for          ║
// ║  `kairos workflows graph <name>`.                                       ║
// ║                                                                          ║
// ║  Spec reference: Roadmap §15.10                                         ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <string>
#include <vector>

namespace kairos::cli {

/// A node in a simplified DAG for rendering.
struct GraphNode {
    std::string id;
    std::string name;
    std::vector<std::string> needs;  ///< IDs of predecessor jobs.
    std::vector<std::string> tags;   ///< Tags for display.
};

/// Render an ASCII-art DAG graph.
///
/// Example output:
/// ```
/// deploy_pipeline
/// ├── setup
/// ├── build ← setup
/// ├── test ← build
/// │   └── [tags: test, ci]
/// └── deploy ← test
///     └── [tags: deploy, production]
/// ```
///
/// @param workflow_name  Human-readable workflow name.
/// @param nodes          DAG nodes (jobs within the workflow).
/// @param use_color      Whether to emit ANSI color codes.
/// @return Multi-line string of the rendered graph.
std::string render_dag_graph(const std::string& workflow_name,
                             const std::vector<GraphNode>& nodes,
                             bool use_color = true);

/// Render as JSON (for --json mode).
std::string render_dag_graph_json(const std::string& workflow_name,
                                  const std::vector<GraphNode>& nodes);

} // namespace kairos::cli
