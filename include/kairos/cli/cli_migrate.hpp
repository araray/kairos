/// include/kairos/cli/cli_migrate.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/cli/cli_migrate.hpp — Migration CLI handler declarations         ║
// ║  Spec reference: §31                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <string>

namespace kairos::cli {

/// Handle `kairos migrate-config` subcommand.
int handle_migrate_config(const std::string& source_name,
                           const std::string& source_config,
                           const std::string& source_watches,
                           const std::string& source_workflows,
                           const std::string& output_dir,
                           bool dry_run,
                           bool json_output);

/// Handle `kairos migrate-db` subcommand.
int handle_migrate_db(const std::string& source_name,
                       const std::string& source_db,
                       const std::string& target_db,
                       bool dry_run,
                       bool json_output);

}  // namespace kairos::cli
