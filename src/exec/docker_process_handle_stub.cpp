/// src/exec/docker_process_handle_stub.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  DockerProcessHandle stub — compiled when KAIROS_DOCKER=OFF             ║
// ║                                                                          ║
// ║  The factory functions exist so the daemon can compile without Docker    ║
// ║  support, but they always return a handle that immediately fails.       ║
// ║                                                                          ║
// ║  Spec reference: §15.3                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/exec/docker_process_handle.hpp"

#include <spdlog/spdlog.h>

namespace kairos::exec {

// ── Stub DockerClient ─────────────────────────────────────────────────

DockerClient::DockerClient(const std::string& socket_path)
    : socket_path_(socket_path) {}

DockerClient::~DockerClient() = default;
DockerClient::DockerClient(DockerClient&&) noexcept = default;
DockerClient& DockerClient::operator=(DockerClient&&) noexcept = default;

int DockerClient::connect_socket() { return -1; }

bool DockerClient::read_exact(int, char*, std::size_t) { return false; }

DockerClient::Response DockerClient::http_request(
    const std::string&, const std::string&,
    const std::string&, const std::string&)
{
    return {0, "Docker support not compiled (KAIROS_DOCKER=OFF)"};
}

DockerClient::Response DockerClient::read_response(int) {
    return {0, "Docker support not compiled"};
}

bool DockerClient::ping() { return false; }

DockerClient::Response DockerClient::pull_image(
    const std::string&, const std::string&)
{
    return {0, "Docker support not compiled"};
}

DockerClient::Response DockerClient::create_container(
    const std::string&, const std::string&)
{
    return {0, "Docker support not compiled"};
}

DockerClient::Response DockerClient::start_container(const std::string&) {
    return {0, "Docker support not compiled"};
}

DockerClient::Response DockerClient::wait_container(const std::string&) {
    return {0, "Docker support not compiled"};
}

bool DockerClient::stream_logs(const std::string&, LogCallback,
                                std::stop_token) {
    return false;
}

DockerClient::Response DockerClient::stop_container(
    const std::string&, int) {
    return {0, "Docker support not compiled"};
}

DockerClient::Response DockerClient::kill_container(const std::string&) {
    return {0, "Docker support not compiled"};
}

DockerClient::Response DockerClient::remove_container(
    const std::string&, bool) {
    return {0, "Docker support not compiled"};
}

DockerClient::Response DockerClient::inspect_container(
    const std::string&) {
    return {0, "Docker support not compiled"};
}

bool DockerClient::image_exists(const std::string&) { return false; }

// ── Stub DockerProcessHandle ────────────────────────────────────────

DockerProcessHandle::DockerProcessHandle(const std::string& socket_path)
    : client_(socket_path) {}

DockerProcessHandle::~DockerProcessHandle() = default;

void DockerProcessHandle::set_output_callback(OutputCallback cb) {
    (void)cb;
}

const ProcessResult& DockerProcessHandle::result() const {
    return result_;
}

bool DockerProcessHandle::is_running() const { return false; }
int64_t DockerProcessHandle::pid() const { return 0; }

bool DockerProcessHandle::spawn(const ProcessSpec&) {
    result_.exit_code = 202;
    result_.termination = ProcessResult::TerminationKind::SpawnFailed;
    result_.termination_reason =
        "Docker runner not compiled (build with -DKAIROS_DOCKER=ON)";
    spdlog::error("{}", result_.termination_reason);
    return false;
}

ProcessResult DockerProcessHandle::wait(std::stop_token) {
    return result_;
}

void DockerProcessHandle::terminate() {}
void DockerProcessHandle::kill() {}

std::string DockerProcessHandle::build_create_body(
    const ProcessSpec&) const {
    return "{}";
}

int DockerProcessHandle::parse_exit_code(const std::string&) const {
    return 1;
}

bool DockerProcessHandle::ensure_image(const ProcessSpec&) {
    return false;
}

void DockerProcessHandle::cleanup() {}

// ── Factory functions ────────────────────────────────────────────────

std::string default_docker_socket_path() {
#ifdef _WIN32
    return "//./pipe/docker_engine";
#else
    return "/var/run/docker.sock";
#endif
}

std::unique_ptr<ProcessHandle> create_docker_process_handle() {
    return std::make_unique<DockerProcessHandle>(
        default_docker_socket_path());
}

std::unique_ptr<ProcessHandle> create_docker_process_handle(
    const std::string& socket_path)
{
    return std::make_unique<DockerProcessHandle>(socket_path);
}

}  // namespace kairos::exec
