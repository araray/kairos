/// tests/unit/persist/metrics_snapshot_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for metrics snapshot persistence (§20.5)                           ║
// ║  Deliverable 12.1: InsertMetricsSnapshot, BatchInsertMetricsSnapshots,   ║
// ║                     MetricsRegistry::snapshot_entries()                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/observability/metrics.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/persist/query_reader.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

namespace kairos::test {

class MetricsSnapshotTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = fs::temp_directory_path() /
                   ("kairos_msnapshot_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this)) +
                    ".db");
        db_ = persist::open_database(db_path_);
        persist::init_database(db_path_);
    }

    void TearDown() override {
        db_.reset();
        std::error_code ec;
        fs::remove(db_path_, ec);
        fs::remove(fs::path(db_path_.string() + "-wal"), ec);
        fs::remove(fs::path(db_path_.string() + "-shm"), ec);
    }

    fs::path db_path_;
    std::unique_ptr<SQLite::Database> db_;
};

// ═══════════════════════════════════════════════════════════════════════════
// MetricsRegistry::snapshot_entries
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(MetricsSnapshotTest, SnapshotEntries_EmptyRegistry) {
    metrics::MetricsRegistry registry;
    auto entries = registry.snapshot_entries();
    EXPECT_TRUE(entries.empty());
}

TEST_F(MetricsSnapshotTest, SnapshotEntries_CountersGaugesHistograms) {
    metrics::MetricsRegistry registry;

    auto* c = registry.register_counter("test_counter", "A counter",
                                         {{"env", "prod"}});
    c->increment(42);

    auto* g = registry.register_gauge("test_gauge", "A gauge");
    g->set(3.14);

    auto* h = registry.register_histogram("test_histogram", "A histogram");
    h->observe(100.0);
    h->observe(200.0);

    auto entries = registry.snapshot_entries();

    ASSERT_EQ(entries.size(), 3u);

    // Counter.
    EXPECT_EQ(entries[0].metric_name, "test_counter");
    EXPECT_EQ(entries[0].metric_type, "counter");
    EXPECT_DOUBLE_EQ(entries[0].value, 42.0);
    EXPECT_EQ(entries[0].labels_json, R"({"env":"prod"})");

    // Gauge.
    EXPECT_EQ(entries[1].metric_name, "test_gauge");
    EXPECT_EQ(entries[1].metric_type, "gauge");
    EXPECT_NEAR(entries[1].value, 3.14, 0.001);
    EXPECT_TRUE(entries[1].labels_json.empty());

    // Histogram (sum value).
    EXPECT_EQ(entries[2].metric_name, "test_histogram");
    EXPECT_EQ(entries[2].metric_type, "histogram");
    EXPECT_DOUBLE_EQ(entries[2].value, 300.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// InsertMetricsSnapshot via DBWriter
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(MetricsSnapshotTest, SingleInsert_PersistsToTable) {
    persist::DBWriterConfig cfg;
    persist::DBWriter writer(*db_, cfg);

    std::stop_source ss;
    writer.start(ss.get_token());

    persist::InsertMetricsSnapshot snap;
    snap.metric_name = "kairos_test_gauge";
    snap.metric_type = "gauge";
    snap.value = 99.5;
    snap.labels_json = R"({"host":"localhost"})";

    writer.enqueue(std::move(snap));

    // Flush.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ss.request_stop();
    writer.flush();

    // Verify.
    SQLite::Statement q(*db_,
        "SELECT metric_name, metric_type, value, labels_json "
        "FROM metrics_snapshots");
    ASSERT_TRUE(q.executeStep());
    EXPECT_EQ(q.getColumn(0).getString(), "kairos_test_gauge");
    EXPECT_EQ(q.getColumn(1).getString(), "gauge");
    EXPECT_DOUBLE_EQ(q.getColumn(2).getDouble(), 99.5);
    EXPECT_EQ(q.getColumn(3).getString(), R"({"host":"localhost"})");
}

TEST_F(MetricsSnapshotTest, BatchInsert_PersistsAllEntries) {
    persist::DBWriterConfig cfg;
    persist::DBWriter writer(*db_, cfg);

    std::stop_source ss;
    writer.start(ss.get_token());

    persist::BatchInsertMetricsSnapshots batch;
    batch.entries.push_back({
        .metric_name = "counter_a",
        .metric_type = "counter",
        .value = 10.0,
        .labels_json = "",
    });
    batch.entries.push_back({
        .metric_name = "gauge_b",
        .metric_type = "gauge",
        .value = 3.14,
        .labels_json = R"({"x":"y"})",
    });

    writer.enqueue(std::move(batch));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ss.request_stop();
    writer.flush();

    // Verify.
    SQLite::Statement q(*db_,
        "SELECT COUNT(*) FROM metrics_snapshots");
    ASSERT_TRUE(q.executeStep());
    EXPECT_EQ(q.getColumn(0).getInt(), 2);
}

// ═══════════════════════════════════════════════════════════════════════════
// End-to-end: snapshot_entries → DBWriter → QueryReader
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(MetricsSnapshotTest, EndToEnd_RegistryToDB) {
    metrics::MetricsRegistry registry;
    auto* c = registry.register_counter("e2e_counter", "test");
    c->increment(7);
    auto* g = registry.register_gauge("e2e_gauge", "test");
    g->set(42.0);

    // Snapshot entries.
    auto entries = registry.snapshot_entries();

    // Write via DBWriter.
    persist::DBWriterConfig cfg;
    persist::DBWriter writer(*db_, cfg);
    std::stop_source ss;
    writer.start(ss.get_token());

    persist::BatchInsertMetricsSnapshots batch;
    for (auto& e : entries) {
        batch.entries.push_back({
            .metric_name = std::move(e.metric_name),
            .metric_type = std::move(e.metric_type),
            .value = e.value,
            .labels_json = std::move(e.labels_json),
        });
    }
    writer.enqueue(std::move(batch));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ss.request_stop();
    writer.flush();

    // Read back via QueryReader.
    persist::QueryReader reader(*db_);
    auto rows = reader.query_metrics_snapshots(10);

    ASSERT_EQ(rows.size(), 2u);

    // Verify we got both metrics.
    bool found_counter = false, found_gauge = false;
    for (const auto& r : rows) {
        if (r.metric_name == "e2e_counter") {
            found_counter = true;
            EXPECT_DOUBLE_EQ(r.value, 7.0);
        }
        if (r.metric_name == "e2e_gauge") {
            found_gauge = true;
            EXPECT_DOUBLE_EQ(r.value, 42.0);
        }
    }
    EXPECT_TRUE(found_counter);
    EXPECT_TRUE(found_gauge);
}

}  // namespace kairos::test
