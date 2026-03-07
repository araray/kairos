/// include/kairos/kel/value.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/kel/value.hpp — KEL type system (6 types, no null)                ║
// ║                                                                           ║
// ║  Types: bool, int64_t, double, string, duration (ms), list               ║
// ║  No null, no maps, no user-defined types.                                ║
// ║                                                                           ║
// ║  Spec reference: §7.2                                                     ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace kairos::kel {

struct KelValue;

/// A list of KEL values (may contain mixed types).
using KelList = std::vector<KelValue>;

/// The value type for KEL expressions.
struct KelValue {
    using Duration = std::chrono::milliseconds;

    std::variant<
        bool,
        int64_t,
        double,
        std::string,
        Duration,
        KelList
    > data;

    // ─── Constructors ─────────────────────────────────────────
    KelValue() : data(false) {}
    KelValue(bool v) : data(v) {}
    KelValue(int64_t v) : data(v) {}
    KelValue(int v) : data(static_cast<int64_t>(v)) {}
    KelValue(double v) : data(v) {}
    KelValue(std::string v) : data(std::move(v)) {}
    KelValue(const char* v) : data(std::string(v)) {}
    KelValue(Duration v) : data(v) {}
    KelValue(KelList v) : data(std::move(v)) {}

    // ─── Type queries ─────────────────────────────────────────
    [[nodiscard]] bool is_bool()     const { return std::holds_alternative<bool>(data); }
    [[nodiscard]] bool is_int()      const { return std::holds_alternative<int64_t>(data); }
    [[nodiscard]] bool is_float()    const { return std::holds_alternative<double>(data); }
    [[nodiscard]] bool is_string()   const { return std::holds_alternative<std::string>(data); }
    [[nodiscard]] bool is_duration() const { return std::holds_alternative<Duration>(data); }
    [[nodiscard]] bool is_list()     const { return std::holds_alternative<KelList>(data); }
    [[nodiscard]] bool is_numeric()  const { return is_int() || is_float(); }

    // ─── Accessors (throw std::bad_variant_access on mismatch) ─
    [[nodiscard]] bool               as_bool()     const { return std::get<bool>(data); }
    [[nodiscard]] int64_t            as_int()      const { return std::get<int64_t>(data); }
    [[nodiscard]] double             as_float()    const { return std::get<double>(data); }
    [[nodiscard]] const std::string& as_string()   const { return std::get<std::string>(data); }
    [[nodiscard]] Duration           as_duration() const { return std::get<Duration>(data); }
    [[nodiscard]] const KelList&     as_list()     const { return std::get<KelList>(data); }

    /// Convert numeric types to double for mixed arithmetic.
    [[nodiscard]] double to_double() const;

    /// Human-readable type name for error messages.
    [[nodiscard]] std::string type_name() const;

    /// Human-readable value representation for error messages.
    [[nodiscard]] std::string to_display_string() const;

    /// Truthiness for bool() conversion:
    ///   bool   → value itself
    ///   int    → 0 is false
    ///   float  → 0.0 is false
    ///   string → empty is false
    ///   duration → zero is false
    ///   list   → empty is false
    [[nodiscard]] bool is_truthy() const;

    // ─── Equality ─────────────────────────────────────────────
    bool operator==(const KelValue& other) const;
    bool operator!=(const KelValue& other) const { return !(*this == other); }
};

}  // namespace kairos::kel
