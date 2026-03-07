/// include/kairos/kel/evaluator.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/kel/evaluator.hpp — AST evaluator with sandboxing                ║
// ║                                                                           ║
// ║  Evaluates a parsed AST against an EvalContext (variables, functions,     ║
// ║  methods, members) with hard safety limits (depth, time, sizes).          ║
// ║                                                                           ║
// ║  Spec reference: §7.6–§7.9                                               ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/kel/ast.hpp"
#include "kairos/kel/value.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>

namespace kairos::kel {

/// Safety limits for evaluation (populated from kairos.kel.* config).
struct EvalLimits {
    uint32_t max_ast_nodes     = 1024;   ///< Max total AST nodes
    uint32_t max_ast_depth     = 32;     ///< Max nesting depth
    uint32_t max_eval_time_ms  = 100;    ///< Timeout for entire evaluation
    uint32_t max_string_length = 65536;  ///< Max string result size
    uint32_t max_list_length   = 10000;  ///< Max list result size
    uint32_t max_function_args = 16;     ///< Max arguments per function call
};

/// Function signature for built-in functions.
using BuiltinFn = std::function<KelValue(const std::vector<KelValue>&)>;

/// The evaluation context: provides variable bindings and function registry.
struct EvalContext {
    /// Named variables available to the expression.
    std::unordered_map<std::string, KelValue> variables;

    /// Built-in functions available to the expression.
    std::unordered_map<std::string, BuiltinFn> functions;

    /// Method resolver: given an object value and method name, returns
    /// a function that takes (object, args) and returns a result.
    /// This enables job("id").finished_within(30m) and similar.
    using MethodFn = std::function<KelValue(const KelValue&,
                                            const std::vector<KelValue>&)>;
    std::unordered_map<std::string, MethodFn> methods;

    /// Member resolver: given an object value and member name, returns
    /// the member's value.  Enables event.type, job("id").last_success.
    /// Key format: "type_name.member_name" (e.g., "job_ref.last_success").
    using MemberFn = std::function<KelValue(const KelValue&)>;
    std::unordered_map<std::string, MemberFn> members;
};

/// Evaluate a parsed AST in the given context.
///
/// Throws:
///   - KelEvalError on type mismatches, unknown identifiers,
///     division by zero, unknown function/method/member.
///   - KelLimitError on depth/time/size limit violations.
KelValue evaluate(const AstNode& ast, const EvalContext& ctx,
                  const EvalLimits& limits = {});

/// Convenience: tokenize + parse + evaluate in one call.
KelValue eval_expression(std::string_view source, const EvalContext& ctx,
                          const EvalLimits& limits = {});

/// Build a default EvalContext with general-purpose and string
/// built-in functions (categories 1 and 2 from §7.7).
/// Does NOT include job-history or aggregation functions — those
/// require a database connection and are added by the caller.
EvalContext make_default_context();

}  // namespace kairos::kel
