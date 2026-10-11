/// src/engine/scheduler.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Scheduler engine implementation                                          ║
// ║  Spec reference: §10.8–§10.9                                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/scheduler.hpp"
#include "kairos/core/id_generator.hpp"

#include <spdlog/spdlog.h>

namespace kairos::engine {

Scheduler::Scheduler(SchedulerConfig config, Dependencies deps)
    : config_(std::move(config)), deps_(std::move(deps))
{
    if (!deps_.clock) {
        throw std::invalid_argument("Scheduler requires a non-null ClockSource");
    }
}

Scheduler::~Scheduler() { stop(); }

void Scheduler::start(std::stop_token stop, TriggerSink sink) {
    thread_ = std::jthread([this, stop, sink = std::move(sink)](
                               std::stop_token) mutable {
        run(stop, std::move(sink));
    });
}

void Scheduler::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        deps_.clock->wake();
        thread_.join();
    }
}

void Scheduler::run(std::stop_token stop, TriggerSink sink) {
    spdlog::info("Scheduler starting");

    // Phase 1: Build initial heap.
    build_heap();
    spdlog::info("Scheduler loaded {} triggers", heap_.size());

    // Phase 2: Resolve misfires.
    auto misfires = resolve_misfires();
    for (auto& evt : misfires) {
        spdlog::info("Scheduler emitting misfire for trigger '{}' -> target '{}'",
                     evt.trigger_id, evt.target_id);
        sink(std::move(evt));
    }

    // Phase 3: Main loop.
    while (!stop.stop_requested()) {
        // Check for reload.
        if (reload_requested_.exchange(false)) {
            {
                std::lock_guard lock(reload_mu_);
                if (pending_registry_) {
                    deps_.registry = std::move(pending_registry_);
                }
            }
            build_heap();
            spdlog::info("Scheduler reloaded: {} triggers", heap_.size());
            continue;
        }

        if (heap_.empty()) {
            // No timers — sleep until woken.
            auto far_future = deps_.clock->steady_now() + std::chrono::hours(24);
            deps_.clock->sleep_until(far_future);
            continue;
        }

        // Peek at top entry.
        auto top = heap_.top();
        auto now_wall = deps_.clock->now();
        auto now_mono = deps_.clock->steady_now();
        auto wait_ms = top.time_until_fire(now_wall, now_mono);

        if (wait_ms <= std::chrono::milliseconds::zero()) {
            // Timer fired!
            heap_.pop();

            // Max-instances check.
            if (deps_.active_runs &&
                !deps_.active_runs->can_fire(top.target_id, top.max_instances)) {
                spdlog::warn("Skipping fire for '{}': max instances ({}) reached",
                             top.target_id, top.max_instances);
                if (reschedule(top)) heap_.push(std::move(top));
                continue;
            }

            // Determine trigger type and expression strings.
            auto type_str = trigger_spec_type_name(top.spec);
            std::string expr_str;
            if (auto* cron = std::get_if<CronTrigger>(&top.spec)) {
                expr_str = cron->expression;
            } else if (auto* intv = std::get_if<IntervalTrigger>(&top.spec)) {
                expr_str = std::to_string(intv->interval.count()) + "ms";
            }

            // Emit TriggerEvent.
            auto correlation_id = core::generate_correlation_id();
            auto evt = TriggerEvent::make_schedule_tick(
                top.trigger_id, top.target_id, top.target_kind,
                now_wall, std::move(correlation_id),
                ScheduleTickPayload{
                    .trigger_type = type_str,
                    .expression = expr_str,
                    .is_misfire = false,
                });

            spdlog::debug("Scheduler firing trigger '{}' -> target '{}' ({})",
                          top.trigger_id, top.target_id, type_str);
            sink(std::move(evt));

            // Persist fire time.
            persist_fire(top.trigger_id, now_wall);

            // Re-schedule.
            if (reschedule(top)) heap_.push(std::move(top));
        } else {
            // Sleep until next fire time.
            auto sleep_target = now_mono + wait_ms;
            deps_.clock->sleep_until(sleep_target);
        }
    }

    spdlog::info("Scheduler stopping");
}

void Scheduler::request_reload(
    std::shared_ptr<const WorkflowRegistry> new_registry)
{
    {
        std::lock_guard lock(reload_mu_);
        pending_registry_ = std::move(new_registry);
    }
    reload_requested_.store(true);
    deps_.clock->wake();
}

void Scheduler::build_heap() {
    heap_ = TimerHeap{};
    if (!deps_.registry) return;

    for (auto entry : deps_.registry->triggers()) {
        if (!entry.enabled) continue;
        if (auto* dt = std::get_if<DateTrigger>(&entry.spec)) {
            if (dt->is_exhausted()) continue;
        }
        compute_next_fire(entry);
        heap_.push(std::move(entry));
    }
}

void Scheduler::compute_next_fire(TimerEntry& entry) {
    auto now_wall = deps_.clock->now();
    auto now_mono = deps_.clock->steady_now();

    std::visit([&](auto& t) {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, CronTrigger>) {
            entry.next_fire_wall = t.next_fire_after(now_wall);
            // Defense-in-depth: if next_fire is not strictly in the
            // future (should not happen with the sub-second ceil fix
            // in CronTrigger::next_fire_after, but guard anyway),
            // advance by 1 second and retry.
            if (entry.next_fire_wall <= now_wall) {
                spdlog::warn("Scheduler: cron next_fire not in future for "
                             "'{}', advancing 1s and retrying",
                             t.expression);
                entry.next_fire_wall = t.next_fire_after(
                    now_wall + std::chrono::seconds(1));
            }
            auto wall_delta = entry.next_fire_wall - now_wall;
            entry.next_fire_mono = now_mono +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(wall_delta);
        } else if constexpr (std::is_same_v<T, IntervalTrigger>) {
            entry.next_fire_mono = t.next_fire_after(now_mono);
            auto mono_delta = entry.next_fire_mono - now_mono;
            entry.next_fire_wall = now_wall +
                std::chrono::duration_cast<std::chrono::system_clock::duration>(mono_delta);
        } else {
            entry.next_fire_wall = t.fire_at;
            auto wall_delta = t.fire_at - now_wall;
            entry.next_fire_mono = now_mono +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(wall_delta);
        }
    }, entry.spec);
}

bool Scheduler::reschedule(TimerEntry& entry) {
    auto now_wall = deps_.clock->now();
    auto now_mono = deps_.clock->steady_now();

    return std::visit([&](auto& t) -> bool {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, CronTrigger>) {
            entry.next_fire_wall = t.next_fire_after(now_wall);
            // Defense-in-depth: guarantee forward progress.
            // Without this, a bug in next_fire_after could cause
            // an infinite spin loop in the scheduler.
            if (entry.next_fire_wall <= now_wall) {
                spdlog::warn("Scheduler: reschedule cron not in future for "
                             "'{}', advancing 1s and retrying",
                             t.expression);
                entry.next_fire_wall = t.next_fire_after(
                    now_wall + std::chrono::seconds(1));
            }
            auto wall_delta = entry.next_fire_wall - now_wall;
            entry.next_fire_mono = now_mono +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(wall_delta);
            return true;
        } else if constexpr (std::is_same_v<T, IntervalTrigger>) {
            entry.next_fire_mono = t.next_fire_after(now_mono);
            auto mono_delta = entry.next_fire_mono - now_mono;
            entry.next_fire_wall = now_wall +
                std::chrono::duration_cast<std::chrono::system_clock::duration>(mono_delta);
            return true;
        } else {
            t.fired = true;
            return false;
        }
    }, entry.spec);
}

std::vector<TriggerEvent> Scheduler::resolve_misfires() {
    // Misfire detection requires DB query for last_fire_at.
    // Deferred until QueryReader gains trigger_state support.
    return {};
}

void Scheduler::persist_fire(const std::string& trigger_id,
                              ClockSource::time_point fire_time)
{
    if (!deps_.db_writer) return;

    auto time_t = std::chrono::system_clock::to_time_t(fire_time);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &time_t);
#else
    gmtime_r(&time_t, &tm_buf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

    deps_.db_writer->enqueue(persist::InsertTriggerHistory{
        .trigger_id = trigger_id,
        .trigger_type = "schedule",
        .target_id = "",
        .fired_at = std::string(buf),
        .status = "fired",
        .run_id = "",
    });
}

std::vector<TimerSnapshot> Scheduler::snapshot() const {
    std::vector<TimerSnapshot> result;
    auto heap_copy = heap_;
    auto now_wall = deps_.clock->now();
    auto now_mono = deps_.clock->steady_now();

    while (!heap_copy.empty()) {
        const auto& entry = heap_copy.top();

        TimerSnapshot snap;
        snap.trigger_id = entry.trigger_id;
        snap.target_id = entry.target_id;
        snap.target_name = entry.target_name;
        snap.trigger_type = trigger_spec_type_name(entry.spec);
        snap.ms_until_fire = entry.time_until_fire(now_wall, now_mono).count();
        snap.max_instances = entry.max_instances;
        snap.enabled = entry.enabled;

        auto tt = std::chrono::system_clock::to_time_t(entry.next_fire_wall);
        std::tm tm_buf{};
#ifdef _WIN32
        gmtime_s(&tm_buf, &tt);
#else
        gmtime_r(&tt, &tm_buf);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
        snap.next_fire = std::string(buf);

        snap.expression = std::visit([](const auto& t) -> std::string {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, CronTrigger>) return t.expression;
            else if constexpr (std::is_same_v<T, IntervalTrigger>)
                return std::to_string(t.interval.count()) + "ms";
            else return "one-shot";
        }, entry.spec);

        result.push_back(std::move(snap));
        heap_copy.pop();
    }
    return result;
}

size_t Scheduler::timer_count() const { return heap_.size(); }

}  // namespace kairos::engine
