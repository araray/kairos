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

    // ── FIX: ceil sub-second precision to next whole second ───────
    //
    // system_clock has sub-second precision (typically nanoseconds)
    // but time_t is seconds-only.  std::chrono::system_clock::to_time_t
    // truncates (floors) the sub-second component.
    //
    // This causes a critical bug in the scheduler loop:
    //   1. Cron fires at wall time 23:55:00.300 (sub-second past match)
    //   2. Scheduler calls reschedule → next_fire_after(23:55:00.300)
    //   3. to_time_t truncates to 23:55:00 (the exact cron match!)
    //   4. croncpp's cron_next(23:55:00) returns 23:55:00 again
    //      (same second — not strictly after)
    //   5. from_time_t(23:55:00) → next_fire_wall = 23:55:00.000
    //   6. time_until_fire = 23:55:00.000 - 23:55:00.300 = negative
    //   7. Scheduler fires AGAIN immediately → spin loop until the
    //      wall clock crosses the second boundary (~700ms of spinning)
    //
    // Fix: if the input has any sub-second component, advance time_t
    // by 1 second so croncpp evaluates from the next whole second.
    // This guarantees the returned time_point is strictly > after.
    auto after_floor = std::chrono::system_clock::from_time_t(tt);
    if (after > after_floor) {
        tt += 1;  // Ceil to next whole second.
    }

    // Apply timezone delta: shift time_t so that when croncpp calls
    // localtime_r internally, the resulting civil-time fields correspond
    // to the configured timezone (kairos.timezone), not the system timezone.
    //
    // For "local" mode, cron_tz_delta_s == 0 → no change (croncpp uses
    // system local time, which is what the user expects).
    // For "+05:30" on a UTC system, delta == +19800 → croncpp sees IST fields.
    tt += cron_tz_delta_s;

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

        // Reverse the delta to get back to true UTC.
        next_tt -= cron_tz_delta_s;

        return std::chrono::system_clock::from_time_t(next_tt);
    } catch (const cron::bad_cronexpr& e) {
        throw std::runtime_error(
            "Invalid cron expression '" + expression + "': " + e.what());
    }
}

}  // namespace kairos::engine
