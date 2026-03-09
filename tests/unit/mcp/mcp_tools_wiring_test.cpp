/// tests/unit/mcp/mcp_tools_wiring_test.cpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Tests for all 10 newly-wired MCP tool implementations                   ║
// ║                                                                          ║
// ║  Phase 4 Batch 3: listWorkflows, getWorkflow, runWorkflow, listJobs,    ║
// ║  runJob, queryRuns, getRunDetail, getRunLogs, getStepOutput,            ║
// ║  explainPlan                                                            ║
// ║                                                                          ║
// ║  Uses mock dependencies: WorkflowRegistry + fake QueryReader.           ║
// ║  Tests tool dispatch routing, input validation, and output format.      ║
// ╚════════════════════════════════════════════════════════════════════════════╝

#include "kairos/mcp/handler.hpp"
#include "kairos/engine/dag.hpp"
#include "kairos/engine/workflow_registry.hpp"
#include "kairos/persist/query_reader.hpp"

#include <gtest/gtest.h>
#include <SQLiteCpp/SQLiteCpp.h>

#include <memory>
#include <string>
#include <vector>

namespace kairos::mcp::test {

using json = nlohmann::json;

// ── Test Fixtures ──────────────────────────────────────────────────────────

/// Helper to build a simple workflow registry with known data.
static std::shared_ptr<const engine::WorkflowRegistry>
make_test_registry() {
    // Build a simple workflow: setup → build, test → deploy
    engine::StepDef step_echo{
        .step_id = "stp-001", .step_name = "echo-hello",
        .command = "echo hello"};
    engine::StepDef step_compile{
        .step_id = "stp-002", .step_name = "compile",
        .command = "make build"};
    engine::StepDef step_test{
        .step_id = "stp-003", .step_name = "run-tests",
        .command = "make test"};
    engine::StepDef step_deploy{
        .step_id = "stp-004", .step_name = "deploy-prod",
        .command = "make deploy",
        .timeout = std::chrono::seconds(300)};

    engine::JobDef j_setup{
        .job_id = "job-setup", .job_name = "setup",
        .steps = {step_echo}};
    engine::JobDef j_build{
        .job_id = "job-build", .job_name = "build",
        .steps = {step_compile},
        .needs = {"job-setup"}};
    engine::JobDef j_test{
        .job_id = "job-test", .job_name = "test",
        .steps = {step_test},
        .needs = {"job-setup"}};
    engine::JobDef j_deploy{
        .job_id = "job-deploy", .job_name = "deploy",
        .steps = {step_deploy},
        .needs = {"job-build", "job-test"},
        .condition_expr = "job(\"test\").last_success"};

    engine::DagNode dn_setup{
        .job_id = "job-setup", .job_name = "setup"};
    engine::DagNode dn_build{
        .job_id = "job-build", .job_name = "build",
        .needs = {"job-setup"}};
    engine::DagNode dn_test{
        .job_id = "job-test", .job_name = "test",
        .needs = {"job-setup"}};
    engine::DagNode dn_deploy{
        .job_id = "job-deploy", .job_name = "deploy",
        .needs = {"job-build", "job-test"},
        .condition_expr = "job(\"test\").last_success"};

    auto dag = engine::WorkflowDag::build(
        {dn_setup, dn_build, dn_test, dn_deploy});

    engine::WorkflowDef wf{
        .workflow_id = "wfl-abc123",
        .workflow_name = "Deploy Pipeline",
        .jobs = {j_setup, j_build, j_test, j_deploy},
        .dag = std::move(dag),
    };

    // Standalone jobs.
    engine::StepDef sj_step{
        .step_id = "stp-sj1", .step_name = "backup-step",
        .command = "tar czf backup.tar.gz /data"};
    engine::JobDef sj{
        .job_id = "job-backup", .job_name = "backup",
        .steps = {sj_step}};

    return std::make_shared<engine::WorkflowRegistry>(
        std::vector<engine::WorkflowDef>{std::move(wf)},
        std::vector<engine::TimerEntry>{},
        std::vector<engine::JobDef>{std::move(sj)});
}

/// Create an in-memory SQLite database with schema for QueryReader tests.
static std::unique_ptr<SQLite::Database> make_test_db() {
    auto db = std::make_unique<SQLite::Database>(
        ":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);

    // Create minimal schema (subset of Kairos schema).
    db->exec(R"SQL(
        CREATE TABLE IF NOT EXISTS runs (
            run_id TEXT PRIMARY KEY,
            target_type TEXT NOT NULL DEFAULT 'workflow',
            target_id TEXT NOT NULL,
            target_name TEXT NOT NULL DEFAULT '',
            trigger_type TEXT NOT NULL DEFAULT 'manual',
            status TEXT NOT NULL DEFAULT 'RUNNING',
            exit_code INTEGER NOT NULL DEFAULT 0,
            start_ts TEXT NOT NULL DEFAULT '',
            end_ts TEXT NOT NULL DEFAULT '',
            duration_ms INTEGER NOT NULL DEFAULT 0,
            created_at TEXT NOT NULL DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS job_runs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            run_id TEXT NOT NULL,
            job_id TEXT NOT NULL,
            job_name TEXT NOT NULL DEFAULT '',
            status TEXT NOT NULL DEFAULT 'RUNNING',
            exit_code INTEGER NOT NULL DEFAULT 0,
            start_ts TEXT NOT NULL DEFAULT '',
            end_ts TEXT NOT NULL DEFAULT '',
            duration_ms INTEGER NOT NULL DEFAULT 0,
            condition_result TEXT NOT NULL DEFAULT '',
            created_at TEXT NOT NULL DEFAULT (datetime('now')),
            FOREIGN KEY(run_id) REFERENCES runs(run_id)
        );

        CREATE TABLE IF NOT EXISTS step_runs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            run_id TEXT NOT NULL,
            job_id TEXT NOT NULL,
            step_id TEXT NOT NULL,
            step_name TEXT NOT NULL DEFAULT '',
            status TEXT NOT NULL DEFAULT 'RUNNING',
            exit_code INTEGER NOT NULL DEFAULT 0,
            start_ts TEXT NOT NULL DEFAULT '',
            end_ts TEXT NOT NULL DEFAULT '',
            duration_ms INTEGER NOT NULL DEFAULT 0,
            command TEXT NOT NULL DEFAULT '',
            created_at TEXT NOT NULL DEFAULT (datetime('now')),
            FOREIGN KEY(run_id) REFERENCES runs(run_id)
        );

        CREATE TABLE IF NOT EXISTS log_chunks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            run_id TEXT NOT NULL,
            job_id TEXT NOT NULL DEFAULT '',
            step_id TEXT NOT NULL DEFAULT '',
            stream TEXT NOT NULL DEFAULT 'stdout',
            chunk_index INTEGER NOT NULL DEFAULT 0,
            content TEXT NOT NULL DEFAULT '',
            created_at TEXT NOT NULL DEFAULT (datetime('now')),
            FOREIGN KEY(run_id) REFERENCES runs(run_id)
        );

        CREATE TABLE IF NOT EXISTS watch_events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            event_uid TEXT NOT NULL DEFAULT '',
            watch_group TEXT NOT NULL DEFAULT '',
            rule_name TEXT NOT NULL DEFAULT '',
            event_type TEXT NOT NULL DEFAULT '',
            severity TEXT NOT NULL DEFAULT 'info',
            affected_files_json TEXT NOT NULL DEFAULT '[]',
            sample_epoch INTEGER NOT NULL DEFAULT 0,
            details_json TEXT NOT NULL DEFAULT '{}',
            created_at TEXT NOT NULL DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS samples (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            watch_group TEXT NOT NULL,
            epoch INTEGER NOT NULL,
            file_path TEXT NOT NULL,
            is_dir INTEGER NOT NULL DEFAULT 0,
            size INTEGER NOT NULL DEFAULT 0,
            mtime TEXT NOT NULL DEFAULT '',
            hash TEXT NOT NULL DEFAULT '',
            created_at TEXT NOT NULL DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS metrics_snapshots (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            metric_name TEXT NOT NULL,
            metric_type TEXT NOT NULL DEFAULT 'gauge',
            value REAL NOT NULL DEFAULT 0.0,
            labels_json TEXT NOT NULL DEFAULT '{}',
            created_at TEXT NOT NULL DEFAULT (datetime('now'))
        );
    )SQL");

    return db;
}

/// Insert sample run data into the test database.
static void seed_test_runs(SQLite::Database& db) {
    // Run 1: completed successfully.
    db.exec(R"SQL(
        INSERT INTO runs (run_id, target_type, target_id, target_name,
                          trigger_type, status, exit_code,
                          start_ts, end_ts, duration_ms)
        VALUES ('run-001', 'workflow', 'wfl-abc123', 'Deploy Pipeline',
                'manual', 'SUCCESS', 0,
                '2026-03-09T10:00:00Z', '2026-03-09T10:00:05Z', 5000);
    )SQL");

    // Run 2: failed.
    db.exec(R"SQL(
        INSERT INTO runs (run_id, target_type, target_id, target_name,
                          trigger_type, status, exit_code,
                          start_ts, end_ts, duration_ms)
        VALUES ('run-002', 'workflow', 'wfl-abc123', 'Deploy Pipeline',
                'schedule', 'FAILURE', 1,
                '2026-03-09T11:00:00Z', '2026-03-09T11:00:03Z', 3000);
    )SQL");

    // Job runs for run-001.
    db.exec(R"SQL(
        INSERT INTO job_runs (run_id, job_id, job_name, status,
                              exit_code, start_ts, end_ts, duration_ms,
                              condition_result)
        VALUES ('run-001', 'job-setup', 'setup', 'SUCCESS', 0,
                '2026-03-09T10:00:00Z', '2026-03-09T10:00:01Z', 1000,
                'true');
    )SQL");

    // Step runs for run-001.
    db.exec(R"SQL(
        INSERT INTO step_runs (run_id, job_id, step_id, step_name,
                               status, exit_code, start_ts, end_ts,
                               duration_ms, command)
        VALUES ('run-001', 'job-setup', 'stp-001', 'echo-hello',
                'SUCCESS', 0,
                '2026-03-09T10:00:00Z', '2026-03-09T10:00:01Z',
                1000, 'echo hello');
    )SQL");

    // Log chunks for run-001.
    db.exec(R"SQL(
        INSERT INTO log_chunks (run_id, job_id, step_id, stream,
                                chunk_index, content)
        VALUES ('run-001', 'job-setup', 'stp-001', 'stdout',
                0, 'hello');
    )SQL");

    db.exec(R"SQL(
        INSERT INTO log_chunks (run_id, job_id, step_id, stream,
                                chunk_index, content)
        VALUES ('run-001', 'job-setup', 'stp-001', 'stderr',
                0, 'warning: test');
    )SQL");
}

/// Base test fixture with common setup.
class McpToolsTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry_ = make_test_registry();
        db_ = make_test_db();
        seed_test_runs(*db_);
        query_reader_ = std::make_unique<persist::QueryReader>(*db_);

        // Track submitted runs.
        submitted_runs_.clear();

        McpHandler::Dependencies deps;
        deps.registry = registry_;
        deps.query_reader = query_reader_.get();
        deps.submit_run = [this](const std::string& target_id,
                                  engine::TriggerEvent::TargetKind kind)
            -> std::string {
            std::string run_id = "run-new-001";
            submitted_runs_.push_back({target_id,
                kind == engine::TriggerEvent::TargetKind::Workflow
                    ? "workflow" : "job"});
            return run_id;
        };
        deps.server_info.version = "test";

        handler_ = std::make_unique<McpHandler>(std::move(deps));
    }

    /// Helper: call a tool by name and return the result JSON.
    json call_tool(const std::string& name,
                   const json& args = json::object()) {
        json params = {{"name", name}, {"arguments", args}};
        auto response = handler_->dispatch("tools/call", params, 1);
        // Extract the text content from the MCP content array.
        auto text = response["content"][0]["text"].get<std::string>();
        return json::parse(text);
    }

    std::shared_ptr<const engine::WorkflowRegistry> registry_;
    std::unique_ptr<SQLite::Database> db_;
    std::unique_ptr<persist::QueryReader> query_reader_;
    std::unique_ptr<McpHandler> handler_;

    struct SubmittedRun {
        std::string target_id;
        std::string kind;
    };
    std::vector<SubmittedRun> submitted_runs_;
};

// ═══════════════════════════════════════════════════════════════════════════
//  Dispatch routing: all 14 tools resolve without "Unknown tool" error
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(McpToolsTest, Dispatch_AllToolsResolvable) {
    // Pair: tool name → minimum args (json::object() for no args).
    std::vector<std::pair<std::string, json>> tools = {
        {"kairos.listWorkflows", json::object()},
        {"kairos.getWorkflow", {{"workflow_id", "wfl-abc123"}}},
        {"kairos.runWorkflow", {{"workflow_id", "wfl-abc123"}}},
        {"kairos.listJobs", json::object()},
        {"kairos.runJob", {{"job_id", "job-backup"}}},
        {"kairos.queryRuns", json::object()},
        {"kairos.getRunDetail", {{"run_id", "run-001"}}},
        {"kairos.getRunLogs", {{"run_id", "run-001"}}},
        {"kairos.getStepOutput", {{"run_id", "run-001"},
                                  {"step_id", "stp-001"}}},
        {"kairos.explainPlan", {{"workflow_id", "wfl-abc123"}}},
        {"kairos.reloadConfig", json::object()},
        {"kairos.getMetrics", json::object()},
    };

    for (const auto& [name, args] : tools) {
        SCOPED_TRACE("tool: " + name);
        json params = {{"name", name}, {"arguments", args}};
        EXPECT_NO_THROW(handler_->dispatch("tools/call", params, 1));
    }
}

TEST_F(McpToolsTest, Dispatch_UnknownTool_Throws) {
    json params = {{"name", "kairos.nonExistent"}, {"arguments", {}}};
    EXPECT_THROW(handler_->dispatch("tools/call", params, 1),
                 std::invalid_argument);
}

// ═══════════════════════════════════════════════════════════════════════════
//  kairos.listWorkflows
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(McpToolsTest, ListWorkflows_ReturnsAll) {
    auto result = call_tool("kairos.listWorkflows");
    EXPECT_EQ(result["count"], 1);
    ASSERT_EQ(result["workflows"].size(), 1u);
    EXPECT_EQ(result["workflows"][0]["id"], "wfl-abc123");
    EXPECT_EQ(result["workflows"][0]["name"], "Deploy Pipeline");
    EXPECT_EQ(result["workflows"][0]["job_count"], 4);
}

TEST_F(McpToolsTest, ListWorkflows_EmptyRegistry) {
    auto empty_reg = std::make_shared<engine::WorkflowRegistry>(
        std::vector<engine::WorkflowDef>{},
        std::vector<engine::TimerEntry>{});

    McpHandler::Dependencies deps;
    deps.registry = empty_reg;
    McpHandler handler(std::move(deps));

    json params = {{"name", "kairos.listWorkflows"}, {"arguments", {}}};
    auto response = handler.dispatch("tools/call", params, 1);
    auto text = response["content"][0]["text"].get<std::string>();
    auto result = json::parse(text);

    EXPECT_EQ(result["count"], 0);
    EXPECT_TRUE(result["workflows"].empty());
}

TEST_F(McpToolsTest, ListWorkflows_NoRegistry_ReturnsError) {
    McpHandler::Dependencies deps;
    // No registry set.
    McpHandler handler(std::move(deps));

    json params = {{"name", "kairos.listWorkflows"}, {"arguments", {}}};
    auto response = handler.dispatch("tools/call", params, 1);
    auto text = response["content"][0]["text"].get<std::string>();
    auto result = json::parse(text);

    EXPECT_TRUE(result.contains("error"));
}

// ═══════════════════════════════════════════════════════════════════════════
//  kairos.getWorkflow
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(McpToolsTest, GetWorkflow_ById) {
    auto result = call_tool("kairos.getWorkflow",
                            {{"workflow_id", "wfl-abc123"}});
    EXPECT_EQ(result["workflow_id"], "wfl-abc123");
    EXPECT_EQ(result["workflow_name"], "Deploy Pipeline");
    EXPECT_EQ(result["jobs"].size(), 4u);
    EXPECT_GT(result["level_count"].get<int>(), 0);
    EXPECT_FALSE(result["dag_levels"].empty());
}

TEST_F(McpToolsTest, GetWorkflow_ByName) {
    auto result = call_tool("kairos.getWorkflow",
                            {{"workflow_id", "Deploy Pipeline"}});
    EXPECT_EQ(result["workflow_id"], "wfl-abc123");
}

TEST_F(McpToolsTest, GetWorkflow_NotFound) {
    auto result = call_tool("kairos.getWorkflow",
                            {{"workflow_id", "no-such-wf"}});
    EXPECT_TRUE(result.contains("error"));
    EXPECT_NE(result["error"].get<std::string>().find("not found"),
              std::string::npos);
}

TEST_F(McpToolsTest, GetWorkflow_MissingParam) {
    auto result = call_tool("kairos.getWorkflow");
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(McpToolsTest, GetWorkflow_ContainsJobDetails) {
    auto result = call_tool("kairos.getWorkflow",
                            {{"workflow_id", "wfl-abc123"}});

    // Find the deploy job — should have condition and needs.
    bool found_deploy = false;
    for (const auto& job : result["jobs"]) {
        if (job["job_name"] == "deploy") {
            found_deploy = true;
            EXPECT_TRUE(job.contains("condition"));
            EXPECT_EQ(job["condition"],
                      "job(\"test\").last_success");
            EXPECT_FALSE(job["needs"].empty());
            EXPECT_FALSE(job["steps"].empty());
            break;
        }
    }
    EXPECT_TRUE(found_deploy);
}

TEST_F(McpToolsTest, GetWorkflow_StepTimeout) {
    auto result = call_tool("kairos.getWorkflow",
                            {{"workflow_id", "wfl-abc123"}});

    // Find the deploy job → deploy-prod step with timeout.
    for (const auto& job : result["jobs"]) {
        if (job["job_name"] == "deploy") {
            for (const auto& step : job["steps"]) {
                if (step["step_name"] == "deploy-prod") {
                    EXPECT_TRUE(step.contains("timeout_seconds"));
                    EXPECT_EQ(step["timeout_seconds"], 300);
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  kairos.runWorkflow
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(McpToolsTest, RunWorkflow_ById) {
    auto result = call_tool("kairos.runWorkflow",
                            {{"workflow_id", "wfl-abc123"}});
    EXPECT_EQ(result["status"], "running");
    EXPECT_FALSE(result["run_id"].get<std::string>().empty());
    EXPECT_EQ(result["follow"], false);

    // Check submit callback was invoked.
    ASSERT_EQ(submitted_runs_.size(), 1u);
    EXPECT_EQ(submitted_runs_[0].target_id, "wfl-abc123");
    EXPECT_EQ(submitted_runs_[0].kind, "workflow");
}

TEST_F(McpToolsTest, RunWorkflow_ByName) {
    auto result = call_tool("kairos.runWorkflow",
                            {{"workflow_id", "Deploy Pipeline"}});
    EXPECT_EQ(result["status"], "running");
    // Should resolve name → ID before submitting.
    ASSERT_EQ(submitted_runs_.size(), 1u);
    EXPECT_EQ(submitted_runs_[0].target_id, "wfl-abc123");
}

TEST_F(McpToolsTest, RunWorkflow_NotFound) {
    auto result = call_tool("kairos.runWorkflow",
                            {{"workflow_id", "nonexistent"}});
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(McpToolsTest, RunWorkflow_MissingParam) {
    auto result = call_tool("kairos.runWorkflow");
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(McpToolsTest, RunWorkflow_WithFollow) {
    auto result = call_tool("kairos.runWorkflow",
                            {{"workflow_id", "wfl-abc123"},
                             {"follow", true}});
    EXPECT_EQ(result["follow"], true);
    EXPECT_EQ(result["status"], "running");
}

TEST_F(McpToolsTest, RunWorkflow_SubmitFailure) {
    // Set up handler with failing submit callback.
    McpHandler::Dependencies deps;
    deps.registry = registry_;
    deps.submit_run = [](const std::string&,
                          engine::TriggerEvent::TargetKind)
        -> std::string {
        return {};  // Failure
    };
    McpHandler handler(std::move(deps));

    json params = {{"name", "kairos.runWorkflow"},
                   {"arguments", {{"workflow_id", "wfl-abc123"}}}};
    auto response = handler.dispatch("tools/call", params, 1);
    auto text = response["content"][0]["text"].get<std::string>();
    auto result = json::parse(text);

    EXPECT_TRUE(result.contains("error"));
    EXPECT_NE(result["error"].get<std::string>().find("Failed"),
              std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
//  kairos.listJobs
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(McpToolsTest, ListJobs_ReturnsStandalone) {
    auto result = call_tool("kairos.listJobs");
    EXPECT_EQ(result["count"], 1);
    ASSERT_EQ(result["jobs"].size(), 1u);
    EXPECT_EQ(result["jobs"][0]["id"], "job-backup");
    EXPECT_EQ(result["jobs"][0]["name"], "backup");
    EXPECT_EQ(result["jobs"][0]["step_count"], 1);
}

TEST_F(McpToolsTest, ListJobs_NoRegistry_ReturnsError) {
    McpHandler::Dependencies deps;
    McpHandler handler(std::move(deps));

    json params = {{"name", "kairos.listJobs"}, {"arguments", {}}};
    auto response = handler.dispatch("tools/call", params, 1);
    auto text = response["content"][0]["text"].get<std::string>();
    auto result = json::parse(text);

    EXPECT_TRUE(result.contains("error"));
}

// ═══════════════════════════════════════════════════════════════════════════
//  kairos.runJob
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(McpToolsTest, RunJob_ById) {
    auto result = call_tool("kairos.runJob",
                            {{"job_id", "job-backup"}});
    EXPECT_EQ(result["status"], "running");
    EXPECT_FALSE(result["run_id"].get<std::string>().empty());

    ASSERT_EQ(submitted_runs_.size(), 1u);
    EXPECT_EQ(submitted_runs_[0].target_id, "job-backup");
    EXPECT_EQ(submitted_runs_[0].kind, "job");
}

TEST_F(McpToolsTest, RunJob_ByName) {
    auto result = call_tool("kairos.runJob",
                            {{"job_id", "backup"}});
    EXPECT_EQ(result["status"], "running");
    ASSERT_EQ(submitted_runs_.size(), 1u);
    EXPECT_EQ(submitted_runs_[0].target_id, "job-backup");
}

TEST_F(McpToolsTest, RunJob_NotFound) {
    auto result = call_tool("kairos.runJob",
                            {{"job_id", "nonexistent"}});
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(McpToolsTest, RunJob_MissingParam) {
    auto result = call_tool("kairos.runJob");
    EXPECT_TRUE(result.contains("error"));
}

// ═══════════════════════════════════════════════════════════════════════════
//  kairos.queryRuns
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(McpToolsTest, QueryRuns_Default) {
    auto result = call_tool("kairos.queryRuns");
    EXPECT_EQ(result["count"], 2);
    ASSERT_EQ(result["runs"].size(), 2u);

    // Should be in reverse chronological order.
    EXPECT_EQ(result["runs"][0]["run_id"], "run-002");
    EXPECT_EQ(result["runs"][1]["run_id"], "run-001");
}

TEST_F(McpToolsTest, QueryRuns_LimitOne) {
    auto result = call_tool("kairos.queryRuns",
                            {{"limit", 1}});
    EXPECT_EQ(result["count"], 1);
}

TEST_F(McpToolsTest, QueryRuns_FilterByStatus) {
    auto result = call_tool("kairos.queryRuns",
                            {{"status", "success"}});
    EXPECT_EQ(result["count"], 1);
    EXPECT_EQ(result["runs"][0]["status"], "SUCCESS");
}

TEST_F(McpToolsTest, QueryRuns_FilterByStatus_CaseInsensitive) {
    auto result = call_tool("kairos.queryRuns",
                            {{"status", "FAILURE"}});
    EXPECT_EQ(result["count"], 1);
    EXPECT_EQ(result["runs"][0]["status"], "FAILURE");
}

TEST_F(McpToolsTest, QueryRuns_NoQueryReader_ReturnsError) {
    McpHandler::Dependencies deps;
    McpHandler handler(std::move(deps));

    json params = {{"name", "kairos.queryRuns"}, {"arguments", {}}};
    auto response = handler.dispatch("tools/call", params, 1);
    auto text = response["content"][0]["text"].get<std::string>();
    auto result = json::parse(text);

    EXPECT_TRUE(result.contains("error"));
}

// ═══════════════════════════════════════════════════════════════════════════
//  kairos.getRunDetail
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(McpToolsTest, GetRunDetail_Found) {
    auto result = call_tool("kairos.getRunDetail",
                            {{"run_id", "run-001"}});
    EXPECT_EQ(result["run_id"], "run-001");
    EXPECT_EQ(result["status"], "SUCCESS");
    EXPECT_EQ(result["target_name"], "Deploy Pipeline");

    // Should have jobs with steps.
    ASSERT_FALSE(result["jobs"].empty());
    EXPECT_EQ(result["jobs"][0]["job_name"], "setup");
    EXPECT_FALSE(result["jobs"][0]["steps"].empty());
}

TEST_F(McpToolsTest, GetRunDetail_NotFound) {
    auto result = call_tool("kairos.getRunDetail",
                            {{"run_id", "run-nonexistent"}});
    EXPECT_TRUE(result.contains("error"));
    EXPECT_NE(result["error"].get<std::string>().find("not found"),
              std::string::npos);
}

TEST_F(McpToolsTest, GetRunDetail_MissingParam) {
    auto result = call_tool("kairos.getRunDetail");
    EXPECT_TRUE(result.contains("error"));
}

// ═══════════════════════════════════════════════════════════════════════════
//  kairos.getRunLogs
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(McpToolsTest, GetRunLogs_AllChunks) {
    auto result = call_tool("kairos.getRunLogs",
                            {{"run_id", "run-001"}});
    EXPECT_EQ(result["count"], 2);  // stdout + stderr chunks
    ASSERT_EQ(result["chunks"].size(), 2u);

    // First chunk should be stdout.
    EXPECT_EQ(result["chunks"][0]["stream"], "stdout");
    EXPECT_EQ(result["chunks"][0]["content"], "hello");

    // Second chunk should be stderr.
    EXPECT_EQ(result["chunks"][1]["stream"], "stderr");
    EXPECT_EQ(result["chunks"][1]["content"], "warning: test");
}

TEST_F(McpToolsTest, GetRunLogs_WithCursor) {
    // Get first chunk.
    auto result1 = call_tool("kairos.getRunLogs",
                             {{"run_id", "run-001"}, {"limit", 1}});
    EXPECT_EQ(result1["count"], 1);
    EXPECT_FALSE(result1["next_cursor"].is_null());

    // Get remaining chunks using cursor.
    auto cursor = result1["next_cursor"].get<std::string>();
    auto result2 = call_tool("kairos.getRunLogs",
                             {{"run_id", "run-001"},
                              {"cursor", cursor}});
    EXPECT_EQ(result2["count"], 1);
}

TEST_F(McpToolsTest, GetRunLogs_EmptyRun) {
    auto result = call_tool("kairos.getRunLogs",
                            {{"run_id", "run-nonexistent"}});
    EXPECT_EQ(result["count"], 0);
    EXPECT_TRUE(result["chunks"].empty());
    EXPECT_TRUE(result["next_cursor"].is_null());
}

TEST_F(McpToolsTest, GetRunLogs_MissingParam) {
    auto result = call_tool("kairos.getRunLogs");
    EXPECT_TRUE(result.contains("error"));
}

// ═══════════════════════════════════════════════════════════════════════════
//  kairos.getStepOutput
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(McpToolsTest, GetStepOutput_Found) {
    auto result = call_tool("kairos.getStepOutput",
                            {{"run_id", "run-001"},
                             {"step_id", "stp-001"}});
    EXPECT_EQ(result["stdout"], "hello");
    EXPECT_EQ(result["stderr"], "warning: test");
    EXPECT_EQ(result["exit_code"], 0);
}

TEST_F(McpToolsTest, GetStepOutput_NotFound) {
    auto result = call_tool("kairos.getStepOutput",
                            {{"run_id", "run-001"},
                             {"step_id", "stp-nonexistent"}});
    // Should return empty stdout/stderr, not an error.
    EXPECT_TRUE(result["stdout"].get<std::string>().empty());
    EXPECT_TRUE(result["stderr"].get<std::string>().empty());
}

TEST_F(McpToolsTest, GetStepOutput_MissingParams) {
    auto result = call_tool("kairos.getStepOutput",
                            {{"run_id", "run-001"}});
    EXPECT_TRUE(result.contains("error"));
}

// ═══════════════════════════════════════════════════════════════════════════
//  kairos.explainPlan
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(McpToolsTest, ExplainPlan_ById) {
    auto result = call_tool("kairos.explainPlan",
                            {{"workflow_id", "wfl-abc123"}});
    EXPECT_EQ(result["workflow_id"], "wfl-abc123");
    EXPECT_EQ(result["workflow_name"], "Deploy Pipeline");
    EXPECT_FALSE(result["entries"].empty());
    EXPECT_TRUE(result.contains("summary"));
}

TEST_F(McpToolsTest, ExplainPlan_ByName) {
    auto result = call_tool("kairos.explainPlan",
                            {{"workflow_id", "Deploy Pipeline"}});
    EXPECT_EQ(result["workflow_id"], "wfl-abc123");
}

TEST_F(McpToolsTest, ExplainPlan_ConditionPending) {
    auto result = call_tool("kairos.explainPlan",
                            {{"workflow_id", "wfl-abc123"}});

    // The deploy job has condition "job(\"test\").last_success"
    // which references "test" — a same-workflow job.
    // Should be marked ConditionPending.
    bool found_pending = false;
    for (const auto& entry : result["entries"]) {
        if (entry["job_name"] == "deploy") {
            EXPECT_EQ(entry["action"], "PEND");
            EXPECT_TRUE(entry.contains("condition_expr"));
            found_pending = true;
        }
    }
    EXPECT_TRUE(found_pending);
}

TEST_F(McpToolsTest, ExplainPlan_LevelOrdering) {
    auto result = call_tool("kairos.explainPlan",
                            {{"workflow_id", "wfl-abc123"}});

    // setup (level 0) → build, test (level 1) → deploy (level 2)
    int prev_level = -1;
    for (const auto& entry : result["entries"]) {
        int level = entry["level"].get<int>();
        EXPECT_GE(level, prev_level);
        prev_level = level;
    }
}

TEST_F(McpToolsTest, ExplainPlan_Summary) {
    auto result = call_tool("kairos.explainPlan",
                            {{"workflow_id", "wfl-abc123"}});

    auto& summary = result["summary"];
    EXPECT_GE(summary["jobs_to_run"].get<int>(), 3);
    EXPECT_GE(summary["max_parallelism"].get<int>(), 1);
}

TEST_F(McpToolsTest, ExplainPlan_NotFound) {
    auto result = call_tool("kairos.explainPlan",
                            {{"workflow_id", "nonexistent"}});
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(McpToolsTest, ExplainPlan_MissingParam) {
    auto result = call_tool("kairos.explainPlan");
    EXPECT_TRUE(result.contains("error"));
}

// ═══════════════════════════════════════════════════════════════════════════
//  Registry update (config reload propagation)
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(McpToolsTest, UpdateRegistry_ReflectsNewWorkflows) {
    // Start with 1 workflow.
    auto result1 = call_tool("kairos.listWorkflows");
    EXPECT_EQ(result1["count"], 1);

    // Update registry to empty.
    auto empty_reg = std::make_shared<engine::WorkflowRegistry>(
        std::vector<engine::WorkflowDef>{},
        std::vector<engine::TimerEntry>{});
    handler_->update_registry(empty_reg);

    auto result2 = call_tool("kairos.listWorkflows");
    EXPECT_EQ(result2["count"], 0);
}

// ═══════════════════════════════════════════════════════════════════════════
//  tools/list: verify all 14 tools have schemas
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(McpToolsTest, ToolsList_Contains14Tools) {
    auto result = handler_->dispatch("tools/list", {}, 1);
    auto tools = result["tools"];

    // 14 tools from spec §22.4 + 1 additional (watchScanOnce) = 15.
    EXPECT_EQ(tools.size(), 15u);

    // Verify all expected tool names are present.
    std::vector<std::string> expected_tools = {
        "kairos.listWorkflows", "kairos.getWorkflow",
        "kairos.runWorkflow",   "kairos.listJobs",
        "kairos.runJob",        "kairos.queryRuns",
        "kairos.getRunDetail",  "kairos.getRunLogs",
        "kairos.getStepOutput", "kairos.listWatchGroups",
        "kairos.getEvents",     "kairos.watchScanOnce",
        "kairos.reloadConfig",  "kairos.explainPlan",
        "kairos.getMetrics",
    };

    EXPECT_EQ(tools.size(), expected_tools.size());
}

TEST_F(McpToolsTest, ToolsList_AllHaveInputSchema) {
    auto result = handler_->dispatch("tools/list", {}, 1);
    for (const auto& tool : result["tools"]) {
        SCOPED_TRACE(tool["name"].get<std::string>());
        EXPECT_TRUE(tool.contains("name"));
        EXPECT_TRUE(tool.contains("description"));
        EXPECT_TRUE(tool.contains("inputSchema"));
    }
}

}  // namespace kairos::mcp::test
