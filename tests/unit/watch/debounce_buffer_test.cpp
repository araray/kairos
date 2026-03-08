/// tests/unit/watch/debounce_buffer_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  DebounceBuffer unit tests — coalescing rules and timing behavior        ║
// ║                                                                           ║
// ║  Tests all coalescing combinations from spec §12.9:                      ║
// ║    - Created→Modified = Created (final state)                            ║
// ║    - Modified→Modified = Modified (single event)                         ║
// ║    - Created→Deleted = Nothing                                           ║
// ║    - Deleted→Created = Modified                                          ║
// ║    - Overflow events bypass debouncing                                   ║
// ║                                                                           ║
// ║  Spec reference: §12.9                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/watch/debounce_buffer.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace kairos::watch {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// Helper: make a NativeEvent with given type and path.
NativeEvent make_event(NativeEventType type, const std::string& path,
                        bool is_dir = false) {
    NativeEvent evt;
    evt.type = type;
    evt.path = fs::path(path);
    evt.is_directory = is_dir;
    evt.timestamp = std::chrono::system_clock::now();
    return evt;
}

// ── Construction ────────────────────────────────────────────────────────

TEST(DebounceBufferTest, DefaultConstruction) {
    DebounceBuffer buf;
    EXPECT_EQ(buf.pending_count(), 0);
    auto settled = buf.drain_settled();
    EXPECT_TRUE(settled.empty());
}

TEST(DebounceBufferTest, CustomDuration) {
    DebounceBuffer buf(500ms);
    EXPECT_EQ(buf.pending_count(), 0);
}

// ── Basic debouncing ────────────────────────────────────────────────────

TEST(DebounceBufferTest, SingleEventSettlesAfterDuration) {
    DebounceBuffer buf(200ms);
    auto t0 = Clock::now();

    buf.add(make_event(NativeEventType::Modified, "/a/b.txt"), t0);
    EXPECT_EQ(buf.pending_count(), 1);

    // Not settled yet at t0 + 100ms.
    auto early = buf.drain_settled(t0 + 100ms);
    EXPECT_TRUE(early.empty());
    EXPECT_EQ(buf.pending_count(), 1);

    // Settled at t0 + 200ms.
    auto settled = buf.drain_settled(t0 + 200ms);
    ASSERT_EQ(settled.size(), 1);
    EXPECT_EQ(settled[0].type, NativeEventType::Modified);
    EXPECT_EQ(settled[0].path.string(), "/a/b.txt");
    EXPECT_EQ(buf.pending_count(), 0);
}

TEST(DebounceBufferTest, NewEventResetsTimer) {
    DebounceBuffer buf(200ms);
    auto t0 = Clock::now();

    buf.add(make_event(NativeEventType::Modified, "/a/b.txt"), t0);
    buf.add(make_event(NativeEventType::Modified, "/a/b.txt"), t0 + 150ms);

    // Original would have settled at t0+200ms, but the second event
    // pushed it to t0+350ms.
    auto early = buf.drain_settled(t0 + 200ms);
    EXPECT_TRUE(early.empty());

    auto settled = buf.drain_settled(t0 + 350ms);
    ASSERT_EQ(settled.size(), 1);
    EXPECT_EQ(settled[0].type, NativeEventType::Modified);
}

TEST(DebounceBufferTest, DifferentPathsIndependent) {
    DebounceBuffer buf(200ms);
    auto t0 = Clock::now();

    buf.add(make_event(NativeEventType::Modified, "/a.txt"), t0);
    buf.add(make_event(NativeEventType::Created, "/b.txt"), t0 + 100ms);
    EXPECT_EQ(buf.pending_count(), 2);

    // /a.txt settles at t0+200ms, /b.txt at t0+300ms.
    auto first = buf.drain_settled(t0 + 200ms);
    ASSERT_EQ(first.size(), 1);
    EXPECT_EQ(first[0].path.string(), "/a.txt");

    auto second = buf.drain_settled(t0 + 300ms);
    ASSERT_EQ(second.size(), 1);
    EXPECT_EQ(second[0].path.string(), "/b.txt");
}

// ── Coalescing rules ────────────────────────────────────────────────────

TEST(DebounceBufferTest, CreatedThenModified_BecomesCreated) {
    // Created → Modified → Modified = Created (with final state)
    DebounceBuffer buf(200ms);
    auto t0 = Clock::now();

    buf.add(make_event(NativeEventType::Created, "/new.txt"), t0);
    buf.add(make_event(NativeEventType::Modified, "/new.txt"), t0 + 50ms);
    buf.add(make_event(NativeEventType::Modified, "/new.txt"), t0 + 100ms);

    auto settled = buf.drain_settled(t0 + 300ms);
    ASSERT_EQ(settled.size(), 1);
    EXPECT_EQ(settled[0].type, NativeEventType::Created);
    EXPECT_EQ(settled[0].path.string(), "/new.txt");
}

TEST(DebounceBufferTest, CreatedThenDeleted_ProducesNothing) {
    // Created → Deleted = Nothing (file appeared and vanished)
    DebounceBuffer buf(200ms);
    auto t0 = Clock::now();

    buf.add(make_event(NativeEventType::Created, "/tmp.txt"), t0);
    buf.add(make_event(NativeEventType::Deleted, "/tmp.txt"), t0 + 50ms);

    // Entry should have been removed.
    EXPECT_EQ(buf.pending_count(), 0);

    auto settled = buf.drain_settled(t0 + 300ms);
    EXPECT_TRUE(settled.empty());
}

TEST(DebounceBufferTest, DeletedThenCreated_BecomesModified) {
    // Deleted → Created = Modified (file was replaced)
    DebounceBuffer buf(200ms);
    auto t0 = Clock::now();

    buf.add(make_event(NativeEventType::Deleted, "/replaced.txt"), t0);
    buf.add(make_event(NativeEventType::Created, "/replaced.txt"), t0 + 50ms);

    auto settled = buf.drain_settled(t0 + 300ms);
    ASSERT_EQ(settled.size(), 1);
    EXPECT_EQ(settled[0].type, NativeEventType::Modified);
}

TEST(DebounceBufferTest, ModifiedThenModified_SingleModified) {
    // Modified → Modified → Modified = Modified (single event)
    DebounceBuffer buf(200ms);
    auto t0 = Clock::now();

    buf.add(make_event(NativeEventType::Modified, "/log.txt"), t0);
    buf.add(make_event(NativeEventType::Modified, "/log.txt"), t0 + 50ms);
    buf.add(make_event(NativeEventType::Modified, "/log.txt"), t0 + 100ms);

    auto settled = buf.drain_settled(t0 + 300ms);
    ASSERT_EQ(settled.size(), 1);
    EXPECT_EQ(settled[0].type, NativeEventType::Modified);
}

TEST(DebounceBufferTest, ModifiedThenDeleted_BecomesDeleted) {
    DebounceBuffer buf(200ms);
    auto t0 = Clock::now();

    buf.add(make_event(NativeEventType::Modified, "/old.txt"), t0);
    buf.add(make_event(NativeEventType::Deleted, "/old.txt"), t0 + 50ms);

    auto settled = buf.drain_settled(t0 + 300ms);
    ASSERT_EQ(settled.size(), 1);
    EXPECT_EQ(settled[0].type, NativeEventType::Deleted);
}

TEST(DebounceBufferTest, DeletedThenModified_BecomesModified) {
    // Deleted → Modified = Modified (file was replaced with modification)
    DebounceBuffer buf(200ms);
    auto t0 = Clock::now();

    buf.add(make_event(NativeEventType::Deleted, "/cfg.txt"), t0);
    buf.add(make_event(NativeEventType::Modified, "/cfg.txt"), t0 + 50ms);

    auto settled = buf.drain_settled(t0 + 300ms);
    ASSERT_EQ(settled.size(), 1);
    EXPECT_EQ(settled[0].type, NativeEventType::Modified);
}

// ── Overflow events ─────────────────────────────────────────────────────

TEST(DebounceBufferTest, OverflowEventsSettleImmediately) {
    DebounceBuffer buf(200ms);
    auto t0 = Clock::now();

    buf.add(make_event(NativeEventType::Overflow, "/"), t0);

    // Overflow settles immediately (last_activity set to past).
    auto settled = buf.drain_settled(t0);
    ASSERT_EQ(settled.size(), 1);
    EXPECT_EQ(settled[0].type, NativeEventType::Overflow);
}

TEST(DebounceBufferTest, ErrorEventsSettleImmediately) {
    DebounceBuffer buf(200ms);
    auto t0 = Clock::now();

    buf.add(make_event(NativeEventType::Error, "/bad"), t0);

    auto settled = buf.drain_settled(t0);
    ASSERT_EQ(settled.size(), 1);
    EXPECT_EQ(settled[0].type, NativeEventType::Error);
}

// ── time_until_next_settle ──────────────────────────────────────────────

TEST(DebounceBufferTest, TimeUntilNextSettle_Empty) {
    DebounceBuffer buf(200ms);
    auto result = buf.time_until_next_settle(Clock::now());
    EXPECT_EQ(result, std::chrono::milliseconds::max());
}

TEST(DebounceBufferTest, TimeUntilNextSettle_HasPending) {
    DebounceBuffer buf(200ms);
    auto t0 = Clock::now();

    buf.add(make_event(NativeEventType::Modified, "/x.txt"), t0);

    auto remaining = buf.time_until_next_settle(t0 + 100ms);
    EXPECT_GE(remaining.count(), 90);   // ~100ms remaining.
    EXPECT_LE(remaining.count(), 110);
}

TEST(DebounceBufferTest, TimeUntilNextSettle_AlreadySettled) {
    DebounceBuffer buf(200ms);
    auto t0 = Clock::now();

    buf.add(make_event(NativeEventType::Modified, "/x.txt"), t0);

    auto remaining = buf.time_until_next_settle(t0 + 300ms);
    EXPECT_EQ(remaining.count(), 0);
}

// ── clear ───────────────────────────────────────────────────────────────

TEST(DebounceBufferTest, ClearRemovesAll) {
    DebounceBuffer buf(200ms);
    auto t0 = Clock::now();

    buf.add(make_event(NativeEventType::Modified, "/a.txt"), t0);
    buf.add(make_event(NativeEventType::Created, "/b.txt"), t0);
    EXPECT_EQ(buf.pending_count(), 2);

    buf.clear();
    EXPECT_EQ(buf.pending_count(), 0);
}

// ── Directory events ────────────────────────────────────────────────────

TEST(DebounceBufferTest, DirectoryEventsPreserveFlag) {
    DebounceBuffer buf(200ms);
    auto t0 = Clock::now();

    buf.add(make_event(NativeEventType::Created, "/new_dir", true), t0);

    auto settled = buf.drain_settled(t0 + 200ms);
    ASSERT_EQ(settled.size(), 1);
    EXPECT_TRUE(settled[0].is_directory);
}

}  // namespace
}  // namespace kairos::watch
