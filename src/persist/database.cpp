// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  database.cpp — SQLite database open, WAL mode, migrations                ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/persist/database.hpp"
#include "kairos/persist/migration.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace kairos::persist {

std::unique_ptr<SQLite::Database> open_database(
    const std::filesystem::path& db_path)
{
    // Ensure parent directory exists.
    if (db_path.has_parent_path()) {
        fs::create_directories(db_path.parent_path());
    }

    // Open the database (creates if not exists).
    auto db = std::make_unique<SQLite::Database>(
        db_path.string(),
        SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE
    );

    // ── Configure pragmas ─────────────────────────────────────────────
    db->exec("PRAGMA journal_mode = WAL");
    db->exec("PRAGMA synchronous = NORMAL");
    db->exec("PRAGMA foreign_keys = ON");
    db->exec("PRAGMA busy_timeout = 5000");

    // ── Run migrations ────────────────────────────────────────────────
    apply_migrations(*db, get_migrations());

    return db;
}

int init_database(const std::filesystem::path& db_path) {
    auto db = open_database(db_path);
    int version = get_schema_version(*db);
    return version;
}

}  // namespace kairos::persist
