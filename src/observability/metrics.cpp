/// src/observability/metrics.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  MetricsRegistry implementation                                          ║
// ║  Spec reference: §20.2–§20.4                                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/observability/metrics.hpp"

#include <iomanip>
#include <sstream>

namespace kairos::metrics {

// ── Label formatting ─────────────────────────────────────────────────────

std::string format_labels(const Labels& labels) {
    if (labels.empty()) return {};

    std::string result = "{";
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) result += ',';
        result += labels[i].first;
        result += "=\"";
        // Escape quotes and backslashes in label values.
        for (char c : labels[i].second) {
            if (c == '"' || c == '\\') result += '\\';
            result += c;
        }
        result += '"';
    }
    result += '}';
    return result;
}

// ── MetricsRegistry ──────────────────────────────────────────────────────

Counter* MetricsRegistry::register_counter(
    const std::string& name, const std::string& help,
    Labels labels) {
    std::lock_guard lock(mutex_);
    counters_.push_back(
        std::make_unique<Counter>(name, help, std::move(labels)));
    return counters_.back().get();
}

Gauge* MetricsRegistry::register_gauge(
    const std::string& name, const std::string& help,
    Labels labels) {
    std::lock_guard lock(mutex_);
    gauges_.push_back(
        std::make_unique<Gauge>(name, help, std::move(labels)));
    return gauges_.back().get();
}

Histogram* MetricsRegistry::register_histogram(
    const std::string& name, const std::string& help,
    std::vector<double> buckets, Labels labels) {
    std::lock_guard lock(mutex_);
    histograms_.push_back(
        std::make_unique<Histogram>(
            name, help, std::move(buckets), std::move(labels)));
    return histograms_.back().get();
}

std::size_t MetricsRegistry::instrument_count() const {
    std::lock_guard lock(mutex_);
    return counters_.size() + gauges_.size() + histograms_.size();
}

// ── Prometheus text exposition format ────────────────────────────────────

std::string MetricsRegistry::to_prometheus() const {
    std::lock_guard lock(mutex_);
    std::ostringstream out;
    out << std::fixed;

    // Counters.
    for (const auto& c : counters_) {
        out << "# HELP " << c->name() << " " << c->help() << "\n";
        out << "# TYPE " << c->name() << " counter\n";
        out << c->name() << format_labels(c->labels())
            << " " << c->value() << "\n";
    }

    // Gauges.
    for (const auto& g : gauges_) {
        out << "# HELP " << g->name() << " " << g->help() << "\n";
        out << "# TYPE " << g->name() << " gauge\n";
        double val = g->value();
        out << g->name() << format_labels(g->labels());
        // Format integer-valued gauges without decimal point.
        if (val == static_cast<int64_t>(val)) {
            out << " " << static_cast<int64_t>(val) << "\n";
        } else {
            out << std::setprecision(6) << " " << val << "\n";
        }
    }

    // Histograms.
    for (const auto& h : histograms_) {
        out << "# HELP " << h->name() << " " << h->help() << "\n";
        out << "# TYPE " << h->name() << " histogram\n";

        // Base labels for this histogram.
        auto base_labels = h->labels();
        std::string base_labels_str = format_labels(base_labels);

        // Bucket lines.
        const auto& bounds = h->upper_bounds();
        for (std::size_t i = 0; i < bounds.size(); ++i) {
            // Build labels with "le" added.
            Labels bucket_labels = base_labels;
            std::ostringstream le_val;
            // Format bucket boundary: integer if possible.
            if (bounds[i] == static_cast<int64_t>(bounds[i])) {
                le_val << static_cast<int64_t>(bounds[i]);
            } else {
                le_val << std::setprecision(6) << bounds[i];
            }
            bucket_labels.emplace_back("le", le_val.str());

            out << h->name() << "_bucket"
                << format_labels(bucket_labels)
                << " " << h->bucket_count(i) << "\n";
        }

        // +Inf bucket.
        Labels inf_labels = base_labels;
        inf_labels.emplace_back("le", "+Inf");
        out << h->name() << "_bucket"
            << format_labels(inf_labels)
            << " " << h->bucket_count(bounds.size()) << "\n";

        // Sum and count.
        double sum_val = h->sum();
        out << h->name() << "_sum" << base_labels_str;
        if (sum_val == static_cast<int64_t>(sum_val)) {
            out << " " << static_cast<int64_t>(sum_val) << "\n";
        } else {
            out << std::setprecision(6) << " " << sum_val << "\n";
        }
        out << h->name() << "_count" << base_labels_str
            << " " << h->count() << "\n";
    }

    return out.str();
}

// ── JSON serialization ──────────────────────────────────────────────────

std::string MetricsRegistry::to_json() const {
    std::lock_guard lock(mutex_);
    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    out << "{\"counters\":[";

    for (std::size_t i = 0; i < counters_.size(); ++i) {
        if (i > 0) out << ',';
        out << "{\"name\":\"" << counters_[i]->name()
            << "\",\"help\":\"" << counters_[i]->help()
            << "\",\"value\":" << counters_[i]->value();
        if (!counters_[i]->labels().empty()) {
            out << ",\"labels\":{";
            for (std::size_t j = 0; j < counters_[i]->labels().size(); ++j) {
                if (j > 0) out << ',';
                out << "\"" << counters_[i]->labels()[j].first
                    << "\":\"" << counters_[i]->labels()[j].second << "\"";
            }
            out << '}';
        }
        out << '}';
    }

    out << "],\"gauges\":[";
    for (std::size_t i = 0; i < gauges_.size(); ++i) {
        if (i > 0) out << ',';
        out << "{\"name\":\"" << gauges_[i]->name()
            << "\",\"help\":\"" << gauges_[i]->help()
            << "\",\"value\":" << gauges_[i]->value() << '}';
    }

    out << "],\"histograms\":[";
    for (std::size_t i = 0; i < histograms_.size(); ++i) {
        if (i > 0) out << ',';
        out << "{\"name\":\"" << histograms_[i]->name()
            << "\",\"help\":\"" << histograms_[i]->help()
            << "\",\"sum\":" << histograms_[i]->sum()
            << ",\"count\":" << histograms_[i]->count()
            << ",\"buckets\":[";
        const auto& bounds = histograms_[i]->upper_bounds();
        for (std::size_t j = 0; j <= bounds.size(); ++j) {
            if (j > 0) out << ',';
            out << "{\"le\":";
            if (j < bounds.size()) {
                out << bounds[j];
            } else {
                out << "\"+Inf\"";
            }
            out << ",\"count\":" << histograms_[i]->bucket_count(j) << '}';
        }
        out << "]}";
    }

    out << "]}";
    return out.str();
}

}  // namespace kairos::metrics
