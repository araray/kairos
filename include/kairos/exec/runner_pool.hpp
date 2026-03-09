/// include/kairos/exec/runner_pool.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/exec/runner_pool.hpp — Worker thread pool for process execution  ║
// ║                                                                          ║
// ║  N jthread workers dequeue WorkItems from a bounded queue, spawn child   ║
// ║  processes via ProcessHandle, capture output, and report results via     ║
// ║  callbacks. Supports backpressure and cooperative shutdown.              ║
// ║                                                                          ║
// ║  Spec reference: §15, §3.3 (threading model)                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/core/bounded_queue.hpp"
#include "kairos/exec/process_handle.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace kairos::exec {

// ── Work item: pipeline → runner pool ────────────────────────────────────

/// A single step to execute. Submitted by the pipeline thread to the
/// runner pool's work queue.
struct WorkItem {
    /// IDs for logging and persistence.
    std::string run_id;
    std::string job_id;
    std::string step_id;
    std::string correlation_id;

    /// The process specification (command, env, timeout, etc.).
    ProcessSpec process_spec;

    /// Callback invoked when the step completes.
    /// Called from the runner worker thread.
    using CompletionCallback = std::function<void(
        const std::string& step_id,
        ProcessResult result)>;
    CompletionCallback on_complete;

    /// Optional output streaming callback.
    /// If set, attached to the ProcessHandle before spawn.
    OutputCallback output_callback;

    /// Optional per-run stop token for individual run cancellation.
    /// When present, the worker merges this with the global stop token
    /// so that cancelling a single run kills only that run's processes
    /// without stopping the entire daemon.
    /// See CancelRegistry (§23.10) for the cancellation flow.
    std::optional<std::stop_token> cancel_token;
};

// ── Runner pool ──────────────────────────────────────────────────────────

/// Configuration for the runner pool.
struct RunnerPoolConfig {
    /// Number of worker threads.
    std::size_t worker_count = 4;

    /// Maximum items in the work queue before backpressure kicks in.
    std::size_t queue_capacity = 256;
};

/// A pool of worker threads that execute WorkItems.
///
/// Thread safety:
///   - submit() is thread-safe (called from the pipeline thread).
///   - start()/shutdown() must be called from a single thread.
class RunnerPool {
public:
    /// Construct with configuration.
    explicit RunnerPool(RunnerPoolConfig config = {});

    ~RunnerPool();

    /// Start the worker threads.
    /// @param stop  Global stop token for cooperative shutdown.
    void start(std::stop_token stop);

    /// Submit a work item. May block if the queue is full (backpressure).
    /// @return true if submitted, false if pool is shut down or timeout.
    bool submit(WorkItem item,
                std::chrono::milliseconds timeout =
                    std::chrono::milliseconds(5000));

    /// Shutdown the pool: close the queue and join all workers.
    /// In-flight work items will complete (or be killed on timeout).
    void shutdown();

    /// @return Number of items waiting in the queue.
    [[nodiscard]] std::size_t queue_depth() const;

    /// @return Number of workers currently busy.
    [[nodiscard]] std::size_t active_workers() const;

    /// @return Total worker count.
    [[nodiscard]] std::size_t worker_count() const;

    /// Factory for ProcessHandle. Can be overridden for testing.
    using ProcessHandleFactory = std::function<
        std::unique_ptr<ProcessHandle>()>;
    void set_process_handle_factory(ProcessHandleFactory factory);

private:
    void worker_loop(std::stop_token stop, std::size_t worker_id);

    RunnerPoolConfig config_;
    core::BoundedQueue<WorkItem> queue_;
    std::vector<std::jthread> workers_;
    std::atomic<std::size_t> active_count_{0};
    ProcessHandleFactory process_factory_;
};

}  // namespace kairos::exec
