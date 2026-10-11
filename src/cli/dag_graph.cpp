/// src/cli/dag_graph.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  dag_graph.cpp — ASCII DAG graph rendering implementation                ║
// ║  Spec reference: Roadmap §15.10                                         ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/cli/dag_graph.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace kairos::cli {

namespace {

// ANSI color codes.
constexpr const char* kReset   = "\033[0m";
constexpr const char* kBold    = "\033[1m";
constexpr const char* kGreen   = "\033[32m";
constexpr const char* kCyan    = "\033[36m";
constexpr const char* kYellow  = "\033[33m";
constexpr const char* kDim     = "\033[2m";

/// Compute topological levels using Kahn's algorithm.
/// Returns map: node_id → level (0 = root).
std::unordered_map<std::string, int> compute_levels(
    const std::vector<GraphNode>& nodes)
{
    std::unordered_map<std::string, int> in_degree;
    std::unordered_map<std::string, std::vector<std::string>> successors;
    std::unordered_set<std::string> all_ids;

    for (const auto& node : nodes) {
        all_ids.insert(node.id);
        if (in_degree.find(node.id) == in_degree.end()) {
            in_degree[node.id] = 0;
        }
        for (const auto& dep : node.needs) {
            successors[dep].push_back(node.id);
            in_degree[node.id]++;
        }
    }

    // BFS from roots.
    std::queue<std::string> q;
    std::unordered_map<std::string, int> level;
    for (const auto& id : all_ids) {
        if (in_degree[id] == 0) {
            q.push(id);
            level[id] = 0;
        }
    }

    while (!q.empty()) {
        auto current = q.front();
        q.pop();
        for (const auto& succ : successors[current]) {
            level[succ] = std::max(level[succ], level[current] + 1);
            in_degree[succ]--;
            if (in_degree[succ] == 0) {
                q.push(succ);
            }
        }
    }

    return level;
}

/// Build a name-lookup map.
std::unordered_map<std::string, const GraphNode*> build_lookup(
    const std::vector<GraphNode>& nodes)
{
    std::unordered_map<std::string, const GraphNode*> lookup;
    for (const auto& node : nodes) {
        lookup[node.id] = &node;
    }
    return lookup;
}

} // anonymous namespace


std::string render_dag_graph(const std::string& workflow_name,
                             const std::vector<GraphNode>& nodes,
                             bool use_color)
{
    if (nodes.empty()) {
        return workflow_name + "\n  (no jobs)\n";
    }

    const char* r   = use_color ? kReset  : "";
    const char* b   = use_color ? kBold   : "";
    const char* g   = use_color ? kGreen  : "";
    const char* c   = use_color ? kCyan   : "";
    const char* y   = use_color ? kYellow : "";
    const char* dim = use_color ? kDim    : "";

    auto levels = compute_levels(nodes);
    auto lookup = build_lookup(nodes);

    // Sort nodes: by level ascending, then by name alphabetically.
    std::vector<const GraphNode*> sorted;
    sorted.reserve(nodes.size());
    for (const auto& node : nodes) {
        sorted.push_back(&node);
    }
    std::sort(sorted.begin(), sorted.end(),
        [&levels](const GraphNode* a, const GraphNode* b_node) {
            int la = levels.count(a->id) ? levels.at(a->id) : 0;
            int lb = levels.count(b_node->id)
                         ? levels.at(b_node->id) : 0;
            if (la != lb) return la < lb;
            return a->name < b_node->name;
        });

    std::ostringstream out;

    // Workflow name header.
    out << b << workflow_name << r << "\n";

    for (size_t i = 0; i < sorted.size(); ++i) {
        const auto* node = sorted[i];
        bool is_last = (i == sorted.size() - 1);

        // Tree branch character.
        const char* branch = is_last
            ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 "   // └──
            : "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ";  // ├──
        const char* indent = is_last
            ? "    "
            : "\xe2\x94\x82   ";                          // │

        // Job name.
        out << branch << g << node->name << r;

        // Dependencies suffix.
        if (!node->needs.empty()) {
            out << " " << dim << "\xe2\x86\x90" << r << " ";  // ←
            for (size_t j = 0; j < node->needs.size(); ++j) {
                if (j > 0) out << ", ";
                auto it = lookup.find(node->needs[j]);
                if (it != lookup.end()) {
                    out << c << it->second->name << r;
                } else {
                    out << c << node->needs[j] << r;
                }
            }
        }
        out << "\n";

        // Tags sub-line (if any).
        if (!node->tags.empty()) {
            out << indent << y << "[tags: ";
            for (size_t j = 0; j < node->tags.size(); ++j) {
                if (j > 0) out << ", ";
                out << node->tags[j];
            }
            out << "]" << r << "\n";
        }
    }

    return out.str();
}


std::string render_dag_graph_json(const std::string& workflow_name,
                                  const std::vector<GraphNode>& nodes)
{
    auto levels = compute_levels(nodes);

    nlohmann::json result;
    result["workflow"] = workflow_name;
    result["jobs"] = nlohmann::json::array();

    for (const auto& node : nodes) {
        nlohmann::json j;
        j["id"] = node.id;
        j["name"] = node.name;
        j["needs"] = node.needs;
        j["tags"] = node.tags;
        j["topo_level"] = levels.count(node.id)
                              ? levels.at(node.id) : 0;
        result["jobs"].push_back(std::move(j));
    }

    return result.dump(2);
}

} // namespace kairos::cli
