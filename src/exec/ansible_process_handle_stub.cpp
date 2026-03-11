/// src/exec/ansible_process_handle_stub.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  AnsibleProcessHandle stub — compiled when KAIROS_ANSIBLE=OFF           ║
// ║                                                                          ║
// ║  Factory functions exist so the daemon compiles without Ansible support, ║
// ║  but they always return a handle that immediately fails with exit 202.  ║
// ║                                                                          ║
// ║  Spec reference: §15.4                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/exec/ansible_process_handle.hpp"

#include <spdlog/spdlog.h>

namespace kairos::exec {

// ── Stub AnsibleProcessHandle ───────────────────────────────────────────

AnsibleProcessHandle::AnsibleProcessHandle(
    const std::string& ansible_playbook_path)
    : ansible_playbook_path_(ansible_playbook_path)
    , inner_(nullptr) {}

AnsibleProcessHandle::~AnsibleProcessHandle() = default;

void AnsibleProcessHandle::set_output_callback(OutputCallback cb) {
    (void)cb;
}

const ProcessResult& AnsibleProcessHandle::result() const {
    return result_;
}

bool AnsibleProcessHandle::is_running() const { return false; }
int64_t AnsibleProcessHandle::pid() const { return 0; }

bool AnsibleProcessHandle::spawn(const ProcessSpec&) {
    result_.exit_code = 202;
    result_.termination = ProcessResult::TerminationKind::SpawnFailed;
    result_.termination_reason =
        "Ansible runner not compiled (build with -DKAIROS_ANSIBLE=ON)";
    spdlog::error("{}", result_.termination_reason);
    return false;
}

ProcessResult AnsibleProcessHandle::wait(std::stop_token) {
    return result_;
}

void AnsibleProcessHandle::terminate() {}
void AnsibleProcessHandle::kill() {}

std::vector<std::string>
AnsibleProcessHandle::build_args(const ProcessSpec&) const {
    return {};
}

std::unordered_map<std::string, std::string>
AnsibleProcessHandle::build_env(const ProcessSpec&) const {
    return {};
}

std::filesystem::path
AnsibleProcessHandle::create_vault_helper() const {
    return {};
}

void AnsibleProcessHandle::cleanup_vault_helper() {}

// ── Factory functions ────────────────────────────────────────────────────

std::unique_ptr<ProcessHandle> create_ansible_process_handle() {
    return std::make_unique<AnsibleProcessHandle>();
}

std::unique_ptr<ProcessHandle> create_ansible_process_handle(
    const std::string& ansible_playbook_path) {
    return std::make_unique<AnsibleProcessHandle>(ansible_playbook_path);
}

std::string find_ansible_playbook() {
    return {};
}

}  // namespace kairos::exec
