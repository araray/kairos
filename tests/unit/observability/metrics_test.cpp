/// tests/unit/observability/metrics_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for MetricsRegistry, Counter, Gauge, Histogram                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/observability/metrics.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

using namespace kairos::metrics;

// ═══════════════════════════════════════════════════════════════════════════
// Counter tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(CounterTest, InitiallyZero) {
    Counter c("test_counter", "A test counter");
    EXPECT_EQ(c.value(), 0);
}

TEST(CounterTest, Increment) {
    Counter c("test_counter", "A test counter");
    c.increment();
    EXPECT_EQ(c.value(), 1);
    c.increment(5);
    EXPECT_EQ(c.value(), 6);
}

TEST(CounterTest, ThreadSafe) {
    Counter c("concurrent_counter", "Thread-safe counter");
    constexpr int kThreads = 8;
    constexpr int kIncrements = 10000;

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < kIncrements; ++j) {
                c.increment();
            }
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(c.value(), kThreads * kIncrements);
}

TEST(CounterTest, WithLabels) {
    Counter c("http_requests_total", "Total HTTP requests",
              {{"method", "GET"}, {"status", "200"}});
    c.increment(42);
    EXPECT_EQ(c.value(), 42);
    EXPECT_EQ(c.labels().size(), 2u);
}

// ═══════════════════════════════════════════════════════════════════════════
// Gauge tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(GaugeTest, InitiallyZero) {
    Gauge g("test_gauge", "A test gauge");
    EXPECT_DOUBLE_EQ(g.value(), 0.0);
}

TEST(GaugeTest, SetAndGet) {
    Gauge g("test_gauge", "A test gauge");
    g.set(42.5);
    EXPECT_DOUBLE_EQ(g.value(), 42.5);
}

TEST(GaugeTest, IncrementDecrement) {
    Gauge g("test_gauge", "A test gauge");
    g.increment(10.0);
    EXPECT_DOUBLE_EQ(g.value(), 10.0);
    g.decrement(3.0);
    EXPECT_DOUBLE_EQ(g.value(), 7.0);
    g.increment();  // +1.0
    EXPECT_DOUBLE_EQ(g.value(), 8.0);
    g.decrement();  // -1.0
    EXPECT_DOUBLE_EQ(g.value(), 7.0);
}

TEST(GaugeTest, ThreadSafe) {
    Gauge g("concurrent_gauge", "Thread-safe gauge");
    constexpr int kThreads = 4;
    constexpr int kOps = 5000;

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < kOps; ++j) {
                g.increment(1.0);
            }
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_DOUBLE_EQ(g.value(), kThreads * kOps);
}

// ═══════════════════════════════════════════════════════════════════════════
// Histogram tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(HistogramTest, SingleObservation) {
    Histogram h("request_duration_ms", "Request duration",
                {10, 50, 100, 500, 1000});
    h.observe(75);

    EXPECT_EQ(h.count(), 1);
    EXPECT_DOUBLE_EQ(h.sum(), 75.0);

    // Buckets: 10(0), 50(0), 100(1), 500(1), 1000(1), +Inf(1)
    EXPECT_EQ(h.bucket_count(0), 0);  // le=10
    EXPECT_EQ(h.bucket_count(1), 0);  // le=50
    EXPECT_EQ(h.bucket_count(2), 1);  // le=100
    EXPECT_EQ(h.bucket_count(3), 1);  // le=500
    EXPECT_EQ(h.bucket_count(4), 1);  // le=1000
    EXPECT_EQ(h.bucket_count(5), 1);  // le=+Inf
}

TEST(HistogramTest, MultipleObservations) {
    Histogram h("test_hist", "Test", {10, 100});

    h.observe(5);    // fits in 10, 100, +Inf
    h.observe(50);   // fits in 100, +Inf
    h.observe(200);  // fits only in +Inf

    EXPECT_EQ(h.count(), 3);
    EXPECT_DOUBLE_EQ(h.sum(), 255.0);

    EXPECT_EQ(h.bucket_count(0), 1);  // le=10
    EXPECT_EQ(h.bucket_count(1), 2);  // le=100
    EXPECT_EQ(h.bucket_count(2), 3);  // le=+Inf
}

TEST(HistogramTest, ThreadSafe) {
    Histogram h("concurrent_hist", "Thread-safe histogram",
                {100, 500, 1000});
    constexpr int kThreads = 4;
    constexpr int kObs = 1000;

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            for (int j = 0; j < kObs; ++j) {
                h.observe(static_cast<double>(i * 100 + j));
            }
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(h.count(), kThreads * kObs);
}

// ═══════════════════════════════════════════════════════════════════════════
// Label formatting tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(LabelsTest, EmptyLabels) {
    EXPECT_EQ(format_labels({}), "");
}

TEST(LabelsTest, SingleLabel) {
    Labels labels = {{"method", "GET"}};
    EXPECT_EQ(format_labels(labels), R"({method="GET"})");
}

TEST(LabelsTest, MultipleLabels) {
    Labels labels = {{"method", "POST"}, {"status", "200"}};
    EXPECT_EQ(format_labels(labels),
              R"({method="POST",status="200"})");
}

TEST(LabelsTest, EscapedQuotes) {
    Labels labels = {{"path", R"(say "hi")"}};
    EXPECT_EQ(format_labels(labels),
              R"({path="say \"hi\""})");
}

// ═══════════════════════════════════════════════════════════════════════════
// MetricsRegistry tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(MetricsRegistryTest, RegisterCounter) {
    MetricsRegistry registry;
    auto* c = registry.register_counter(
        "kairos_runs_total", "Total runs",
        {{"status", "success"}});
    ASSERT_NE(c, nullptr);
    c->increment(5);
    EXPECT_EQ(c->value(), 5);
    EXPECT_EQ(registry.instrument_count(), 1u);
}

TEST(MetricsRegistryTest, RegisterGauge) {
    MetricsRegistry registry;
    auto* g = registry.register_gauge(
        "kairos_runs_active", "Active runs");
    ASSERT_NE(g, nullptr);
    g->set(3);
    EXPECT_DOUBLE_EQ(g->value(), 3.0);
}

TEST(MetricsRegistryTest, RegisterHistogram) {
    MetricsRegistry registry;
    auto* h = registry.register_histogram(
        "kairos_run_duration_ms", "Run duration",
        {100, 250, 500, 1000, 2500, 5000});
    ASSERT_NE(h, nullptr);
    h->observe(350);
    EXPECT_EQ(h->count(), 1);
}

TEST(MetricsRegistryTest, PrometheusFormat) {
    MetricsRegistry registry;

    auto* c = registry.register_counter(
        "kairos_runs_total", "Total completed runs",
        {{"status", "success"}});
    c->increment(42);

    auto* g = registry.register_gauge(
        "kairos_runs_active", "Currently executing runs");
    g->set(2);

    auto* h = registry.register_histogram(
        "kairos_run_duration_ms", "Run duration",
        {100, 500, 1000});
    h->observe(250);
    h->observe(750);

    std::string prom = registry.to_prometheus();

    // Verify counter output.
    EXPECT_NE(prom.find("# HELP kairos_runs_total"), std::string::npos);
    EXPECT_NE(prom.find("# TYPE kairos_runs_total counter"), std::string::npos);
    EXPECT_NE(prom.find(R"(kairos_runs_total{status="success"} 42)"),
              std::string::npos);

    // Verify gauge output.
    EXPECT_NE(prom.find("# TYPE kairos_runs_active gauge"), std::string::npos);
    EXPECT_NE(prom.find("kairos_runs_active 2"), std::string::npos);

    // Verify histogram output.
    EXPECT_NE(prom.find("# TYPE kairos_run_duration_ms histogram"),
              std::string::npos);
    EXPECT_NE(prom.find("kairos_run_duration_ms_bucket"), std::string::npos);
    EXPECT_NE(prom.find("kairos_run_duration_ms_sum"), std::string::npos);
    EXPECT_NE(prom.find("kairos_run_duration_ms_count 2"), std::string::npos);
}

TEST(MetricsRegistryTest, JsonFormat) {
    MetricsRegistry registry;

    auto* c = registry.register_counter(
        "kairos_test", "Test counter");
    c->increment(10);

    std::string json = registry.to_json();
    EXPECT_NE(json.find("\"counters\""), std::string::npos);
    EXPECT_NE(json.find("kairos_test"), std::string::npos);
    EXPECT_NE(json.find("\"value\":10"), std::string::npos);
}

TEST(MetricsRegistryTest, InstrumentCount) {
    MetricsRegistry registry;
    EXPECT_EQ(registry.instrument_count(), 0u);

    registry.register_counter("c1", "");
    registry.register_counter("c2", "");
    registry.register_gauge("g1", "");
    registry.register_histogram("h1", "");

    EXPECT_EQ(registry.instrument_count(), 4u);
}

TEST(MetricsRegistryTest, PrometheusHistogramBuckets) {
    MetricsRegistry registry;

    auto* h = registry.register_histogram(
        "test_hist", "Test",
        {10, 50, 100});
    h->observe(5);
    h->observe(30);
    h->observe(75);
    h->observe(200);

    std::string prom = registry.to_prometheus();

    // le=10: 1 observation (5)
    EXPECT_NE(prom.find(R"(test_hist_bucket{le="10"} 1)"),
              std::string::npos);
    // le=50: 2 observations (5, 30)
    EXPECT_NE(prom.find(R"(test_hist_bucket{le="50"} 2)"),
              std::string::npos);
    // le=100: 3 observations (5, 30, 75)
    EXPECT_NE(prom.find(R"(test_hist_bucket{le="100"} 3)"),
              std::string::npos);
    // le=+Inf: 4 observations (all)
    EXPECT_NE(prom.find(R"(test_hist_bucket{le="+Inf"} 4)"),
              std::string::npos);
    // Count = 4
    EXPECT_NE(prom.find("test_hist_count 4"), std::string::npos);
}

TEST(MetricsRegistryTest, EmptyRegistryPrometheus) {
    MetricsRegistry registry;
    std::string prom = registry.to_prometheus();
    EXPECT_TRUE(prom.empty());
}
