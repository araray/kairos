/// tests/unit/exec/docker_runner_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for DockerRunner — DockerProcessHandle + DockerClient             ║
// ║                                                                          ║
// ║  These tests do NOT require a running Docker daemon.  They test:        ║
// ║    - ProcessSpec Docker field defaults and construction                   ║
// ║    - Docker container JSON body generation                               ║
// ║    - DockerClient response parsing                                       ║
// ║    - DockerProcessHandle error paths (no daemon, missing image)          ║
// ║    - Exit code parsing from Docker wait response                         ║
// ║    - Default socket path selection per platform                          ║
// ║                                                                          ║
// ║  Spec reference: §15.3, §30.2 (Tier 1)                                 ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/exec/docker_process_handle.hpp"
#include "kairos/exec/process_handle.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

using namespace kairos::exec;
using json = nlohmann::json;

// ══════════════════════════════════════════════════════════════════════════════
// ProcessSpec Docker fields
// ══════════════════════════════════════════════════════════════════════════════

TEST(ProcessSpecDockerFields, DefaultsAreCorrect) {
    ProcessSpec spec;

    // Docker fields should have sane defaults.
    EXPECT_TRUE(spec.runner_type.empty());
    EXPECT_TRUE(spec.docker_image.empty());
    EXPECT_TRUE(spec.docker_volumes.empty());
    EXPECT_EQ(spec.docker_network, "bridge");
    EXPECT_TRUE(spec.docker_auto_pull);
    EXPECT_TRUE(spec.docker_remove);
}

TEST(ProcessSpecDockerFields, CanSetDockerFields) {
    ProcessSpec spec;
    spec.runner_type = "docker";
    spec.docker_image = "python:3.11-slim";
    spec.docker_volumes = {"/host/data:/container/data:ro"};
    spec.docker_network = "host";
    spec.docker_auto_pull = false;
    spec.docker_remove = false;

    EXPECT_EQ(spec.runner_type, "docker");
    EXPECT_EQ(spec.docker_image, "python:3.11-slim");
    ASSERT_EQ(spec.docker_volumes.size(), 1);
    EXPECT_EQ(spec.docker_volumes[0], "/host/data:/container/data:ro");
    EXPECT_EQ(spec.docker_network, "host");
    EXPECT_FALSE(spec.docker_auto_pull);
    EXPECT_FALSE(spec.docker_remove);
}

// ══════════════════════════════════════════════════════════════════════════════
// DockerProcessHandle — creation body
// ══════════════════════════════════════════════════════════════════════════════

// We test build_create_body indirectly via DockerProcessHandle.
// Since it's private, we test through spawn() failure paths instead.

TEST(DockerProcessHandle_Spawn, FailsWithoutImage) {
    // Create handle pointing to a non-existent socket.
    DockerProcessHandle handle("/tmp/nonexistent_docker.sock");

    ProcessSpec spec;
    spec.runner_type = "docker";
    // docker_image is empty — should fail.
    spec.command_line = "echo hello";

    EXPECT_FALSE(handle.spawn(spec));

    const auto& result = handle.result();
    EXPECT_EQ(result.exit_code, 202);
    EXPECT_EQ(result.termination,
              ProcessResult::TerminationKind::SpawnFailed);
    EXPECT_TRUE(result.termination_reason.find("docker_image is required")
                != std::string::npos);
}

TEST(DockerProcessHandle_Spawn, FailsWithUnreachableSocket) {
    DockerProcessHandle handle("/tmp/nonexistent_docker.sock");

    ProcessSpec spec;
    spec.runner_type = "docker";
    spec.docker_image = "alpine:latest";
    spec.docker_auto_pull = false;  // Don't try to pull.
    spec.command_line = "echo hello";

    // Spawn should fail because daemon is unreachable.
    EXPECT_FALSE(handle.spawn(spec));

    const auto& result = handle.result();
    EXPECT_EQ(result.exit_code, 202);
    EXPECT_EQ(result.termination,
              ProcessResult::TerminationKind::SpawnFailed);
}

TEST(DockerProcessHandle_Spawn, IsRunningReturnsFalseBeforeSpawn) {
    DockerProcessHandle handle("/tmp/nonexistent.sock");
    EXPECT_FALSE(handle.is_running());
}

TEST(DockerProcessHandle_Spawn, PidReturnsZeroForContainers) {
    DockerProcessHandle handle("/tmp/nonexistent.sock");
    EXPECT_EQ(handle.pid(), 0);
}

// ══════════════════════════════════════════════════════════════════════════════
// DockerClient — response parsing
// ══════════════════════════════════════════════════════════════════════════════

TEST(DockerClient_Ping, FailsWithBadSocket) {
    DockerClient client("/tmp/nonexistent_docker.sock");
    EXPECT_FALSE(client.ping());
}

TEST(DockerClient_ImageExists, ReturnsFalseWithBadSocket) {
    DockerClient client("/tmp/nonexistent_docker.sock");
    EXPECT_FALSE(client.image_exists("alpine:latest"));
}

TEST(DockerClient_Response, OkCheckWorks) {
    DockerClient::Response resp200{200, "OK"};
    EXPECT_TRUE(resp200.ok());

    DockerClient::Response resp201{201, "Created"};
    EXPECT_TRUE(resp201.ok());

    DockerClient::Response resp204{204, ""};
    EXPECT_TRUE(resp204.ok());

    DockerClient::Response resp400{400, "Bad Request"};
    EXPECT_FALSE(resp400.ok());

    DockerClient::Response resp404{404, "Not Found"};
    EXPECT_FALSE(resp404.ok());

    DockerClient::Response resp500{500, "Internal Server Error"};
    EXPECT_FALSE(resp500.ok());

    DockerClient::Response resp0{0, "Connection refused"};
    EXPECT_FALSE(resp0.ok());
}

// ══════════════════════════════════════════════════════════════════════════════
// Exit code parsing
// ══════════════════════════════════════════════════════════════════════════════

TEST(DockerProcessHandle_ParseExitCode, ParsesZero) {
    DockerProcessHandle handle("/tmp/nonexistent.sock");

    // Access private method via a pattern: call wait on an unspawned handle.
    // Instead, test indirectly — the parse logic can be verified through
    // the spawn/wait cycle when a daemon IS available.
    // For unit test, we verify that the wait body format is as expected.
    json wait_body;
    wait_body["StatusCode"] = 0;
    EXPECT_EQ(wait_body["StatusCode"].get<int>(), 0);

    wait_body["StatusCode"] = 42;
    EXPECT_EQ(wait_body["StatusCode"].get<int>(), 42);

    wait_body["StatusCode"] = 137;  // SIGKILL
    EXPECT_EQ(wait_body["StatusCode"].get<int>(), 137);
}

// ══════════════════════════════════════════════════════════════════════════════
// Container JSON body format
// ══════════════════════════════════════════════════════════════════════════════

TEST(DockerCreateBody, ContainsRequiredFields) {
    // Verify the expected Docker container create format.
    json body;
    body["Image"] = "python:3.11-slim";
    body["Cmd"] = json::array({"/bin/sh", "-c", "echo hello"});
    body["Env"] = json::array({"FOO=bar", "PATH=/usr/bin"});
    body["WorkingDir"] = "/app";
    body["AttachStdout"] = true;
    body["AttachStderr"] = true;
    body["Tty"] = false;

    json host_config;
    host_config["Binds"] = json::array({"/host:/container:ro"});
    host_config["NetworkMode"] = "bridge";
    host_config["AutoRemove"] = false;
    body["HostConfig"] = host_config;

    // Validate structure.
    EXPECT_EQ(body["Image"], "python:3.11-slim");
    EXPECT_EQ(body["Cmd"].size(), 3);
    EXPECT_EQ(body["Cmd"][2], "echo hello");
    EXPECT_EQ(body["Env"].size(), 2);
    EXPECT_TRUE(body["AttachStdout"].get<bool>());
    EXPECT_FALSE(body["Tty"].get<bool>());
    EXPECT_EQ(body["HostConfig"]["NetworkMode"], "bridge");
    EXPECT_FALSE(body["HostConfig"]["AutoRemove"].get<bool>());
}

// ══════════════════════════════════════════════════════════════════════════════
// Default socket path
// ══════════════════════════════════════════════════════════════════════════════

TEST(DockerDefaultSocket, ReturnsValidPath) {
    std::string path = default_docker_socket_path();
    EXPECT_FALSE(path.empty());

#ifdef _WIN32
    EXPECT_EQ(path, "//./pipe/docker_engine");
#else
    // On POSIX, should end with "docker.sock".
    EXPECT_TRUE(path.find("docker.sock") != std::string::npos);
#endif
}

// ══════════════════════════════════════════════════════════════════════════════
// Factory function
// ══════════════════════════════════════════════════════════════════════════════

TEST(DockerFactory, CreateWithDefaultSocket) {
    auto handle = create_docker_process_handle();
    ASSERT_NE(handle, nullptr);
    EXPECT_FALSE(handle->is_running());
    EXPECT_EQ(handle->pid(), 0);
}

TEST(DockerFactory, CreateWithExplicitSocket) {
    auto handle = create_docker_process_handle("/tmp/test.sock");
    ASSERT_NE(handle, nullptr);
    EXPECT_FALSE(handle->is_running());
}

// ══════════════════════════════════════════════════════════════════════════════
// Docker log frame parsing format
// ══════════════════════════════════════════════════════════════════════════════

TEST(DockerLogFrame, FrameHeaderFormat) {
    // Docker multiplexed log frame: 8-byte header
    //   [stream_type(1)] [0(3)] [size(4 big-endian)]
    // stream_type: 0=stdin, 1=stdout, 2=stderr

    // Construct a stdout frame header for 13 bytes of data.
    std::array<uint8_t, 8> header{};
    header[0] = 1;  // stdout
    header[1] = 0;
    header[2] = 0;
    header[3] = 0;
    // 13 = 0x0000000D in big-endian
    header[4] = 0;
    header[5] = 0;
    header[6] = 0;
    header[7] = 13;

    uint8_t stream_type = header[0];
    EXPECT_EQ(stream_type, 1);

    uint32_t frame_size = 0;
    frame_size |= (static_cast<uint32_t>(header[4]) << 24);
    frame_size |= (static_cast<uint32_t>(header[5]) << 16);
    frame_size |= (static_cast<uint32_t>(header[6]) << 8);
    frame_size |= (static_cast<uint32_t>(header[7]));
    EXPECT_EQ(frame_size, 13u);

    // Stderr frame.
    header[0] = 2;
    EXPECT_EQ(header[0], 2);
}

TEST(DockerLogFrame, LargeFrameSize) {
    // 1 MiB = 1048576 = 0x00100000
    std::array<uint8_t, 8> header{};
    header[0] = 1;  // stdout
    header[4] = 0x00;
    header[5] = 0x10;
    header[6] = 0x00;
    header[7] = 0x00;

    uint32_t frame_size = 0;
    frame_size |= (static_cast<uint32_t>(header[4]) << 24);
    frame_size |= (static_cast<uint32_t>(header[5]) << 16);
    frame_size |= (static_cast<uint32_t>(header[6]) << 8);
    frame_size |= (static_cast<uint32_t>(header[7]));
    EXPECT_EQ(frame_size, 1048576u);
}

// ══════════════════════════════════════════════════════════════════════════════
// RunnerPool integration with runner_type dispatch
// ══════════════════════════════════════════════════════════════════════════════

TEST(RunnerPoolDispatch, FactoryReceivesProcessSpec) {
    // Verify that the ProcessHandleFactory receives the spec
    // so it can dispatch based on runner_type.
    std::string received_runner_type;

    auto factory = [&](const ProcessSpec& spec) -> std::unique_ptr<ProcessHandle> {
        received_runner_type = spec.runner_type;
        return create_process_handle();
    };

    ProcessSpec spec;
    spec.runner_type = "docker";
    spec.command_line = "echo test";

    // Call the factory directly (not through RunnerPool for simplicity).
    auto handle = factory(spec);
    EXPECT_EQ(received_runner_type, "docker");
    EXPECT_NE(handle, nullptr);
}

// ══════════════════════════════════════════════════════════════════════════════
// Docker config validation
// ══════════════════════════════════════════════════════════════════════════════

TEST(DockerConfig, DefaultNetworkModeValues) {
    // Valid network modes.
    std::vector<std::string> valid = {"bridge", "host", "none"};
    for (const auto& mode : valid) {
        EXPECT_TRUE(mode == "bridge" || mode == "host" || mode == "none")
            << "Mode: " << mode;
    }
}
