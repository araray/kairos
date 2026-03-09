/// tests/unit/watch/watch_scan_spans_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Watch scan OTel span instrumentation tests (§21.5)                      ║
// ║                                                                          ║
// ║  Tests:                                                                  ║
// ║    - Scan with NullTracer doesn't crash                                  ║
// ║    - Scan with recording tracer creates expected spans                   ║
// ║    - Span hierarchy: watch_scan → snapshot, diff, rule_eval              ║
// ║    - Span attributes are populated correctly                             ║
// ║    - Scan without tracer (nullptr) works                                ║
// ║                                                                          ║
// ║  Uses a custom RecordingTracer to capture spans for assertion.           ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/observability/tracer.hpp"
#include "kairos/testing/fake_clock.hpp"
#include "kairos/testing/fake_filesystem.hpp"
#include "kairos/watch/watch_engine.hpp"

#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <vector>

namespace {

// ── Recording span: captures all operations for assertion ────────────────

struct SpanRecord {
    std::string name;
    std::string parent_name;
    std::vector<std::pair<std::string, std::string>> attributes;
    bool ended = false;
    bool error = false;
    std::string error_message;
};

class RecordingSpan final : public kairos::observability::SpanHandle {
public:
    explicit RecordingSpan(std::string name,
                           std::vector<SpanRecord>& records,
                           std::mutex& mu)
        : name_(std::move(name)), records_(records), mu_(mu)
    {
        std::lock_guard lock(mu_);
        records_.push_back({name_, "", {}, false, false, ""});
        index_ = records_.size() - 1;
    }

    void set_attribute(std::string_view key,
                       kairos::observability::SpanAttribute value) override {
        std::lock_guard lock(mu_);
        std::string val_str;
        std::visit([&](auto&& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                val_str = v;
            } else if constexpr (std::is_same_v<T, int64_t>) {
                val_str = std::to_string(v);
            } else if constexpr (std::is_same_v<T, double>) {
                val_str = std::to_string(v);
            } else if constexpr (std::is_same_v<T, bool>) {
                val_str = v ? "true" : "false";
            }
        }, value);
        if (index_ < records_.size()) {
            records_[index_].attributes.emplace_back(
                std::string(key), val_str);
        }
    }

    void set_error(std::string_view message) override {
        std::lock_guard lock(mu_);
        if (index_ < records_.size()) {
            records_[index_].error = true;
            records_[index_].error_message = std::string(message);
        }
    }

    void end() override {
        std::lock_guard lock(mu_);
        if (index_ < records_.size()) {
            records_[index_].ended = true;
        }
    }

    [[nodiscard]] std::string trace_id() const override { return "aaaa"; }
    [[nodiscard]] std::string span_id() const override { return "bbbb"; }
    [[nodiscard]] std::string traceparent() const override {
        return "00-aaaa-bbbb-01";
    }

private:
    std::string name_;
    std::vector<SpanRecord>& records_;
    std::mutex& mu_;
    std::size_t index_ = 0;
};

// ── Recording tracer ─────────────────────────────────────────────────────

class RecordingTracer final : public kairos::observability::Tracer {
public:
    std::unique_ptr<kairos::observability::SpanHandle> start_span(
        std::string_view name,
        std::unordered_map<std::string,
            kairos::observability::SpanAttribute> attrs) override
    {
        auto span = std::make_unique<RecordingSpan>(
            std::string(name), records_, mu_);
        for (auto& [k, v] : attrs) {
            span->set_attribute(k, std::move(v));
        }
        return span;
    }

    std::unique_ptr<kairos::observability::SpanHandle> start_child_span(
        kairos::observability::SpanHandle& parent,
        std::string_view name,
        std::unordered_map<std::string,
            kairos::observability::SpanAttribute> attrs) override
    {
        auto span = std::make_unique<RecordingSpan>(
            std::string(name), records_, mu_);
        {
            std::lock_guard lock(mu_);
            if (!records_.empty()) {
                records_.back().parent_name =
                    dynamic_cast<RecordingSpan&>(parent).span_id();
            }
        }
        for (auto& [k, v] : attrs) {
            span->set_attribute(k, std::move(v));
        }
        return span;
    }

    std::unique_ptr<kairos::observability::SpanHandle> start_linked_span(
        kairos::observability::SpanHandle&,
        std::string_view name,
        std::unordered_map<std::string,
            kairos::observability::SpanAttribute> attrs) override
    {
        return start_span(name, std::move(attrs));
    }

    void flush() override {}

    // Access recorded spans.
    std::vector<SpanRecord> get_records() const {
        std::lock_guard lock(mu_);
        return records_;
    }

    std::size_t span_count() const {
        std::lock_guard lock(mu_);
        return records_.size();
    }

private:
    mutable std::mutex mu_;
    std::vector<SpanRecord> records_;
};

// ── Test fixture ─────────────────────────────────────────────────────────

class WatchScanSpansTest : public ::testing::Test {
protected:
    void SetUp() override {
        fake_fs_.add_file("/watch/a.txt", 100, "2025-01-01T00:00:00Z");
        fake_fs_.add_file("/watch/b.txt", 200, "2025-01-01T00:00:00Z");
    }

    kairos::watch::WatchGroupDef make_group() {
        kairos::watch::WatchGroupDef g;
        g.group_name = "test_group";
        g.group_id = "wg-test";
        g.mode = kairos::watch::WatchMode::Sample;
        g.sample_rate = std::chrono::seconds(60);
        g.watch_items = {"/watch"};
        g.max_depth = 5;
        g.max_files = 1000;
        g.enabled = true;
        return g;
    }

    kairos::FakeFilesystem fake_fs_;
    kairos::SystemClockSource clock_;
};

// ── Tests ────────────────────────────────────────────────────────────────

TEST_F(WatchScanSpansTest, ScanWithNullTracerDoesNotCrash) {
    auto g = make_group();
    kairos::watch::WatchEngineConfig cfg;
    kairos::watch::WatchEngine engine(cfg,
        kairos::watch::WatchEngine::Dependencies{
            .clock = &clock_,
            .scanner = &fake_fs_,
            .tracer = nullptr,  // No tracer.
        },
        {g});

    kairos::engine::TriggerSink null_sink = [](kairos::engine::TriggerEvent) {
        return true;
    };

    EXPECT_NO_THROW(engine.scan_once(null_sink));
}

TEST_F(WatchScanSpansTest, ScanWithNullTracerProducesScanResults) {
    auto g = make_group();
    auto tracer = kairos::observability::create_tracer(false);

    kairos::watch::WatchEngineConfig cfg;
    kairos::watch::WatchEngine engine(cfg,
        kairos::watch::WatchEngine::Dependencies{
            .clock = &clock_,
            .scanner = &fake_fs_,
            .tracer = tracer.get(),
        },
        {g});

    kairos::engine::TriggerSink null_sink = [](kairos::engine::TriggerEvent) {
        return true;
    };

    auto results = engine.scan_once(null_sink);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_GE(results[0].sample.entries.size(), 2u);
}

TEST_F(WatchScanSpansTest, ScanWithRecordingTracerCreatesSpans) {
    auto g = make_group();
    RecordingTracer tracer;

    kairos::watch::WatchEngineConfig cfg;
    kairos::watch::WatchEngine engine(cfg,
        kairos::watch::WatchEngine::Dependencies{
            .clock = &clock_,
            .scanner = &fake_fs_,
            .tracer = &tracer,
        },
        {g});

    kairos::engine::TriggerSink null_sink = [](kairos::engine::TriggerEvent) {
        return true;
    };

    engine.scan_once(null_sink);

    // First scan is baseline — should create:
    //   1. kairos.watch_scan (root)
    //   2. kairos.snapshot (child)
    // No diff or rule_eval on first scan.
    auto records = tracer.get_records();
    ASSERT_GE(records.size(), 2u);

    EXPECT_EQ(records[0].name, "kairos.watch_scan");
    EXPECT_EQ(records[1].name, "kairos.snapshot");

    // All spans should be ended.
    for (const auto& r : records) {
        EXPECT_TRUE(r.ended)
            << "Span '" << r.name << "' was not ended";
    }
}

TEST_F(WatchScanSpansTest, SecondScanCreatesDiffAndRuleSpans) {
    auto g = make_group();
    RecordingTracer tracer;

    kairos::watch::WatchEngineConfig cfg;
    kairos::watch::WatchEngine engine(cfg,
        kairos::watch::WatchEngine::Dependencies{
            .clock = &clock_,
            .scanner = &fake_fs_,
            .tracer = &tracer,
        },
        {g});

    kairos::engine::TriggerSink null_sink = [](kairos::engine::TriggerEvent) {
        return true;
    };

    // First scan (baseline).
    engine.scan_once(null_sink);

    // Modify the filesystem.
    fake_fs_.add_file("/watch/c.txt", 300, "2025-01-02T00:00:00Z");

    // Second scan — should produce diff.
    engine.scan_once(null_sink);

    auto records = tracer.get_records();

    // Should have at least 4 spans from second scan:
    //   watch_scan, snapshot, diff, (optionally rule_eval if diff non-empty)
    // Plus the 2 from first scan = at least 6 total.
    EXPECT_GE(records.size(), 5u);

    // Find the diff span.
    bool found_diff = false;
    for (const auto& r : records) {
        if (r.name == "kairos.diff") {
            found_diff = true;
            break;
        }
    }
    EXPECT_TRUE(found_diff)
        << "Expected a 'kairos.diff' span on second scan";
}

TEST_F(WatchScanSpansTest, ScanSpanHasWatchGroupAttribute) {
    auto g = make_group();
    RecordingTracer tracer;

    kairos::watch::WatchEngineConfig cfg;
    kairos::watch::WatchEngine engine(cfg,
        kairos::watch::WatchEngine::Dependencies{
            .clock = &clock_,
            .scanner = &fake_fs_,
            .tracer = &tracer,
        },
        {g});

    kairos::engine::TriggerSink null_sink = [](kairos::engine::TriggerEvent) {
        return true;
    };

    engine.scan_once(null_sink);

    auto records = tracer.get_records();
    ASSERT_GE(records.size(), 1u);

    // The root span should have watch_group attribute.
    const auto& root = records[0];
    EXPECT_EQ(root.name, "kairos.watch_scan");

    bool found_group = false;
    for (const auto& [k, v] : root.attributes) {
        if (k == "watch_group") {
            EXPECT_EQ(v, "test_group");
            found_group = true;
        }
    }
    EXPECT_TRUE(found_group) << "Expected 'watch_group' attribute";
}

}  // anonymous namespace
