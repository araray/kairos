/// include/kairos/exec/docker_process_handle.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  DockerProcessHandle — Execute steps in Docker containers               ║
// ║                                                                          ║
// ║  Implements the ProcessHandle interface using the Docker Engine          ║
// ║  REST API v1.43+ over Unix domain socket (POSIX) or named pipe (Win).   ║
// ║                                                                          ║
// ║  Container lifecycle per step:                                           ║
// ║    1. Create container (POST /containers/create)                         ║
// ║    2. Start container (POST /containers/{id}/start)                      ║
// ║    3. Stream logs (GET /containers/{id}/logs?follow=true)                ║
// ║    4. Wait for exit (POST /containers/{id}/wait)                         ║
// ║    5. Remove container (DELETE /containers/{id})                         ║
// ║                                                                          ║
// ║  Spec reference: §15.3                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/exec/process_handle.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace kairos::exec {

// ── Docker Engine API client (HTTP-over-UDS) ───────────────────────────

/// Low-level Docker Engine API client.  Communicates over a Unix domain
/// socket (Linux/macOS) or named pipe (Windows).
///
/// Thread safety: methods are NOT thread-safe — each DockerProcessHandle
/// holds its own DockerClient instance.
class DockerClient {
public:
    /// Construct a client connected to the Docker socket.
    /// @param socket_path  Path to the Docker daemon socket.
    ///   Linux:   /var/run/docker.sock
    ///   macOS:   /var/run/docker.sock (or ~/.docker/run/docker.sock)
    ///   Windows: //./pipe/docker_engine
    explicit DockerClient(const std::string& socket_path);
    ~DockerClient();

    // Non-copyable, movable.
    DockerClient(const DockerClient&) = delete;
    DockerClient& operator=(const DockerClient&) = delete;
    DockerClient(DockerClient&&) noexcept;
    DockerClient& operator=(DockerClient&&) noexcept;

    /// Response from the Docker Engine API.
    struct Response {
        int status_code = 0;
        std::string body;
        bool ok() const { return status_code >= 200 && status_code < 300; }
    };

    /// Check if the Docker daemon is reachable.
    /// Sends GET /_ping and expects "OK".
    [[nodiscard]] bool ping();

    /// POST /images/create?fromImage={image}&tag={tag}
    /// Pulls an image from Docker Hub.
    Response pull_image(const std::string& image, const std::string& tag);

    /// POST /containers/create
    /// Creates a container from JSON config body.
    /// @return Container ID on success, or error message on failure.
    Response create_container(const std::string& json_body,
                              const std::string& name = "");

    /// POST /containers/{id}/start
    Response start_container(const std::string& container_id);

    /// POST /containers/{id}/wait
    /// Blocks until the container exits.
    Response wait_container(const std::string& container_id);

    /// GET /containers/{id}/logs?follow=true&stdout=true&stderr=true
    /// Streams log output and invokes callback per chunk.
    /// Docker uses a multiplexed frame format:
    ///   Header: [stream_type(1)] [0(3)] [size(4 big-endian)]
    ///   stream_type: 0=stdin, 1=stdout, 2=stderr
    /// The callback receives (chunk_data, is_stderr).
    using LogCallback = std::function<void(std::string_view, bool is_stderr)>;
    bool stream_logs(const std::string& container_id,
                     LogCallback callback,
                     std::stop_token stop);

    /// POST /containers/{id}/stop?t={grace_seconds}
    Response stop_container(const std::string& container_id,
                            int grace_seconds);

    /// POST /containers/{id}/kill
    Response kill_container(const std::string& container_id);

    /// DELETE /containers/{id}?force=true
    Response remove_container(const std::string& container_id,
                              bool force = true);

    /// GET /containers/{id}/json — inspect container.
    Response inspect_container(const std::string& container_id);

    /// Check if an image exists locally.
    /// GET /images/{name}/json
    bool image_exists(const std::string& image);

private:
    /// Internal: connect to the Unix domain socket.
    /// Returns the connected socket fd, or -1 on failure.
    int connect_socket();

    /// Internal: send an HTTP request and read the response.
    Response http_request(const std::string& method,
                          const std::string& path,
                          const std::string& body = "",
                          const std::string& content_type = "");

    /// Internal: read a full HTTP response from the socket.
    Response read_response(int fd);

    /// Internal: read exactly n bytes from a socket fd.
    bool read_exact(int fd, char* buf, std::size_t n);

    std::string socket_path_;
};

// ── DockerProcessHandle ────────────────────────────────────────────────

/// ProcessHandle implementation that executes steps inside Docker
/// containers instead of spawning local child processes.
///
/// The ProcessSpec is translated to a Docker container configuration:
///   - spec.command_line → Cmd: ["/bin/sh", "-c", command_line]
///   - spec.environment  → Env: ["KEY=VALUE", ...]
///   - spec.working_dir  → WorkingDir: "/path"
///   - spec.docker_image → Image: "python:3.11-slim"
///   - spec.docker_volumes → HostConfig.Binds: [...]
///   - spec.docker_network → HostConfig.NetworkMode: "bridge"
///   - spec.timeout      → Enforced via std::jthread + stop
///   - spec.kill_timeout  → Grace period for stop before kill
class DockerProcessHandle : public ProcessHandle {
public:
    /// Construct with the Docker daemon socket path.
    explicit DockerProcessHandle(const std::string& socket_path);
    ~DockerProcessHandle() override;

    // ── ProcessHandle interface ──────────────────────────────────────

    [[nodiscard]] bool spawn(const ProcessSpec& spec) override;
    ProcessResult wait(std::stop_token stop) override;
    void terminate() override;
    void kill() override;
    [[nodiscard]] bool is_running() const override;
    [[nodiscard]] int64_t pid() const override;
    void set_output_callback(OutputCallback cb) override;
    [[nodiscard]] const ProcessResult& result() const override;

private:
    /// Build Docker container creation JSON from ProcessSpec.
    std::string build_create_body(const ProcessSpec& spec) const;

    /// Parse "StatusCode" from wait response body.
    int parse_exit_code(const std::string& wait_body) const;

    /// Ensure the image exists locally (pull if needed + auto_pull).
    bool ensure_image(const ProcessSpec& spec);

    /// Clean up the container (stop + remove).
    void cleanup();

    DockerClient client_;
    std::string container_id_;
    OutputCallback output_callback_;
    ProcessResult result_;
    ProcessSpec spec_;  // Cached for timeout/cleanup.
    std::atomic<bool> running_{false};
    std::atomic<bool> spawned_{false};
};

/// Factory function for DockerProcessHandle.
/// Uses the default Docker socket path for the platform.
std::unique_ptr<ProcessHandle> create_docker_process_handle();

/// Factory function with explicit socket path.
std::unique_ptr<ProcessHandle> create_docker_process_handle(
    const std::string& socket_path);

/// Return the default Docker socket path for the current platform.
std::string default_docker_socket_path();

}  // namespace kairos::exec
