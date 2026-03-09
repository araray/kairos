/// include/kairos/engine/cancel_registry.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/engine/cancel_registry.hpp — Per-run cancellation registry       ║
// ║                                                                          ║
// ║  Manages per-run stop_source objects so that individual runs can be      ║
// ║  cancelled without stopping the entire daemon. The CLI writes cancel    ║
// ║  command files, the daemon's CommandReader reads them, and dispatches   ║
// ║  to this registry.                                                      ║
// ║                                                                          ║
// ║  The Pipeline registers a stop_source when a run starts and removes     ║
// ║  it when the run completes. The combined stop_token (global OR per-run) ║
// ║  propagates to ProcessHandle::wait() for actual process termination.    ║
// ║                                                                          ║
// ║  Thread safety: all methods are thread-safe (protected by mutex).       ║
// ║                                                                          ║
// ║  Spec reference: §23.10                                                 ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <mutex>
#include <stop_token>
#include <string>
#include <unordered_map>

namespace kairos::engine {

/// Registry of per-run stop_source objects for cancellation.
///
/// Lifecycle:
///   1. Pipeline calls register_run() at run start → gets a stop_token
///   2. Pipeline threads combine this token with the global stop_token
///   3. CLI cancel → daemon → cancel(run_id) → stop_source.request_stop()
///   4. Pipeline calls unregister_run() at run completion
///
/// If cancel() is called for a run_id that is not registered (e.g., the
/// run completed before the cancel was processed), it is a safe no-op.
class CancelRegistry {
public:
    /// Register a new run. Returns the per-run stop_token.
    /// The caller should combine this with the global stop_token.
    ///
    /// @param run_id  The run identifier.
    /// @return Per-run stop_token. Fires when cancel() is called.
    [[nodiscard]] std::stop_token register_run(const std::string& run_id);

    /// Unregister a completed run. Removes the stop_source.
    /// Safe to call multiple times or for non-existent run_ids.
    void unregister_run(const std::string& run_id);

    /// Cancel a running run. Fires the per-run stop_token.
    /// Returns true if the run was found and cancelled.
    /// Returns false if the run_id is not registered (already done or unknown).
    bool cancel(const std::string& run_id);

    /// Check if a run_id is registered (still active).
    [[nodiscard]] bool is_registered(const std::string& run_id) const;

    /// @return Number of currently registered (active) runs.
    [[nodiscard]] std::size_t active_count() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::stop_source> sources_;
};

// ── Combined stop_token utility ─────────────────────────────────────────

/// Create a stop_source whose token fires when EITHER of the input
/// tokens fires. Installs stop_callbacks on both inputs.
///
/// The returned struct owns the combined stop_source and the callbacks.
/// It must outlive any use of the combined_token(). Typically stored
/// as a local in the Pipeline's process_event() method — destroyed
/// when the run completes.
///
/// Why not just check two tokens? Because ProcessHandle::wait() takes
/// a single stop_token. We need a single token that represents the
/// OR of global shutdown and per-run cancel.
struct CombinedStopToken {
    std::stop_source combined;

    /// Install callbacks. Must be called once after construction.
    /// @param global_stop  The daemon's global stop token.
    /// @param run_stop     The per-run stop token from CancelRegistry.
    void arm(std::stop_token global_stop, std::stop_token run_stop);

    /// Get the combined stop token.
    [[nodiscard]] std::stop_token token() const {
        return combined.get_token();
    }

private:
    // stop_callback is non-movable (move ctor is deleted per the
    // C++20 spec). We need type erasure because stop_callback<Lambda>
    // has a lambda-dependent type. Solution: forward constructor args
    // to build the stop_callback in-place on the heap — no move needed.
    struct CallbackHolder {
        virtual ~CallbackHolder() = default;
    };
    template <typename T>
    struct CallbackHolderImpl : CallbackHolder {
        T cb;
        template <typename... Args>
        explicit CallbackHolderImpl(Args&&... args)
            : cb(std::forward<Args>(args)...) {}
    };
    std::unique_ptr<CallbackHolder> cb1_;
    std::unique_ptr<CallbackHolder> cb2_;
};

}  // namespace kairos::engine
