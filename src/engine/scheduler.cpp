/// src/engine/scheduler.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Scheduler engine implementation                                          ║
// ║  Spec reference: §10.8–§10.9                                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/scheduler.hpp"
#include "kairos/core/id_generator.hpp"
#include "kairos/engine/villa_integration.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <tuple>
#include <unordered_map>

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

    // Phase 1: Build initial heap (engine start → new timer epochs).
    build_heap(/*initial=*/true);
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
            build_heap(/*initial=*/false);
            spdlog::info("Scheduler reloaded: {} triggers (registry {})",
                         heap_.size(),
                         deps_.registry ? deps_.registry->registry_digest()
                                        : std::string("-"));
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
            const auto scheduled_for =
                villa::format_rfc3339_ms(top.next_fire_wall);

            auto requeue = [this](TimerEntry& e) {
                if (reschedule(e)) {
                    heap_.push(std::move(e));
                } else {
                    consumed_.insert(e.continuity_key);
                }
                publish_snapshot();
            };

            // Max-instances check.
            if (deps_.active_runs &&
                !deps_.active_runs->can_fire(top.target_id, top.max_instances)) {
                spdlog::warn("Skipping fire for '{}': max instances ({}) reached",
                             top.target_id, top.max_instances);
                record_occurrence(top, scheduled_for,
                                  villa::kDispositionSkipped);
                requeue(top);
                continue;
            }

            // Villa integration v1: persist the occurrence BEFORE the
            // trigger enters the execution queue. A persistence failure
            // fails closed — the occurrence is not delivered.
            std::string occurrence_id;
            const std::string applied_digest = deps_.registry
                ? deps_.registry->registry_digest()
                : villa::registry_digest({});
            if (deps_.occurrences) {
                try {
                    auto rec = deps_.occurrences->record(persist::NewOccurrence{
                        .workflow_name = top.target_name,
                        .schedule_id = top.schedule_id,
                        .trigger_id = top.trigger_id,
                        .registry_digest = applied_digest,
                        .scheduled_for = scheduled_for,
                        .disposition = std::string(villa::kDispositionEmitted),
                    });
                    occurrence_id = rec.occurrence_id;
                } catch (const std::exception& e) {
                    spdlog::error("Scheduler: cannot persist occurrence for "
                                  "'{}' due {}: {} — not delivering",
                                  top.target_name, scheduled_for, e.what());
                    requeue(top);
                    continue;
                }
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
                    .occurrence_id = occurrence_id,
                    .scheduled_for = scheduled_for,
                    .registry_digest = applied_digest,
                });

            spdlog::debug("Scheduler firing trigger '{}' -> target '{}' ({})",
                          top.trigger_id, top.target_id, type_str);
            bool delivered = sink(std::move(evt));
            if (!delivered && !occurrence_id.empty()) {
                spdlog::warn("Scheduler: trigger bus rejected occurrence {} "
                             "for '{}' — marking abandoned",
                             occurrence_id, top.target_name);
                try {
                    deps_.occurrences->set_disposition(
                        occurrence_id,
                        std::string(villa::kDispositionAbandoned));
                } catch (const std::exception& e) {
                    spdlog::error("Scheduler: cannot mark occurrence {} "
                                  "abandoned: {}", occurrence_id, e.what());
                }
            }

            // Persist fire time.
            persist_fire(top.trigger_id, now_wall);

            // Re-schedule.
            requeue(top);
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

void Scheduler::build_heap(bool initial) {
    // ── Carry live timer state by continuity key (reload only) ───────
    struct Carry {
        SystemTimePoint wall;
        SteadyTimePoint mono;
        std::string epoch;
    };
    std::unordered_map<std::string, Carry> carry;
    if (!initial) {
        auto old = heap_;
        while (!old.empty()) {
            const auto& e = old.top();
            if (!e.continuity_key.empty()) {
                carry[e.continuity_key] =
                    Carry{e.next_fire_wall, e.next_fire_mono, e.timer_epoch};
            }
            old.pop();
        }
    }
    auto old_consumed = std::move(consumed_);
    consumed_.clear();
    if (initial) old_consumed.clear();

    heap_ = TimerHeap{};
    if (!deps_.registry) {
        publish_snapshot();
        return;
    }

    // Highest registered generation per Villa schedule. Lower generations
    // of the same schedule are superseded: their timer state transfers to
    // the newest generation (spec §7.2.3 item 7) instead of double-firing.
    std::unordered_map<std::string, int64_t> max_gen;
    for (const auto* wf : deps_.registry->workflows()) {
        if (auto id = villa::parse_schedule_name(wf->workflow_name)) {
            auto& g = max_gen[id->schedule_id];
            g = std::max(g, id->generation);
        }
    }

    std::unordered_map<std::string, int> ordinals;
    size_t preserved = 0, fresh = 0;
    for (auto entry : deps_.registry->triggers()) {
        if (!entry.enabled) continue;
        if (auto* dt = std::get_if<DateTrigger>(&entry.spec)) {
            if (dt->is_exhausted()) continue;
        }

        entry.trigger_digest = villa::trigger_digest(entry.spec);
        if (auto id = villa::parse_schedule_name(entry.target_name)) {
            entry.schedule_id = id->schedule_id;
            entry.schedule_generation = id->generation;
            if (id->generation < max_gen[id->schedule_id]) {
                spdlog::debug("Scheduler: '{}' superseded by generation {}",
                              entry.target_name, max_gen[id->schedule_id]);
                continue;
            }
        }

        // Continuity key: stable Villa schedule identity when available,
        // otherwise the trigger id; always qualified by the trigger
        // digest (and the cron timezone delta, which changes evaluation).
        std::string base = entry.schedule_id.empty()
            ? "T|" + entry.trigger_id
            : "S|" + entry.schedule_id;
        base += "|" + entry.trigger_digest;
        if (auto* cron = std::get_if<CronTrigger>(&entry.spec)) {
            base += "|tz" + std::to_string(cron->cron_tz_delta_s);
        }
        entry.continuity_key = base + "#" + std::to_string(ordinals[base]++);

        if (old_consumed.count(entry.continuity_key)) {
            consumed_.insert(entry.continuity_key);
            continue;
        }

        if (auto it = carry.find(entry.continuity_key); it != carry.end()) {
            entry.next_fire_wall = it->second.wall;
            entry.next_fire_mono = it->second.mono;
            entry.timer_epoch = it->second.epoch;
            ++preserved;
            heap_.push(std::move(entry));
            continue;
        }

        // New timer epoch (engine start, changed trigger, or a timer that
        // was absent and came back): initial semantics.
        entry.timer_epoch = villa::random_id("te_");
        compute_next_fire(entry);

        if (auto* dt = std::get_if<DateTrigger>(&entry.spec)) {
            auto due = villa::format_rfc3339_ms(dt->fire_at);
            if (deps_.occurrences) {
                try {
                    if (deps_.occurrences->exists_for(
                            entry.target_name, entry.schedule_id, due)) {
                        consumed_.insert(entry.continuity_key);
                        continue;  // Already emitted/skipped once.
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("Scheduler: occurrence lookup failed: {}",
                                 e.what());
                }
            }
            auto now = deps_.clock->now();
            // Skip-only misfire policy: a one-shot whose instant passed
            // while the engine was down is skipped (recorded once). A
            // freshly registered one-shot within the grace window still
            // fires (covers registration/reload latency).
            if (dt->fire_at < now &&
                (initial || now - dt->fire_at > config_.misfire_grace)) {
                record_occurrence(entry, due, villa::kDispositionSkipped);
                consumed_.insert(entry.continuity_key);
                continue;
            }
        }
        ++fresh;
        heap_.push(std::move(entry));
    }
    if (!initial) {
        spdlog::debug("Scheduler: {} timer(s) preserved, {} new epoch(s)",
                      preserved, fresh);
    }
    publish_snapshot();
}

void Scheduler::record_occurrence(const TimerEntry& entry,
                                  const std::string& scheduled_for,
                                  std::string_view disposition) {
    if (!deps_.occurrences) return;
    try {
        deps_.occurrences->record(persist::NewOccurrence{
            .workflow_name = entry.target_name,
            .schedule_id = entry.schedule_id,
            .trigger_id = entry.trigger_id,
            .registry_digest = deps_.registry
                ? deps_.registry->registry_digest()
                : villa::registry_digest({}),
            .scheduled_for = scheduled_for,
            .disposition = std::string(disposition),
        });
    } catch (const std::exception& e) {
        spdlog::error("Scheduler: cannot record {} occurrence for '{}': {}",
                      disposition, entry.target_name, e.what());
    }
}

void Scheduler::publish_snapshot() {
    IntegrationSnapshot snap;
    snap.applied = true;
    snap.applied_digest = deps_.registry ? deps_.registry->registry_digest()
                                         : villa::registry_digest({});
    auto copy = heap_;
    while (!copy.empty()) {
        const auto& e = copy.top();
        snap.timers.push_back(IntegrationTimer{
            .workflow = e.target_name,
            .trigger_id = e.trigger_id,
            .trigger_type = trigger_spec_type_name(e.spec),
            .schedule_id = e.schedule_id,
            .schedule_generation = e.schedule_generation,
            .trigger_digest = e.trigger_digest,
            .timer_epoch = e.timer_epoch,
            .next_due_at = villa::format_rfc3339_ms(e.next_fire_wall),
        });
        copy.pop();
    }
    std::sort(snap.timers.begin(), snap.timers.end(),
        [](const IntegrationTimer& a, const IntegrationTimer& b) {
            return std::tie(a.workflow, a.trigger_id) <
                   std::tie(b.workflow, b.trigger_id);
        });
    std::lock_guard lock(snap_mu_);
    snap_ = std::move(snap);
}

IntegrationSnapshot Scheduler::integration_snapshot() const {
    std::lock_guard lock(snap_mu_);
    return snap_;
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
            // Phase-preserving: next deadline = previous deadline +
            // interval; slots already in the past are skipped (never
            // replayed), so processing latency does not drift the phase.
            if (t.interval <= std::chrono::milliseconds::zero()) {
                entry.next_fire_mono = t.next_fire_after(now_mono);
            } else if (entry.next_fire_mono == SteadyTimePoint{}) {
                entry.next_fire_mono = t.next_fire_after(now_mono);
            } else {
                auto next = entry.next_fire_mono + t.interval;
                if (next <= now_mono) {
                    auto behind = now_mono - next;
                    auto steps = behind / t.interval + 1;
                    next += t.interval * steps;
                }
                entry.next_fire_mono = next;
            }
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
