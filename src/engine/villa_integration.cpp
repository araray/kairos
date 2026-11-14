/// src/engine/villa_integration.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Villa integration contract v1 — pure helpers                            ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/engine/villa_integration.hpp"
#include "kairos/core/id_generator.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ctime>

namespace kairos::engine::villa {

std::string sha256_hex(std::string_view bytes) {
    auto d = core::sha256(bytes);
    return core::to_hex(d.data(), d.size());
}

std::string registry_digest(
    std::vector<std::pair<std::string, std::string>> pairs)
{
    std::sort(pairs.begin(), pairs.end());
    nlohmann::json arr = nlohmann::json::array();
    for (auto& [name, hash] : pairs) {
        arr.push_back(nlohmann::json::array({name, hash}));
    }
    // Compact dump: no whitespace, UTF-8 passthrough (ensure_ascii=false).
    return sha256_hex(arr.dump());
}

std::string format_rfc3339_ms(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    auto ms_total = duration_cast<milliseconds>(tp.time_since_epoch()).count();
    auto secs = ms_total / 1000;
    auto ms = ms_total % 1000;
    if (ms < 0) { ms += 1000; secs -= 1; }
    std::time_t tt = static_cast<std::time_t>(secs);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &tt);
#else
    gmtime_r(&tt, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03dZ", buf, static_cast<int>(ms));
    return out;
}

std::string canonical_trigger_json(const TriggerSpec& spec) {
    nlohmann::json j = std::visit([](const auto& t) -> nlohmann::json {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, CronTrigger>) {
            return {{"type", "cron"}, {"expression", t.expression}};
        } else if constexpr (std::is_same_v<T, IntervalTrigger>) {
            return {{"type", "interval"},
                    {"interval_ms", static_cast<int64_t>(t.interval.count())}};
        } else {
            return {{"type", "date"}, {"at", format_rfc3339_ms(t.fire_at)}};
        }
    }, spec);
    return j.dump();  // std::map-backed object → sorted keys, compact.
}

std::string trigger_digest(const TriggerSpec& spec) {
    return sha256_hex(canonical_trigger_json(spec));
}

std::optional<ScheduleIdentity> parse_schedule_name(std::string_view name) {
    static constexpr std::string_view kPrefix = "villa.schedule.sch_";
    if (name.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
    auto rest = name.substr(kPrefix.size());
    if (rest.size() < 32 + 3) return std::nullopt;  // hex + ".g" + digit
    auto hex = rest.substr(0, 32);
    for (char c : hex) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return std::nullopt;
    }
    auto tail = rest.substr(32);
    if (tail.substr(0, 2) != ".g") return std::nullopt;
    auto digits = tail.substr(2);
    if (digits.empty() || digits.size() > 18) return std::nullopt;
    int64_t gen = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') return std::nullopt;
        gen = gen * 10 + (c - '0');
    }
    return ScheduleIdentity{
        .schedule_id = "sch_" + std::string(hex),
        .generation = gen,
    };
}

std::optional<std::vector<std::string>> split_command_args(
    std::string_view cmd)
{
    std::vector<std::string> out;
    std::string cur;
    bool in_token = false;
    for (size_t i = 0; i < cmd.size(); ++i) {
        char c = cmd[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (in_token) { out.push_back(std::move(cur)); cur.clear(); }
            in_token = false;
            continue;
        }
        in_token = true;
        if (c == '\'') {
            auto end = cmd.find('\'', i + 1);
            if (end == std::string_view::npos) return std::nullopt;
            cur.append(cmd.substr(i + 1, end - i - 1));
            i = end;
        } else if (c == '"') {
            size_t j = i + 1;
            for (; j < cmd.size() && cmd[j] != '"'; ++j) {
                if (cmd[j] == '\\' && j + 1 < cmd.size() &&
                    (cmd[j + 1] == '"' || cmd[j + 1] == '\\')) {
                    ++j;
                }
                cur.push_back(cmd[j]);
            }
            if (j >= cmd.size()) return std::nullopt;
            i = j;
        } else if (c == '\\') {
            if (i + 1 >= cmd.size()) return std::nullopt;
            cur.push_back(cmd[++i]);
        } else {
            cur.push_back(c);
        }
    }
    if (in_token) out.push_back(std::move(cur));
    return out;
}

std::string random_id(std::string_view prefix) {
    return std::string(prefix) + core::generate_correlation_id();
}

}  // namespace kairos::engine::villa
