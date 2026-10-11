/// src/exec/docker_process_handle.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  DockerProcessHandle — Docker Engine API client + ProcessHandle impl     ║
// ║                                                                          ║
// ║  Communicates with the Docker daemon via HTTP-over-UDS (Unix domain     ║
// ║  socket) on POSIX.  Windows named pipe support is stubbed (§25.2).      ║
// ║                                                                          ║
// ║  Spec reference: §15.3                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/exec/docker_process_handle.hpp"
#include "kairos/platform/environment.hpp"

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

// Platform-specific socket includes.
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#else
// Windows named pipe — minimal stub for forward declaration.
// Full Win32 implementation deferred to Phase 5 Batch 3.
#include <windows.h>
#endif

namespace kairos::exec {

using json = nlohmann::json;
using namespace std::chrono_literals;

// ══════════════════════════════════════════════════════════════════════════════
// DockerClient implementation
// ══════════════════════════════════════════════════════════════════════════════

DockerClient::DockerClient(const std::string& socket_path)
    : socket_path_(socket_path) {}

DockerClient::~DockerClient() = default;

DockerClient::DockerClient(DockerClient&&) noexcept = default;
DockerClient& DockerClient::operator=(DockerClient&&) noexcept = default;

#ifndef _WIN32

// ── POSIX socket connection ──────────────────────────────────────────────

int DockerClient::connect_socket() {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        spdlog::error("Docker: failed to create Unix socket: {}",
                      std::strerror(errno));
        return -1;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;

    if (socket_path_.size() >= sizeof(addr.sun_path)) {
        spdlog::error("Docker: socket path too long: {}", socket_path_);
        ::close(fd);
        return -1;
    }
    std::strncpy(addr.sun_path, socket_path_.c_str(),
                 sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) < 0) {
        spdlog::error("Docker: failed to connect to {}: {}",
                      socket_path_, std::strerror(errno));
        ::close(fd);
        return -1;
    }

    return fd;
}

bool DockerClient::read_exact(int fd, char* buf, std::size_t n) {
    std::size_t total = 0;
    while (total < n) {
        auto r = ::read(fd, buf + total, n - total);
        if (r <= 0) return false;
        total += static_cast<std::size_t>(r);
    }
    return true;
}

// ── HTTP request/response over UDS ──────────────────────────────────────

DockerClient::Response DockerClient::http_request(
    const std::string& method,
    const std::string& path,
    const std::string& body,
    const std::string& content_type)
{
    int fd = connect_socket();
    if (fd < 0) {
        return {0, "Failed to connect to Docker daemon"};
    }

    // Build HTTP request.
    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\n";
    req << "Host: localhost\r\n";
    req << "Connection: close\r\n";

    if (!body.empty()) {
        req << "Content-Type: "
            << (content_type.empty() ? "application/json" : content_type)
            << "\r\n";
        req << "Content-Length: " << body.size() << "\r\n";
    }
    req << "\r\n";
    if (!body.empty()) {
        req << body;
    }

    std::string request_str = req.str();
    auto written = ::write(fd, request_str.c_str(), request_str.size());
    if (written < 0 ||
        static_cast<std::size_t>(written) != request_str.size()) {
        ::close(fd);
        return {0, "Failed to send request"};
    }

    // Read response.
    auto response = read_response(fd);
    ::close(fd);
    return response;
}

DockerClient::Response DockerClient::read_response(int fd) {
    Response resp;

    // Read the full response into a buffer.
    std::string raw;
    std::array<char, 8192> buf{};
    while (true) {
        auto n = ::read(fd, buf.data(), buf.size());
        if (n <= 0) break;
        raw.append(buf.data(), static_cast<std::size_t>(n));
    }

    if (raw.empty()) {
        resp.status_code = 0;
        resp.body = "Empty response from Docker daemon";
        return resp;
    }

    // Parse status line: "HTTP/1.1 200 OK\r\n"
    auto status_end = raw.find("\r\n");
    if (status_end == std::string::npos) {
        resp.status_code = 0;
        resp.body = "Malformed HTTP response";
        return resp;
    }

    auto status_line = raw.substr(0, status_end);
    auto sp1 = status_line.find(' ');
    if (sp1 != std::string::npos) {
        auto sp2 = status_line.find(' ', sp1 + 1);
        auto code_str = status_line.substr(
            sp1 + 1,
            (sp2 != std::string::npos) ? sp2 - sp1 - 1 : std::string::npos);
        try {
            resp.status_code = std::stoi(code_str);
        } catch (...) {
            resp.status_code = 0;
        }
    }

    // Find body after "\r\n\r\n".
    auto body_start = raw.find("\r\n\r\n");
    if (body_start != std::string::npos) {
        auto body_raw = raw.substr(body_start + 4);

        // Handle chunked transfer encoding.
        auto te_pos = raw.find("Transfer-Encoding: chunked");
        if (te_pos != std::string::npos && te_pos < body_start) {
            // Decode chunked body.
            std::string decoded;
            std::string_view remaining(body_raw);

            while (!remaining.empty()) {
                auto crlf = remaining.find("\r\n");
                if (crlf == std::string_view::npos) break;

                auto chunk_size_str = remaining.substr(0, crlf);
                unsigned long chunk_size = 0;
                try {
                    chunk_size = std::stoul(
                        std::string(chunk_size_str), nullptr, 16);
                } catch (...) {
                    break;
                }

                if (chunk_size == 0) break;  // Terminal chunk.

                remaining.remove_prefix(crlf + 2);
                if (remaining.size() < chunk_size) break;

                decoded.append(remaining.data(), chunk_size);
                remaining.remove_prefix(chunk_size);

                // Skip trailing \r\n after chunk data.
                if (remaining.size() >= 2 &&
                    remaining[0] == '\r' && remaining[1] == '\n') {
                    remaining.remove_prefix(2);
                }
            }
            resp.body = std::move(decoded);
        } else {
            resp.body = std::move(body_raw);
        }
    }

    return resp;
}

#else  // _WIN32

// ── Windows named pipe (minimal stub) ────────────────────────────────────

int DockerClient::connect_socket() {
    // Windows named pipe connection to //./pipe/docker_engine.
    // Full implementation deferred.
    spdlog::error("Docker: Windows named pipe not yet implemented");
    return -1;
}

bool DockerClient::read_exact(int fd, char* buf, std::size_t n) {
    (void)fd; (void)buf; (void)n;
    return false;
}

DockerClient::Response DockerClient::http_request(
    const std::string& method,
    const std::string& path,
    const std::string& body,
    const std::string& content_type)
{
    (void)method; (void)path; (void)body; (void)content_type;
    return {0, "Docker runner not yet supported on Windows"};
}

DockerClient::Response DockerClient::read_response(int fd) {
    (void)fd;
    return {0, "Not implemented on Windows"};
}

#endif  // _WIN32

// ── Docker API methods ─────────────────────────────────────────────────

bool DockerClient::ping() {
    auto resp = http_request("GET", "/_ping");
    return resp.ok() && resp.body.find("OK") != std::string::npos;
}

DockerClient::Response DockerClient::pull_image(
    const std::string& image, const std::string& tag)
{
    std::string path = "/images/create?fromImage=" + image;
    if (!tag.empty()) {
        path += "&tag=" + tag;
    }
    return http_request("POST", path);
}

DockerClient::Response DockerClient::create_container(
    const std::string& json_body, const std::string& name)
{
    std::string path = "/containers/create";
    if (!name.empty()) {
        path += "?name=" + name;
    }
    return http_request("POST", path, json_body, "application/json");
}

DockerClient::Response DockerClient::start_container(
    const std::string& container_id)
{
    return http_request("POST",
        "/containers/" + container_id + "/start");
}

DockerClient::Response DockerClient::wait_container(
    const std::string& container_id)
{
    return http_request("POST",
        "/containers/" + container_id + "/wait");
}

bool DockerClient::stream_logs(
    const std::string& container_id,
    LogCallback callback,
    std::stop_token stop)
{
#ifndef _WIN32
    int fd = connect_socket();
    if (fd < 0) return false;

    // Build request.
    std::string path =
        "/containers/" + container_id +
        "/logs?follow=true&stdout=true&stderr=true&timestamps=false";

    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n";
    req << "Host: localhost\r\n";
    req << "Connection: close\r\n";
    req << "\r\n";

    auto request_str = req.str();
    auto written = ::write(fd, request_str.c_str(), request_str.size());
    if (written < 0) {
        ::close(fd);
        return false;
    }

    // Read past HTTP headers.
    std::string header_buf;
    while (true) {
        char ch;
        auto r = ::read(fd, &ch, 1);
        if (r <= 0) { ::close(fd); return false; }
        header_buf.push_back(ch);
        if (header_buf.size() >= 4 &&
            header_buf.substr(header_buf.size() - 4) == "\r\n\r\n") {
            break;
        }
        // Safety: header shouldn't exceed 64KB.
        if (header_buf.size() > 65536) { ::close(fd); return false; }
    }

    // Read Docker multiplexed log frames.
    // Header format: [stream_type(1)][0(3)][size(4 big-endian)]
    //   stream_type: 0=stdin, 1=stdout, 2=stderr
    while (!stop.stop_requested()) {
        // Set non-blocking for poll.
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;

        int poll_rc = ::poll(&pfd, 1, 200);  // 200ms timeout.
        if (poll_rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (poll_rc == 0) continue;  // Timeout, check stop.

        // Read 8-byte frame header.
        std::array<char, 8> frame_hdr{};
        if (!read_exact(fd, frame_hdr.data(), 8)) {
            break;  // EOF or error — container exited.
        }

        uint8_t stream_type = static_cast<uint8_t>(frame_hdr[0]);
        bool is_stderr = (stream_type == 2);

        // Frame size is big-endian uint32 at bytes 4–7.
        uint32_t frame_size = 0;
        frame_size |= (static_cast<uint32_t>(
                           static_cast<uint8_t>(frame_hdr[4])) << 24);
        frame_size |= (static_cast<uint32_t>(
                           static_cast<uint8_t>(frame_hdr[5])) << 16);
        frame_size |= (static_cast<uint32_t>(
                           static_cast<uint8_t>(frame_hdr[6])) << 8);
        frame_size |= (static_cast<uint32_t>(
                           static_cast<uint8_t>(frame_hdr[7])));

        if (frame_size == 0) continue;

        // Cap single frame read to 1 MiB for safety.
        if (frame_size > 1048576) {
            spdlog::warn("Docker: frame size {} exceeds 1 MiB limit",
                         frame_size);
            break;
        }

        // Read frame data.
        std::string frame_data(frame_size, '\0');
        if (!read_exact(fd, frame_data.data(), frame_size)) {
            break;
        }

        // Deliver to callback.
        if (callback) {
            callback(frame_data, is_stderr);
        }
    }

    ::close(fd);
    return true;
#else
    (void)container_id; (void)callback; (void)stop;
    return false;
#endif
}

DockerClient::Response DockerClient::stop_container(
    const std::string& container_id, int grace_seconds)
{
    return http_request("POST",
        "/containers/" + container_id +
        "/stop?t=" + std::to_string(grace_seconds));
}

DockerClient::Response DockerClient::kill_container(
    const std::string& container_id)
{
    return http_request("POST",
        "/containers/" + container_id + "/kill");
}

DockerClient::Response DockerClient::remove_container(
    const std::string& container_id, bool force)
{
    std::string path = "/containers/" + container_id;
    if (force) path += "?force=true";
    return http_request("DELETE", path);
}

DockerClient::Response DockerClient::inspect_container(
    const std::string& container_id)
{
    return http_request("GET",
        "/containers/" + container_id + "/json");
}

bool DockerClient::image_exists(const std::string& image) {
    auto resp = http_request("GET", "/images/" + image + "/json");
    return resp.ok();
}

// ══════════════════════════════════════════════════════════════════════════════
// DockerProcessHandle implementation
// ══════════════════════════════════════════════════════════════════════════════

DockerProcessHandle::DockerProcessHandle(const std::string& socket_path)
    : client_(socket_path) {}

DockerProcessHandle::~DockerProcessHandle() {
    // Ensure cleanup on destruction — remove container if it exists.
    if (spawned_ && !container_id_.empty()) {
        cleanup();
    }
}

// ── ProcessHandle interface ─────────────────────────────────────────────

void DockerProcessHandle::set_output_callback(OutputCallback cb) {
    output_callback_ = std::move(cb);
}

const ProcessResult& DockerProcessHandle::result() const {
    return result_;
}

bool DockerProcessHandle::is_running() const {
    return running_.load(std::memory_order_relaxed);
}

int64_t DockerProcessHandle::pid() const {
    // Docker containers don't have a meaningful PID from the host's
    // perspective in this abstraction.  Return 0 to indicate "container".
    return 0;
}

bool DockerProcessHandle::spawn(const ProcessSpec& spec) {
    spec_ = spec;
    auto start_time = std::chrono::steady_clock::now();

    // Validate: image is required for Docker runner.
    if (spec.docker_image.empty()) {
        result_.exit_code = 202;
        result_.termination = ProcessResult::TerminationKind::SpawnFailed;
        result_.termination_reason =
            "Docker runner: docker_image is required";
        spdlog::error("{}", result_.termination_reason);
        return false;
    }

    // Step 0: Ensure image exists (auto-pull if needed).
    if (!ensure_image(spec)) {
        result_.exit_code = 202;
        result_.termination = ProcessResult::TerminationKind::SpawnFailed;
        result_.termination_reason =
            "Docker runner: image '" + spec.docker_image +
            "' not available and pull failed";
        spdlog::error("{}", result_.termination_reason);
        return false;
    }

    // Step 1: Create container.
    std::string create_body = build_create_body(spec);

    spdlog::debug("Docker: creating container (image={})",
                  spec.docker_image);

    auto create_resp = client_.create_container(create_body);
    if (!create_resp.ok()) {
        result_.exit_code = 202;
        result_.termination = ProcessResult::TerminationKind::SpawnFailed;
        result_.termination_reason =
            "Docker: container create failed (HTTP " +
            std::to_string(create_resp.status_code) + "): " +
            create_resp.body;
        spdlog::error("{}", result_.termination_reason);
        return false;
    }

    // Parse container ID from response.
    try {
        auto resp_json = json::parse(create_resp.body);
        container_id_ = resp_json.value("Id", "");
    } catch (const json::exception& e) {
        result_.exit_code = 202;
        result_.termination = ProcessResult::TerminationKind::SpawnFailed;
        result_.termination_reason =
            "Docker: failed to parse container ID: " + std::string(e.what());
        return false;
    }

    if (container_id_.empty()) {
        result_.exit_code = 202;
        result_.termination = ProcessResult::TerminationKind::SpawnFailed;
        result_.termination_reason = "Docker: empty container ID returned";
        return false;
    }

    // Truncate container ID for logging (first 12 chars, like Docker CLI).
    std::string short_id = container_id_.substr(
        0, std::min<std::size_t>(container_id_.size(), 12));

    spdlog::info("Docker: container {} created (image={})",
                 short_id, spec.docker_image);

    // Step 2: Start container.
    auto start_resp = client_.start_container(container_id_);
    if (!start_resp.ok()) {
        result_.exit_code = 202;
        result_.termination = ProcessResult::TerminationKind::SpawnFailed;
        result_.termination_reason =
            "Docker: container start failed (HTTP " +
            std::to_string(start_resp.status_code) + "): " +
            start_resp.body;
        spdlog::error("{}", result_.termination_reason);
        cleanup();
        return false;
    }

    spawned_ = true;
    running_ = true;

    spdlog::debug("Docker: container {} started", short_id);
    return true;
}

ProcessResult DockerProcessHandle::wait(std::stop_token stop) {
    auto start_time = std::chrono::steady_clock::now();

    if (!spawned_ || container_id_.empty()) {
        return result_;
    }

    std::string short_id = container_id_.substr(
        0, std::min<std::size_t>(container_id_.size(), 12));

    // Stream logs in the background while waiting.
    // We use a separate thread because stream_logs blocks until the
    // container exits (the Docker logs API with follow=true blocks).
    std::jthread log_thread;
    if (output_callback_) {
        log_thread = std::jthread(
            [this, stop](std::stop_token thread_stop) {
                (void)thread_stop;
                client_.stream_logs(
                    container_id_,
                    [this](std::string_view data, bool is_stderr) {
                        if (output_callback_) {
                            output_callback_(data, is_stderr);
                        }
                    },
                    stop);
            });
    }

    // Timeout enforcement via a separate thread.
    std::stop_source timeout_source;
    std::jthread timeout_thread;
    if (spec_.timeout.has_value()) {
        timeout_thread = std::jthread(
            [this, stop, &timeout_source](std::stop_token t_stop) {
                auto deadline = std::chrono::steady_clock::now() +
                                *spec_.timeout;
                while (!t_stop.stop_requested() &&
                       !stop.stop_requested()) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        spdlog::warn("Docker: container timeout, stopping");
                        timeout_source.request_stop();
                        return;
                    }
                    std::this_thread::sleep_for(200ms);
                }
            });
    }

    // Wait for container exit (blocking call).
    // Also monitor stop_token and timeout in parallel.
    std::jthread wait_thread([this](std::stop_token) {
        auto resp = client_.wait_container(container_id_);
        if (resp.ok()) {
            result_.exit_code = parse_exit_code(resp.body);
            result_.termination = ProcessResult::TerminationKind::Normal;
        } else {
            // wait failed — inspect container for status.
            result_.exit_code = 1;
            result_.termination = ProcessResult::TerminationKind::Normal;
            result_.termination_reason =
                "Docker wait failed: " + resp.body;
        }
        running_ = false;
    });

    // Poll until: container exits, stop requested, or timeout.
    while (running_.load(std::memory_order_relaxed)) {
        if (stop.stop_requested()) {
            spdlog::info("Docker: cancellation requested for {}", short_id);
            result_.termination = ProcessResult::TerminationKind::Cancelled;
            result_.termination_reason = "Cancelled via stop_token";
            result_.exit_code = 130;  // Conventional SIGINT exit.
            terminate();
            break;
        }
        if (timeout_source.stop_requested()) {
            spdlog::warn("Docker: timeout exceeded for {}", short_id);
            result_.termination = ProcessResult::TerminationKind::SoftKill;
            result_.termination_reason = "Timeout exceeded";
            result_.exit_code = 200;
            terminate();
            break;
        }
        std::this_thread::sleep_for(100ms);
    }

    // Wait for threads to complete.
    if (timeout_thread.joinable()) {
        timeout_thread.request_stop();
        timeout_thread.join();
    }
    if (wait_thread.joinable()) {
        wait_thread.join();
    }
    if (log_thread.joinable()) {
        log_thread.request_stop();
        log_thread.join();
    }

    running_ = false;

    auto end_time = std::chrono::steady_clock::now();
    result_.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);

    // Cleanup: remove container (unless debug mode).
    if (spec_.docker_remove) {
        cleanup();
    }

    spdlog::info("Docker: container {} finished (exit_code={}, {}ms)",
                 short_id, result_.exit_code, result_.duration.count());

    return result_;
}

void DockerProcessHandle::terminate() {
    if (!spawned_ || container_id_.empty()) return;

    auto grace = static_cast<int>(spec_.kill_timeout.count());
    spdlog::debug("Docker: stopping container {} (grace={}s)",
                  container_id_.substr(0, 12), grace);

    client_.stop_container(container_id_, grace);
}

void DockerProcessHandle::kill() {
    if (!spawned_ || container_id_.empty()) return;

    spdlog::debug("Docker: killing container {}",
                  container_id_.substr(0, 12));

    client_.kill_container(container_id_);
}

// ── Private helpers ─────────────────────────────────────────────────────

std::string DockerProcessHandle::build_create_body(
    const ProcessSpec& spec) const
{
    json body;

    // Image.
    body["Image"] = spec.docker_image;

    // Command: wrap in shell for consistency with local_shell runner.
    body["Cmd"] = json::array({"/bin/sh", "-c", spec.command_line});

    // Environment variables as "KEY=VALUE" array.
    json env_arr = json::array();
    for (const auto& [key, val] : spec.environment) {
        env_arr.push_back(key + "=" + val);
    }
    body["Env"] = env_arr;

    // Working directory.
    if (!spec.working_dir.empty()) {
        body["WorkingDir"] = spec.working_dir.string();
    }

    // Attach stdout/stderr for log streaming.
    body["AttachStdout"] = true;
    body["AttachStderr"] = true;
    body["Tty"] = false;

    // Host config.
    json host_config;

    // Volume mounts.
    if (!spec.docker_volumes.empty()) {
        host_config["Binds"] = spec.docker_volumes;
    }

    // Network mode.
    if (!spec.docker_network.empty()) {
        host_config["NetworkMode"] = spec.docker_network;
    }

    // Auto-remove is handled by Kairos (not Docker auto-remove),
    // so we always set AutoRemove=false.
    host_config["AutoRemove"] = false;

    body["HostConfig"] = host_config;

    return body.dump();
}

int DockerProcessHandle::parse_exit_code(
    const std::string& wait_body) const
{
    try {
        auto j = json::parse(wait_body);
        return j.value("StatusCode", 1);
    } catch (...) {
        spdlog::warn("Docker: failed to parse exit code from: {}",
                     wait_body);
        return 1;
    }
}

bool DockerProcessHandle::ensure_image(const ProcessSpec& spec) {
    // Check if image exists locally.
    if (client_.image_exists(spec.docker_image)) {
        return true;
    }

    if (!spec.docker_auto_pull) {
        spdlog::error(
            "Docker: image '{}' not found locally and auto_pull is disabled",
            spec.docker_image);
        return false;
    }

    // Parse image:tag.
    std::string image = spec.docker_image;
    std::string tag = "latest";
    auto colon = image.rfind(':');
    if (colon != std::string::npos) {
        tag = image.substr(colon + 1);
        image = image.substr(0, colon);
    }

    spdlog::info("Docker: pulling image {}:{}", image, tag);

    auto resp = client_.pull_image(image, tag);
    if (!resp.ok()) {
        spdlog::error("Docker: pull failed (HTTP {}): {}",
                      resp.status_code, resp.body);
        return false;
    }

    spdlog::info("Docker: image {}:{} pulled successfully", image, tag);
    return true;
}

void DockerProcessHandle::cleanup() {
    if (container_id_.empty()) return;

    std::string short_id = container_id_.substr(
        0, std::min<std::size_t>(container_id_.size(), 12));

    // Remove the container (force=true to handle running containers).
    auto resp = client_.remove_container(container_id_, true);
    if (resp.ok() || resp.status_code == 404) {
        spdlog::debug("Docker: container {} removed", short_id);
    } else {
        spdlog::warn("Docker: failed to remove container {} (HTTP {})",
                     short_id, resp.status_code);
    }

    container_id_.clear();
}

// ── Factory functions ───────────────────────────────────────────────────

std::string default_docker_socket_path() {
#ifdef _WIN32
    return "//./pipe/docker_engine";
#elif defined(__APPLE__)
    // macOS: try Docker Desktop default first, then standard path.
    auto home = platform::get_env("HOME");
    if (home) {
        std::string desktop_path =
            *home + "/.docker/run/docker.sock";
        if (std::filesystem::exists(desktop_path)) {
            return desktop_path;
        }
    }
    return "/var/run/docker.sock";
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
