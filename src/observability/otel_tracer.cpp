/// src/observability/otel_tracer.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  OTelTracer — OpenTelemetry C++ SDK wrapper                             ║
// ║                                                                          ║
// ║  Only compiled when KAIROS_OTEL is defined (cmake -DKAIROS_OTEL=ON).   ║
// ║  Provides the real implementation of Tracer and SpanHandle.             ║
// ║                                                                          ║
// ║  Design:                                                                ║
// ║    - OTLP HTTP exporter when KAIROS_OTEL_OTLP is defined (system SDK). ║
// ║    - ostream exporter fallback (FetchContent — no protobuf needed).    ║
// ║    - TracerProvider is initialized once and stored as a shared_ptr.     ║
// ║    - OTelSpanHandle wraps opentelemetry::trace::Span.                  ║
// ║    - OTelTracer wraps opentelemetry::trace::Tracer.                    ║
// ║    - Trace context propagation via W3C traceparent (§21.5).            ║
// ║                                                                          ║
// ║  Span hierarchy (§21.2):                                               ║
// ║    kairos.run → kairos.plan → kairos.job → kairos.step                 ║
// ║    kairos.watch.scan → kairos.watch.rule                               ║
// ║    Schedule ticks use span links (not parent-child).                   ║
// ║                                                                          ║
// ║  Spec reference: §21.1–§21.6                                          ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#ifdef KAIROS_OTEL

#include "kairos/observability/tracer.hpp"

#ifdef KAIROS_OTEL_OTLP
#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#endif
#include <opentelemetry/exporters/ostream/span_exporter_factory.h>
#include <opentelemetry/common/attribute_value.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/simple_processor_factory.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/trace/tracer.h>
#include <opentelemetry/trace/span_context.h>
#include <opentelemetry/context/propagation/global_propagator.h>

#include <array>
#include <iomanip>
#include <sstream>

namespace kairos::observability {

// ── Hex formatting helpers ──────────────────────────────────────────────

namespace {

/// Convert a byte array to a hex string.
template <size_t N>
std::string to_hex(const std::array<uint8_t, N>& bytes) {
    std::ostringstream oss;
    for (auto b : bytes) {
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(b);
    }
    return oss.str();
}

/// Convert OTel TraceId (16 bytes) to 32-char hex string.
std::string trace_id_hex(
    const opentelemetry::trace::TraceId& id)
{
    std::array<uint8_t, 16> buf{};
    // TraceId::Id() returns a span<const uint8_t, 16>.
    auto span = id.Id();
    std::copy(span.begin(), span.end(), buf.begin());
    return to_hex(buf);
}

/// Convert OTel SpanId (8 bytes) to 16-char hex string.
std::string span_id_hex(
    const opentelemetry::trace::SpanId& id)
{
    std::array<uint8_t, 8> buf{};
    auto span = id.Id();
    std::copy(span.begin(), span.end(), buf.begin());
    return to_hex(buf);
}

/// Set OTel attribute value from our SpanAttribute variant.
void set_otel_attribute(
    opentelemetry::trace::Span& span,
    std::string_view key,
    const SpanAttribute& value)
{
    std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
            span.SetAttribute(
                opentelemetry::nostd::string_view(key.data(), key.size()),
                opentelemetry::nostd::string_view(v.data(), v.size()));
        } else if constexpr (std::is_same_v<T, int64_t>) {
            span.SetAttribute(
                opentelemetry::nostd::string_view(key.data(), key.size()),
                v);
        } else if constexpr (std::is_same_v<T, double>) {
            span.SetAttribute(
                opentelemetry::nostd::string_view(key.data(), key.size()),
                v);
        } else if constexpr (std::is_same_v<T, bool>) {
            span.SetAttribute(
                opentelemetry::nostd::string_view(key.data(), key.size()),
                v);
        }
    }, value);
}

}  // namespace

// ── OTelSpanHandle ──────────────────────────────────────────────────────

/// Wraps an opentelemetry::trace::Span as our SpanHandle abstraction.
class OTelSpanHandle final : public SpanHandle {
public:
    explicit OTelSpanHandle(
        opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span)
        : span_(std::move(span))
    {}

    void set_attribute(std::string_view key,
                       SpanAttribute value) override
    {
        if (span_) {
            set_otel_attribute(*span_, key, value);
        }
    }

    void set_error(std::string_view message) override {
        if (span_) {
            span_->SetStatus(
                opentelemetry::trace::StatusCode::kError,
                std::string(message));
            span_->AddEvent("exception",
                {{"exception.message",
                  opentelemetry::nostd::string_view(
                      message.data(), message.size())}});
        }
    }

    void end() override {
        if (span_) {
            span_->End();
        }
    }

    [[nodiscard]] std::string trace_id() const override {
        if (!span_) return "";
        auto ctx = span_->GetContext();
        return trace_id_hex(ctx.trace_id());
    }

    [[nodiscard]] std::string span_id() const override {
        if (!span_) return "";
        auto ctx = span_->GetContext();
        return span_id_hex(ctx.span_id());
    }

    [[nodiscard]] std::string traceparent() const override {
        if (!span_) return "";
        auto ctx = span_->GetContext();
        // W3C Trace Context: "00-{trace_id}-{span_id}-{flags}"
        std::string flags = ctx.IsSampled() ? "01" : "00";
        return "00-" + trace_id_hex(ctx.trace_id()) + "-" +
               span_id_hex(ctx.span_id()) + "-" + flags;
    }

    /// Access the underlying OTel span (for creating child spans).
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>
        otel_span() const
    {
        return span_;
    }

private:
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span_;
};

// ── OTelTracer ──────────────────────────────────────────────────────────

/// Real tracer implementation backed by the OpenTelemetry C++ SDK.
///
/// Initialization:
///   1. Create exporter:
///      - OTLP HTTP (when KAIROS_OTEL_OTLP defined) → collector endpoint
///      - ostream (fallback) → stderr
///   2. Create BatchSpanProcessor → batches spans for export
///   3. Create TracerProvider → owns the processor
///   4. Get a named Tracer ("kairos") → creates spans
class OTelTracer final : public Tracer {
public:
    OTelTracer(std::string_view endpoint,
               std::string_view service_name)
    {
        namespace sdk_trace = opentelemetry::sdk::trace;

#ifdef KAIROS_OTEL_OTLP
        // ── OTLP HTTP exporter (system SDK with protobuf) ─────────
        namespace otlp = opentelemetry::exporter::otlp;

        otlp::OtlpHttpExporterOptions exporter_opts;
        exporter_opts.url = std::string(endpoint);
        if (exporter_opts.url.empty()) {
            exporter_opts.url = "http://localhost:4318/v1/traces";
        }
        // If the endpoint doesn't start with http, prepend it.
        if (exporter_opts.url.find("://") == std::string::npos) {
            exporter_opts.url = "http://" + exporter_opts.url;
        }
        // If the endpoint doesn't end with /v1/traces, append it.
        if (exporter_opts.url.find("/v1/traces") == std::string::npos) {
            if (exporter_opts.url.back() != '/') {
                exporter_opts.url += "/";
            }
            exporter_opts.url += "v1/traces";
        }

        auto exporter = otlp::OtlpHttpExporterFactory::Create(
            exporter_opts);

        // Batch processor for OTLP (async, batched export).
        sdk_trace::BatchSpanProcessorOptions processor_opts;
        processor_opts.max_queue_size = 2048;
        processor_opts.schedule_delay_millis =
            std::chrono::milliseconds(5000);
        processor_opts.max_export_batch_size = 512;

        auto processor = sdk_trace::BatchSpanProcessorFactory::Create(
            std::move(exporter), processor_opts);
#else
        // ── ostream exporter (FetchContent fallback — no protobuf) ─
        // Exports spans to stderr in human-readable format.
        // Suitable for local development and debugging.
        (void)endpoint;  // Unused without OTLP.

        auto exporter =
            opentelemetry::exporter::trace::OStreamSpanExporterFactory::Create();

        // Simple processor for ostream (synchronous, no batching).
        auto processor = sdk_trace::SimpleSpanProcessorFactory::Create(
            std::move(exporter));
#endif

        // ── Configure resource (service name) ─────────────────────
        auto resource = opentelemetry::sdk::resource::Resource::Create({
            {"service.name",
             std::string(service_name)},
            {"service.version",
             std::string(KAIROS_VERSION)},
        });

        // ── Create tracer provider ────────────────────────────────
        // TracerProviderFactory::Create returns unique_ptr<sdk TracerProvider>.
        // We need nostd::shared_ptr<trace::TracerProvider> for the global.
        auto provider_uptr = sdk_trace::TracerProviderFactory::Create(
            std::move(processor), resource);
        provider_ = opentelemetry::nostd::shared_ptr<
            opentelemetry::trace::TracerProvider>(provider_uptr.release());

        // Set as global provider (allows OTel propagation to work).
        opentelemetry::trace::Provider::SetTracerProvider(provider_);

        // Get our named tracer.
        tracer_ = provider_->GetTracer(
            std::string(service_name), KAIROS_VERSION);
    }

    ~OTelTracer() override {
        flush();
    }

    [[nodiscard]] std::unique_ptr<SpanHandle> start_span(
        std::string_view name,
        std::unordered_map<std::string, SpanAttribute> attributes) override
    {
        opentelemetry::trace::StartSpanOptions opts;
        opts.kind = opentelemetry::trace::SpanKind::kInternal;

        auto span = tracer_->StartSpan(
            opentelemetry::nostd::string_view(name.data(), name.size()),
            opts);

        for (const auto& [key, value] : attributes) {
            set_otel_attribute(*span, key, value);
        }

        return std::make_unique<OTelSpanHandle>(std::move(span));
    }

    [[nodiscard]] std::unique_ptr<SpanHandle> start_child_span(
        SpanHandle& parent,
        std::string_view name,
        std::unordered_map<std::string, SpanAttribute> attributes) override
    {
        auto* otel_parent = dynamic_cast<OTelSpanHandle*>(&parent);
        if (!otel_parent || !otel_parent->otel_span()) {
            // Fallback: create a root span if parent is invalid.
            return start_span(name, std::move(attributes));
        }

        opentelemetry::trace::StartSpanOptions opts;
        opts.kind = opentelemetry::trace::SpanKind::kInternal;
        opts.parent = otel_parent->otel_span()->GetContext();

        auto span = tracer_->StartSpan(
            opentelemetry::nostd::string_view(name.data(), name.size()),
            opts);

        for (const auto& [key, value] : attributes) {
            set_otel_attribute(*span, key, value);
        }

        return std::make_unique<OTelSpanHandle>(std::move(span));
    }

    [[nodiscard]] std::unique_ptr<SpanHandle> start_linked_span(
        SpanHandle& link_target,
        std::string_view name,
        std::unordered_map<std::string, SpanAttribute> attributes) override
    {
        auto* otel_link = dynamic_cast<OTelSpanHandle*>(&link_target);

        opentelemetry::trace::StartSpanOptions opts;
        opts.kind = opentelemetry::trace::SpanKind::kInternal;

        // Create a link to the target span's context.
        opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span;
        if (otel_link && otel_link->otel_span()) {
            // Create with span link. Use explicit types to satisfy
            // the OTel SDK's template deduction requirements.
            auto link_ctx = otel_link->otel_span()->GetContext();

            // The StartSpan overload wants:
            //   (name, attributes_init_list, links_init_list, opts)
            // We pass empty attributes and one link with empty link-attrs.
            using AttrKV = std::pair<
                opentelemetry::nostd::string_view,
                opentelemetry::common::AttributeValue>;
            using LinkKV = std::pair<
                opentelemetry::trace::SpanContext,
                std::initializer_list<AttrKV>>;

            std::initializer_list<AttrKV> empty_attrs = {};
            std::initializer_list<LinkKV> links = {{link_ctx, {}}};

            span = tracer_->StartSpan(
                opentelemetry::nostd::string_view(name.data(), name.size()),
                empty_attrs,
                links,
                opts);
        } else {
            span = tracer_->StartSpan(
                opentelemetry::nostd::string_view(name.data(), name.size()),
                opts);
        }

        for (const auto& [key, value] : attributes) {
            set_otel_attribute(*span, key, value);
        }

        return std::make_unique<OTelSpanHandle>(std::move(span));
    }

    void flush() override {
        if (provider_) {
            // Force-flush all pending spans to the exporter.
            auto* sdk_provider = dynamic_cast<
                opentelemetry::sdk::trace::TracerProvider*>(
                    provider_.get());
            if (sdk_provider) {
                sdk_provider->ForceFlush(
                    std::chrono::milliseconds(5000));
            }
        }
    }

private:
    opentelemetry::nostd::shared_ptr<
        opentelemetry::trace::TracerProvider> provider_;
    opentelemetry::nostd::shared_ptr<
        opentelemetry::trace::Tracer> tracer_;
};

// ── Factory (when KAIROS_OTEL is ON) ────────────────────────────────────

std::unique_ptr<Tracer> create_tracer(
    bool otel_enabled,
    std::string_view endpoint,
    std::string_view service_name)
{
    if (!otel_enabled) {
        // Even with KAIROS_OTEL compiled in, the user can disable
        // tracing at runtime via config (kairos.otel.enabled=false).
        // Return the NullTracer from null_tracer.cpp? No — that's
        // guarded by #ifndef KAIROS_OTEL. We need an inline NullTracer.
        //
        // Actually, since null_tracer.cpp's create_tracer is guarded
        // by #ifndef KAIROS_OTEL, and this file IS compiled when
        // KAIROS_OTEL is defined, we must handle the disabled case here.
        class InlineNullSpan final : public SpanHandle {
        public:
            void set_attribute(std::string_view, SpanAttribute) override {}
            void set_error(std::string_view) override {}
            void end() override {}
            std::string trace_id() const override { return ""; }
            std::string span_id() const override { return ""; }
            std::string traceparent() const override { return ""; }
        };

        class InlineNullTracer final : public Tracer {
        public:
            std::unique_ptr<SpanHandle> start_span(
                std::string_view,
                std::unordered_map<std::string, SpanAttribute>) override
            {
                return std::make_unique<InlineNullSpan>();
            }
            std::unique_ptr<SpanHandle> start_child_span(
                SpanHandle&, std::string_view,
                std::unordered_map<std::string, SpanAttribute>) override
            {
                return std::make_unique<InlineNullSpan>();
            }
            std::unique_ptr<SpanHandle> start_linked_span(
                SpanHandle&, std::string_view,
                std::unordered_map<std::string, SpanAttribute>) override
            {
                return std::make_unique<InlineNullSpan>();
            }
            void flush() override {}
        };

        return std::make_unique<InlineNullTracer>();
    }

    return std::make_unique<OTelTracer>(endpoint, service_name);
}

}  // namespace kairos::observability

#endif  // KAIROS_OTEL
