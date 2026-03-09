/// include/kairos/exec/output_sink.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/exec/output_sink.hpp — Multiplexing output sink + RunStream     ║
// ║                                                                          ║
// ║  OutputMultiplexer: fans out each process output chunk to multiple       ║
// ║  downstream consumers (DB writer, RunStream, etc.) after applying       ║
// ║  secret masking.                                                         ║
// ║                                                                          ║
// ║  RunStream: per-run pub-sub channel for live log following.              ║
// ║  Subscribers: CLI --follow, MCP log streaming, HTTP SSE.                ║
// ║                                                                          ║
// ║  Spec reference: §14.7                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/exec/process_handle.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kairos::exec {

// ── OutputMultiplexer ─────────────────────────────────────────────────────

/// Multiplexing output sink that fans out each chunk to multiple
/// downstream consumers. Applies secret masking before delivery.
///
/// Thread safety: all methods are thread-safe (protected by mutex).
/// OutputCallback calls deliver() from ProcessHandle reader threads.
class OutputMultiplexer {
public:
    /// Downstream consumer signature.
    /// @param chunk     The output data (after secret masking).
    /// @param is_stderr True if the chunk came from stderr.
    using SinkFn = std::function<void(std::string_view chunk,
                                       bool is_stderr)>;

    /// Add a downstream consumer.
    /// @return A handle for removal via remove_sink().
    /// Thread-safe.
    std::size_t add_sink(SinkFn sink);

    /// Remove a downstream consumer by handle.
    /// Thread-safe. No-op if handle is invalid.
    void remove_sink(std::size_t handle);

    /// Called by the ProcessHandle's OutputCallback.
    /// Masks secrets, then delivers to all registered sinks.
    /// Thread-safe. Must not throw.
    void deliver(std::string_view chunk, bool is_stderr);

    /// Set the list of secret values to mask.
    /// Typically called once before the process starts.
    /// Thread-safe.
    void set_secret_values(std::vector<std::string> secrets);

    /// @return Current number of registered sinks.
    [[nodiscard]] std::size_t sink_count() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<std::size_t, SinkFn>> sinks_;
    std::size_t next_handle_ = 0;
    std::vector<std::string> secret_values_;

    /// Replace occurrences of any secret value with "***".
    /// Operates on a copy to avoid modifying the original.
    [[nodiscard]] std::string mask_secrets(std::string_view input) const;
};

// ── RunStream ─────────────────────────────────────────────────────────────

/// Per-run pub-sub channel. CLI `kairos logs --follow` and MCP
/// `stream_logs` subscribe to this channel to receive real-time
/// output from a running job.
///
/// Thread safety: all methods are thread-safe (protected by mutex).
///
/// Lifecycle:
///   1. Pipeline creates RunStream (one global instance shared by daemon)
///   2. Before a run starts, subscribers register via subscribe()
///   3. During execution, OutputMultiplexer calls publish()
///   4. When the run completes, close_run() removes all subscribers
///
/// Memory safety: close_run() ensures no stale callbacks accumulate.
class RunStream {
public:
    using SubscriberId = std::size_t;

    /// Subscriber callback.
    /// @param run_id   The run this chunk belongs to.
    /// @param job_id   The job producing this output.
    /// @param step_id  The step producing this output.
    /// @param chunk    The output data (already secret-masked).
    /// @param is_stderr True if from stderr.
    using Callback = std::function<void(
        const std::string& run_id,
        const std::string& job_id,
        const std::string& step_id,
        std::string_view chunk,
        bool is_stderr)>;

    /// Subscribe to output from a specific run.
    /// @param run_id  The run ID to subscribe to.
    /// @param cb      Callback invoked for each chunk.
    /// @return A subscriber ID for unsubscribe().
    SubscriberId subscribe(const std::string& run_id, Callback cb);

    /// Unsubscribe a specific subscriber.
    /// Thread-safe. No-op if the subscriber doesn't exist.
    void unsubscribe(SubscriberId id);

    /// Publish a chunk to all subscribers for this run.
    /// Called from the OutputMultiplexer on runner worker threads.
    /// Must not throw.
    void publish(const std::string& run_id,
                 const std::string& job_id,
                 const std::string& step_id,
                 std::string_view chunk,
                 bool is_stderr);

    /// Remove all subscribers for a completed run.
    /// Called by the pipeline after a run finishes.
    void close_run(const std::string& run_id);

    /// @return Number of active runs with subscribers.
    [[nodiscard]] std::size_t active_run_count() const;

    /// @return Total number of subscribers across all runs.
    [[nodiscard]] std::size_t total_subscriber_count() const;

    /// @return Number of subscribers for a specific run.
    [[nodiscard]] std::size_t subscriber_count(
        const std::string& run_id) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string,
        std::vector<std::pair<SubscriberId, Callback>>> subscribers_;
    SubscriberId next_id_ = 0;
};

// ── Convenience: create an OutputCallback that feeds the multiplexer ──────

/// Build an OutputCallback suitable for ProcessHandle that routes
/// all output through the given OutputMultiplexer.
inline OutputCallback make_output_callback(OutputMultiplexer& mux) {
    return [&mux](std::string_view chunk, bool is_stderr) {
        mux.deliver(chunk, is_stderr);
    };
}

}  // namespace kairos::exec
