/// src/kel/evaluator.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  KEL Evaluator — AST walker with sandboxing                               ║
// ║                                                                           ║
// ║  Key behaviors:                                                           ║
// ║    • Short-circuit evaluation for 'and' / 'or'                            ║
// ║    • Type promotion: int + float → float                                  ║
// ║    • Cross-type == returns false (not error)                               ║
// ║    • Ordered comparison requires matching types                           ║
// ║    • Time-bounded evaluation (checks deadline at each node)               ║
// ║    • String/list length limits on results                                 ║
// ║                                                                           ║
// ║  Spec reference: §7.6–§7.9                                               ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/kel/evaluator.hpp"
#include "kairos/kel/ast.hpp"
#include "kairos/kel/errors.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <regex>
#include <stdexcept>

namespace kairos::kel {

namespace {

using Clock = std::chrono::steady_clock;

// ═══════════════════════════════════════════════════════════════════════════
// Evaluation state (tracks deadline and limits)
// ═══════════════════════════════════════════════════════════════════════════

struct EvalState {
    const EvalContext& ctx;
    const EvalLimits&  limits;
    Clock::time_point  deadline;
    uint32_t           nodes_evaluated = 0;

    EvalState(const EvalContext& c, const EvalLimits& l)
        : ctx(c), limits(l),
          deadline(Clock::now() + std::chrono::milliseconds(l.max_eval_time_ms)) {}

    void check_deadline() {
        if (Clock::now() > deadline) {
            throw KelLimitError("evaluation time limit exceeded ("
                + std::to_string(limits.max_eval_time_ms) + "ms)");
        }
    }

    void check_node_limit() {
        ++nodes_evaluated;
        if (nodes_evaluated > limits.max_ast_nodes * 2) {
            // The *2 gives headroom for re-evaluation (short-circuit
            // skips prevent this from being precisely 1:1 with AST nodes).
            throw KelLimitError("evaluation node limit exceeded");
        }
    }

    void check_string_length(const std::string& s) {
        if (s.size() > limits.max_string_length) {
            throw KelLimitError("string result exceeds maximum length ("
                + std::to_string(limits.max_string_length) + ")");
        }
    }

    void check_list_length(const KelList& lst) {
        if (lst.size() > limits.max_list_length) {
            throw KelLimitError("list result exceeds maximum length ("
                + std::to_string(limits.max_list_length) + ")");
        }
    }
};

// Forward declaration.
KelValue eval_node(const AstNode& node, EvalState& state);

// ═══════════════════════════════════════════════════════════════════════════
// Binary operator evaluation
// ═══════════════════════════════════════════════════════════════════════════

KelValue eval_binary(const BinaryOpNode& node, EvalState& state) {
    // Short-circuit for logical operators.
    if (node.op == TokenType::And) {
        auto left = eval_node(*node.left, state);
        if (!left.is_bool()) {
            throw KelEvalError("'and' requires bool operands, got " + left.type_name());
        }
        if (!left.as_bool()) return KelValue(false);  // short-circuit
        auto right = eval_node(*node.right, state);
        if (!right.is_bool()) {
            throw KelEvalError("'and' requires bool operands, got " + right.type_name());
        }
        return right;
    }

    if (node.op == TokenType::Or) {
        auto left = eval_node(*node.left, state);
        if (!left.is_bool()) {
            throw KelEvalError("'or' requires bool operands, got " + left.type_name());
        }
        if (left.as_bool()) return KelValue(true);  // short-circuit
        auto right = eval_node(*node.right, state);
        if (!right.is_bool()) {
            throw KelEvalError("'or' requires bool operands, got " + right.type_name());
        }
        return right;
    }

    // Evaluate both sides for all other operators.
    auto left = eval_node(*node.left, state);
    auto right = eval_node(*node.right, state);

    // ─── Equality / inequality ────────────────────────────────────────
    if (node.op == TokenType::EqualEqual) return KelValue(left == right);
    if (node.op == TokenType::BangEqual)  return KelValue(left != right);

    // ─── 'in' operator ───────────────────────────────────────────────
    if (node.op == TokenType::In) {
        if (!right.is_list()) {
            throw KelEvalError("'in' requires a list on the right, got " + right.type_name());
        }
        for (auto& elem : right.as_list()) {
            if (left == elem) return KelValue(true);
        }
        return KelValue(false);
    }

    // ─── Arithmetic operators ─────────────────────────────────────────

    // Duration + Duration, Duration - Duration
    if (left.is_duration() && right.is_duration()) {
        auto l = left.as_duration();
        auto r = right.as_duration();
        if (node.op == TokenType::Plus)  return KelValue(l + r);
        if (node.op == TokenType::Minus) return KelValue(l - r);
        if (node.op == TokenType::Less)         return KelValue(l < r);
        if (node.op == TokenType::LessEqual)    return KelValue(l <= r);
        if (node.op == TokenType::Greater)      return KelValue(l > r);
        if (node.op == TokenType::GreaterEqual) return KelValue(l >= r);
        throw KelEvalError("unsupported operator for duration values");
    }

    // int * duration or duration * int
    if (node.op == TokenType::Star) {
        if (left.is_int() && right.is_duration()) {
            return KelValue(KelValue::Duration(left.as_int() * right.as_duration().count()));
        }
        if (left.is_duration() && right.is_int()) {
            return KelValue(KelValue::Duration(left.as_duration().count() * right.as_int()));
        }
    }

    // String concatenation.
    if (node.op == TokenType::Plus && left.is_string() && right.is_string()) {
        auto result = left.as_string() + right.as_string();
        state.check_string_length(result);
        return KelValue(std::move(result));
    }

    // String ordered comparison.
    if (left.is_string() && right.is_string()) {
        auto& ls = left.as_string();
        auto& rs = right.as_string();
        switch (node.op) {
            case TokenType::Less:         return KelValue(ls < rs);
            case TokenType::LessEqual:    return KelValue(ls <= rs);
            case TokenType::Greater:      return KelValue(ls > rs);
            case TokenType::GreaterEqual: return KelValue(ls >= rs);
            default:
                throw KelEvalError("unsupported operator '" +
                    std::string(token_type_name(node.op)) + "' for string values");
        }
    }

    // Numeric operations.
    if (left.is_numeric() && right.is_numeric()) {
        // Both int → int operations.
        if (left.is_int() && right.is_int()) {
            auto l = left.as_int();
            auto r = right.as_int();
            switch (node.op) {
                case TokenType::Plus:    return KelValue(l + r);
                case TokenType::Minus:   return KelValue(l - r);
                case TokenType::Star:    return KelValue(l * r);
                case TokenType::Slash:
                    if (r == 0) throw KelEvalError("division by zero");
                    return KelValue(l / r);
                case TokenType::Percent:
                    if (r == 0) throw KelEvalError("modulo by zero");
                    return KelValue(l % r);
                case TokenType::Less:         return KelValue(l < r);
                case TokenType::LessEqual:    return KelValue(l <= r);
                case TokenType::Greater:      return KelValue(l > r);
                case TokenType::GreaterEqual: return KelValue(l >= r);
                default: break;
            }
        }

        // Mixed numeric → promote to float.
        auto l = left.to_double();
        auto r = right.to_double();
        switch (node.op) {
            case TokenType::Plus:    return KelValue(l + r);
            case TokenType::Minus:   return KelValue(l - r);
            case TokenType::Star:    return KelValue(l * r);
            case TokenType::Slash:
                if (r == 0.0) throw KelEvalError("division by zero");
                return KelValue(l / r);
            case TokenType::Less:         return KelValue(l < r);
            case TokenType::LessEqual:    return KelValue(l <= r);
            case TokenType::Greater:      return KelValue(l > r);
            case TokenType::GreaterEqual: return KelValue(l >= r);
            default: break;
        }
    }

    // Ordered comparison type mismatch.
    switch (node.op) {
        case TokenType::Less:
        case TokenType::LessEqual:
        case TokenType::Greater:
        case TokenType::GreaterEqual:
            throw KelEvalError(
                "ordered comparison requires matching numeric, string, or duration types; "
                "got " + left.type_name() + " and " + right.type_name());
        default: break;
    }

    throw KelEvalError(
        "unsupported operator '" + std::string(token_type_name(node.op)) +
        "' for types " + left.type_name() + " and " + right.type_name());
}

// ═══════════════════════════════════════════════════════════════════════════
// Main evaluation dispatcher
// ═══════════════════════════════════════════════════════════════════════════

KelValue eval_node(const AstNode& node, EvalState& state) {
    state.check_deadline();
    state.check_node_limit();

    return std::visit([&state](const auto& n) -> KelValue {
        using T = std::decay_t<decltype(n)>;

        // ─── Literal ──────────────────────────────────────────────────
        if constexpr (std::is_same_v<T, LiteralNode>) {
            return n.value;
        }

        // ─── Identifier (variable lookup) ─────────────────────────────
        if constexpr (std::is_same_v<T, IdentifierNode>) {
            auto it = state.ctx.variables.find(n.name);
            if (it == state.ctx.variables.end()) {
                throw KelEvalError("unknown variable: '" + n.name + "'");
            }
            return it->second;
        }

        // ─── Unary operation ──────────────────────────────────────────
        if constexpr (std::is_same_v<T, UnaryOpNode>) {
            auto operand = eval_node(*n.operand, state);

            if (n.op == TokenType::Not) {
                if (!operand.is_bool()) {
                    throw KelEvalError("'not' requires bool operand, got " + operand.type_name());
                }
                return KelValue(!operand.as_bool());
            }
            if (n.op == TokenType::Minus) {
                if (operand.is_int())   return KelValue(-operand.as_int());
                if (operand.is_float()) return KelValue(-operand.as_float());
                throw KelEvalError("unary '-' requires numeric operand, got " + operand.type_name());
            }
            throw KelEvalError("unknown unary operator");
        }

        // ─── Binary operation ─────────────────────────────────────────
        if constexpr (std::is_same_v<T, BinaryOpNode>) {
            return eval_binary(n, state);
        }

        // ─── Function call ────────────────────────────────────────────
        if constexpr (std::is_same_v<T, FunctionCallNode>) {
            if (n.args.size() > state.limits.max_function_args) {
                throw KelLimitError("function '" + n.name + "' has too many arguments ("
                    + std::to_string(n.args.size()) + ", max "
                    + std::to_string(state.limits.max_function_args) + ")");
            }

            auto it = state.ctx.functions.find(n.name);
            if (it == state.ctx.functions.end()) {
                throw KelEvalError("unknown function: '" + n.name + "'");
            }

            std::vector<KelValue> args;
            args.reserve(n.args.size());
            for (auto& arg : n.args) {
                args.push_back(eval_node(*arg, state));
            }

            try {
                return it->second(args);
            } catch (const KelError&) {
                throw;  // re-throw KEL errors as-is
            } catch (const std::exception& e) {
                throw KelEvalError("function '" + n.name + "' failed: " + e.what());
            }
        }

        // ─── Member access ────────────────────────────────────────────
        if constexpr (std::is_same_v<T, MemberAccessNode>) {
            auto obj = eval_node(*n.object, state);

            // Look up member by "type_name.member_name".
            std::string key = obj.type_name() + "." + n.member;
            auto it = state.ctx.members.find(key);
            if (it != state.ctx.members.end()) {
                return it->second(obj);
            }

            // Also try with a generic key for custom types (e.g., "job_ref.last_success").
            // If the object is a string, it might be used as a named reference.
            if (obj.is_string()) {
                std::string alt_key = "job_ref." + n.member;
                auto it2 = state.ctx.members.find(alt_key);
                if (it2 != state.ctx.members.end()) {
                    return it2->second(obj);
                }
            }

            throw KelEvalError("unknown member '" + n.member + "' on type " + obj.type_name());
        }

        // ─── Method call ──────────────────────────────────────────────
        if constexpr (std::is_same_v<T, MethodCallNode>) {
            auto obj = eval_node(*n.object, state);

            auto it = state.ctx.methods.find(n.method);
            if (it == state.ctx.methods.end()) {
                throw KelEvalError("unknown method '" + n.method + "' on type " + obj.type_name());
            }

            std::vector<KelValue> args;
            args.reserve(n.args.size());
            for (auto& arg : n.args) {
                args.push_back(eval_node(*arg, state));
            }

            try {
                return it->second(obj, args);
            } catch (const KelError&) {
                throw;
            } catch (const std::exception& e) {
                throw KelEvalError("method '" + n.method + "' failed: " + e.what());
            }
        }

        // ─── List literal ─────────────────────────────────────────────
        if constexpr (std::is_same_v<T, ListLiteralNode>) {
            KelList elements;
            elements.reserve(n.elements.size());
            for (auto& el : n.elements) {
                elements.push_back(eval_node(*el, state));
            }
            state.check_list_length(elements);
            return KelValue(std::move(elements));
        }

        throw KelEvalError("unknown AST node type");
    }, node);
}

}  // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════════════════

KelValue evaluate(const AstNode& ast, const EvalContext& ctx,
                  const EvalLimits& limits) {
    // Check AST node count.
    auto count = ast_node_count(ast);
    if (count > limits.max_ast_nodes) {
        throw KelLimitError("AST node count " + std::to_string(count) +
            " exceeds limit " + std::to_string(limits.max_ast_nodes));
    }

    EvalState state(ctx, limits);
    return eval_node(ast, state);
}

KelValue eval_expression(std::string_view source, const EvalContext& ctx,
                          const EvalLimits& limits) {
    auto ast = parse(source);
    return evaluate(*ast, ctx, limits);
}

// ═══════════════════════════════════════════════════════════════════════════
// Default context builder (general-purpose + string functions)
// ═══════════════════════════════════════════════════════════════════════════

EvalContext make_default_context() {
    EvalContext ctx;

    // ── Category 1: General-purpose functions ─────────────────────────

    ctx.functions["min"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 2)
            throw KelEvalError("min() requires exactly 2 arguments");
        if (!args[0].is_numeric() || !args[1].is_numeric())
            throw KelEvalError("min() requires numeric arguments");
        if (args[0].is_int() && args[1].is_int())
            return KelValue(std::min(args[0].as_int(), args[1].as_int()));
        return KelValue(std::min(args[0].to_double(), args[1].to_double()));
    };

    ctx.functions["max"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 2)
            throw KelEvalError("max() requires exactly 2 arguments");
        if (!args[0].is_numeric() || !args[1].is_numeric())
            throw KelEvalError("max() requires numeric arguments");
        if (args[0].is_int() && args[1].is_int())
            return KelValue(std::max(args[0].as_int(), args[1].as_int()));
        return KelValue(std::max(args[0].to_double(), args[1].to_double()));
    };

    ctx.functions["abs"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 1)
            throw KelEvalError("abs() requires exactly 1 argument");
        if (args[0].is_int()) return KelValue(std::abs(args[0].as_int()));
        if (args[0].is_float()) return KelValue(std::abs(args[0].as_float()));
        throw KelEvalError("abs() requires a numeric argument");
    };

    ctx.functions["len"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 1)
            throw KelEvalError("len() requires exactly 1 argument");
        if (args[0].is_string()) return KelValue(static_cast<int64_t>(args[0].as_string().size()));
        if (args[0].is_list())   return KelValue(static_cast<int64_t>(args[0].as_list().size()));
        throw KelEvalError("len() requires a string or list argument");
    };

    ctx.functions["sum"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 1 || !args[0].is_list())
            throw KelEvalError("sum() requires a single list argument");
        auto& lst = args[0].as_list();
        if (lst.empty()) return KelValue(int64_t(0));
        bool has_float = false;
        for (auto& v : lst) {
            if (!v.is_numeric())
                throw KelEvalError("sum() list must contain only numeric values");
            if (v.is_float()) has_float = true;
        }
        if (has_float) {
            double total = 0.0;
            for (auto& v : lst) total += v.to_double();
            return KelValue(total);
        }
        int64_t total = 0;
        for (auto& v : lst) total += v.as_int();
        return KelValue(total);
    };

    ctx.functions["avg"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 1 || !args[0].is_list())
            throw KelEvalError("avg() requires a single list argument");
        auto& lst = args[0].as_list();
        if (lst.empty()) return KelValue(0.0);
        double total = 0.0;
        for (auto& v : lst) {
            if (!v.is_numeric())
                throw KelEvalError("avg() list must contain only numeric values");
            total += v.to_double();
        }
        return KelValue(total / static_cast<double>(lst.size()));
    };

    ctx.functions["round"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 1 || !args[0].is_float())
            throw KelEvalError("round() requires a single float argument");
        return KelValue(static_cast<int64_t>(std::round(args[0].as_float())));
    };

    ctx.functions["floor"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 1 || !args[0].is_float())
            throw KelEvalError("floor() requires a single float argument");
        return KelValue(static_cast<int64_t>(std::floor(args[0].as_float())));
    };

    ctx.functions["ceil"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 1 || !args[0].is_float())
            throw KelEvalError("ceil() requires a single float argument");
        return KelValue(static_cast<int64_t>(std::ceil(args[0].as_float())));
    };

    ctx.functions["int"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 1)
            throw KelEvalError("int() requires exactly 1 argument");
        if (args[0].is_int())    return args[0];
        if (args[0].is_float())  return KelValue(static_cast<int64_t>(args[0].as_float()));
        if (args[0].is_bool())   return KelValue(args[0].as_bool() ? int64_t(1) : int64_t(0));
        if (args[0].is_string()) {
            try {
                auto val = static_cast<int64_t>(std::stoll(args[0].as_string()));
                return KelValue(val);
            }
            catch (...) { throw KelEvalError("int() cannot convert string '" + args[0].as_string() + "' to integer"); }
        }
        throw KelEvalError("int() cannot convert " + args[0].type_name());
    };

    ctx.functions["float"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 1)
            throw KelEvalError("float() requires exactly 1 argument");
        if (args[0].is_float())  return args[0];
        if (args[0].is_int())    return KelValue(static_cast<double>(args[0].as_int()));
        if (args[0].is_string()) {
            try {
                double val = std::stod(args[0].as_string());
                return KelValue(val);
            }
            catch (...) { throw KelEvalError("float() cannot convert string '" + args[0].as_string() + "'"); }
        }
        throw KelEvalError("float() cannot convert " + args[0].type_name());
    };

    ctx.functions["str"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 1)
            throw KelEvalError("str() requires exactly 1 argument");
        return KelValue(args[0].to_display_string());
    };

    ctx.functions["bool"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 1)
            throw KelEvalError("bool() requires exactly 1 argument");
        return KelValue(args[0].is_truthy());
    };

    // ── Category 2: String functions ──────────────────────────────────

    ctx.functions["starts_with"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 2 || !args[0].is_string() || !args[1].is_string())
            throw KelEvalError("starts_with() requires two string arguments");
        auto& s = args[0].as_string();
        auto& prefix = args[1].as_string();
        return KelValue(s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0);
    };

    ctx.functions["ends_with"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 2 || !args[0].is_string() || !args[1].is_string())
            throw KelEvalError("ends_with() requires two string arguments");
        auto& s = args[0].as_string();
        auto& suffix = args[1].as_string();
        return KelValue(s.size() >= suffix.size() &&
            s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0);
    };

    ctx.functions["contains"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 2 || !args[0].is_string() || !args[1].is_string())
            throw KelEvalError("contains() requires two string arguments");
        return KelValue(args[0].as_string().find(args[1].as_string()) != std::string::npos);
    };

    ctx.functions["matches"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 2 || !args[0].is_string() || !args[1].is_string())
            throw KelEvalError("matches() requires two string arguments");
        try {
            std::regex re(args[1].as_string(), std::regex_constants::ECMAScript);
            return KelValue(std::regex_search(args[0].as_string(), re));
        } catch (const std::regex_error&) {
            // Invalid regex → return false (defensive).
            return KelValue(false);
        }
    };

    ctx.functions["lower"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 1 || !args[0].is_string())
            throw KelEvalError("lower() requires a single string argument");
        auto s = args[0].as_string();
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return KelValue(std::move(s));
    };

    ctx.functions["upper"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 1 || !args[0].is_string())
            throw KelEvalError("upper() requires a single string argument");
        auto s = args[0].as_string();
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        return KelValue(std::move(s));
    };

    ctx.functions["trim"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 1 || !args[0].is_string())
            throw KelEvalError("trim() requires a single string argument");
        auto s = args[0].as_string();
        auto start = s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return KelValue(std::string(""));
        auto end = s.find_last_not_of(" \t\n\r");
        return KelValue(s.substr(start, end - start + 1));
    };

    ctx.functions["replace"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 3 || !args[0].is_string() || !args[1].is_string() || !args[2].is_string())
            throw KelEvalError("replace() requires three string arguments");
        auto s = args[0].as_string();
        auto& old_str = args[1].as_string();
        auto& new_str = args[2].as_string();
        if (old_str.empty()) return KelValue(std::move(s));
        auto pos = s.find(old_str);
        if (pos != std::string::npos) {
            s.replace(pos, old_str.size(), new_str);
        }
        return KelValue(std::move(s));
    };

    // ── Category 5: Time functions ────────────────────────────────────

    ctx.functions["duration_seconds"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 1 || !args[0].is_duration())
            throw KelEvalError("duration_seconds() requires a single duration argument");
        return KelValue(static_cast<int64_t>(args[0].as_duration().count() / 1000));
    };

    ctx.functions["duration_minutes"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 1 || !args[0].is_duration())
            throw KelEvalError("duration_minutes() requires a single duration argument");
        return KelValue(static_cast<int64_t>(args[0].as_duration().count() / 60000));
    };

    ctx.functions["duration_hours"] = [](const std::vector<KelValue>& args) -> KelValue {
        if (args.size() != 1 || !args[0].is_duration())
            throw KelEvalError("duration_hours() requires a single duration argument");
        return KelValue(static_cast<int64_t>(args[0].as_duration().count() / 3600000));
    };

    return ctx;
}

}  // namespace kairos::kel
