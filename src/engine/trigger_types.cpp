/// src/engine/trigger_types.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  trigger_types.cpp — Schedule trigger type implementations                ║
// ║                                                                           ║
// ║  Provides CronTrigger::next_fire_after using croncpp library.            ║
// ║                                                                           ║
// ║  Spec reference: §10.3.1 (cron triggers)                                ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/trigger_types.hpp"

#include <croncpp.h>

#include <ctime>
#include <stdexcept>

namespace kairos::engine {

SystemTimePoint CronTrigger::next_fire_after(
    SystemTimePoint after) const
{
    // Convert system_clock time_point to std::time_t for croncpp.
    auto tt = std::chrono::system_clock::to_time_t(after);

    // croncpp expects local time via std::tm.
    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &tt);
#else
    localtime_r(&tt, &local_tm);
#endif

    try {
        // croncpp requires 6-field cron (with seconds). Standard cron
        // is 5-field (min hour dom month dow). Auto-detect and prepend
        // "0 " (seconds=0) for 5-field expressions per spec §10.3.1.
        std::string expr_6field = expression;
        {
            int field_count = 1;
            bool in_space = false;
            for (char c : expression) {
                if (c == ' ' || c == '\t') {
                    if (!in_space) { ++field_count; in_space = true; }
                } else {
                    in_space = false;
                }
            }
            if (field_count == 5) {
                expr_6field = "0 " + expression;
            }
        }

        auto parsed = cron::make_cron(expr_6field);
        auto next_tt = cron::cron_next(parsed, tt);
        return std::chrono::system_clock::from_time_t(next_tt);
    } catch (const cron::bad_cronexpr& e) {
        throw std::runtime_error(
            "Invalid cron expression '" + expression + "': " + e.what());
    }
}

}  // namespace kairos::engine
