/// tests/unit/observability/tracer_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tracer abstraction unit tests                                           ║
// ║                                                                          ║
// ║  Tests NullTracer, NullSpan, ScopedSpan RAII, and create_tracer factory.║
// ║  When KAIROS_OTEL=OFF (default), all operations are no-ops.            ║
// ║                                                                          ║
// ║  Spec reference: §21.3–§21.4                                           ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/observability/tracer.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace kairos::observability;

// ── NullTracer factory tests ─────────────────────────────────────────────

TEST(TracerTest, CreateTracerReturnsNonNull) {
    auto tracer = create_tracer(false);
    ASSERT_NE(tracer, nullptr);
}

TEST(TracerTest, CreateTracerWithOtelFlagStillReturnsNullTracerWhenNotCompiled) {
    // When KAIROS_OTEL is not defined, requesting OTel still
    // returns a NullTracer (graceful degradation).
    auto tracer = create_tracer(true, "localhost:4317", "test-svc");
    ASSERT_NE(tracer, nullptr);
}

// ── NullSpan tests ──────────────────────────────────────────────────────

TEST(TracerTest, StartSpanReturnsNonNullHandle) {
    auto tracer = create_tracer();
    auto span = tracer->start_span("test.operation");
    ASSERT_NE(span, nullptr);
}

TEST(TracerTest, NullSpanReturnsEmptyTraceId) {
    auto tracer = create_tracer();
    auto span = tracer->start_span("test.op");
    EXPECT_EQ(span->trace_id(), "");
}

TEST(TracerTest, NullSpanReturnsEmptySpanId) {
    auto tracer = create_tracer();
    auto span = tracer->start_span("test.op");
    EXPECT_EQ(span->span_id(), "");
}

TEST(TracerTest, NullSpanReturnsEmptyTraceparent) {
    auto tracer = create_tracer();
    auto span = tracer->start_span("test.op");
    EXPECT_EQ(span->traceparent(), "");
}

TEST(TracerTest, NullSpanSetAttributeIsNoOp) {
    auto tracer = create_tracer();
    auto span = tracer->start_span("test.op");
    // These should not throw or crash.
    span->set_attribute("key", std::string("value"));
    span->set_attribute("count", int64_t{42});
    span->set_attribute("ratio", 3.14);
    span->set_attribute("flag", true);
}

TEST(TracerTest, NullSpanSetErrorIsNoOp) {
    auto tracer = create_tracer();
    auto span = tracer->start_span("test.op");
    span->set_error("something went wrong");
    // Should not throw.
}

TEST(TracerTest, NullSpanEndIsIdempotent) {
    auto tracer = create_tracer();
    auto span = tracer->start_span("test.op");
    span->end();
    span->end();  // Double-end should be safe.
}

// ── Child and linked spans ──────────────────────────────────────────────

TEST(TracerTest, StartChildSpanReturnsNonNull) {
    auto tracer = create_tracer();
    auto parent = tracer->start_span("parent");
    auto child = tracer->start_child_span(*parent, "child");
    ASSERT_NE(child, nullptr);
}

TEST(TracerTest, StartLinkedSpanReturnsNonNull) {
    auto tracer = create_tracer();
    auto target = tracer->start_span("target");
    auto linked = tracer->start_linked_span(*target, "linked.op");
    ASSERT_NE(linked, nullptr);
}

TEST(TracerTest, StartSpanWithAttributes) {
    auto tracer = create_tracer();
    auto span = tracer->start_span("test.op", {
        {"group", SpanAttribute{std::string("my-group")}},
        {"count", SpanAttribute{int64_t{100}}},
    });
    ASSERT_NE(span, nullptr);
}

TEST(TracerTest, FlushIsNoOp) {
    auto tracer = create_tracer();
    tracer->flush();  // Should not throw.
}

// ── ScopedSpan RAII tests ───────────────────────────────────────────────

TEST(ScopedSpanTest, ScopedSpanEndsOnDestruction) {
    auto tracer = create_tracer();
    {
        ScopedSpan scoped(tracer->start_span("scoped.op"));
        scoped.set_attribute("key", SpanAttribute{std::string("value")});
        // Span ends when scoped goes out of scope.
    }
    // Should not leak or crash.
}

TEST(ScopedSpanTest, ScopedSpanPropagatesError) {
    auto tracer = create_tracer();
    {
        ScopedSpan scoped(tracer->start_span("scoped.error"));
        scoped.mark_error("test failure");
        // Error is set on the span when scoped is destroyed.
    }
}

TEST(ScopedSpanTest, ScopedSpanAccessors) {
    auto tracer = create_tracer();
    ScopedSpan scoped(tracer->start_span("test.op"));

    EXPECT_EQ(scoped.trace_id(), "");
    EXPECT_EQ(scoped.span_id(), "");
    EXPECT_EQ(scoped.traceparent(), "");
}

TEST(ScopedSpanTest, ScopedSpanIsMoveConstructible) {
    auto tracer = create_tracer();
    ScopedSpan s1(tracer->start_span("test.op"));
    ScopedSpan s2(std::move(s1));
    // s2 now owns the span; should end cleanly.
}

TEST(ScopedSpanTest, ScopedSpanIsMoveAssignable) {
    auto tracer = create_tracer();
    ScopedSpan s1(tracer->start_span("op1"));
    ScopedSpan s2(tracer->start_span("op2"));
    s2 = std::move(s1);
    // s2 now owns op1's span; op2's span was ended by the assignment.
}

TEST(ScopedSpanTest, ScopedSpanHandleAccess) {
    auto tracer = create_tracer();
    ScopedSpan scoped(tracer->start_span("test.op"));
    SpanHandle& handle = scoped.handle();
    handle.set_attribute("via_handle", SpanAttribute{int64_t{1}});
}

// ── Edge cases ──────────────────────────────────────────────────────────

TEST(TracerTest, MultipleSpansSimultaneously) {
    auto tracer = create_tracer();
    auto span1 = tracer->start_span("op1");
    auto span2 = tracer->start_span("op2");
    auto span3 = tracer->start_child_span(*span1, "child1");

    span1->set_attribute("order", int64_t{1});
    span2->set_attribute("order", int64_t{2});
    span3->set_attribute("order", int64_t{3});

    span3->end();
    span2->end();
    span1->end();
}

TEST(TracerTest, SpanWithVariantAttributeTypes) {
    auto tracer = create_tracer();
    auto span = tracer->start_span("typed.attrs");

    span->set_attribute("str_val", SpanAttribute{std::string("hello")});
    span->set_attribute("int_val", SpanAttribute{int64_t{42}});
    span->set_attribute("dbl_val", SpanAttribute{3.14});
    span->set_attribute("bool_val", SpanAttribute{true});

    span->end();
}
