/// include/kairos/http/dag_svg.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/http/dag_svg.hpp — server-rendered workflow DAG visualization     ║
// ║                                                                          ║
// ║  Renders a WorkflowDag as a layered inline SVG: topological levels become ║
// ║  columns, needs become edges, nodes link to their job pages. Status      ║
// ║  coloring (optional) uses the page's theme CSS variables, so the graph   ║
// ║  follows the blue/dark/light theme with zero client assets (§20.1).      ║
// ║                                                                          ║
// ║  Roadmap §3.7 — chosen over CDN dagre-d3/ELK to stay fully offline.      ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#ifdef KAIROS_HTTP_ENABLED

#include "kairos/engine/workflow_registry.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace kairos::http::dag {

/// Node status for coloring; nullopt = neutral (definition view).
struct NodeStatus {
    std::string status;   ///< SUCCESS | FAILURE | RUNNING | CANCELLED | …
    int64_t duration_ms = -1;
};

/// Render the DAG as inline SVG.
///
/// @param dag            The workflow DAG (levels + needs).
/// @param job_link_base  URL prefix for node links ("" = no links);
///                       node id is appended ("/jobs/<id>").
/// @param statuses       Optional per-job-id status for coloring.
/// @param max_width_px   Nominal viewport width (SVG scales responsively).
[[nodiscard]] std::string render_dag_svg(
    const engine::WorkflowDag& dag,
    const std::string& job_link_base = "",
    const std::optional<std::map<std::string, NodeStatus>>& statuses =
        std::nullopt,
    int max_width_px = 900);

}  // namespace kairos::http::dag

#endif  // KAIROS_HTTP_ENABLED
