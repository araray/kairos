/// include/kairos/testing/fake_process.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/testing/fake_process.hpp — Fake process handle for tests         ║
// ║                                                                           ║
// ║  Simulates child process behavior (exit codes, output, timeouts)         ║
// ║  without spawning real processes. Used by runner pool tests and          ║
// ║  pipeline component tests.                                                ║
// ║                                                                           ║
// ║  Spec reference: §30.5                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/exec/process_handle.hpp"
#include "kairos/exec/runner_pool.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace kairos::testing {

/// A fake process that produces scripted output and exit codes.
///
/// Usage:
///   auto proc = std::make_shared<FakeProcessHandle>();
///   proc->set_stdout_data("hello\nworld\n");
///   proc->set_exit_code(0);
///   // When the runner calls proc->spawn() and proc->wait():
///   // it receives the scripted behavior deterministically.
class FakeProcessHandle : public kairos::exec::ProcessHandle {
public:
    /// Script the stdout output.
    void set_stdout_data(std::string data) {
        result_.stdout_data = std::move(data);
    }

    /// Script the stderr output.
    void set_stderr_data(std::string data) {
        result_.stderr_data = std::move(data);
    }

    /// Script the exit code.
    void set_exit_code(int code) {
        result_.exit_code = code;
    }

    /// Script the simulated runtime.
    void set_runtime(std::chrono::milliseconds d) {
        result_.duration = d;
    }

    /// Script the termination kind.
    void set_termination(kairos::exec::ProcessResult::TerminationKind k) {
        result_.termination = k;
    }

    /// If true, wait() never returns (simulates a hang; must terminate/kill).
    void set_hangs(bool hangs) { hangs_ = hangs; }

    // ── ProcessHandle interface ─────────────────────────────────────

    [[nodiscard]] bool spawn(
        const kairos::exec::ProcessSpec& spec) override
    {
        spec_ = spec;
        spawned_ = true;
        running_ = true;
        // Deliver output via callback if set.
        if (output_cb_) {
            if (!result_.stdout_data.empty()) {
                output_cb_(result_.stdout_data, false);
            }
            if (!result_.stderr_data.empty()) {
                output_cb_(result_.stderr_data, true);
            }
        }
        return !spawn_fail_;
    }

    kairos::exec::ProcessResult wait(
        [[maybe_unused]] std::stop_token stop) override
    {
        if (hangs_) {
            // Simulate hanging until terminated.
            while (!terminated_ && !killed_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if (stop.stop_requested()) {
                    result_.exit_code = 200;
                    result_.termination =
                        kairos::exec::ProcessResult::TerminationKind::Cancelled;
                    break;
                }
            }
        }
        running_ = false;
        return result_;
    }

    void terminate() override {
        terminated_ = true;
        running_ = false;
    }

    void kill() override {
        killed_ = true;
        running_ = false;
    }

    [[nodiscard]] bool is_running() const override {
        return running_;
    }

    [[nodiscard]] int64_t pid() const override { return fake_pid_; }

    void set_output_callback(kairos::exec::OutputCallback cb) override {
        output_cb_ = std::move(cb);
    }

    [[nodiscard]] const kairos::exec::ProcessResult& result() const override {
        return result_;
    }

    // ── Inspection for test assertions ──────────────────────────────

    [[nodiscard]] bool was_spawned() const { return spawned_; }
    [[nodiscard]] bool was_terminated() const { return terminated_; }
    [[nodiscard]] bool was_killed() const { return killed_; }
    [[nodiscard]] const kairos::exec::ProcessSpec& last_spec() const {
        return spec_;
    }

    /// Make spawn() return false (simulate spawn failure).
    void set_spawn_fails(bool fails) { spawn_fail_ = fails; }

    /// Set a fake PID.
    void set_pid(int64_t pid) { fake_pid_ = pid; }

private:
    kairos::exec::ProcessSpec spec_;
    kairos::exec::ProcessResult result_;
    kairos::exec::OutputCallback output_cb_;
    int64_t fake_pid_ = 42;
    bool hangs_ = false;
    bool spawned_ = false;
    bool running_ = false;
    bool terminated_ = false;
    bool killed_ = false;
    bool spawn_fail_ = false;
};

/// Factory that returns scripted FakeProcessHandle instances.
/// Tests register expected processes by command pattern; the factory
/// returns the matching fake when the runner calls create_process().
class FakeProcessFactory {
public:
    /// Register a fake process for commands containing the given pattern.
    void register_process(
        const std::string& command_contains,
        std::shared_ptr<FakeProcessHandle> proc)
    {
        registry_.emplace_back(command_contains, std::move(proc));
    }

    /// Create a fake process matching the command.
    /// Returns the first registered match, or a default 127 process.
    [[nodiscard]] std::unique_ptr<kairos::exec::ProcessHandle> create(
        const std::string& command) const
    {
        for (const auto& [pattern, proc] : registry_) {
            if (command.find(pattern) != std::string::npos) {
                // Return a clone-like wrapper (shares state with original).
                return std::make_unique<FakeProcessHandleRef>(proc);
            }
        }
        // Default: command not found.
        auto def = std::make_unique<FakeProcessHandle>();
        def->set_exit_code(127);
        return def;
    }

    /// Create a simple factory function for RunnerPool::set_process_handle_factory.
    [[nodiscard]] kairos::exec::RunnerPool::ProcessHandleFactory
    make_factory() const
    {
        // Capture this by value for use in lambda.
        auto registry = registry_;
        return [registry](const kairos::exec::ProcessSpec&) -> std::unique_ptr<kairos::exec::ProcessHandle> {
            // Return a generic FakeProcessHandle; actual behavior is set
            // by the test via spec matching in the runner.
            auto proc = std::make_unique<FakeProcessHandle>();
            proc->set_exit_code(0);
            return proc;
        };
    }

private:
    /// A thin wrapper that delegates to a shared FakeProcessHandle.
    class FakeProcessHandleRef : public kairos::exec::ProcessHandle {
    public:
        explicit FakeProcessHandleRef(
            std::shared_ptr<FakeProcessHandle> inner)
            : inner_(std::move(inner)) {}

        [[nodiscard]] bool spawn(
            const kairos::exec::ProcessSpec& s) override {
            return inner_->spawn(s);
        }
        kairos::exec::ProcessResult wait(std::stop_token st) override {
            return inner_->wait(st);
        }
        void terminate() override { inner_->terminate(); }
        void kill() override { inner_->kill(); }
        [[nodiscard]] bool is_running() const override {
            return inner_->is_running();
        }
        [[nodiscard]] int64_t pid() const override {
            return inner_->pid();
        }
        void set_output_callback(kairos::exec::OutputCallback cb) override {
            inner_->set_output_callback(std::move(cb));
        }
        [[nodiscard]] const kairos::exec::ProcessResult&
        result() const override {
            return inner_->result();
        }

    private:
        std::shared_ptr<FakeProcessHandle> inner_;
    };

    std::vector<std::pair<
        std::string, std::shared_ptr<FakeProcessHandle>>> registry_;
};

}  // namespace kairos::testing
