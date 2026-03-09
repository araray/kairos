/// include/kairos/daemon/command_reader.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/daemon/command_reader.hpp — Daemon-side command file polling     ║
// ║                                                                          ║
// ║  Periodically scans data_dir/commands/ for command files written by      ║
// ║  the CLI (e.g., cancel_{run_id}, reload). Parses and dispatches them    ║
// ║  to the appropriate daemon subsystem, then removes the command file.    ║
// ║                                                                          ║
// ║  Spec reference: §23.10                                                 ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace kairos::daemon {

// ── Command types ─────────────────────────────────────────────────────────

/// A parsed command from a command file.
struct Command {
    enum class Type {
        Cancel,   ///< Cancel a running run.
        Reload,   ///< Reload configuration.
        Unknown,  ///< Unrecognized command.
    };

    Type type = Type::Unknown;
    std::string run_id;        ///< For Cancel commands.
    std::string source_file;   ///< Original file path (for cleanup).
};

// ── CommandReader ─────────────────────────────────────────────────────────

/// Reads command files from the data_dir/commands/ directory.
///
/// Design choice: file-based IPC over Unix sockets / named pipes.
/// Rationale (§23.10):
///   - Simpler: no socket/pipe abstraction layer needed
///   - More portable: works identically on Linux/macOS/Windows
///   - Survives daemon restart: files persist on disk
///   - Latency overhead (~one filesystem round-trip) is negligible
///     for human-interactive commands
///
/// Usage: call poll() from the daemon main loop. It returns any
/// pending commands, which the daemon dispatches accordingly.
class CommandReader {
public:
    /// Construct with the commands directory path.
    /// Creates the directory if it doesn't exist.
    explicit CommandReader(std::filesystem::path commands_dir);

    /// Scan for new command files, parse them, and return the results.
    /// Successfully parsed files are deleted after reading.
    /// Malformed files are logged and deleted (no retry).
    ///
    /// @return Vector of parsed commands (may be empty).
    [[nodiscard]] std::vector<Command> poll();

    /// @return Path to the commands directory.
    [[nodiscard]] const std::filesystem::path& dir() const {
        return commands_dir_;
    }

    /// Write a response file for a given command UUID.
    /// Used to acknowledge CLI commands that expect a response.
    /// @param uuid     The command UUID (from the command filename).
    /// @param success  Whether the command succeeded.
    /// @param message  Human-readable result message.
    void write_response(const std::string& uuid,
                        bool success,
                        const std::string& message);

private:
    std::filesystem::path commands_dir_;
    std::filesystem::path responses_dir_;

    /// Parse a single command file.
    /// @param path  Path to the command file.
    /// @return Parsed command, or Unknown if malformed.
    [[nodiscard]] Command parse_command_file(
        const std::filesystem::path& path) const;
};

}  // namespace kairos::daemon
