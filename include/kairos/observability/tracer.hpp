/// include/kairos/observability/tracer.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/observability/tracer.hpp — Distributed tracing abstraction       ║
// ║                                                                          ║
// ║  Thin abstraction over OpenTelemetry C++ SDK (when KAIROS_OTEL=ON)      ║
// ║  or a NullTracer (zero overhead when KAIROS_OTEL=OFF).                  ║
// ║                                                                          ║
// ║  The NullTracer is always available as the default. The OTelTracer      ║
// ║  is conditionally compiled behind the KAIROS_OTEL build flag.           ║
// ║                                                                          ║
// ║  Key design: The ScopedSpan RAII guard starts a span on construction    ║
// ║  and ends it on destruction, with automatic error propagation.          ║
// ║                                                                          ║
// ║  Spec reference: §21.1–§21.5                                           ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>

namespace kairos::observability {

// ── Span attribute value ─────────────────────────────────────────────────

/// Attribute value: string, int64, double, or bool.
using SpanAttribute = std::variant<std::string, int64_t, double, bool>;

// ── SpanHandle ───────────────────────────────────────────────────────────

/// Opaque span handle. Represents an active or finished trace span.
///
/// The real implementation wraps opentelemetry::trace::Span when
/// KAIROS_OTEL=ON; the null implementation is a no-op.
class SpanHandle {
public:
    virtual ~SpanHandle() = default;

    /// Set an attribute on this span.
    virtual void set_attribute(std::string_view key,
                               SpanAttribute value) = 0;

    /// Record an error event on this span.
    virtual void set_error(std::string_view message) = 0;

    /// End the span (records the end timestamp).
    /// After this call, the span is immutable.
    virtual void end() = 0;

    /// Get the trace ID as a hex string (32 chars).
    [[nodiscard]] virtual std::string trace_id() const = 0;

    /// Get the span ID as a hex string (16 chars).
    [[nodiscard]] virtual std::string span_id() const = 0;

    /// Get the W3C traceparent header value for propagation.
    /// Format: "00-{trace_id}-{span_id}-01"
    [[nodiscard]] virtual std::string traceparent() const = 0;
};

// ── Tracer ───────────────────────────────────────────────────────────────

/// Kairos tracer interface. Abstracts over OTel SDK or null impl.
class Tracer {
public:
    virtual ~Tracer() = default;

    /// Start a new root span (no parent).
    [[nodiscard]] virtual std::unique_ptr<SpanHandle> start_span(
        std::string_view name,
        std::unordered_map<std::string, SpanAttribute> attributes = {})
        = 0;

    /// Start a child span under the given parent.
    [[nodiscard]] virtual std::unique_ptr<SpanHandle> start_child_span(
        SpanHandle& parent,
        std::string_view name,
        std::unordered_map<std::string, SpanAttribute> attributes = {})
        = 0;

    /// Start a span linked to another span (e.g., schedule_tick → run).
    /// A link captures causal relationship without implying parent-child
    /// duration semantics.
    [[nodiscard]] virtual std::unique_ptr<SpanHandle> start_linked_span(
        SpanHandle& link_target,
        std::string_view name,
        std::unordered_map<std::string, SpanAttribute> attributes = {})
        = 0;

    /// Flush pending spans to the exporter.
    virtual void flush() = 0;
};

// ── ScopedSpan (RAII guard) ──────────────────────────────────────────────

/// RAII guard: starts a span on construction, ends it on destruction.
/// Sets error status automatically if the guarded scope exits via
/// explicit mark_error().
///
/// Usage:
///   auto span = tracer->start_span("watch.scan");
///   ScopedSpan scoped(std::move(span));
///   scoped.set_attribute("group", group_name);
///   // ... do work ...
///   // span ends automatically when scoped goes out of scope
class ScopedSpan {
public:
    explicit ScopedSpan(std::unique_ptr<SpanHandle> span)
        : span_(std::move(span)) {}

    ~ScopedSpan() {
        if (span_) {
            if (error_) {
                span_->set_error(error_message_);
            }
            span_->end();
        }
    }

    // Non-copyable, movable.
    ScopedSpan(const ScopedSpan&) = delete;
    ScopedSpan& operator=(const ScopedSpan&) = delete;
    ScopedSpan(ScopedSpan&&) = default;
    ScopedSpan& operator=(ScopedSpan&&) = default;

    /// Access the underlying span handle.
    SpanHandle& handle() { return *span_; }
    const SpanHandle& handle() const { return *span_; }

    /// Set an attribute on the span.
    void set_attribute(std::string_view key, SpanAttribute value) {
        if (span_) span_->set_attribute(key, std::move(value));
    }

    /// Mark this span as errored. The error message will be recorded
    /// when the span ends (destructor).
    void mark_error(std::string_view message) {
        error_ = true;
        error_message_ = std::string(message);
    }

    /// Get the trace ID (empty string for NullTracer).
    [[nodiscard]] std::string trace_id() const {
        return span_ ? span_->trace_id() : "";
    }

    /// Get the span ID (empty string for NullTracer).
    [[nodiscard]] std::string span_id() const {
        return span_ ? span_->span_id() : "";
    }

    /// Get the W3C traceparent value (empty string for NullTracer).
    [[nodiscard]] std::string traceparent() const {
        return span_ ? span_->traceparent() : "";
    }

private:
    std::unique_ptr<SpanHandle> span_;
    bool error_ = false;
    std::string error_message_;
};

// ── Factory ──────────────────────────────────────────────────────────────

/// Create the appropriate tracer based on build flags and config.
///
/// If KAIROS_OTEL=ON and otel_enabled=true:
///   Returns an OTelTracer backed by the OpenTelemetry SDK.
/// Otherwise:
///   Returns a NullTracer (all operations are no-ops).
///
/// @param otel_enabled  Enable OTel tracing (ignored when KAIROS_OTEL=OFF).
/// @param endpoint      OTLP exporter endpoint (e.g., "localhost:4317").
/// @param service_name  Service name for the tracer (default: "kairos").
std::unique_ptr<Tracer> create_tracer(
    bool otel_enabled = false,
    std::string_view endpoint = "",
    std::string_view service_name = "kairos");

}  // namespace kairos::observability
