/// tests/unit/observability/otel_tracer_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  OTelTracer unit tests                                                   ║
// ║                                                                          ║
// ║  When KAIROS_OTEL is defined:                                           ║
// ║    Tests the real OTelTracer with span creation, attributes, errors,   ║
// ║    parent-child spans, linked spans, W3C traceparent format,           ║
// ║    and flush behavior.                                                  ║
// ║                                                                          ║
// ║  When KAIROS_OTEL is not defined:                                       ║
// ║    Tests that create_tracer returns a NullTracer regardless of the     ║
// ║    otel_enabled flag.                                                   ║
// ║                                                                          ║
// ║  Spec reference: §21.1–§21.6                                          ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include <gtest/gtest.h>
#include "kairos/observability/tracer.hpp"

namespace kairos::observability::test {

// ── Tests that work with NullTracer OR OTelTracer ───────────────────────

/// Verify that create_tracer(false) returns a working tracer
/// that produces spans with no-op behavior.
TEST(CreateTracerTest, DisabledReturnsWorkingTracer) {
    auto tracer = create_tracer(false, "", "kairos-test");
    ASSERT_NE(tracer, nullptr);

    // start_span should return a valid handle.
    auto span = tracer->start_span("test.noop");
    ASSERT_NE(span, nullptr);

    // Operations should not throw.
    span->set_attribute("key", std::string("value"));
    span->set_attribute("count", int64_t(42));
    span->set_attribute("ratio", 3.14);
    span->set_attribute("active", true);
    span->set_error("test error");
    span->end();

    // flush should not throw.
    tracer->flush();
}

/// Verify that NullTracer returns empty trace/span IDs.
TEST(CreateTracerTest, DisabledReturnsEmptyIds) {
    auto tracer = create_tracer(false);
    auto span = tracer->start_span("test.ids");
    ASSERT_NE(span, nullptr);

    EXPECT_EQ(span->trace_id(), "");
    EXPECT_EQ(span->span_id(), "");
    EXPECT_EQ(span->traceparent(), "");
}

/// Child spans from NullTracer should also be valid no-ops.
TEST(CreateTracerTest, DisabledChildSpanWorks) {
    auto tracer = create_tracer(false);
    auto parent = tracer->start_span("test.parent");
    ASSERT_NE(parent, nullptr);

    auto child = tracer->start_child_span(*parent, "test.child");
    ASSERT_NE(child, nullptr);

    child->set_attribute("step", std::string("compile"));
    child->end();
    parent->end();
}

/// Linked spans from NullTracer should also be valid no-ops.
TEST(CreateTracerTest, DisabledLinkedSpanWorks) {
    auto tracer = create_tracer(false);
    auto tick = tracer->start_span("schedule.tick");
    ASSERT_NE(tick, nullptr);

    auto run = tracer->start_linked_span(*tick, "kairos.run");
    ASSERT_NE(run, nullptr);

    run->set_attribute("run_id", std::string("run-abc123"));
    run->end();
    tick->end();
}

/// ScopedSpan RAII guard works with both tracer types.
TEST(ScopedSpanTest, AutoEndsOnDestruction) {
    auto tracer = create_tracer(false);
    {
        auto raw = tracer->start_span("test.scoped");
        ScopedSpan scoped(std::move(raw));
        scoped.set_attribute("key", std::string("value"));
        // span ends when scoped goes out of scope.
    }
    // No crash = success.
}

/// ScopedSpan error propagation.
TEST(ScopedSpanTest, ErrorPropagation) {
    auto tracer = create_tracer(false);
    {
        auto raw = tracer->start_span("test.error");
        ScopedSpan scoped(std::move(raw));
        scoped.mark_error("something went wrong");
        // Error is recorded when span ends in destructor.
    }
    // No crash = success.
}

/// ScopedSpan move semantics.
TEST(ScopedSpanTest, MoveSemantics) {
    auto tracer = create_tracer(false);
    auto raw = tracer->start_span("test.move");
    ScopedSpan scoped(std::move(raw));

    // Move to another ScopedSpan.
    ScopedSpan moved = std::move(scoped);
    moved.set_attribute("moved", true);
    // Original scoped should be empty (moved-from).
}

// ── OTel-specific tests (only when KAIROS_OTEL is compiled) ────────────

#ifdef KAIROS_OTEL

/// When OTel is compiled in and enabled, create_tracer should return
/// an OTelTracer that produces real trace/span IDs.
TEST(OTelTracerTest, EnabledReturnsRealTracer) {
    // Use a non-existent endpoint — we don't actually need to export.
    // The OTLP exporter will fail silently, which is fine for testing.
    auto tracer = create_tracer(
        true, "http://localhost:19999/v1/traces", "kairos-test");
    ASSERT_NE(tracer, nullptr);

    auto span = tracer->start_span("kairos.run", {
        {"run_id", std::string("run-test-123")},
        {"trigger_type", std::string("manual")},
    });
    ASSERT_NE(span, nullptr);

    // Real tracer should produce non-empty IDs.
    auto tid = span->trace_id();
    auto sid = span->span_id();
    EXPECT_EQ(tid.size(), 32u) << "Trace ID should be 32 hex chars";
    EXPECT_EQ(sid.size(), 16u) << "Span ID should be 16 hex chars";

    // traceparent should follow W3C format.
    auto tp = span->traceparent();
    EXPECT_TRUE(tp.starts_with("00-"))
        << "traceparent should start with version '00-'";
    EXPECT_EQ(tp.size(), 55u)
        << "traceparent should be 55 chars: 00-{32}-{16}-{2}";

    span->end();
    tracer->flush();
}

/// Parent-child span relationship.
TEST(OTelTracerTest, ChildSpanSharesTraceId) {
    auto tracer = create_tracer(
        true, "http://localhost:19999/v1/traces", "kairos-test");

    auto parent = tracer->start_span("kairos.run");
    auto child = tracer->start_child_span(*parent, "kairos.job");

    // Child should share the same trace ID.
    EXPECT_EQ(child->trace_id(), parent->trace_id());

    // But have a different span ID.
    EXPECT_NE(child->span_id(), parent->span_id());

    child->end();
    parent->end();
}

/// Linked spans have different trace IDs.
TEST(OTelTracerTest, LinkedSpanHasDifferentTraceId) {
    auto tracer = create_tracer(
        true, "http://localhost:19999/v1/traces", "kairos-test");

    auto tick = tracer->start_span("schedule.tick");
    auto run = tracer->start_linked_span(*tick, "kairos.run");

    // Linked span is a new root → different trace ID.
    EXPECT_NE(run->trace_id(), tick->trace_id());

    run->end();
    tick->end();
}

/// Error recording on a real span.
TEST(OTelTracerTest, ErrorRecording) {
    auto tracer = create_tracer(
        true, "http://localhost:19999/v1/traces", "kairos-test");

    auto span = tracer->start_span("kairos.step");
    span->set_error("exit code 1");
    span->end();

    // No crash = success. The error is recorded as an event.
}

/// Multiple attribute types.
TEST(OTelTracerTest, AttributeTypes) {
    auto tracer = create_tracer(
        true, "http://localhost:19999/v1/traces", "kairos-test");

    auto span = tracer->start_span("kairos.run");
    span->set_attribute("run_id", std::string("run-abc"));
    span->set_attribute("exit_code", int64_t(0));
    span->set_attribute("duration_s", 1.234);
    span->set_attribute("success", true);
    span->end();
}

/// Deep span hierarchy: run → job → step.
TEST(OTelTracerTest, DeepSpanHierarchy) {
    auto tracer = create_tracer(
        true, "http://localhost:19999/v1/traces", "kairos-test");

    auto run = tracer->start_span("kairos.run");
    auto job = tracer->start_child_span(*run, "kairos.job");
    auto step = tracer->start_child_span(*job, "kairos.step");

    // All share the same trace ID.
    EXPECT_EQ(job->trace_id(), run->trace_id());
    EXPECT_EQ(step->trace_id(), run->trace_id());

    // All have unique span IDs.
    EXPECT_NE(run->span_id(), job->span_id());
    EXPECT_NE(job->span_id(), step->span_id());

    step->end();
    job->end();
    run->end();
}

#else  // !KAIROS_OTEL

/// When OTel is NOT compiled, even requesting enabled=true should
/// return a NullTracer.
TEST(OTelTracerTest, WithoutOtelCompiledAlwaysReturnsNull) {
    auto tracer = create_tracer(true, "localhost:4317", "kairos-test");
    ASSERT_NE(tracer, nullptr);

    auto span = tracer->start_span("test");
    EXPECT_EQ(span->trace_id(), "");
    EXPECT_EQ(span->span_id(), "");
    span->end();
}

#endif  // KAIROS_OTEL

}  // namespace kairos::observability::test
