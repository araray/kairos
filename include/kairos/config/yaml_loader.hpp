/// include/kairos/config/yaml_loader.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/config/yaml_loader.hpp — YAML → WorkflowDef/WatchGroupDef        ║
// ║                                                                           ║
// ║  Parses workflow and watch-group YAML files into the engine's type        ║
// ║  system. Validates schema, builds content-addressable IDs, checks DAG    ║
// ║  acyclicity, and reports all errors (not just the first).                ║
// ║                                                                           ║
// ║  Spec reference: §6.7 (config init), §4.5 (workflow YAML), §4.6 (watch) ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/engine/workflow_registry.hpp"
#include "kairos/watch/watch_group_def.hpp"

#include <filesystem>
#include <string>
#include <variant>
#include <vector>

namespace kairos::config {

namespace fs = std::filesystem;

// ── Error reporting ─────────────────────────────────────────────────────

/// A single validation error from YAML loading.
struct YamlError {
    std::string file;       ///< Source YAML file path (or "<inline>").
    std::string path;       ///< YAML path within file (e.g., "jobs.build.steps[0]").
    std::string message;    ///< Human-readable error description.
};

// ── Load result ─────────────────────────────────────────────────────────

/// Result of loading one or more YAML files.
struct YamlLoadResult {
    std::vector<engine::WorkflowDef> workflows;
    std::vector<engine::TimerEntry> triggers;
    std::vector<engine::JobDef> standalone_jobs;
    std::vector<watch::WatchGroupDef> watch_groups;
    std::vector<YamlError> errors;

    [[nodiscard]] bool ok() const { return errors.empty(); }
};

// ── Loader API ──────────────────────────────────────────────────────────

/// Load a single workflow YAML file.
/// The file may contain one workflow definition (top-level keys: id, name,
/// jobs, triggers, etc.) per spec §4.5.
///
/// On parse/validation error, the result's `errors` vector is populated
/// and the workflow is NOT added to `workflows`.
YamlLoadResult load_workflow_file(const fs::path& path);

/// Load a single workflow from a YAML string (for testing / inline config).
YamlLoadResult load_workflow_string(const std::string& yaml_content,
                                     const std::string& source_name = "<inline>");

/// Scan a directory for *.yaml / *.yml files and load all workflows.
/// Checks for duplicate IDs across files.
YamlLoadResult load_workflows_dir(const fs::path& dir);

/// Load watch-group definitions from a YAML file.
/// The file contains a top-level `watch_groups` array per spec §4.6.
YamlLoadResult load_watch_groups_file(const fs::path& path);

/// Load watch-group definitions from a YAML string.
YamlLoadResult load_watch_groups_string(const std::string& yaml_content,
                                         const std::string& source_name = "<inline>");

/// Scan a directory for watch-group YAML files.
YamlLoadResult load_watch_groups_dir(const fs::path& dir);

/// Load everything: workflows from workflows_dir, watch groups from
/// watch_groups_file or watch_groups_dir, and return a unified result.
///
/// This is the main entry point used by config initialization (§6.7 step 5–6).
YamlLoadResult load_all(
    const fs::path& workflows_dir,
    const fs::path& watch_groups_path);  ///< File or directory.

}  // namespace kairos::config
