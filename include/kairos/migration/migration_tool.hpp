/// include/kairos/migration/migration_tool.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/migration/migration_tool.hpp — Legacy tool migration             ║
// ║                                                                         ║
// ║  Provides `kairos migrate-config` and `kairos migrate-db` commands      ║
// ║  that translate configuration and database from:                        ║
// ║    - AVScheduler (TOML → Kairos TOML + YAML)                          ║
// ║    - EventWatcher (TOML + YAML → Kairos TOML + YAML)                  ║
// ║    - LocalFlow (YAML → Kairos YAML)                                   ║
// ║                                                                         ║
// ║  Migration is always read-only on source data.  Imported records are   ║
// ║  tagged with `migrated_from` metadata for auditability.                ║
// ║                                                                         ║
// ║  Spec reference: §31                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kairos::migration {

// ── Source tool identifiers ──────────────────────────────────────────────

enum class SourceTool {
    kAVScheduler,
    kEventWatcher,
    kLocalFlow,
};

/// Parse a source tool name from a string.
/// Accepted values: "avscheduler", "eventwatcher", "localflow" (case-insensitive).
/// @throws std::invalid_argument if the name is not recognized.
SourceTool parse_source_tool(const std::string& name);

/// Human-readable name for a source tool.
std::string source_tool_name(SourceTool tool);

// ── Migration warnings/errors ────────────────────────────────────────────

/// A single warning or error from a migration operation.
struct MigrationMessage {
    enum class Level { kInfo, kWarning, kError };

    Level level;
    std::string context;   ///< e.g., "job 'backup'", "watch group 'logs'"
    std::string message;   ///< Human-readable description

    /// True if this message is an error (migration should not proceed).
    bool is_error() const { return level == Level::kError; }
};

// ── Config migration ─────────────────────────────────────────────────────

/// Options for `kairos migrate-config`.
struct ConfigMigrationOptions {
    SourceTool source;

    /// Path to the source config file.
    /// AVScheduler:  config.toml
    /// EventWatcher: config.toml
    /// LocalFlow:    (unused — workflow dir is the source)
    std::filesystem::path source_config;

    /// Path to the source watch-groups file (EventWatcher only).
    std::filesystem::path source_watches;

    /// Path to the source workflows directory (LocalFlow only).
    std::filesystem::path source_workflows;

    /// Output directory for generated Kairos config + YAML files.
    std::filesystem::path output_dir;

    /// If true, don't write files — just print what would be generated.
    bool dry_run = false;
};

/// Result of a config migration.
struct ConfigMigrationResult {
    bool success = false;

    /// Files that were (or would be) created.
    std::vector<std::filesystem::path> files_created;

    /// Warnings and errors encountered during migration.
    std::vector<MigrationMessage> messages;

    /// Number of jobs/workflows migrated.
    int entities_migrated = 0;
};

/// Migrate configuration from a legacy tool to Kairos format.
///
/// The operation is read-only on the source.  Output files are written
/// to `opts.output_dir`.  If `opts.dry_run` is true, no files are written.
ConfigMigrationResult migrate_config(const ConfigMigrationOptions& opts);

// ── Database migration ───────────────────────────────────────────────────

/// Options for `kairos migrate-db`.
struct DbMigrationOptions {
    SourceTool source;

    /// Path to the source SQLite database.
    std::filesystem::path source_db;

    /// Path to the target Kairos SQLite database (must exist and be
    /// initialized via `kairos init-db`).
    std::filesystem::path target_db;

    /// If true, don't write to the target — just report what would be
    /// imported.
    bool dry_run = false;
};

/// Result of a database migration.
struct DbMigrationResult {
    bool success = false;

    /// Number of records imported by entity type.
    int runs_imported = 0;
    int steps_imported = 0;
    int events_imported = 0;
    int samples_imported = 0;
    int duplicates_skipped = 0;

    /// Warnings and errors.
    std::vector<MigrationMessage> messages;
};

/// Migrate database records from a legacy tool to the Kairos schema.
///
/// The operation is READ-ONLY on the source database.  Imported records
/// are tagged with `migrated_from = <source_tool>` in the Kairos metadata.
///
/// Duplicate detection: if a record with the same timestamp and entity
/// already exists in the target, it is skipped.
DbMigrationResult migrate_db(const DbMigrationOptions& opts);

}  // namespace kairos::migration
