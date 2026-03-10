/// src/cli/cli_migrate.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  cli_migrate.cpp — CLI handlers for migrate-config and migrate-db       ║
// ║                                                                         ║
// ║  These are standalone functions called from cli_app.cpp dispatch.       ║
// ║  Separated for maintainability — the main cli_app.cpp is already       ║
// ║  3000+ lines.                                                          ║
// ║                                                                         ║
// ║  Spec reference: §31                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/migration/migration_tool.hpp"
#include "kairos/cli/table.hpp"
#include "kairos/core/exit_codes.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace kairos::migration;

namespace kairos::cli {

/// Print migration messages with color coding.
static void print_messages(const std::vector<MigrationMessage>& messages,
                            bool use_color) {
    for (const auto& msg : messages) {
        std::string prefix;
        switch (msg.level) {
            case MigrationMessage::Level::kInfo:
                prefix = use_color ? "\033[36m[INFO]\033[0m " : "[INFO] ";
                break;
            case MigrationMessage::Level::kWarning:
                prefix = use_color ? "\033[33m[WARN]\033[0m " : "[WARN] ";
                break;
            case MigrationMessage::Level::kError:
                prefix = use_color ? "\033[31m[ERROR]\033[0m " : "[ERROR] ";
                break;
        }
        std::cerr << prefix;
        if (!msg.context.empty()) {
            std::cerr << msg.context << ": ";
        }
        std::cerr << msg.message << "\n";
    }
}

int handle_migrate_config(const std::string& source_name,
                           const std::string& source_config,
                           const std::string& source_watches,
                           const std::string& source_workflows,
                           const std::string& output_dir,
                           bool dry_run,
                           bool json_output) {
    SourceTool source;
    try {
        source = parse_source_tool(source_name);
    } catch (const std::invalid_argument& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    ConfigMigrationOptions opts;
    opts.source = source;
    opts.source_config = source_config;
    opts.source_watches = source_watches;
    opts.source_workflows = source_workflows;
    opts.output_dir = output_dir.empty() ? "." : output_dir;
    opts.dry_run = dry_run;

    auto result = migrate_config(opts);

    if (json_output) {
        json j;
        j["success"] = result.success;
        j["entities_migrated"] = result.entities_migrated;
        j["dry_run"] = dry_run;

        json files = json::array();
        for (const auto& f : result.files_created) {
            files.push_back(f.string());
        }
        j["files"] = files;

        json msgs = json::array();
        for (const auto& m : result.messages) {
            msgs.push_back({
                {"level", m.level == MigrationMessage::Level::kError
                              ? "error"
                          : m.level == MigrationMessage::Level::kWarning
                              ? "warning"
                              : "info"},
                {"context", m.context},
                {"message", m.message}
            });
        }
        j["messages"] = msgs;

        std::cout << j.dump(2) << "\n";
    } else {
        if (dry_run) {
            std::cout << "Dry run — no files written.\n\n";
        }

        if (result.success) {
            std::cout << "Migration from " << source_tool_name(source)
                      << " completed: " << result.entities_migrated
                      << " entities migrated.\n";

            if (!result.files_created.empty()) {
                std::cout << "\nFiles "
                          << (dry_run ? "that would be " : "")
                          << "created:\n";
                for (const auto& f : result.files_created) {
                    std::cout << "  " << f.string() << "\n";
                }
            }
        } else {
            std::cerr << "Migration failed.\n";
        }

        if (!result.messages.empty()) {
            std::cout << "\n";
            print_messages(result.messages, !json_output);
        }
    }

    return result.success ? 0 : 1;
}

int handle_migrate_db(const std::string& source_name,
                       const std::string& source_db,
                       const std::string& target_db,
                       bool dry_run,
                       bool json_output) {
    SourceTool source;
    try {
        source = parse_source_tool(source_name);
    } catch (const std::invalid_argument& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    DbMigrationOptions opts;
    opts.source = source;
    opts.source_db = source_db;
    opts.target_db = target_db;
    opts.dry_run = dry_run;

    auto result = migrate_db(opts);

    if (json_output) {
        json j;
        j["success"] = result.success;
        j["dry_run"] = dry_run;
        j["runs_imported"] = result.runs_imported;
        j["steps_imported"] = result.steps_imported;
        j["events_imported"] = result.events_imported;
        j["samples_imported"] = result.samples_imported;
        j["duplicates_skipped"] = result.duplicates_skipped;

        json msgs = json::array();
        for (const auto& m : result.messages) {
            msgs.push_back({
                {"level", m.level == MigrationMessage::Level::kError
                              ? "error"
                          : m.level == MigrationMessage::Level::kWarning
                              ? "warning"
                              : "info"},
                {"context", m.context},
                {"message", m.message}
            });
        }
        j["messages"] = msgs;

        std::cout << j.dump(2) << "\n";
    } else {
        if (dry_run) {
            std::cout << "Dry run — target database not modified.\n\n";
        }

        if (result.success) {
            std::cout << "Database migration from "
                      << source_tool_name(source) << " completed.\n\n";

            if (result.runs_imported > 0 || result.steps_imported > 0) {
                std::cout << "  Runs imported:     " << result.runs_imported << "\n"
                          << "  Steps imported:    " << result.steps_imported << "\n";
            }
            if (result.events_imported > 0) {
                std::cout << "  Events imported:   " << result.events_imported << "\n";
            }
            if (result.samples_imported > 0) {
                std::cout << "  Samples imported:  " << result.samples_imported << "\n";
            }
            if (result.duplicates_skipped > 0) {
                std::cout << "  Duplicates skipped: " << result.duplicates_skipped << "\n";
            }
        } else {
            std::cerr << "Database migration failed.\n";
        }

        if (!result.messages.empty()) {
            std::cout << "\n";
            print_messages(result.messages, !json_output);
        }
    }

    return result.success ? 0 : 1;
}

}  // namespace kairos::cli
