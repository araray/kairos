/// src/exec/output_sink.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  OutputMultiplexer + RunStream implementations                            ║
// ║  Spec reference: §14.7                                                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/exec/output_sink.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace kairos::exec {

// ── OutputMultiplexer ─────────────────────────────────────────────────────

std::size_t OutputMultiplexer::add_sink(SinkFn sink) {
    std::lock_guard lock(mutex_);
    auto handle = next_handle_++;
    sinks_.emplace_back(handle, std::move(sink));
    return handle;
}

void OutputMultiplexer::remove_sink(std::size_t handle) {
    std::lock_guard lock(mutex_);
    sinks_.erase(
        std::remove_if(sinks_.begin(), sinks_.end(),
            [handle](const auto& p) { return p.first == handle; }),
        sinks_.end());
}

void OutputMultiplexer::deliver(std::string_view chunk, bool is_stderr) {
    if (chunk.empty()) return;

    std::lock_guard lock(mutex_);

    // Mask secrets before delivery to any sink.
    std::string masked = mask_secrets(chunk);
    std::string_view to_deliver = masked.empty() ? chunk : masked;

    for (const auto& [handle, sink] : sinks_) {
        try {
            sink(to_deliver, is_stderr);
        } catch (const std::exception& e) {
            // Sinks must not throw, but guard defensively.
            spdlog::warn("OutputMultiplexer: sink {} threw: {}",
                         handle, e.what());
        }
    }
}

void OutputMultiplexer::set_secret_values(
    std::vector<std::string> secrets)
{
    std::lock_guard lock(mutex_);
    // Remove empty strings — they would match everything.
    secrets.erase(
        std::remove_if(secrets.begin(), secrets.end(),
            [](const std::string& s) { return s.empty(); }),
        secrets.end());
    secret_values_ = std::move(secrets);
}

std::size_t OutputMultiplexer::sink_count() const {
    std::lock_guard lock(mutex_);
    return sinks_.size();
}

std::string OutputMultiplexer::mask_secrets(
    std::string_view input) const
{
    if (secret_values_.empty()) return {};  // No masking needed.

    std::string result(input);
    bool changed = false;

    for (const auto& secret : secret_values_) {
        if (secret.empty()) continue;

        std::string::size_type pos = 0;
        while ((pos = result.find(secret, pos)) != std::string::npos) {
            result.replace(pos, secret.size(), "***");
            pos += 3;  // Skip past replacement.
            changed = true;
        }
    }

    return changed ? result : std::string{};
}

// ── RunStream ─────────────────────────────────────────────────────────────

RunStream::SubscriberId RunStream::subscribe(
    const std::string& run_id, Callback cb)
{
    std::lock_guard lock(mutex_);
    auto id = next_id_++;
    subscribers_[run_id].emplace_back(id, std::move(cb));
    return id;
}

void RunStream::unsubscribe(SubscriberId id) {
    std::lock_guard lock(mutex_);
    for (auto& [run_id, subs] : subscribers_) {
        subs.erase(
            std::remove_if(subs.begin(), subs.end(),
                [id](const auto& p) { return p.first == id; }),
            subs.end());
    }
}

void RunStream::publish(
    const std::string& run_id,
    const std::string& job_id,
    const std::string& step_id,
    std::string_view chunk,
    bool is_stderr)
{
    if (chunk.empty()) return;

    std::lock_guard lock(mutex_);
    auto it = subscribers_.find(run_id);
    if (it == subscribers_.end()) return;

    for (const auto& [id, cb] : it->second) {
        try {
            cb(run_id, job_id, step_id, chunk, is_stderr);
        } catch (const std::exception& e) {
            spdlog::warn("RunStream: subscriber {} threw: {}",
                         id, e.what());
        }
    }
}

void RunStream::close_run(const std::string& run_id) {
    std::lock_guard lock(mutex_);
    subscribers_.erase(run_id);
}

std::size_t RunStream::active_run_count() const {
    std::lock_guard lock(mutex_);
    return subscribers_.size();
}

std::size_t RunStream::total_subscriber_count() const {
    std::lock_guard lock(mutex_);
    std::size_t total = 0;
    for (const auto& [run_id, subs] : subscribers_) {
        total += subs.size();
    }
    return total;
}

std::size_t RunStream::subscriber_count(
    const std::string& run_id) const
{
    std::lock_guard lock(mutex_);
    auto it = subscribers_.find(run_id);
    return it != subscribers_.end() ? it->second.size() : 0;
}

}  // namespace kairos::exec
