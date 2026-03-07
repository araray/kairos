/// include/kairos/exec/env_builder.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/exec/env_builder.hpp — Child process environment construction    ║
// ║                                                                          ║
// ║  7-layer precedence chain:                                               ║
// ║    1. Inherited parent env (or clean slate)                              ║
// ║    2. Global config env (from kairos.toml)                               ║
// ║    3. Workflow-level env                                                 ║
// ║    4. Job-level env                                                      ║
// ║    5. Step-level env                                                     ║
// ║    6. Secret references resolved (${{ secrets.key }})                    ║
// ║    7. Kairos metadata variables (KAIROS_RUN_ID, etc.)                   ║
// ║                                                                          ║
// ║  Spec reference: §14.6                                                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kairos::exec {

/// Builds the complete environment for a child process.
///
/// Usage:
///   EnvBuilder builder;
///   builder.inherit_parent()
///          .add_global(config_env)
///          .add_workflow(wf_env)
///          .add_job(job_env)
///          .add_step(step_env)
///          .resolve_secrets(secret_fn)
///          .add_kairos_vars(run_id, job_id, ...);
///   auto env = builder.build();
class EnvBuilder {
public:
    /// Layer 1: Inherit the parent process environment.
    /// If not called, starts with an empty environment.
    /// PATH is always preserved even if starting clean.
    EnvBuilder& inherit_parent();

    /// Layer 2: Merge global config env variables.
    EnvBuilder& add_global(
        const std::unordered_map<std::string, std::string>& global_env);

    /// Layer 3: Merge workflow-level env.
    EnvBuilder& add_workflow(
        const std::unordered_map<std::string, std::string>& wf_env);

    /// Layer 4: Merge job-level env.
    EnvBuilder& add_job(
        const std::unordered_map<std::string, std::string>& job_env);

    /// Layer 5: Merge step-level env.
    EnvBuilder& add_step(
        const std::unordered_map<std::string, std::string>& step_env);

    /// Layer 6: Resolve ${{ secrets.key }} references.
    /// The resolver is called with the key name and returns the
    /// decrypted value, or std::nullopt if the key is not found.
    /// Unresolved references are left as-is (Phase 5 logs a warning).
    using SecretResolver = std::function<
        std::optional<std::string>(const std::string& key)>;
    EnvBuilder& resolve_secrets(SecretResolver resolver);

    /// Layer 7: Inject Kairos metadata variables.
    /// These are prefixed with KAIROS_ and always present.
    EnvBuilder& add_kairos_vars(
        const std::string& run_id,
        const std::string& job_id,
        const std::string& step_id,
        const std::string& workflow_id,
        const std::string& trigger_type,
        const std::string& correlation_id,
        const std::string& data_dir,
        const std::string& db_path);

    /// Build the final environment map.
    [[nodiscard]] std::unordered_map<std::string, std::string>
    build() const;

    /// Return the list of secret keys that were resolved.
    /// Used by the secret masker to know which values to redact.
    [[nodiscard]] const std::vector<std::string>&
    resolved_secret_keys() const;

private:
    std::unordered_map<std::string, std::string> env_;
    std::vector<std::string> resolved_secrets_;

    /// Merge a layer into the environment (later layers override earlier).
    void merge(const std::unordered_map<std::string, std::string>& layer);

    /// Scan all values for ${{ secrets.key }} and resolve them.
    void scan_and_resolve(SecretResolver& resolver);
};

/// Capture the current process environment as a map.
/// Used by inherit_parent() and by tests.
std::unordered_map<std::string, std::string> capture_current_env();

}  // namespace kairos::exec
