/// src/http/dag_svg.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Server-rendered DAG SVG — see include/kairos/http/dag_svg.hpp           ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef KAIROS_HTTP_ENABLED

#include "kairos/http/dag_svg.hpp"

#include <sstream>

namespace kairos::http::dag {
namespace {

constexpr int kNodeW = 150;
constexpr int kNodeH = 46;
constexpr int kColGap = 70;
constexpr int kRowGap = 18;
constexpr int kPad = 12;
constexpr int kLabelH = 22;

std::string xml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;
        }
    }
    return out;
}

/// CSS class for a status value ("dag-" prefix handled in kairos.css).
std::string status_class(const NodeStatus* st) {
    if (!st) return "";
    if (st->status == "SUCCESS") return " dag-success";
    if (st->status == "FAILURE") return " dag-failure";
    if (st->status == "RUNNING") return " dag-running";
    if (st->status == "CANCELLED") return " dag-cancelled";
    if (st->status == "SKIPPED" || st->status == "PENDING")
        return " dag-muted";
    return "";
}

}  // namespace

std::string render_dag_svg(
    const engine::WorkflowDag& dag,
    const std::string& job_link_base,
    const std::optional<std::map<std::string, NodeStatus>>& statuses,
    int max_width_px)
{
    const int level_count = dag.level_count() > 0 ? dag.level_count() : 1;

    // Layout pass: position every node in a level×row grid.
    struct Pos { double x = 0, y = 0; };
    std::map<std::string, Pos> pos;
    std::map<std::string, std::string> names;
    std::vector<std::vector<std::string>> by_level(level_count);

    for (int lvl = 0; lvl < level_count; ++lvl) {
        for (const auto& job_id : dag.jobs_at_level(lvl)) {
            by_level[lvl].push_back(job_id);
            names[job_id] = dag.node(job_id).job_name;
        }
    }

    int max_rows = 1;
    for (int lvl = 0; lvl < level_count; ++lvl) {
        max_rows = std::max(max_rows,
                            static_cast<int>(by_level[lvl].size()));
        for (size_t row = 0; row < by_level[lvl].size(); ++row) {
            const auto& job_id = by_level[lvl][row];
            pos[job_id].x = kPad + lvl * (kNodeW + kColGap);
            // Center the column vertically against the tallest level.
            const double col_h =
                static_cast<double>(by_level[lvl].size()) *
                (kNodeH + kRowGap) - kRowGap;
            const double total_h =
                static_cast<double>(max_rows) * (kNodeH + kRowGap) - kRowGap;
            pos[job_id].y = kPad + kLabelH + (total_h - col_h) / 2.0 +
                            static_cast<double>(row) * (kNodeH + kRowGap);
        }
    }

    const double width = kPad * 2.0 +
        level_count * kNodeW + (level_count - 1) * kColGap;
    const double height = kPad * 2.0 + kLabelH +
        max_rows * kNodeH + (max_rows - 1) * kRowGap;

    std::ostringstream svg;
    svg << "<svg class=\"dag-svg\" viewBox=\"0 0 "
        << static_cast<int>(width) << " " << static_cast<int>(height)
        << "\" xmlns=\"http://www.w3.org/2000/svg\" role=\"img\""
        << " aria-label=\"workflow dag\" style=\"max-width:"
        << max_width_px << "px\">\n";

    // Level labels.
    for (int lvl = 0; lvl < level_count; ++lvl) {
        svg << "<text class=\"dag-level-label\" x=\""
            << static_cast<int>(kPad + lvl * (kNodeW + kColGap) + kNodeW / 2.0)
            << "\" y=\"" << (kPad + 12) << "\" text-anchor=\"middle\">L"
            << lvl << "</text>\n";
    }

    // Edges first (under the nodes).
    for (int lvl = 0; lvl < level_count; ++lvl) {
        for (const auto& job_id : by_level[lvl]) {
            const auto& p = pos[job_id];
            for (const auto& need : dag.node(job_id).needs) {
                auto it = pos.find(need);
                if (it == pos.end()) continue;
                const auto& q = it->second;
                const double x1 = q.x + kNodeW;
                const double y1 = q.y + kNodeH / 2.0;
                const double x2 = p.x;
                const double y2 = p.y + kNodeH / 2.0;
                const double mx = (x1 + x2) / 2.0;
                svg << "<path class=\"dag-edge\" d=\"M " << x1 << " " << y1
                    << " C " << mx << " " << y1 << ", " << mx << " " << y2
                    << ", " << x2 << " " << y2 << "\" />\n";
            }
        }
    }

    // Nodes.
    for (int lvl = 0; lvl < level_count; ++lvl) {
        for (const auto& job_id : by_level[lvl]) {
            const auto& p = pos[job_id];
            const NodeStatus* st = nullptr;
            if (statuses) {
                auto sit = statuses->find(job_id);
                if (sit != statuses->end()) st = &sit->second;
            }

            if (!job_link_base.empty()) {
                svg << "<a href=\"" << job_link_base << "/"
                    << xml_escape(job_id) << "\">";
            }
            svg << "<g class=\"dag-node" << status_class(st)
                << "\">";
            svg << "<rect x=\"" << static_cast<int>(p.x) << "\" y=\""
                << static_cast<int>(p.y) << "\" width=\"" << kNodeW
                << "\" height=\"" << kNodeH << "\" rx=\"8\" />";
            svg << "<text x=\"" << static_cast<int>(p.x + kNodeW / 2.0)
                << "\" y=\"" << static_cast<int>(p.y + 19)
                << "\" text-anchor=\"middle\" font-size=\"13\">"
                << xml_escape(names[job_id]) << "</text>";

            std::string sub;
            if (st) {
                sub = st->status;
                if (st->duration_ms >= 0) {
                    sub += " · " + std::to_string(st->duration_ms) + "ms";
                }
            }
            if (sub.empty()) sub = "level " + std::to_string(lvl);
            svg << "<text class=\"dag-sub\" x=\""
                << static_cast<int>(p.x + kNodeW / 2.0) << "\" y=\""
                << static_cast<int>(p.y + 35)
                << "\" text-anchor=\"middle\" font-size=\"11\">"
                << xml_escape(sub) << "</text>";
            svg << "</g>";
            if (!job_link_base.empty()) svg << "</a>";
            svg << "\n";
        }
    }

    svg << "</svg>";
    return svg.str();
}

}  // namespace kairos::http::dag

#endif  // KAIROS_HTTP_ENABLED
