/// include/kairos/persist/database.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/persist/database.hpp — SQLite database initialization             ║
// ║                                                                           ║
// ║  Opens the database in WAL mode and runs pending migrations.              ║
// ║  Spec reference: §16.1                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <SQLiteCpp/SQLiteCpp.h>

#include <filesystem>
#include <memory>
#include <string>

namespace kairos::persist {

/// Open (or create) the Kairos SQLite database at the given path.
///
/// Configures:
///   - WAL journal mode (concurrent readers + single writer)
///   - Synchronous = NORMAL (safe for WAL)
///   - Foreign keys = ON
///   - Busy timeout = 5000ms
///
/// Then runs all pending schema migrations.
///
/// @param db_path  Path to the SQLite database file.
/// @return         Owning pointer to the opened database.
/// @throws std::runtime_error on open or migration failure.
std::unique_ptr<SQLite::Database> open_database(
    const std::filesystem::path& db_path);

/// Initialize a database for the `kairos init-db` CLI command.
/// Same as open_database but also logs what it did.
///
/// @param db_path  Path to the SQLite database file.
/// @return         The schema version after initialization.
int init_database(const std::filesystem::path& db_path);

}  // namespace kairos::persist
