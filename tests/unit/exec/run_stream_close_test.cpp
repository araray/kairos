/// tests/unit/exec/run_stream_close_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  RunStream on_close callback tests (§14.7 extension)                    ║
// ║                                                                          ║
// ║  Tests:                                                                  ║
// ║    1. on_close callback fires on close_run                              ║
// ║    2. on_close not called if unsubscribed before close                   ║
// ║    3. Multiple close callbacks all fire                                  ║
// ║    4. close_run for unregistered run is safe                            ║
// ║    5. on_close callback receives correct run_id                         ║
// ║    6. Output subscribers and close callbacks are independent             ║
// ║    7. close_run cleans up both subscribers and close callbacks           ║
// ║    8. on_close callback exception doesn't crash                         ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/exec/output_sink.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

TEST(RunStreamCloseTest, OnCloseFiresOnCloseRun) {
    kairos::exec::RunStream rs;

    bool fired = false;
    std::string received_id;

    rs.on_close("run-001", [&](const std::string& rid) {
        fired = true;
        received_id = rid;
    });

    rs.close_run("run-001");

    EXPECT_TRUE(fired);
    EXPECT_EQ(received_id, "run-001");
}

TEST(RunStreamCloseTest, OnCloseNotCalledIfUnsubscribed) {
    kairos::exec::RunStream rs;

    bool fired = false;
    auto id = rs.on_close("run-002", [&](const std::string&) {
        fired = true;
    });

    rs.unsubscribe(id);
    rs.close_run("run-002");

    EXPECT_FALSE(fired);
}

TEST(RunStreamCloseTest, MultipleCloseCallbacksAllFire) {
    kairos::exec::RunStream rs;

    int count = 0;
    rs.on_close("run-003", [&](const std::string&) { ++count; });
    rs.on_close("run-003", [&](const std::string&) { ++count; });
    rs.on_close("run-003", [&](const std::string&) { ++count; });

    rs.close_run("run-003");

    EXPECT_EQ(count, 3);
}

TEST(RunStreamCloseTest, CloseUnregisteredRunIsSafe) {
    kairos::exec::RunStream rs;

    EXPECT_NO_THROW(rs.close_run("nonexistent"));
}

TEST(RunStreamCloseTest, OnCloseReceivesCorrectRunId) {
    kairos::exec::RunStream rs;

    std::string received;
    rs.on_close("run-abc", [&](const std::string& rid) {
        received = rid;
    });

    // Close a different run first — should not fire our callback.
    rs.close_run("run-xyz");
    EXPECT_TRUE(received.empty());

    // Now close the right run.
    rs.close_run("run-abc");
    EXPECT_EQ(received, "run-abc");
}

TEST(RunStreamCloseTest, OutputAndCloseCallbacksIndependent) {
    kairos::exec::RunStream rs;

    bool output_fired = false;
    bool close_fired = false;

    rs.subscribe("run-004",
        [&](const std::string&, const std::string&,
            const std::string&, std::string_view, bool) {
            output_fired = true;
        });

    rs.on_close("run-004", [&](const std::string&) {
        close_fired = true;
    });

    // Publish an output chunk — only output callback should fire.
    rs.publish("run-004", "j", "s", "data", false);
    EXPECT_TRUE(output_fired);
    EXPECT_FALSE(close_fired);

    // Close — now close callback should fire.
    rs.close_run("run-004");
    EXPECT_TRUE(close_fired);
}

TEST(RunStreamCloseTest, CloseRunCleansUpBothTypes) {
    kairos::exec::RunStream rs;

    rs.subscribe("run-005",
        [](const std::string&, const std::string&,
           const std::string&, std::string_view, bool) {});
    rs.on_close("run-005", [](const std::string&) {});

    EXPECT_GE(rs.subscriber_count("run-005"), 1u);

    rs.close_run("run-005");

    EXPECT_EQ(rs.subscriber_count("run-005"), 0u);
}

TEST(RunStreamCloseTest, OnCloseExceptionDoesNotCrash) {
    kairos::exec::RunStream rs;

    int count = 0;
    rs.on_close("run-006", [](const std::string&) {
        throw std::runtime_error("intentional test exception");
    });
    rs.on_close("run-006", [&](const std::string&) {
        ++count;  // Should still fire even after the first throws.
    });

    EXPECT_NO_THROW(rs.close_run("run-006"));
    EXPECT_EQ(count, 1);
}

}  // anonymous namespace
