/// include/kairos/cli/cli_app.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/cli/cli_app.hpp — CLI application skeleton                        ║
// ║  Spec reference: §23                                                      ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

namespace kairos::cli {

/// Run the CLI application.  Parses argc/argv and dispatches to the
/// appropriate subcommand handler.
///
/// Phase 1 subcommands:
///   - version     Print version string
///   - start       Start the daemon
///   - init-db     Initialize the SQLite database
///
/// Phase 2+ subcommands (stubs):
///   - workflows   Manage workflows
///   - jobs        Manage jobs
///   - runs        Query run history
///   - logs        View/follow logs
///   - events      View watch events
///   - explain     Explain execution plan
///   - mcp         Start MCP stdio server
///
/// @return  Exit code (0 on success, 2 on config error, 1 on other errors).
int run(int argc, char** argv);

}  // namespace kairos::cli
