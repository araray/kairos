/// tests/unit/watch/kel_aggregate_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  KEL aggregate()/previous() function tests — spec §7.7 category 3       ║
// ║                                                                           ║
// ║  Tests:                                                                   ║
// ║    - aggregate(data, glob, metric, func) with sum/min/max/avg/count      ║
// ║    - previous(group, glob, metric) via SQLite sample data                ║
// ║    - glob matching (*.log, specific filenames)                            ║
// ║    - edge cases: empty samples, no matches, unknown metrics              ║
// ║                                                                           ║
// ║  Spec reference: §7.7, §7.8                                              ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/kel/evaluator.hpp"
#include "kairos/kel/errors.hpp"
#include "kairos/persist/database.hpp"
#include "kairos/persist/db_writer.hpp"
#include "kairos/persist/migration.hpp"
#include "kairos/persist/query_reader.hpp"
#include "kairos/watch/sample.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

namespace kairos::watch {
namespace {

using namespace std::chrono_literals;

// ── Helper: build a sample with test files ──────────────────────────────

Sample make_test_sample() {
    Sample s;
    s.epoch = 1;

    FileMetrics f1;
    f1.path = "/var/log/app.log";
    f1.entry_type = "file";
    f1.size = 1000;
    f1.last_modified = std::chrono::system_clock::now() - 1h;
    s.entries[f1.path] = f1;

    FileMetrics f2;
    f2.path = "/var/log/error.log";
    f2.entry_type = "file";
    f2.size = 2500;
    f2.last_modified = std::chrono::system_clock::now() - 30min;
    f2.pattern_found = true;
    s.entries[f2.path] = f2;

    FileMetrics f3;
    f3.path = "/var/log/access.log";
    f3.entry_type = "file";
    f3.size = 5000;
    f3.last_modified = std::chrono::system_clock::now() - 2h;
    s.entries[f3.path] = f3;

    FileMetrics f4;
    f4.path = "/var/data/config.yml";
    f4.entry_type = "file";
    f4.size = 200;
    f4.last_modified = std::chrono::system_clock::now() - 24h;
    s.entries[f4.path] = f4;

    return s;
}

// ── Build a KEL context with aggregate() bound to the test sample ───────

kel::EvalContext make_aggregate_context(const Sample& sample) {
    auto ctx = kel::make_default_context();

    // Bind the data variable (sentinel).
    ctx.variables["data"] = kel::KelValue(std::string("__sample_data__"));

    // Register aggregate() capturing the sample.
    ctx.functions["aggregate"] =
        [&sample](const std::vector<kel::KelValue>& args) -> kel::KelValue
    {
        if (args.size() != 4) {
            throw kel::KelEvalError("aggregate() needs 4 args");
        }
        const auto& glob = args[1].as_string();
        const auto& metric = args[2].as_string();
        const auto& func = args[3].as_string();

        std::vector<double> values;
        for (const auto& [path, fm] : sample.entries) {
            // Match glob against filename.
            std::string fname = path;
            auto sep = path.find_last_of("/\\");
            if (sep != std::string::npos) fname = path.substr(sep + 1);

            // Simple glob match (only * supported in test).
            bool match = false;
            if (glob == "*") {
                match = true;
            } else if (glob.front() == '*' && glob.size() > 1) {
                auto suffix = glob.substr(1);
                match = (fname.size() >= suffix.size() &&
                         fname.compare(fname.size() - suffix.size(),
                                      suffix.size(), suffix) == 0);
            } else {
                match = (fname == glob);
            }

            if (!match) continue;

            if (metric == "size") {
                values.push_back(static_cast<double>(fm.size));
            } else if (metric == "pattern_found") {
                values.push_back(
                    (fm.pattern_found.has_value() && *fm.pattern_found)
                        ? 1.0 : 0.0);
            }
        }

        double result = 0.0;
        if (!values.empty()) {
            if (func == "sum") {
                for (double v : values) result += v;
            } else if (func == "count") {
                result = static_cast<double>(values.size());
            } else if (func == "min") {
                result = *std::min_element(values.begin(), values.end());
            } else if (func == "max") {
                result = *std::max_element(values.begin(), values.end());
            } else if (func == "avg") {
                for (double v : values) result += v;
                result /= static_cast<double>(values.size());
            }
        }

        if (result == static_cast<double>(static_cast<int64_t>(result))) {
            return kel::KelValue(static_cast<int64_t>(result));
        }
        return kel::KelValue(result);
    };

    return ctx;
}

// ═══════════════════════════════════════════════════════════════════════════
// aggregate() tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelAggregateTest, SumSizeAllFiles) {
    auto sample = make_test_sample();
    auto ctx = make_aggregate_context(sample);

    auto result = kel::eval_expression(
        R"(aggregate(data, "*", "size", "sum"))", ctx);
    // 1000 + 2500 + 5000 + 200 = 8700
    EXPECT_EQ(result.as_int(), 8700);
}

TEST(KelAggregateTest, SumSizeLogFiles) {
    auto sample = make_test_sample();
    auto ctx = make_aggregate_context(sample);

    auto result = kel::eval_expression(
        R"(aggregate(data, "*.log", "size", "sum"))", ctx);
    // 1000 + 2500 + 5000 = 8500
    EXPECT_EQ(result.as_int(), 8500);
}

TEST(KelAggregateTest, CountLogFiles) {
    auto sample = make_test_sample();
    auto ctx = make_aggregate_context(sample);

    auto result = kel::eval_expression(
        R"(aggregate(data, "*.log", "size", "count"))", ctx);
    EXPECT_EQ(result.as_int(), 3);
}

TEST(KelAggregateTest, MaxSizeLogFiles) {
    auto sample = make_test_sample();
    auto ctx = make_aggregate_context(sample);

    auto result = kel::eval_expression(
        R"(aggregate(data, "*.log", "size", "max"))", ctx);
    EXPECT_EQ(result.as_int(), 5000);
}

TEST(KelAggregateTest, MinSizeLogFiles) {
    auto sample = make_test_sample();
    auto ctx = make_aggregate_context(sample);

    auto result = kel::eval_expression(
        R"(aggregate(data, "*.log", "size", "min"))", ctx);
    EXPECT_EQ(result.as_int(), 1000);
}

TEST(KelAggregateTest, AvgSizeLogFiles) {
    auto sample = make_test_sample();
    auto ctx = make_aggregate_context(sample);

    auto result = kel::eval_expression(
        R"(aggregate(data, "*.log", "size", "avg"))", ctx);
    // (1000 + 2500 + 5000) / 3 ≈ 2833.33
    EXPECT_NEAR(result.to_double(), 2833.33, 1.0);
}

TEST(KelAggregateTest, CountPatternFound) {
    auto sample = make_test_sample();
    auto ctx = make_aggregate_context(sample);

    auto result = kel::eval_expression(
        R"(aggregate(data, "*.log", "pattern_found", "sum"))", ctx);
    // Only error.log has pattern_found=true → 1
    EXPECT_EQ(result.as_int(), 1);
}

TEST(KelAggregateTest, NoMatchingFilesReturnsZero) {
    auto sample = make_test_sample();
    auto ctx = make_aggregate_context(sample);

    auto result = kel::eval_expression(
        R"(aggregate(data, "*.xyz", "size", "sum"))", ctx);
    EXPECT_EQ(result.as_int(), 0);
}

TEST(KelAggregateTest, EmptySampleReturnsZero) {
    Sample empty;
    auto ctx = make_aggregate_context(empty);

    auto result = kel::eval_expression(
        R"(aggregate(data, "*", "size", "sum"))", ctx);
    EXPECT_EQ(result.as_int(), 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// previous() tests (via SQLite)
// ═══════════════════════════════════════════════════════════════════════════

class KelPreviousTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create in-memory database with schema.
        db_ = persist::open_database(":memory:");
    }

    void insert_sample_row(const std::string& group, int64_t epoch,
                            const std::string& path, int64_t size,
                            const std::string& hash = "") {
        SQLite::Statement stmt(*db_,
            "INSERT INTO watch_samples (watch_group, sample_epoch, "
            "file_path, file_size, hash) VALUES (?, ?, ?, ?, ?)");
        stmt.bind(1, group);
        stmt.bind(2, epoch);
        stmt.bind(3, path);
        stmt.bind(4, size);
        if (!hash.empty()) {
            stmt.bind(5, hash);
        }
        stmt.exec();
    }

    std::unique_ptr<SQLite::Database> db_;
};

TEST_F(KelPreviousTest, NoPriorSampleReturnsZero) {
    persist::QueryReader reader(*db_);
    auto ctx = kel::make_default_context();
    reader.register_watch_kel_bindings(ctx);

    auto result = kel::eval_expression(
        R"(previous("mygroup", "*.log", "size"))", ctx);
    EXPECT_EQ(result.as_int(), 0);
}

TEST_F(KelPreviousTest, SumSizeFromPriorSample) {
    // Insert sample data for epoch 5.
    insert_sample_row("logs", 5, "/var/log/app.log", 1000);
    insert_sample_row("logs", 5, "/var/log/error.log", 2500);
    insert_sample_row("logs", 5, "/var/data/config.yml", 200);

    persist::QueryReader reader(*db_);
    auto ctx = kel::make_default_context();
    reader.register_watch_kel_bindings(ctx);

    auto result = kel::eval_expression(
        R"(previous("logs", "*.log", "size"))", ctx);
    // Matches app.log (1000) + error.log (2500) = 3500
    EXPECT_EQ(result.as_int(), 3500);
}

TEST_F(KelPreviousTest, UsesLatestEpoch) {
    // Epoch 3 has old data.
    insert_sample_row("logs", 3, "/var/log/app.log", 500);

    // Epoch 5 has newer data — this should be used.
    insert_sample_row("logs", 5, "/var/log/app.log", 1000);

    persist::QueryReader reader(*db_);
    auto ctx = kel::make_default_context();
    reader.register_watch_kel_bindings(ctx);

    auto result = kel::eval_expression(
        R"(previous("logs", "*.log", "size"))", ctx);
    EXPECT_EQ(result.as_int(), 1000);
}

TEST_F(KelPreviousTest, NoMatchingFilesReturnsZero) {
    insert_sample_row("logs", 1, "/var/log/app.log", 1000);

    persist::QueryReader reader(*db_);
    auto ctx = kel::make_default_context();
    reader.register_watch_kel_bindings(ctx);

    auto result = kel::eval_expression(
        R"(previous("logs", "*.xyz", "size"))", ctx);
    EXPECT_EQ(result.as_int(), 0);
}

TEST_F(KelPreviousTest, WrongGroupReturnsZero) {
    insert_sample_row("logs", 1, "/var/log/app.log", 1000);

    persist::QueryReader reader(*db_);
    auto ctx = kel::make_default_context();
    reader.register_watch_kel_bindings(ctx);

    auto result = kel::eval_expression(
        R"(previous("other_group", "*.log", "size"))", ctx);
    EXPECT_EQ(result.as_int(), 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// aggregate() in condition expressions
// ═══════════════════════════════════════════════════════════════════════════

TEST(KelAggregateConditionTest, CompareWithThreshold) {
    auto sample = make_test_sample();
    auto ctx = make_aggregate_context(sample);

    // Total size of log files > 5000?
    auto result = kel::eval_expression(
        R"(aggregate(data, "*.log", "size", "sum") > 5000)", ctx);
    EXPECT_TRUE(result.as_bool());  // 8500 > 5000
}

TEST(KelAggregateConditionTest, CountComparison) {
    auto sample = make_test_sample();
    auto ctx = make_aggregate_context(sample);

    // More than 2 log files?
    auto result = kel::eval_expression(
        R"(aggregate(data, "*.log", "size", "count") > 2)", ctx);
    EXPECT_TRUE(result.as_bool());  // 3 > 2
}

}  // namespace
}  // namespace kairos::watch
