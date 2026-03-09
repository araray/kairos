/// src/engine/cancel_registry.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  CancelRegistry implementation                                           ║
// ║  Spec reference: §23.10                                                 ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/cancel_registry.hpp"

#include <spdlog/spdlog.h>

namespace kairos::engine {

// ── CancelRegistry ──────────────────────────────────────────────────────

std::stop_token CancelRegistry::register_run(const std::string& run_id) {
    std::lock_guard lock(mutex_);

    // Insert a new stop_source for this run. If already registered
    // (shouldn't happen in normal flow), replace the old one.
    auto [it, inserted] = sources_.try_emplace(run_id);
    if (!inserted) {
        spdlog::warn("CancelRegistry: run '{}' already registered — "
                     "replacing stop_source", run_id);
        it->second = std::stop_source{};
    }

    spdlog::debug("CancelRegistry: registered run '{}'", run_id);
    return it->second.get_token();
}

void CancelRegistry::unregister_run(const std::string& run_id) {
    std::lock_guard lock(mutex_);
    auto erased = sources_.erase(run_id);
    if (erased > 0) {
        spdlog::debug("CancelRegistry: unregistered run '{}'", run_id);
    }
}

bool CancelRegistry::cancel(const std::string& run_id) {
    std::lock_guard lock(mutex_);
    auto it = sources_.find(run_id);
    if (it == sources_.end()) {
        spdlog::debug("CancelRegistry: cancel requested for unknown "
                     "run '{}' (already completed?)", run_id);
        return false;
    }

    bool already = it->second.stop_requested();
    if (already) {
        spdlog::debug("CancelRegistry: run '{}' already cancelled", run_id);
        return true;
    }

    it->second.request_stop();
    spdlog::info("CancelRegistry: cancelled run '{}'", run_id);
    return true;
}

bool CancelRegistry::is_registered(const std::string& run_id) const {
    std::lock_guard lock(mutex_);
    return sources_.contains(run_id);
}

std::size_t CancelRegistry::active_count() const {
    std::lock_guard lock(mutex_);
    return sources_.size();
}

// ── CombinedStopToken ───────────────────────────────────────────────────

void CombinedStopToken::arm(std::stop_token global_stop,
                             std::stop_token run_stop) {
    // If either token is already stopped, fire immediately.
    if (global_stop.stop_requested() || run_stop.stop_requested()) {
        combined.request_stop();
        return;
    }

    // Install callbacks on both tokens.
    // When either fires, request_stop() on the combined source.
    auto fire = [this]() { combined.request_stop(); };

    using CbType = std::stop_callback<decltype(fire)>;

    // Construct stop_callbacks in-place on the heap via forwarding.
    // Cannot use make_unique: stop_callback's move ctor is deleted.
    // CallbackHolderImpl's variadic ctor forwards args directly to
    // the stop_callback(token, callable) constructor — no move.
    cb1_.reset(new CallbackHolderImpl<CbType>(global_stop, fire));
    cb2_.reset(new CallbackHolderImpl<CbType>(run_stop, fire));
}

}  // namespace kairos::engine
