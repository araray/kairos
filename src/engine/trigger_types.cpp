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
        auto parsed = cron::make_cron(expression);
        auto next_tt = cron::cron_next(parsed, tt);
        return std::chrono::system_clock::from_time_t(next_tt);
    } catch (const cron::bad_cronexpr& e) {
        throw std::runtime_error(
            "Invalid cron expression '" + expression + "': " + e.what());
    }
}

}  // namespace kairos::engine
