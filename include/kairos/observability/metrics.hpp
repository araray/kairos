/// include/kairos/observability/metrics.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/observability/metrics.hpp — Lightweight metrics registry          ║
// ║                                                                          ║
// ║  Self-contained metrics system with three instrument types:              ║
// ║    Counter   — monotonically increasing (atomic)                         ║
// ║    Gauge     — can go up or down (atomic)                                ║
// ║    Histogram — distribution with fixed buckets (atomic per bucket)       ║
// ║                                                                          ║
// ║  Thread-safe: registration uses a mutex (rare, startup-only);            ║
// ║  instrument updates are lock-free via std::atomic.                       ║
// ║                                                                          ║
// ║  Spec reference: §20.1–§20.4                                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace kairos::metrics {

// ── Labels ───────────────────────────────────────────────────────────────

/// A label is a key-value pair attached to a metric.
using Labels = std::vector<std::pair<std::string, std::string>>;

/// Format labels as Prometheus-style: {key="value",key2="value2"}
std::string format_labels(const Labels& labels);

// ── Counter ──────────────────────────────────────────────────────────────

/// Monotonically increasing counter. Thread-safe via std::atomic.
class Counter {
public:
    Counter(std::string name, std::string help, Labels labels = {})
        : name_(std::move(name))
        , help_(std::move(help))
        , labels_(std::move(labels)) {}

    /// Increment by 1.
    void increment() noexcept {
        value_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Increment by a positive amount.
    void increment(int64_t amount) noexcept {
        value_.fetch_add(amount, std::memory_order_relaxed);
    }

    [[nodiscard]] int64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] const std::string& help() const { return help_; }
    [[nodiscard]] const Labels& labels() const { return labels_; }

private:
    std::string name_;
    std::string help_;
    Labels labels_;
    std::atomic<int64_t> value_{0};
};

// ── Gauge ────────────────────────────────────────────────────────────────

/// A value that can go up and down. Thread-safe via std::atomic.
class Gauge {
public:
    Gauge(std::string name, std::string help, Labels labels = {})
        : name_(std::move(name))
        , help_(std::move(help))
        , labels_(std::move(labels)) {}

    void set(double val) noexcept {
        uint64_t bits;
        std::memcpy(&bits, &val, sizeof(bits));
        bits_.store(bits, std::memory_order_relaxed);
    }

    void increment(double amount = 1.0) noexcept {
        uint64_t old_bits = bits_.load(std::memory_order_relaxed);
        double old_val;
        std::memcpy(&old_val, &old_bits, sizeof(old_val));
        double new_val = old_val + amount;
        uint64_t new_bits;
        std::memcpy(&new_bits, &new_val, sizeof(new_bits));
        while (!bits_.compare_exchange_weak(
            old_bits, new_bits,
            std::memory_order_relaxed, std::memory_order_relaxed)) {
            std::memcpy(&old_val, &old_bits, sizeof(old_val));
            new_val = old_val + amount;
            std::memcpy(&new_bits, &new_val, sizeof(new_bits));
        }
    }

    void decrement(double amount = 1.0) noexcept {
        increment(-amount);
    }

    [[nodiscard]] double value() const noexcept {
        uint64_t bits = bits_.load(std::memory_order_relaxed);
        double val;
        std::memcpy(&val, &bits, sizeof(val));
        return val;
    }

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] const std::string& help() const { return help_; }
    [[nodiscard]] const Labels& labels() const { return labels_; }

private:
    std::string name_;
    std::string help_;
    Labels labels_;
    std::atomic<uint64_t> bits_{0};  // Bit-punned double.
};

// ── Histogram ────────────────────────────────────────────────────────────

/// Fixed-bucket histogram. Thread-safe via atomic bucket counters.
///
/// Default buckets: [5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000]
/// (suitable for durations in milliseconds).
class Histogram {
public:
    static inline const std::vector<double> kDefaultBuckets = {
        5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000};

    Histogram(std::string name, std::string help,
              std::vector<double> buckets = kDefaultBuckets,
              Labels labels = {})
        : name_(std::move(name))
        , help_(std::move(help))
        , labels_(std::move(labels))
        , upper_bounds_(std::move(buckets))
        // +1 for the +Inf bucket.
        , bucket_counts_(upper_bounds_.size() + 1) {}

    /// Record an observation.
    void observe(double value) noexcept {
        // Increment all buckets where value <= upper_bound.
        for (std::size_t i = 0; i < upper_bounds_.size(); ++i) {
            if (value <= upper_bounds_[i]) {
                bucket_counts_[i].fetch_add(1, std::memory_order_relaxed);
            }
        }
        // +Inf bucket is always incremented.
        bucket_counts_.back().fetch_add(1, std::memory_order_relaxed);

        // Update sum via atomic CAS.
        uint64_t old_bits = sum_bits_.load(std::memory_order_relaxed);
        double old_sum;
        std::memcpy(&old_sum, &old_bits, sizeof(old_sum));
        double new_sum = old_sum + value;
        uint64_t new_bits;
        std::memcpy(&new_bits, &new_sum, sizeof(new_bits));
        while (!sum_bits_.compare_exchange_weak(
            old_bits, new_bits,
            std::memory_order_relaxed, std::memory_order_relaxed)) {
            std::memcpy(&old_sum, &old_bits, sizeof(old_sum));
            new_sum = old_sum + value;
            std::memcpy(&new_bits, &new_sum, sizeof(new_bits));
        }

        count_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] const std::string& help() const { return help_; }
    [[nodiscard]] const Labels& labels() const { return labels_; }
    [[nodiscard]] const std::vector<double>& upper_bounds() const {
        return upper_bounds_;
    }

    [[nodiscard]] int64_t bucket_count(std::size_t i) const {
        return bucket_counts_.at(i).load(std::memory_order_relaxed);
    }

    [[nodiscard]] double sum() const {
        uint64_t bits = sum_bits_.load(std::memory_order_relaxed);
        double val;
        std::memcpy(&val, &bits, sizeof(val));
        return val;
    }

    [[nodiscard]] int64_t count() const {
        return count_.load(std::memory_order_relaxed);
    }

private:
    std::string name_;
    std::string help_;
    Labels labels_;
    std::vector<double> upper_bounds_;
    std::vector<std::atomic<int64_t>> bucket_counts_;
    std::atomic<uint64_t> sum_bits_{0};
    std::atomic<int64_t> count_{0};
};

// ── Metrics Registry ─────────────────────────────────────────────────────

/// Central metrics registry. Owns all instruments and provides
/// Prometheus text exposition and JSON serialization.
///
/// Thread-safe: registration uses a mutex (rare, startup-only);
/// instrument access is lock-free (frequent, hot path).
class MetricsRegistry {
public:
    /// Register a new counter. Returns a non-owning pointer.
    Counter* register_counter(
        const std::string& name, const std::string& help,
        Labels labels = {});

    /// Register a new gauge.
    Gauge* register_gauge(
        const std::string& name, const std::string& help,
        Labels labels = {});

    /// Register a new histogram.
    Histogram* register_histogram(
        const std::string& name, const std::string& help,
        std::vector<double> buckets = Histogram::kDefaultBuckets,
        Labels labels = {});

    /// Serialize all metrics in Prometheus text exposition format.
    [[nodiscard]] std::string to_prometheus() const;

    /// Serialize all metrics as JSON.
    [[nodiscard]] std::string to_json() const;

    /// @return Total number of registered instruments.
    [[nodiscard]] std::size_t instrument_count() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Counter>> counters_;
    std::vector<std::unique_ptr<Gauge>> gauges_;
    std::vector<std::unique_ptr<Histogram>> histograms_;
};

}  // namespace kairos::metrics
