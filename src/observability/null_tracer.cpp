/// src/observability/null_tracer.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  null_tracer.cpp — No-op tracer implementation                           ║
// ║                                                                          ║
// ║  When KAIROS_OTEL is not defined (default), all tracing operations      ║
// ║  compile to no-ops. The compiler optimizes away the virtual calls.      ║
// ║                                                                          ║
// ║  When KAIROS_OTEL is defined, the OTelTracer (in otel_tracer.cpp)      ║
// ║  provides the real implementation, and this file's create_tracer         ║
// ║  is excluded via the #ifndef guard.                                     ║
// ║                                                                          ║
// ║  Spec reference: §21.4                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/observability/tracer.hpp"

namespace kairos::observability {

// ── NullSpan ─────────────────────────────────────────────────────────────

/// No-op span handle. All operations are empty — the compiler
/// devirtualizes and inlines these in optimized builds.
class NullSpan final : public SpanHandle {
public:
    void set_attribute(std::string_view /*key*/,
                       SpanAttribute /*value*/) override {}
    void set_error(std::string_view /*message*/) override {}
    void end() override {}

    [[nodiscard]] std::string trace_id() const override { return ""; }
    [[nodiscard]] std::string span_id() const override { return ""; }
    [[nodiscard]] std::string traceparent() const override { return ""; }
};

// ── NullTracer ───────────────────────────────────────────────────────────

/// No-op tracer. All span creation returns NullSpan instances.
class NullTracer final : public Tracer {
public:
    [[nodiscard]] std::unique_ptr<SpanHandle> start_span(
        std::string_view /*name*/,
        std::unordered_map<std::string, SpanAttribute> /*attrs*/) override
    {
        return std::make_unique<NullSpan>();
    }

    [[nodiscard]] std::unique_ptr<SpanHandle> start_child_span(
        SpanHandle& /*parent*/,
        std::string_view /*name*/,
        std::unordered_map<std::string, SpanAttribute> /*attrs*/) override
    {
        return std::make_unique<NullSpan>();
    }

    [[nodiscard]] std::unique_ptr<SpanHandle> start_linked_span(
        SpanHandle& /*link_target*/,
        std::string_view /*name*/,
        std::unordered_map<std::string, SpanAttribute> /*attrs*/) override
    {
        return std::make_unique<NullSpan>();
    }

    void flush() override {}
};

// ── Factory (when KAIROS_OTEL is OFF) ────────────────────────────────────

#ifndef KAIROS_OTEL

std::unique_ptr<Tracer> create_tracer(
    bool /*otel_enabled*/,
    std::string_view /*endpoint*/,
    std::string_view /*service_name*/)
{
    // Always return NullTracer when OTel is not compiled in.
    return std::make_unique<NullTracer>();
}

#endif  // !KAIROS_OTEL

}  // namespace kairos::observability
