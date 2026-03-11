/// include/kairos/exec/ansible_process_handle.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  AnsibleProcessHandle — Execute Ansible playbooks via ansible-playbook  ║
// ║                                                                          ║
// ║  Wraps the local ProcessHandle to invoke ansible-playbook with the      ║
// ║  correct arguments, environment, and vault password injection.          ║
// ║                                                                          ║
// ║  Vault password injection strategy (§15.4):                             ║
// ║    - ANSIBLE_VAULT_PASSWORD is set in the child environment             ║
// ║    - A temporary vault-password-file script reads from this env var     ║
// ║    - The password never appears on the command line (/proc safe)        ║
// ║                                                                          ║
// ║  Spec reference: §15.4                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/exec/process_handle.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace kairos::exec {

/// ProcessHandle implementation that executes Ansible playbooks by
/// constructing and running the `ansible-playbook` command.
///
/// The ProcessSpec is translated to an ansible-playbook invocation:
///   ansible-playbook <playbook_path>
///     -i <inventory>
///     --extra-vars '<json>'
///     --vault-password-file <helper_script>
///     [--limit <limit>]
///     [--tags <tags>]
///     [--check]           (if dry_run)
///     [--diff]            (if diff_mode)
///
/// Output handling:
///   ANSIBLE_STDOUT_CALLBACK=json is set automatically. The structured
///   JSON output is forwarded through the OutputCallback for step-level
///   status reporting.
///
/// Lifecycle:
///   1. spawn() builds the command line, creates the vault helper script,
///      and delegates to the inner local ProcessHandle.
///   2. wait() / terminate() / kill() delegate directly.
///   3. Destructor cleans up the temporary vault helper script.
class AnsibleProcessHandle : public ProcessHandle {
public:
    /// Construct with the path to the ansible-playbook binary.
    /// If empty, "ansible-playbook" is assumed to be on PATH.
    explicit AnsibleProcessHandle(
        const std::string& ansible_playbook_path = "");

    ~AnsibleProcessHandle() override;

    // Non-copyable, non-movable (owns temp file + inner handle).
    AnsibleProcessHandle(const AnsibleProcessHandle&) = delete;
    AnsibleProcessHandle& operator=(const AnsibleProcessHandle&) = delete;

    // ── ProcessHandle interface ──────────────────────────────────────

    /// Translate ansible-specific ProcessSpec fields into an
    /// ansible-playbook invocation and spawn via local process handle.
    ///
    /// Required ProcessSpec fields:
    ///   - ansible_playbook:  Path to the .yml/.yaml playbook file
    ///
    /// Optional ProcessSpec fields:
    ///   - ansible_inventory: Inventory file or host pattern (default: "localhost,")
    ///   - ansible_extra_vars: JSON string of extra variables
    ///   - ansible_vault_password: Vault decryption password (injected via env)
    ///   - ansible_limit: Limit to specific hosts
    ///   - ansible_tags: Comma-separated tags to run
    ///   - ansible_skip_tags: Comma-separated tags to skip
    ///   - ansible_check: If true, run in check mode (--check)
    ///   - ansible_diff: If true, show diffs (--diff)
    ///   - ansible_verbosity: 0–4 → -v, -vv, -vvv, -vvvv
    [[nodiscard]] bool spawn(const ProcessSpec& spec) override;

    ProcessResult wait(std::stop_token stop) override;
    void terminate() override;
    void kill() override;
    [[nodiscard]] bool is_running() const override;
    [[nodiscard]] int64_t pid() const override;
    void set_output_callback(OutputCallback cb) override;
    [[nodiscard]] const ProcessResult& result() const override;

private:
    /// Build the argument vector for ansible-playbook.
    std::vector<std::string> build_args(const ProcessSpec& spec) const;

    /// Build the environment map for the child process.
    /// Merges the caller's env with Ansible-specific vars.
    std::unordered_map<std::string, std::string>
    build_env(const ProcessSpec& spec) const;

    /// Create a temporary vault password helper script.
    /// Returns the path to the script, or empty on failure.
    /// The script reads ANSIBLE_VAULT_PASSWORD from the environment
    /// and prints it to stdout (consumed by --vault-password-file).
    ///
    /// POSIX: #!/bin/sh\necho "$ANSIBLE_VAULT_PASSWORD"
    /// Windows: @echo off\necho %ANSIBLE_VAULT_PASSWORD%
    std::filesystem::path create_vault_helper() const;

    /// Remove the temporary vault helper script.
    void cleanup_vault_helper();

    std::string ansible_playbook_path_;
    std::unique_ptr<ProcessHandle> inner_;
    OutputCallback output_callback_;
    ProcessResult result_;
    std::filesystem::path vault_helper_path_;
};

/// Factory function for AnsibleProcessHandle.
/// Uses "ansible-playbook" from PATH.
std::unique_ptr<ProcessHandle> create_ansible_process_handle();

/// Factory function with explicit ansible-playbook path.
std::unique_ptr<ProcessHandle> create_ansible_process_handle(
    const std::string& ansible_playbook_path);

/// Check if ansible-playbook is available on PATH.
/// Returns the resolved path, or empty string if not found.
std::string find_ansible_playbook();

}  // namespace kairos::exec
