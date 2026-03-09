/// src/daemon/command_reader.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  CommandReader — daemon-side command file polling                          ║
// ║  Spec reference: §23.10                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/daemon/command_reader.hpp"

#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>

namespace kairos::daemon {

namespace fs = std::filesystem;

CommandReader::CommandReader(fs::path commands_dir)
    : commands_dir_(std::move(commands_dir))
    , responses_dir_(commands_dir_.parent_path() / "responses")
{
    // Ensure both directories exist.
    std::error_code ec;
    fs::create_directories(commands_dir_, ec);
    if (ec) {
        spdlog::warn("CommandReader: failed to create commands dir {}: {}",
                     commands_dir_.string(), ec.message());
    }
    fs::create_directories(responses_dir_, ec);
    if (ec) {
        spdlog::warn("CommandReader: failed to create responses dir {}: {}",
                     responses_dir_.string(), ec.message());
    }
}

std::vector<Command> CommandReader::poll() {
    std::vector<Command> result;

    std::error_code ec;
    if (!fs::exists(commands_dir_, ec) || ec) return result;

    for (const auto& entry : fs::directory_iterator(commands_dir_, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) continue;

        auto cmd = parse_command_file(entry.path());
        cmd.source_file = entry.path().string();

        if (cmd.type != Command::Type::Unknown) {
            result.push_back(std::move(cmd));
        } else {
            spdlog::warn("CommandReader: unrecognized command file: {}",
                         entry.path().filename().string());
        }

        // Delete the command file after reading (regardless of parse result).
        // This prevents reprocessing on the next poll cycle.
        std::error_code rm_ec;
        fs::remove(entry.path(), rm_ec);
        if (rm_ec) {
            spdlog::warn("CommandReader: failed to remove {}: {}",
                         entry.path().string(), rm_ec.message());
        }
    }

    return result;
}

Command CommandReader::parse_command_file(
    const fs::path& path) const
{
    Command cmd;
    auto filename = path.filename().string();

    // Convention (§23.10):
    //   cancel_{run_id}      — cancel a running run
    //   reload_{uuid}        — reload configuration
    //   (file content may contain additional JSON metadata)

    if (filename.starts_with("cancel_")) {
        cmd.type = Command::Type::Cancel;
        cmd.run_id = filename.substr(7);  // Skip "cancel_"

        // Also try to read run_id from file content (overrides filename).
        std::ifstream ifs(path);
        if (ifs.is_open()) {
            std::string content;
            std::getline(ifs, content);
            if (!content.empty()) {
                // File content is the run_id (simple text protocol).
                cmd.run_id = content;
            }
        }

        // Validate: run_id should not be empty.
        if (cmd.run_id.empty()) {
            spdlog::warn("CommandReader: cancel command with empty run_id");
            cmd.type = Command::Type::Unknown;
        }

    } else if (filename.starts_with("reload")) {
        cmd.type = Command::Type::Reload;

    } else {
        cmd.type = Command::Type::Unknown;
    }

    return cmd;
}

void CommandReader::write_response(
    const std::string& uuid,
    bool success,
    const std::string& message)
{
    auto response_path = responses_dir_ / uuid;

    std::ofstream ofs(response_path);
    if (!ofs.is_open()) {
        spdlog::warn("CommandReader: failed to write response to {}",
                     response_path.string());
        return;
    }

    ofs << (success ? "OK" : "ERROR") << "\n" << message << "\n";
    ofs.close();

    spdlog::debug("CommandReader: wrote response to {}",
                  response_path.string());
}

}  // namespace kairos::daemon
