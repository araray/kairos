/// src/exec/runner_pool.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  RunnerPool implementation                                               ║
// ║  Spec reference: §15                                                     ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/exec/runner_pool.hpp"

#include <spdlog/spdlog.h>

namespace kairos::exec {

RunnerPool::RunnerPool(RunnerPoolConfig config)
    : config_(config)
    , queue_(config.queue_capacity)
    , process_factory_(nullptr) {}

RunnerPool::~RunnerPool() {
    shutdown();
}

void RunnerPool::start(std::stop_token stop) {
    workers_.reserve(config_.worker_count);
    for (std::size_t i = 0; i < config_.worker_count; ++i) {
        workers_.emplace_back([this, stop, i](std::stop_token worker_stop) {
            // Combine the global stop with the per-thread stop.
            // In practice, we use the global one passed at start().
            (void)worker_stop;
            worker_loop(stop, i);
        });
    }
    spdlog::info("Runner pool started with {} workers",
                 config_.worker_count);
}

bool RunnerPool::submit(WorkItem item,
                        std::chrono::milliseconds timeout) {
    return queue_.push(std::move(item), timeout);
}

void RunnerPool::shutdown() {
    queue_.close();

    // jthread destructors request stop and join automatically,
    // but we clear explicitly for deterministic shutdown logging.
    for (auto& w : workers_) {
        if (w.joinable()) {
            w.request_stop();
        }
    }
    workers_.clear();

    spdlog::info("Runner pool shut down");
}

std::size_t RunnerPool::queue_depth() const {
    return queue_.size();
}

std::size_t RunnerPool::active_workers() const {
    return active_count_.load(std::memory_order_relaxed);
}

std::size_t RunnerPool::worker_count() const {
    return config_.worker_count;
}

void RunnerPool::set_process_handle_factory(ProcessHandleFactory factory) {
    process_factory_ = std::move(factory);
}

void RunnerPool::worker_loop(std::stop_token stop, std::size_t worker_id) {
    spdlog::debug("Runner worker-{} started", worker_id);

    while (!stop.stop_requested()) {
        // Dequeue a work item (blocks until available or stop).
        auto item_opt = queue_.pop(std::chrono::milliseconds(200));
        if (!item_opt.has_value()) {
            // Timeout, closed, or stop — check if we should exit.
            if (queue_.is_closed()) break;
            continue;
        }

        auto& item = *item_opt;
        active_count_.fetch_add(1, std::memory_order_relaxed);

        spdlog::debug("Runner worker-{} executing step {} (job={}, run={})",
                       worker_id, item.step_id, item.job_id, item.run_id);

        // Create process handle.
        if (!process_factory_) {
            spdlog::error("Runner worker-{}: no process factory set",
                          worker_id);
            if (item.on_complete) {
                ProcessResult fail_result;
                fail_result.exit_code = 202;
                fail_result.termination =
                    ProcessResult::TerminationKind::SpawnFailed;
                fail_result.termination_reason =
                    "No process handle factory configured";
                item.on_complete(item.step_id, std::move(fail_result));
            }
            active_count_.fetch_sub(1, std::memory_order_relaxed);
            continue;
        }
        auto proc = process_factory_();

        // Attach output callback if provided.
        if (item.output_callback) {
            proc->set_output_callback(item.output_callback);
        }

        // Spawn.
        ProcessResult result;
        if (!proc->spawn(item.process_spec)) {
            result = proc->result();
            spdlog::warn("Runner worker-{} spawn failed for step {}: {}",
                         worker_id, item.step_id,
                         result.termination_reason);
        } else {
            // Wait for completion (with stop_token for cancellation).
            result = proc->wait(stop);

            if (result.success()) {
                spdlog::debug(
                    "Runner worker-{} step {} completed successfully "
                    "({}ms)",
                    worker_id, item.step_id, result.duration.count());
            } else {
                spdlog::info(
                    "Runner worker-{} step {} exited with code {} "
                    "({}ms, {})",
                    worker_id, item.step_id, result.exit_code,
                    result.duration.count(),
                    result.termination_reason);
            }
        }

        // Invoke completion callback.
        if (item.on_complete) {
            try {
                item.on_complete(item.step_id, std::move(result));
            } catch (const std::exception& e) {
                spdlog::error(
                    "Runner worker-{} completion callback threw: {}",
                    worker_id, e.what());
            }
        }

        active_count_.fetch_sub(1, std::memory_order_relaxed);
    }

    spdlog::debug("Runner worker-{} stopped", worker_id);
}

}  // namespace kairos::exec
