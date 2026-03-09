/// include/kairos/engine/workflow_registry.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  kairos/engine/workflow_registry.hpp — Loaded workflow/job definitions    ║
// ║                                                                           ║
// ║  Holds pre-parsed workflow DAGs, job/step definitions, and trigger        ║
// ║  entries. Shared (immutable) between Scheduler and Pipeline threads.      ║
// ║  Rebuilt atomically on config reload.                                     ║
// ║                                                                           ║
// ║  Spec reference: §6.7, §10.2, §11.2                                     ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include "kairos/engine/dag.hpp"
#include "kairos/engine/trigger_types.hpp"
#include "kairos/watch/watch_group_def.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kairos::engine {

// ── Step definition (parsed from YAML) ──────────────────────────────────

/// A single step within a job. Maps to a ProcessSpec at execution time.
struct StepDef {
    std::string step_id;         ///< Content-addressable ID (stp-xxx)
    std::string step_name;       ///< Human-readable name
    std::string command;         ///< Shell command to execute
    std::filesystem::path working_dir;  ///< Working directory (empty = inherit)
    std::unordered_map<std::string, std::string> env;  ///< Extra env vars
    std::optional<std::chrono::seconds> timeout;  ///< Per-step timeout
    bool use_shell = true;       ///< Execute via shell (default)
};

// ── Job definition (parsed from YAML) ───────────────────────────────────

/// A job definition within a workflow or standalone.
struct JobDef {
    std::string job_id;          ///< Content-addressable ID (job-xxx)
    std::string job_name;        ///< Human-readable name
    std::vector<StepDef> steps;  ///< Ordered list of steps
    std::vector<std::string> needs;  ///< IDs of predecessor jobs
    std::optional<std::string> condition_expr;  ///< KEL condition
    bool continue_on_error = false;
    std::unordered_map<std::string, std::string> env;  ///< Job-level env
    std::filesystem::path working_dir;  ///< Job-level working dir
};

// ── Workflow definition (parsed from YAML) ──────────────────────────────

/// A complete workflow definition with its pre-built DAG.
struct WorkflowDef {
    std::string workflow_id;     ///< Content-addressable ID (wfl-xxx)
    std::string workflow_name;   ///< Human-readable name
    std::vector<JobDef> jobs;    ///< All jobs in the workflow
    WorkflowDag dag;             ///< Pre-built DAG from job definitions
};

// ── Workflow Registry ───────────────────────────────────────────────────

/// Immutable registry of all loaded workflows, standalone jobs, and
/// trigger definitions. Shared via shared_ptr across threads.
///
/// Thread safety: immutable after construction. Safe to share without
/// synchronization. Rebuilt atomically on config reload.
class WorkflowRegistry {
public:
    /// Build a registry from definitions.
    ///
    /// @param workflows  Loaded workflow definitions (with pre-built DAGs).
    /// @param triggers   All trigger entries (for the scheduler).
    /// @param standalone_jobs  Standalone job definitions (not in workflows).
    /// @param watch_groups  Watch group definitions (for the watch engine).
    WorkflowRegistry(
        std::vector<WorkflowDef> workflows,
        std::vector<TimerEntry> triggers,
        std::vector<JobDef> standalone_jobs = {},
        std::vector<watch::WatchGroupDef> watch_groups = {})
        : triggers_(std::move(triggers))
        , watch_groups_(std::move(watch_groups))
    {
        for (auto& wf : workflows) {
            std::string id = wf.workflow_id;
            // Build job map for this workflow.
            for (const auto& j : wf.jobs) {
                job_defs_[j.job_id] = &wf.jobs[0]; // Will be fixed below
            }
            workflow_defs_.emplace(std::move(id), std::move(wf));
        }

        // Re-build job_defs_ pointing to stable storage.
        job_defs_.clear();
        for (const auto& [wf_id, wf] : workflow_defs_) {
            for (const auto& j : wf.jobs) {
                job_defs_[j.job_id] = &j;
            }
        }

        for (auto& sj : standalone_jobs) {
            std::string id = sj.job_id;
            standalone_jobs_.emplace(id, std::move(sj));
            job_defs_[id] = &standalone_jobs_.at(id);
        }
    }

    // ── Workflow queries ─────────────────────────────────────────

    /// Get a workflow definition by ID.
    [[nodiscard]] const WorkflowDef* workflow(
        const std::string& id) const
    {
        auto it = workflow_defs_.find(id);
        return it != workflow_defs_.end() ? &it->second : nullptr;
    }

    /// Get a workflow definition by name.
    [[nodiscard]] const WorkflowDef* workflow_by_name(
        const std::string& name) const
    {
        for (const auto& [id, wf] : workflow_defs_) {
            if (wf.workflow_name == name) return &wf;
        }
        return nullptr;
    }

    /// Get all workflow IDs.
    [[nodiscard]] std::vector<std::string> workflow_ids() const {
        std::vector<std::string> ids;
        ids.reserve(workflow_defs_.size());
        for (const auto& [id, _] : workflow_defs_) {
            ids.push_back(id);
        }
        return ids;
    }

    /// Number of workflows.
    [[nodiscard]] size_t workflow_count() const {
        return workflow_defs_.size();
    }

    /// Get all workflow definitions as a vector (for CLI listing).
    [[nodiscard]] std::vector<const WorkflowDef*> workflows() const {
        std::vector<const WorkflowDef*> result;
        result.reserve(workflow_defs_.size());
        for (const auto& [id, wf] : workflow_defs_) {
            result.push_back(&wf);
        }
        return result;
    }

    // ── Job queries ──────────────────────────────────────────────

    /// Get a job definition by ID (from any workflow or standalone).
    [[nodiscard]] const JobDef* job(const std::string& id) const {
        auto it = job_defs_.find(id);
        return it != job_defs_.end() ? it->second : nullptr;
    }

    /// Get a standalone job definition.
    [[nodiscard]] const JobDef* standalone_job(
        const std::string& id) const
    {
        auto it = standalone_jobs_.find(id);
        return it != standalone_jobs_.end() ? &it->second : nullptr;
    }

    // ── Trigger queries ──────────────────────────────────────────

    /// All trigger entries (for the scheduler's timer heap).
    [[nodiscard]] const std::vector<TimerEntry>& triggers() const {
        return triggers_;
    }

    /// Number of triggers.
    [[nodiscard]] size_t trigger_count() const {
        return triggers_.size();
    }

    // ── Target resolution ────────────────────────────────────────

    /// Resolve a target ID to its DAG.
    /// For workflows: returns the pre-built DAG.
    /// For standalone jobs: returns a single-node DAG.
    [[nodiscard]] std::optional<WorkflowDag> resolve_dag(
        const std::string& target_id,
        TriggerEvent::TargetKind kind) const
    {
        if (kind == TriggerEvent::TargetKind::Workflow) {
            auto wf = workflow(target_id);
            if (wf) return wf->dag;
        } else {
            auto sj = standalone_job(target_id);
            if (sj) return make_standalone_dag(
                sj->job_id, sj->job_name, sj->condition_expr);
        }
        return std::nullopt;
    }

    /// Resolve a target name (human-readable) to its ID.
    [[nodiscard]] std::string resolve_name_to_id(
        const std::string& name) const
    {
        // Check workflows by name.
        auto wf = workflow_by_name(name);
        if (wf) return wf->workflow_id;

        // Check standalone jobs by name.
        for (const auto& [id, sj] : standalone_jobs_) {
            if (sj.job_name == name) return id;
        }

        // Might already be an ID.
        return name;
    }

    // ── Watch group queries ──────────────────────────────────────

    /// All watch group definitions (for the watch engine).
    [[nodiscard]] const std::vector<watch::WatchGroupDef>&
    watch_groups() const {
        return watch_groups_;
    }

    /// Number of watch groups.
    [[nodiscard]] size_t watch_group_count() const {
        return watch_groups_.size();
    }

private:
    std::unordered_map<std::string, WorkflowDef> workflow_defs_;
    std::unordered_map<std::string, JobDef> standalone_jobs_;
    std::unordered_map<std::string, const JobDef*> job_defs_;
    std::vector<TimerEntry> triggers_;
    std::vector<watch::WatchGroupDef> watch_groups_;
};

}  // namespace kairos::engine
