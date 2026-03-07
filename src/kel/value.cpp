/// src/kel/value.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  KelValue implementation                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/kel/value.hpp"
#include "kairos/kel/errors.hpp"

#include <sstream>

namespace kairos::kel {

double KelValue::to_double() const {
    if (is_int())   return static_cast<double>(as_int());
    if (is_float()) return as_float();
    throw KelEvalError("to_double() called on non-numeric type: " + type_name());
}

std::string KelValue::type_name() const {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>)        return "bool";
        if constexpr (std::is_same_v<T, int64_t>)     return "int";
        if constexpr (std::is_same_v<T, double>)       return "float";
        if constexpr (std::is_same_v<T, std::string>)  return "string";
        if constexpr (std::is_same_v<T, Duration>)     return "duration";
        if constexpr (std::is_same_v<T, KelList>)      return "list";
        return "unknown";
    }, data);
}

std::string KelValue::to_display_string() const {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) {
            return v ? "true" : "false";
        }
        if constexpr (std::is_same_v<T, int64_t>) {
            return std::to_string(v);
        }
        if constexpr (std::is_same_v<T, double>) {
            std::ostringstream oss;
            oss << v;
            return oss.str();
        }
        if constexpr (std::is_same_v<T, std::string>) {
            return "\"" + v + "\"";
        }
        if constexpr (std::is_same_v<T, Duration>) {
            auto ms = v.count();
            if (ms == 0) return "0s";

            std::string result;
            // Pick the largest clean unit.
            if (ms % 86400000 == 0)      result = std::to_string(ms / 86400000) + "d";
            else if (ms % 3600000 == 0)   result = std::to_string(ms / 3600000) + "h";
            else if (ms % 60000 == 0)     result = std::to_string(ms / 60000) + "m";
            else if (ms % 1000 == 0)      result = std::to_string(ms / 1000) + "s";
            else                          result = std::to_string(ms) + "ms";
            return result;
        }
        if constexpr (std::is_same_v<T, KelList>) {
            std::string result = "[";
            for (size_t i = 0; i < v.size(); ++i) {
                if (i > 0) result += ", ";
                if (i >= 5 && v.size() > 6) {
                    result += "...(";
                    result += std::to_string(v.size() - 5);
                    result += " more)";
                    break;
                }
                result += v[i].to_display_string();
            }
            result += "]";
            return result;
        }
        return "<?>";
    }, data);
}

bool KelValue::is_truthy() const {
    return std::visit([](const auto& v) -> bool {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>)        return v;
        if constexpr (std::is_same_v<T, int64_t>)     return v != 0;
        if constexpr (std::is_same_v<T, double>)       return v != 0.0;
        if constexpr (std::is_same_v<T, std::string>)  return !v.empty();
        if constexpr (std::is_same_v<T, Duration>)     return v.count() != 0;
        if constexpr (std::is_same_v<T, KelList>)      return !v.empty();
        return false;
    }, data);
}

bool KelValue::operator==(const KelValue& other) const {
    // Cross-type equality: if types differ, result is false (not error).
    if (data.index() != other.data.index()) {
        // Special case: int vs. float promote to float comparison.
        if (is_numeric() && other.is_numeric()) {
            return to_double() == other.to_double();
        }
        return false;
    }
    return data == other.data;
}

}  // namespace kairos::kel
