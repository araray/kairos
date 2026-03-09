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
#include "kairos/testing/fake_fs_scanner.hpp"
#include "kairos/watch/watch_engine.hpp"

#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <vector>

using namespace kairos::watch;
using namespace kairos::testing;
using namespace kairos::engine;
using namespace std::chrono_literals;

namespace {

// ── Recording span: captures all operations for assertion ────────────────

struct SpanRecord {
    std::string name;
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
        records_.push_back({name_, {}, false, false, ""});
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
        kairos::observability::SpanHandle& /*parent*/,
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

    std::unique_ptr<kairos::observability::SpanHandle> start_linked_span(
        kairos::observability::SpanHandle&,
        std::string_view name,
        std::unordered_map<std::string,
            kairos::observability::SpanAttribute> attrs) override
    {
        return start_span(name, std::move(attrs));
    }

    void flush() override {}

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

// ── Helper ───────────────────────────────────────────────────────────────

WatchGroupDef make_group() {
    WatchGroupDef g;
    g.group_name = "test_group";
    g.group_id = "wg-test";
    g.mode = WatchMode::Sample;
    g.sample_rate = 60s;
    g.watch_items = {"/watch"};
    g.max_depth = 5;
    g.max_files = 1000;
    g.enabled = true;
    return g;
}

auto T0 = std::chrono::system_clock::from_time_t(1700000000);

// ── Tests ────────────────────────────────────────────────────────────────

TEST(WatchScanSpansTest, ScanWithNullTracerDoesNotCrash) {
    FakeFilesystem fs;
    fs.add_file("/watch/a.txt", FakeFileEntry{.size = 100, .mtime = T0});
    FakeFilesystemScanner scanner(fs);
    kairos::SystemClockSource clock;

    auto g = make_group();
    WatchEngine engine(WatchEngineConfig{},
        WatchEngine::Dependencies{
            .clock = &clock,
            .scanner = &scanner,
            .tracer = nullptr,
        },
        {g});

    TriggerSink null_sink = [](TriggerEvent) { return true; };
    EXPECT_NO_THROW(engine.scan_once(null_sink));
}

TEST(WatchScanSpansTest, ScanWithNullTracerProducesScanResults) {
    FakeFilesystem fs;
    fs.add_file("/watch/a.txt", FakeFileEntry{.size = 100, .mtime = T0});
    fs.add_file("/watch/b.txt", FakeFileEntry{.size = 200, .mtime = T0});
    FakeFilesystemScanner scanner(fs);
    kairos::SystemClockSource clock;

    auto tracer = kairos::observability::create_tracer(false);
    auto g = make_group();

    WatchEngine engine(WatchEngineConfig{},
        WatchEngine::Dependencies{
            .clock = &clock,
            .scanner = &scanner,
            .tracer = tracer.get(),
        },
        {g});

    TriggerSink null_sink = [](TriggerEvent) { return true; };
    auto results = engine.scan_once(null_sink);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_GE(results[0].sample.entries.size(), 2u);
}

TEST(WatchScanSpansTest, ScanWithRecordingTracerCreatesSpans) {
    FakeFilesystem fs;
    fs.add_file("/watch/a.txt", FakeFileEntry{.size = 100, .mtime = T0});
    FakeFilesystemScanner scanner(fs);
    kairos::SystemClockSource clock;
    RecordingTracer tracer;

    auto g = make_group();
    WatchEngine engine(WatchEngineConfig{},
        WatchEngine::Dependencies{
            .clock = &clock,
            .scanner = &scanner,
            .tracer = &tracer,
        },
        {g});

    TriggerSink null_sink = [](TriggerEvent) { return true; };
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
        EXPECT_TRUE(r.ended) << "Span '" << r.name << "' was not ended";
    }
}

TEST(WatchScanSpansTest, SecondScanCreatesDiffSpan) {
    FakeFilesystem fs;
    fs.add_file("/watch/a.txt", FakeFileEntry{.size = 100, .mtime = T0});
    FakeFilesystemScanner scanner(fs);
    kairos::SystemClockSource clock;
    RecordingTracer tracer;

    auto g = make_group();
    WatchEngine engine(WatchEngineConfig{},
        WatchEngine::Dependencies{
            .clock = &clock,
            .scanner = &scanner,
            .tracer = &tracer,
        },
        {g});

    TriggerSink null_sink = [](TriggerEvent) { return true; };

    // First scan (baseline).
    engine.scan_once(null_sink);

    // Modify the filesystem.
    auto T1 = T0 + 60s;
    fs.add_file("/watch/c.txt", FakeFileEntry{.size = 300, .mtime = T1});

    // Second scan — should produce diff.
    engine.scan_once(null_sink);

    auto records = tracer.get_records();

    // Find the diff span from second scan.
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

TEST(WatchScanSpansTest, ScanSpanHasWatchGroupAttribute) {
    FakeFilesystem fs;
    fs.add_file("/watch/a.txt", FakeFileEntry{.size = 100, .mtime = T0});
    FakeFilesystemScanner scanner(fs);
    kairos::SystemClockSource clock;
    RecordingTracer tracer;

    auto g = make_group();
    WatchEngine engine(WatchEngineConfig{},
        WatchEngine::Dependencies{
            .clock = &clock,
            .scanner = &scanner,
            .tracer = &tracer,
        },
        {g});

    TriggerSink null_sink = [](TriggerEvent) { return true; };
    engine.scan_once(null_sink);

    auto records = tracer.get_records();
    ASSERT_GE(records.size(), 1u);

    const auto& root = records[0];
    EXPECT_EQ(root.name, "kairos.watch_scan");

    bool found_group = false;
    for (const auto& [k, v] : root.attributes) {
        if (k == "watch_group") {
            EXPECT_EQ(v, "test_group");
            found_group = true;
        }
    }
    EXPECT_TRUE(found_group) << "Expected 'watch_group' attribute on root span";
}

TEST(WatchScanSpansTest, AllSpansAreEnded) {
    FakeFilesystem fs;
    fs.add_file("/watch/a.txt", FakeFileEntry{.size = 100, .mtime = T0});
    FakeFilesystemScanner scanner(fs);
    kairos::SystemClockSource clock;
    RecordingTracer tracer;

    auto g = make_group();
    WatchEngine engine(WatchEngineConfig{},
        WatchEngine::Dependencies{
            .clock = &clock,
            .scanner = &scanner,
            .tracer = &tracer,
        },
        {g});

    TriggerSink null_sink = [](TriggerEvent) { return true; };

    // Two scans to exercise all span types.
    engine.scan_once(null_sink);
    auto T1 = T0 + 60s;
    fs.add_file("/watch/new.txt", FakeFileEntry{.size = 50, .mtime = T1});
    engine.scan_once(null_sink);

    auto records = tracer.get_records();
    for (const auto& r : records) {
        EXPECT_TRUE(r.ended)
            << "Span '" << r.name << "' was not properly ended (leak)";
    }
}

}  // anonymous namespace
