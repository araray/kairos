/// include/kairos/core/bounded_queue.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/core/bounded_queue.hpp — Thread-safe bounded MPMC queue          ║
// ║                                                                          ║
// ║  Provides backpressure: producers block when the queue is full.          ║
// ║  Supports cooperative cancellation via std::stop_token.                  ║
// ║                                                                          ║
// ║  Spec reference: §3.3 (threading model, inter-thread communication)     ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>

namespace kairos::core {

/// A bounded, thread-safe, multi-producer multi-consumer queue.
///
/// When the queue is full, `push()` blocks until space is available
/// or the timeout expires — this is the backpressure mechanism.
///
/// When the queue is empty, `pop()` blocks until an item is available,
/// a timeout expires, or a stop is requested.
///
/// Thread safety: all public methods are thread-safe.
template <typename T>
class BoundedQueue {
public:
    /// Construct with maximum capacity.
    /// @param capacity  Maximum number of items the queue can hold.
    explicit BoundedQueue(std::size_t capacity)
        : capacity_(capacity) {}

    /// Push an item. Blocks if the queue is full.
    ///
    /// @param item     Item to push (moved into the queue).
    /// @param timeout  Maximum time to wait for space.
    /// @return true if the item was pushed, false on timeout.
    bool push(T item, std::chrono::milliseconds timeout =
                  std::chrono::milliseconds(1000)) {
        std::unique_lock lock(mutex_);

        if (!not_full_.wait_for(lock, timeout, [this] {
                return queue_.size() < capacity_ || closed_;
            })) {
            return false;  // Timeout — backpressure signal.
        }

        if (closed_) {
            return false;
        }

        queue_.push_back(std::move(item));
        not_empty_.notify_one();
        return true;
    }

    /// Push an item, respecting a stop_token for cancellation.
    ///
    /// @param item   Item to push.
    /// @param stop   Cooperative cancellation token.
    /// @return true if pushed, false if queue is closed or stop requested.
    bool push(T item, std::stop_token stop) {
        std::unique_lock lock(mutex_);

        // Use condition_variable_any to support stop_token.
        std::condition_variable_any cv;
        if (!not_full_.wait(lock, stop, [this] {
                return queue_.size() < capacity_ || closed_;
            })) {
            return false;  // Stop requested.
        }

        if (closed_) {
            return false;
        }

        queue_.push_back(std::move(item));
        not_empty_.notify_one();
        return true;
    }

    /// Try to push without blocking.
    /// @return true if the item was pushed, false if queue is full/closed.
    bool try_push(T item) {
        std::lock_guard lock(mutex_);
        if (queue_.size() >= capacity_ || closed_) {
            return false;
        }
        queue_.push_back(std::move(item));
        not_empty_.notify_one();
        return true;
    }

    /// Pop an item. Blocks if the queue is empty.
    ///
    /// @param timeout  Maximum time to wait for an item.
    /// @return The item, or std::nullopt on timeout or queue closed.
    std::optional<T> pop(std::chrono::milliseconds timeout =
                             std::chrono::milliseconds(1000)) {
        std::unique_lock lock(mutex_);

        if (!not_empty_.wait_for(lock, timeout, [this] {
                return !queue_.empty() || closed_;
            })) {
            return std::nullopt;  // Timeout.
        }

        if (queue_.empty()) {
            return std::nullopt;  // Closed and drained.
        }

        T item = std::move(queue_.front());
        queue_.pop_front();
        not_full_.notify_one();
        return item;
    }

    /// Pop an item, respecting a stop_token for cancellation.
    ///
    /// @param stop   Cooperative cancellation token.
    /// @return The item, or std::nullopt on stop/close.
    std::optional<T> pop(std::stop_token stop) {
        std::unique_lock lock(mutex_);

        if (!not_empty_.wait(lock, stop, [this] {
                return !queue_.empty() || closed_;
            })) {
            return std::nullopt;  // Stop requested.
        }

        if (queue_.empty()) {
            return std::nullopt;  // Closed and drained.
        }

        T item = std::move(queue_.front());
        queue_.pop_front();
        not_full_.notify_one();
        return item;
    }

    /// Try to pop without blocking.
    /// @return The item, or std::nullopt if queue is empty.
    std::optional<T> try_pop() {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        T item = std::move(queue_.front());
        queue_.pop_front();
        not_full_.notify_one();
        return item;
    }

    /// Drain up to `max_count` items without blocking.
    /// Used by the DB writer for batch processing.
    ///
    /// @param max_count  Maximum number of items to drain.
    /// @return Vector of drained items (may be empty).
    std::vector<T> drain(std::size_t max_count) {
        std::lock_guard lock(mutex_);
        std::vector<T> batch;
        std::size_t count = std::min(max_count, queue_.size());
        batch.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            batch.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        if (count > 0) {
            not_full_.notify_all();
        }
        return batch;
    }

    /// Close the queue. No more items can be pushed.
    /// Wakes all waiting threads.
    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    /// @return Current number of items in the queue.
    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

    /// @return Maximum capacity of the queue.
    [[nodiscard]] std::size_t capacity() const { return capacity_; }

    /// @return true if the queue has been closed.
    [[nodiscard]] bool is_closed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    /// @return true if the queue is empty.
    [[nodiscard]] bool empty() const {
        std::lock_guard lock(mutex_);
        return queue_.empty();
    }

private:
    const std::size_t capacity_;
    std::deque<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    bool closed_ = false;
};

}  // namespace kairos::core
