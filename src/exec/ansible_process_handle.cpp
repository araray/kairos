/// src/exec/ansible_process_handle.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  AnsibleProcessHandle implementation                                     ║
// ║                                                                          ║
// ║  Translates Ansible-specific ProcessSpec fields into an                  ║
// ║  ansible-playbook invocation, handles vault password injection           ║
// ║  via a temporary helper script, and delegates execution to the           ║
// ║  local ProcessHandle.                                                    ║
// ║                                                                          ║
// ║  Spec reference: §15.4                                                   ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/exec/ansible_process_handle.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>

namespace kairos::exec {

// ── Helpers ──────────────────────────────────────────────────────────────

namespace {

/// Generate a random hex string for temp file names.
std::string random_hex(std::size_t length) {
    static constexpr const char hex_chars[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 15);
    std::string result;
    result.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        result += hex_chars[dist(gen)];
    }
    return result;
}

/// Get the system temp directory.
std::filesystem::path temp_dir() {
    std::error_code ec;
    auto t = std::filesystem::temp_directory_path(ec);
    if (ec) return ".";
    return t;
}

}  // namespace

// ── Construction / Destruction ───────────────────────────────────────────

AnsibleProcessHandle::AnsibleProcessHandle(
    const std::string& ansible_playbook_path)
    : ansible_playbook_path_(ansible_playbook_path.empty()
          ? "ansible-playbook" : ansible_playbook_path)
    , inner_(nullptr)
{}

AnsibleProcessHandle::~AnsibleProcessHandle() {
    cleanup_vault_helper();
}

// ── ProcessHandle interface ──────────────────────────────────────────────

bool AnsibleProcessHandle::spawn(const ProcessSpec& spec) {
    // Validate: playbook is required.
    if (spec.ansible_playbook.empty()) {
        result_.exit_code = 202;
        result_.termination = ProcessResult::TerminationKind::SpawnFailed;
        result_.termination_reason =
            "Ansible runner: ansible_playbook path is required";
        spdlog::error("{}", result_.termination_reason);
        return false;
    }

    // Verify the playbook file exists.
    std::error_code ec;
    if (!std::filesystem::exists(spec.ansible_playbook, ec)) {
        result_.exit_code = 202;
        result_.termination = ProcessResult::TerminationKind::SpawnFailed;
        result_.termination_reason =
            "Ansible runner: playbook not found: " + spec.ansible_playbook;
        spdlog::error("{}", result_.termination_reason);
        return false;
    }

    // Build the ProcessSpec for the local process handle.
    ProcessSpec local_spec;
    local_spec.use_shell = false;
    local_spec.args = build_args(spec);
    local_spec.environment = build_env(spec);
    local_spec.working_dir = spec.working_dir;
    local_spec.timeout = spec.timeout;
    local_spec.kill_timeout = spec.kill_timeout;
    local_spec.max_output_bytes = spec.max_output_bytes;

    spdlog::info("Ansible runner: executing playbook '{}' with inventory '{}'",
                 spec.ansible_playbook,
                 spec.ansible_inventory.empty()
                     ? "localhost," : spec.ansible_inventory);

    if (spdlog::should_log(spdlog::level::debug)) {
        std::string args_str;
        for (const auto& arg : local_spec.args) {
            if (!args_str.empty()) args_str += ' ';
            args_str += arg;
        }
        spdlog::debug("Ansible runner: command = {}", args_str);
    }

    // Create the inner local process handle and delegate.
    inner_ = create_process_handle();
    if (output_callback_) {
        inner_->set_output_callback(output_callback_);
    }

    bool ok = inner_->spawn(local_spec);
    if (!ok) {
        result_ = inner_->result();
        spdlog::error("Ansible runner: spawn failed: {}",
                      result_.termination_reason);
    }
    return ok;
}

ProcessResult AnsibleProcessHandle::wait(std::stop_token stop) {
    if (!inner_) {
        return result_;
    }
    result_ = inner_->wait(stop);

    // Map exit codes for Ansible-specific meanings.
    // ansible-playbook exit codes:
    //   0 = success
    //   1 = error (host unreachable, module failure, etc.)
    //   2 = failed tasks
    //   4 = unreachable hosts
    //   5 = skipped tasks (--check + no changes)
    //  99 = user interrupted
    // 250 = unexpected exception
    if (result_.exit_code != 0 && result_.termination_reason.empty()) {
        switch (result_.exit_code) {
        case 2:
            result_.termination_reason = "Ansible: one or more tasks failed";
            break;
        case 4:
            result_.termination_reason = "Ansible: one or more hosts unreachable";
            break;
        case 5:
            result_.termination_reason = "Ansible: no changes detected (check mode)";
            break;
        case 99:
            result_.termination_reason = "Ansible: user interrupted";
            break;
        case 250:
            result_.termination_reason = "Ansible: unexpected exception";
            break;
        default:
            result_.termination_reason =
                "Ansible: exit code " + std::to_string(result_.exit_code);
            break;
        }
    }

    // Clean up vault helper now that the process is done.
    cleanup_vault_helper();

    return result_;
}

void AnsibleProcessHandle::terminate() {
    if (inner_) inner_->terminate();
}

void AnsibleProcessHandle::kill() {
    if (inner_) inner_->kill();
}

bool AnsibleProcessHandle::is_running() const {
    return inner_ && inner_->is_running();
}

int64_t AnsibleProcessHandle::pid() const {
    return inner_ ? inner_->pid() : 0;
}

void AnsibleProcessHandle::set_output_callback(OutputCallback cb) {
    output_callback_ = std::move(cb);
    if (inner_) {
        inner_->set_output_callback(output_callback_);
    }
}

const ProcessResult& AnsibleProcessHandle::result() const {
    return result_;
}

// ── Private helpers ──────────────────────────────────────────────────────

std::vector<std::string>
AnsibleProcessHandle::build_args(const ProcessSpec& spec) const {
    std::vector<std::string> args;

    // Executable.
    args.push_back(ansible_playbook_path_);

    // Playbook path.
    args.push_back(spec.ansible_playbook);

    // Inventory.
    if (!spec.ansible_inventory.empty()) {
        args.push_back("-i");
        args.push_back(spec.ansible_inventory);
    } else {
        // Default: localhost with implicit inventory.
        args.push_back("-i");
        args.push_back("localhost,");
        args.push_back("--connection");
        args.push_back("local");
    }

    // Extra variables (JSON string).
    if (!spec.ansible_extra_vars.empty()) {
        args.push_back("--extra-vars");
        args.push_back(spec.ansible_extra_vars);
    }

    // Vault password via helper script.
    if (!spec.ansible_vault_password.empty()) {
        auto helper = create_vault_helper();
        if (!helper.empty()) {
            // The const_cast is safe here because we store the path
            // in the mutable member via the const method (create is
            // logically const from the ProcessSpec perspective but
            // mutates internal state).
            const_cast<AnsibleProcessHandle*>(this)->vault_helper_path_ =
                helper;
            args.push_back("--vault-password-file");
            args.push_back(helper.string());
        }
    }

    // Host limit.
    if (!spec.ansible_limit.empty()) {
        args.push_back("--limit");
        args.push_back(spec.ansible_limit);
    }

    // Tags.
    if (!spec.ansible_tags.empty()) {
        args.push_back("--tags");
        args.push_back(spec.ansible_tags);
    }

    // Skip tags.
    if (!spec.ansible_skip_tags.empty()) {
        args.push_back("--skip-tags");
        args.push_back(spec.ansible_skip_tags);
    }

    // Check mode (dry run).
    if (spec.ansible_check) {
        args.push_back("--check");
    }

    // Diff mode.
    if (spec.ansible_diff) {
        args.push_back("--diff");
    }

    // Verbosity.
    if (spec.ansible_verbosity > 0) {
        int v = std::min(spec.ansible_verbosity, 4);
        args.push_back("-" + std::string(static_cast<std::size_t>(v), 'v'));
    }

    // Force color off (structured output is better without ANSI escapes).
    args.push_back("--force-handlers");

    return args;
}

std::unordered_map<std::string, std::string>
AnsibleProcessHandle::build_env(const ProcessSpec& spec) const {
    // Start with the caller's environment.
    auto env = spec.environment;

    // Force JSON output callback for structured output parsing.
    env["ANSIBLE_STDOUT_CALLBACK"] = "json";

    // Disable retry files (noisy in automated runs).
    env["ANSIBLE_RETRY_FILES_ENABLED"] = "False";

    // Disable deprecation warnings in output (clean structured output).
    env["ANSIBLE_DEPRECATION_WARNINGS"] = "False";

    // Inject vault password into environment (never on command line).
    if (!spec.ansible_vault_password.empty()) {
        env["ANSIBLE_VAULT_PASSWORD"] = spec.ansible_vault_password;
    }

    // Set ANSIBLE_FORCE_COLOR=0 for clean JSON output.
    env["ANSIBLE_FORCE_COLOR"] = "0";

    // Prevent Ansible from reading user-level ansible.cfg that could
    // override our settings unexpectedly.
    if (env.find("ANSIBLE_CONFIG") == env.end()) {
        // Only set if the user hasn't explicitly provided one.
        // Using /dev/null means "no config file".
#ifdef _WIN32
        env["ANSIBLE_CONFIG"] = "NUL";
#else
        env["ANSIBLE_CONFIG"] = "/dev/null";
#endif
    }

    return env;
}

std::filesystem::path
AnsibleProcessHandle::create_vault_helper() const {
    auto dir = temp_dir();
    auto name = "kairos_vault_" + random_hex(8);

#ifdef _WIN32
    name += ".cmd";
    auto path = dir / name;
    std::ofstream f(path);
    if (!f) {
        spdlog::error("Ansible runner: failed to create vault helper: {}",
                      path.string());
        return {};
    }
    f << "@echo off\n";
    f << "echo %ANSIBLE_VAULT_PASSWORD%\n";
    f.close();
#else
    auto path = dir / name;
    std::ofstream f(path);
    if (!f) {
        spdlog::error("Ansible runner: failed to create vault helper: {}",
                      path.string());
        return {};
    }
    f << "#!/bin/sh\n";
    f << "echo \"$ANSIBLE_VAULT_PASSWORD\"\n";
    f.close();

    // Make executable.
    std::error_code ec;
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_exec |
        std::filesystem::perms::owner_read |
        std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
    if (ec) {
        spdlog::warn("Ansible runner: chmod vault helper failed: {}",
                     ec.message());
    }
#endif

    spdlog::debug("Ansible runner: vault helper created: {}", path.string());
    return path;
}

void AnsibleProcessHandle::cleanup_vault_helper() {
    if (vault_helper_path_.empty()) return;
    std::error_code ec;
    std::filesystem::remove(vault_helper_path_, ec);
    if (ec) {
        spdlog::warn("Ansible runner: failed to remove vault helper '{}': {}",
                     vault_helper_path_.string(), ec.message());
    } else {
        spdlog::debug("Ansible runner: vault helper removed: {}",
                      vault_helper_path_.string());
    }
    vault_helper_path_.clear();
}

// ── Factory functions ────────────────────────────────────────────────────

std::unique_ptr<ProcessHandle> create_ansible_process_handle() {
    return std::make_unique<AnsibleProcessHandle>();
}

std::unique_ptr<ProcessHandle> create_ansible_process_handle(
    const std::string& ansible_playbook_path) {
    return std::make_unique<AnsibleProcessHandle>(ansible_playbook_path);
}

std::string find_ansible_playbook() {
    // Try to locate ansible-playbook on PATH.
    // Use `which` on POSIX, `where` on Windows.
#ifdef _WIN32
    const char* cmd = "where ansible-playbook 2>NUL";
#else
    const char* cmd = "which ansible-playbook 2>/dev/null";
#endif

    std::array<char, 256> buffer{};
    std::string result;

    FILE* pipe = ::popen(cmd, "r");
    if (!pipe) return {};

    while (::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        result += buffer.data();
    }

    int status = ::pclose(pipe);
    if (status != 0) return {};

    // Trim trailing whitespace.
    while (!result.empty() &&
           (result.back() == '\n' || result.back() == '\r' ||
            result.back() == ' ')) {
        result.pop_back();
    }

    return result;
}

}  // namespace kairos::exec
