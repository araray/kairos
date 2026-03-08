/// src/engine/execution_plan.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  execution_plan.cpp — ExecutionPlan JSON rendering                        ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/execution_plan.hpp"

#include <nlohmann/json.hpp>

namespace kairos::engine {

std::string ExecutionPlan::render_json() const {
    nlohmann::json j;
    j["workflow_id"]   = workflow_id;
    j["workflow_name"] = workflow_name;
    j["trigger_type"]  = trigger_type;

    j["summary"] = {
        {"jobs_to_run",  jobs_to_run()},
        {"jobs_to_skip", jobs_to_skip()},
        {"jobs_pending", jobs_pending()},
        {"max_parallelism", max_parallelism()},
    };

    auto& arr = j["entries"] = nlohmann::json::array();
    for (const auto& e : entries) {
        nlohmann::json entry;
        entry["job_id"]   = e.job_id;
        entry["job_name"] = e.job_name;
        entry["level"]    = e.level;
        entry["action"]   = std::string(plan_action_to_string(e.action));
        entry["reason"]   = e.reason;
        if (!e.condition_expr.empty()) {
            entry["condition_expr"]   = e.condition_expr;
            entry["condition_result"] = e.condition_result;
        }
        if (!e.needs.empty()) {
            entry["needs"] = e.needs;
        }
        arr.push_back(std::move(entry));
    }

    return j.dump(2);
}

}  // namespace kairos::engine
