// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/persist/migration.hpp — Forward-only schema migration system      ║
// ║                                                                           ║
// ║  Migrations are embedded as C++ string constants (no external files).     ║
// ║  Each migration runs inside a transaction: fail → rollback entire batch.  ║
// ║                                                                           ║
// ║  Spec reference: §16.4                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <SQLiteCpp/SQLiteCpp.h>

#include <functional>
#include <string>
#include <vector>

namespace kairos::persist {

/// A single migration step.
struct Migration {
    int         version;       ///< Sequential version number (1, 2, 3, …)
    std::string description;   ///< Human-readable description
    std::string sql;           ///< SQL to execute (may contain multiple statements)
};

/// Run all pending migrations.
///
/// Compares the current schema_version table to the known migration list
/// and applies any that have not yet been applied.  Migrations run inside
/// a transaction: if any migration fails, the entire batch is rolled back
/// and the function throws.
///
/// @param db          Open SQLite database (WAL mode should already be set).
/// @param migrations  The global migration registry.
/// @return            The final schema version after applying migrations.
/// @throws std::runtime_error on migration failure.
int apply_migrations(SQLite::Database& db,
                     const std::vector<Migration>& migrations);

/// Get the current schema version from the database.
/// Returns 0 if the schema_version table does not exist.
int get_schema_version(SQLite::Database& db);

/// The global migration registry.  New migrations are appended here.
/// RULE: Never modify an existing migration.  Always add a new one.
const std::vector<Migration>& get_migrations();

}  // namespace kairos::persist
