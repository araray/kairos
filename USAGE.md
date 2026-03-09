# Kairos Usage Guide

> *Unified orchestration for scheduling, workflows, and filesystem monitoring.*

This guide covers installation, configuration, CLI usage, common workflows, and troubleshooting.

---

## Table of Contents

1. [Installation](#1-installation)
2. [Configuration](#2-configuration)
3. [CLI Reference](#3-cli-reference)
4. [Workflow Definitions](#4-workflow-definitions)
5. [Running the Daemon](#5-running-the-daemon)
6. [Database Management](#6-database-management)
7. [Filesystem Monitoring](#7-filesystem-monitoring)
8. [MCP Agent Interface](#8-mcp-agent-interface)
9. [Scripted Output (JSON Mode)](#9-scripted-output-json-mode)
10. [Logging](#10-logging)
11. [Environment Variables](#11-environment-variables)
12. [Build Scripts](#12-build-scripts)
13. [Examples](#13-examples)
14. [Troubleshooting](#14-troubleshooting)

---

## 1. Installation

### From Source (Linux / macOS)

```bash
# Install prerequisites
sudo apt-get install -y cmake g++ libsqlite3-dev    # Ubuntu/Debian
# or
brew install cmake sqlite3                            # macOS

# Clone and build
git clone https://github.com/araray/kairos.git
cd kairos

# Option A: Use the build script (recommended)
./scripts/build.sh --release

# Option B: Manual CMake
cmake -B build -DCMAKE_BUILD_TYPE=Release -DKAIROS_BUILD_TESTS=ON
cmake --build build -j$(nproc)

# Run tests
./scripts/test.sh
# or: cd build && ctest --output-on-failure

# (Optional) Install system-wide
sudo cmake --install build
```

### From Source (Windows)

```powershell
# Requires Visual Studio Build Tools with C++ workload
git clone https://github.com/araray/kairos.git
cd kairos

# Option A: Use the build script (recommended)
.\scripts\build.ps1 -Release

# Option B: Manual CMake
cmake -B build -G "Visual Studio 17 2022" -DKAIROS_BUILD_TESTS=ON
cmake --build build --config Release -j

# Run tests
.\scripts\test.ps1
```

### Verify Installation

```bash
kairos version
# Output: Kairos v2.0.0

kairos --json version
# Output: {"version":"2.0.0"}
```

---

## 2. Configuration

Kairos uses [confy-cpp](https://github.com/araray/confy-cpp) for layered configuration. Sources are merged in this order (later overrides earlier):

| Priority | Source | Example |
|----------|--------|---------|
| 1 (lowest) | Hardcoded defaults | `worker_pool_size = 4` |
| 2 | Config file (TOML) | `kairos.toml` |
| 3 | `.env` file | `KAIROS_LOGGING_LEVEL=debug` |
| 4 | Environment variables | `export KAIROS_RUNNERS_WORKER__POOL__SIZE=8` |
| 5 (highest) | CLI overrides | `--set kairos.logging.level=trace` |

### Config File Locations

Kairos searches for `kairos.toml` in this order:

1. `--config <path>` CLI flag
2. `KAIROS_CONFIG_FILE` environment variable
3. Platform default:
   - **Linux:** `$XDG_CONFIG_HOME/kairos/kairos.toml` or `~/.config/kairos/kairos.toml`
   - **macOS:** `~/Library/Application Support/kairos/kairos.toml`
   - **Windows:** `%APPDATA%\kairos\kairos.toml`
4. `./kairos.toml` (current directory)

### Creating Your Config

```bash
# Copy the example and edit
mkdir -p ~/.config/kairos
cp deploy/kairos.toml.example ~/.config/kairos/kairos.toml
```

### Minimal Config

The two mandatory keys are `data_dir` and `db_path`:

```toml
[kairos]
data_dir = "~/.local/share/kairos"
db_path  = "~/.local/share/kairos/kairos.db"
```

Everything else has sensible defaults. See `deploy/kairos.toml.example` for the complete reference with all 47 keys documented.

### Key Configuration Sections

**Logging** — control verbosity, format, and file output:

```toml
[kairos.logging]
level  = "info"     # trace | debug | info | warn | error | critical
format = "auto"     # auto | json | text  ("auto" = JSON when piped, text in TTY)
# file = "/var/log/kairos/kairos.log"   # uncomment for file logging
```

**Runners** — control process execution:

```toml
[kairos.runners]
worker_pool_size  = 4       # concurrent job slots
default_timeout_s = 3600    # 1 hour default timeout
kill_timeout_s    = 10      # grace period before SIGKILL
```

**Watch engine** — filesystem monitoring defaults:

```toml
[kairos.watch]
default_mode     = "hybrid"       # native | sample | hybrid
sample_interval_s = 30
max_depth         = 10
hash_policy       = "mtime+size"  # mtime+size | sha256
exclude_patterns  = [".git", "node_modules", "__pycache__", ".DS_Store"]
```

**Persistence** — database and retention:

```toml
[kairos.persistence]
retention_days    = 90
wal_mode          = true
```

**Telemetry** — metrics snapshots:

```toml
[kairos.telemetry]
metrics_snapshot_interval_seconds = 60   # 0 to disable
metrics_retention_days            = 7    # prune snapshots older than this
```

### Validating Your Config

```bash
kairos config validate
# Configuration is valid.

kairos config validate --config ./kairos.toml
# or with JSON output:
kairos --json config validate
# {"valid": true}
```

### Viewing Effective Config

```bash
kairos config show
# Config file: /home/user/.config/kairos/kairos.toml
# Data dir:    /home/user/.local/share/kairos
# DB path:     /home/user/.local/share/kairos/kairos.db
```

### Environment Variable Overrides

All config keys can be overridden via environment variables with the `KAIROS_` prefix. Dots become underscores; underscores in key names use double underscore:

```bash
# kairos.logging.level = "debug"
export KAIROS_LOGGING_LEVEL=debug

# kairos.runners.worker_pool_size = 8
export KAIROS_RUNNERS_WORKER__POOL__SIZE=8
```

---

## 3. CLI Reference

### Global Options

```
kairos [GLOBAL OPTIONS] <subcommand> [SUBCOMMAND OPTIONS]

Global Options:
  -c, --config <path>       Path to kairos.toml
  --log-level <level>       Override log level (trace|debug|info|warn|error|critical)
  --json                    Force JSON output for scripting
  -h, --help                Show help
```

### Subcommand Tree

```
kairos
├── version                  Print version information
├── start                    Start the Kairos daemon
│   └── -f, --foreground     Run in foreground (default behavior)
├── init-db                  Initialize the SQLite database
│   └── --db-path            Override database path
├── status                   Show daemon and engine status
│   └── --history            Include metric trends
├── mcp                      Start MCP stdio server for agent integration
│
├── workflows                Workflow management
│   ├── list                 List all workflows
│   ├── show <id>            Show workflow detail (DAG, jobs, steps)
│   ├── run <id>             Trigger a workflow run
│   │   └── -f, --follow     Follow log output until completion
│   └── explain <id>         [disabled] Explain execution plan
│
├── jobs                     Standalone job management
│   ├── list                 List standalone jobs
│   ├── show <id>            Show job detail (steps, conditions)
│   └── run <id>             Run a standalone job
│       └── -f, --follow     Follow log output until completion
│
├── runs                     Run history
│   ├── list                 List recent runs
│   │   ├── -n, --limit      Max runs to show (default: 20)
│   │   ├── --status         Filter by status (SUCCESS|FAILURE|RUNNING|CANCELLED)
│   │   ├── --workflow       Filter by workflow name (substring)
│   │   └── --since          Only runs after ISO-8601 timestamp
│   ├── show <run_id>        Show run detail with jobs and steps
│   └── cancel <run_id>      [disabled] Cancel a running run
│
├── logs <run_id>            View logs for a run
│   └── -f, --follow         Stream live logs (polls at 200ms)
│
├── watches                  Watch group management
│   ├── list                 List watch groups and status
│   ├── show <name>          Show watch group detail
│   └── scan-once [group]    Run a single scan cycle
│
├── events                   Watch events
│   └── list                 List recent events
│       ├── --watch-group    Filter by watch group
│       └── -n, --limit      Max events (default: 50)
│
├── config                   Configuration management
│   ├── show                 Show effective configuration
│   ├── validate             Validate configuration files
│   └── reload               Reload config (send SIGHUP to daemon)
│
├── explain                  [disabled] Explain execution plan
├── stop                     [disabled] Stop the daemon
└── prune                    [disabled] Prune old records
```

### Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Generic error |
| 2 | Configuration error (missing/invalid config, mandatory key missing) |
| 4 | Not found (workflow/job/run ID does not exist) |
| 5 | Already running (daemon already started) |
| 6 | Not running (daemon not started, for commands that require it) |
| 126 | Command not executable (permissions) |
| 127 | Command not found |
| 200 | Timeout exceeded (soft kill — SIGTERM) |
| 201 | Timeout exceeded (hard kill — SIGKILL) |
| 202 | Runner configuration error |
| 203 | Dependency failure (upstream job failed) |
| 204 | Condition evaluation error (KEL) |

---

### `kairos version`

Print version information.

```bash
$ kairos version
Kairos v2.0.0

$ kairos --json version
{"version":"2.0.0"}
```

---

### `kairos init-db`

Initialize the SQLite database (create tables, run migrations). Safe to run repeatedly (idempotent).

```bash
# Use path from config
$ kairos init-db
Database initialized: /home/user/.local/share/kairos/kairos.db (schema v1)

# Override path
$ kairos init-db --db-path /tmp/test.db
Database initialized: /tmp/test.db (schema v1)
```

---

### `kairos start`

Start the Kairos daemon. The daemon runs the scheduler, watch engine, and pipeline.

```bash
# Start with default config
$ kairos start

# Start with explicit config
$ kairos start --config /etc/kairos/kairos.toml

# Start with debug logging
$ kairos start --log-level debug

# Stop with SIGTERM or Ctrl+C
$ kill -TERM $(cat ~/.local/share/kairos/kairos.lock)
```

---

### `kairos status`

Show daemon status, run statistics, and optionally metric trends.

```bash
$ kairos status

  KAIROS v2.0.0
  Status: RUNNING  (PID 12345)
  Config: /home/user/.config/kairos/kairos.toml
  Data:   /home/user/.local/share/kairos

  Workflows:    3    Watch Groups: 2
  DB Size:      1.2 MB    Total Runs:   142
  Runs Today:   12    Failures:     1

$ kairos status --history

  KAIROS v2.0.0
  ...

  Metrics History (latest snapshots):

  METRIC                          TYPE       VALUE     RECORDED AT
  ────────────────────────────────  ─────────  ────────  ───────────────────
  kairos_runs_total               counter    142       2026-03-08T21:30:00
  kairos_runs_active              gauge      0.00      2026-03-08T21:30:00
  kairos_uptime_seconds           gauge      86423.70  2026-03-08T21:30:00

$ kairos --json status --history
{
  "version": "2.0.0",
  "daemon_running": true,
  "daemon_pid": "12345",
  ...
  "metrics_history": [
    {"metric_name": "kairos_runs_total", "metric_type": "counter", "value": 142.0, ...}
  ]
}
```

---

### `kairos workflows list`

List all configured workflows.

```bash
$ kairos workflows list
WORKFLOW                        ID            JOBS
──────────────────────────────  ────────────  ────────────────────────
data-pipeline                   wfl-a1b2c3d4  extract, transform, load
deploy-staging                  wfl-e5f6g7h8  build, test, deploy

$ kairos --json workflows list
[
  {"id": "wfl-a1b2c3d4", "name": "data-pipeline", "job_count": 3, "jobs": ["extract", "transform", "load"]},
  ...
]
```

---

### `kairos workflows show <id>`

Show workflow detail including DAG structure, jobs, conditions, and steps.

```bash
$ kairos workflows show data-pipeline

  Workflow: data-pipeline
  ID:       wfl-a1b2c3d4e5f6
  Jobs:     3
  DAG:      2 level(s)

  Execution Order (DAG):
  Level 0: extract
  ↓
  Level 1: transform, load

  Jobs:

    extract  (job-111aaa)
      steps (2):
        1. fetch-data: curl -o /tmp/data.csv https://api.example.com/data
        2. validate: python3 validate.py /tmp/data.csv

    transform  (job-222bbb)
      needs: job-111aaa
      condition: job("extract").last_success
      steps (1):
        1. run-etl: python3 etl.py --input /tmp/data.csv

    load  (job-333ccc)
      needs: job-111aaa
      steps (1):
        1. load-db: psql -f load.sql
```

---

### `kairos workflows run <id>`

Trigger a workflow run. The CLI creates a standalone pipeline (no running daemon required), executes the workflow synchronously, and reports the result.

```bash
$ kairos workflows run data-pipeline
[ok] Workflow 'data-pipeline' completed: SUCCESS
  Run ID: run-abc123def456
  View details: kairos runs show run-abc123def456
  View logs: kairos logs run-abc123def456

# With log following (displays output after completion)
$ kairos workflows run data-pipeline --follow
   | Fetching data from API...
   | 200 OK (1.2MB)
   | Validating schema...
   | Running ETL pipeline...
   | Loading to database...
[ok] Workflow 'data-pipeline' completed: SUCCESS
  Run ID: run-abc123def456

# JSON output (for scripting)
$ kairos --json workflows run data-pipeline
{
  "run_id": "run-abc123def456",
  "status": "SUCCESS",
  "workflow": "data-pipeline"
}
```

---

### `kairos jobs list`

List all standalone jobs (jobs not embedded in a workflow).

```bash
$ kairos jobs list
JOB              ID            STEPS            CONDITION
───────────────  ────────────  ───────────────  ──────────────────────────
daily-backup     job-aaa111    dump-db, upload  job("cleanup").last_success
health-check     job-bbb222    ping, report

2 standalone job(s)
```

---

### `kairos jobs show <id>`

Show standalone job detail with steps, conditions, and environment.

```bash
$ kairos jobs show daily-backup

  Job: daily-backup
  ID:  job-aaa111bbb222
  Condition: job("cleanup").last_success

  Steps (2):
    1. dump-db
       cmd: pg_dump -Fc mydb > /tmp/backup.dump
       cwd: /opt/backups
    2. upload
       cmd: aws s3 cp /tmp/backup.dump s3://backups/
       env: 2 var(s)
```

---

### `kairos jobs run <id>`

Run a standalone job via a standalone pipeline.

```bash
$ kairos jobs run daily-backup
[ok] Job 'daily-backup' completed: SUCCESS
  Run ID: run-xyz789
  View details: kairos runs show run-xyz789
  View logs: kairos logs run-xyz789

# With log following
$ kairos jobs run daily-backup --follow
   | Dumping database...
   | Uploading to S3...
[ok] Job 'daily-backup' completed: SUCCESS
```

---

### `kairos runs list`

List recent run history with filtering options.

```bash
$ kairos runs list
RUN ID          TARGET                STATUS              TRIGGER   STARTED              DURATION
──────────────  ────────────────────  ──────────────────  ────────  ───────────────────  ────────
run-abc123de..  data-pipeline         ✓ SUCCESS           schedule  2026-03-08T21:00:00  1.2s
run-def456gh..  deploy-staging        ✗ FAILURE           manual    2026-03-08T20:45:00  3.5s
run-ijk789lm..  daily-backup          ✓ SUCCESS           schedule  2026-03-08T20:00:00  890ms

3 run(s)

# Filter by status
$ kairos runs list --status FAILURE
# Filter by workflow name
$ kairos runs list --workflow deploy
# Filter by time
$ kairos runs list --since 2026-03-08T00:00:00Z
# Limit results
$ kairos runs list -n 5
```

Note: Status indicators use colored Unicode symbols (✓ ✗ ● ○) when the terminal supports ANSI colors, and plain text `[ok]` `[!!]` `[>>]` `[--]` otherwise.

---

### `kairos runs show <run_id>`

Show full detail for a single run, including jobs and steps.

```bash
$ kairos runs show run-abc123def456

  Run: run-abc123def456
  Workflow: data-pipeline (wfl-a1b2c3d4e5f6)
  Status: ✓ SUCCESS  Exit: 0
  Trigger: schedule
  Started: 2026-03-08T21:00:00.123Z
  Finished: 2026-03-08T21:00:01.345Z  Duration: 1.2s

  Jobs (3):
    ✓ extract — SUCCESS (450ms)
        ✓ fetch-data — SUCCESS (exit 0, 320ms)
        ✓ validate — SUCCESS (exit 0, 120ms)
    ✓ transform — SUCCESS (600ms)
        ✓ run-etl — SUCCESS (exit 0, 590ms)
    ✓ load — SUCCESS (150ms)
        ✓ load-db — SUCCESS (exit 0, 140ms)
```

---

### `kairos logs <run_id>`

View captured stdout/stderr for a run.

```bash
# View all logs for a run
$ kairos logs run-abc123def456
   | Fetching data from API...
   | 200 OK (1.2MB)
   | Validating schema...
   | All checks passed.

# Follow mode — poll for new log output every 200ms until run completes
$ kairos logs run-abc123def456 --follow
   | Building project...
   | [1/5] Compiling main.cpp
   | [2/5] Compiling util.cpp
   ...

--- Run SUCCESS (exit 0) ---

# JSON output (one JSON object per line, for streaming parsers)
$ kairos --json logs run-abc123def456
{"id":1,"job_id":"job-111","step_id":"stp-aaa","stream":"stdout","content":"Fetching...","timestamp":"..."}
```

Stderr lines are prefixed with `ERR|`:

```
   | Starting process...
ERR| Warning: deprecated API version
   | Processing complete.
```

---

### `kairos watches list`

List configured watch groups and their status.

```bash
$ kairos watches list
GROUP                MODE     PATHS  FILES  LAST SCAN              STATUS
--------------------------------------------------------------------------------
config-files         hybrid   3      42     2026-03-08T21:30:00Z   active
log-rotation         sample   1      128    2026-03-08T21:29:30Z   active
```

---

### `kairos watches show <name>`

Show watch group detail including paths, rules, and configuration.

```bash
$ kairos watches show config-files
Watch Group: config-files
ID: wg-abc123
Mode: hybrid
Sample Rate: 30s
Max Depth: 5
Max Files: 1000

Watch Items:
  /etc/myapp/
  /opt/myapp/config/
  /opt/myapp/secrets/

Exclude Globs:
  *.tmp
  *.bak

Rules (2):
  config-changed: file.modified && file.path matches "*.conf" [warning]
  secret-rotation: file.created && file.path matches "*.key" [critical]
```

---

### `kairos watches scan-once [group]`

Run a single scan cycle without the daemon (for testing and debugging).

```bash
# Scan all groups
$ kairos watches scan-once
Group: config-files — 42 files scanned (+0 -0 ~2) in 15ms
  Triggered: config-changed (file_modified, 2 files)
Group: log-rotation — 128 files scanned (+3 -1 ~0) in 28ms

# Scan a specific group
$ kairos watches scan-once config-files
Group: config-files — 42 files scanned (+0 -0 ~0) in 12ms
```

---

### `kairos events list`

List recent filesystem watch events.

```bash
$ kairos events list
GROUP                RULE            TYPE            SEVERITY   CREATED
--------------------------------------------------------------------------------
config-files         config-changed  file_modified   warning    2026-03-08T21:30:00
log-rotation         size-exceeded   threshold       critical   2026-03-08T21:15:00

2 event(s)

# Filter by watch group
$ kairos events list --watch-group config-files

# Limit number of results
$ kairos events list -n 10
```

---

### `kairos config reload`

Signal the running daemon to reload its configuration files (sends `SIGHUP`).

```bash
$ kairos config reload
Reload signal sent to daemon (PID 12345)
```

Note: This is POSIX-only. On Windows, use the MCP `reloadConfig` tool instead.

---

### `kairos mcp`

Start the MCP (Model Context Protocol) stdio server for AI agent integration.

```bash
$ kairos mcp --config kairos.toml
# (stdin/stdout become JSON-RPC 2.0 transport)
# Logging goes to stderr
```

The MCP server exposes 14 tools for agents: `kairos.listWorkflows`, `kairos.runWorkflow`, `kairos.queryRuns`, `kairos.getRunDetail`, `kairos.getRunLogs`, `kairos.listJobs`, `kairos.runJob`, `kairos.listWatchGroups`, `kairos.getEvents`, `kairos.reloadConfig`, `kairos.explainPlan`, `kairos.getMetrics`, `kairos.getWorkflow`, `kairos.getStepOutput`.

---

## 4. Workflow Definitions

Workflows and standalone jobs are defined in YAML files. By default, Kairos looks for:

- `workflows/` directory (relative to config file) — workflow definitions
- `watch_groups/` directory — watch group definitions

### Workflow YAML

```yaml
# workflows/data-pipeline.yaml
name: data-pipeline

jobs:
  - name: extract
    steps:
      - name: fetch-data
        command: curl -o /tmp/data.csv https://api.example.com/data
      - name: validate
        command: python3 validate.py /tmp/data.csv

  - name: transform
    needs: [extract]
    condition: 'job("extract").last_success'
    steps:
      - name: run-etl
        command: python3 etl.py --input /tmp/data.csv

  - name: load
    needs: [extract]
    steps:
      - name: load-db
        command: psql -f load.sql
```

### Standalone Job YAML

```yaml
# workflows/backup.yaml
name: daily-backup
standalone: true
condition: 'job("cleanup").last_success'

steps:
  - name: dump-db
    command: pg_dump -Fc mydb > /tmp/backup.dump
    working_dir: /opt/backups
  - name: upload
    command: aws s3 cp /tmp/backup.dump s3://backups/
    env:
      AWS_REGION: us-east-1
```

### Watch Group YAML

```yaml
# watch_groups/config-files.yaml
name: config-files
mode: hybrid
sample_rate: 30s
max_depth: 5

watch:
  - /etc/myapp/
  - /opt/myapp/config/

exclude:
  - "*.tmp"
  - "*.bak"

rules:
  - name: config-changed
    condition: 'file.modified && file.path matches "*.conf"'
    severity: warning
  - name: secret-rotation
    condition: 'file.created && file.path matches "*.key"'
    severity: critical
```

---

## 5. Running the Daemon

### Foreground (Development)

```bash
kairos start --config kairos.toml --log-level debug
```

Structured JSON logs go to stdout. Press `Ctrl+C` to stop.

### Signals

| Signal | Effect |
|--------|--------|
| `SIGTERM` / `SIGINT` / `Ctrl+C` | Graceful shutdown |
| `SIGHUP` | Configuration reload |

### Single-Instance Enforcement

Kairos uses a lock file (`kairos.lock` in the data directory) to ensure only one daemon instance runs at a time:

```bash
$ kairos start &
# Kairos v2.0.0 started

$ kairos start
# Error: Another Kairos instance is already running (lock: …/kairos.lock)
```

The lock is automatically released on clean shutdown.

### Daemon Subsystems

When the daemon starts, it initializes and runs these subsystems:

1. **Scheduler** — evaluates cron/interval triggers, fires TriggerEvents
2. **Watch Engine** — monitors filesystem paths, detects changes, fires TriggerEvents
3. **Pipeline** — consumes TriggerEvents, resolves DAGs, evaluates KEL conditions, dispatches work
4. **Runner Pool** — executes process steps with timeouts, captures stdout/stderr
5. **DB Writer** — async batched persistence to SQLite (WAL mode)
6. **Metrics** — periodic metric snapshots to SQLite (default: every 60s, pruned after 7 days)

---

## 6. Database Management

### Schema

The database uses SQLite in WAL mode with these tables:

- `runs` — workflow and standalone job executions
- `run_jobs` — per-job results within a workflow run
- `run_steps` — per-step results within a job run
- `log_chunks` — captured stdout/stderr from executions
- `watch_samples` — filesystem snapshot samples
- `watch_events` — detected file change events
- `trigger_state` — scheduler trigger state (next fire time, etc.)
- `metrics_snapshots` — periodic metrics persistence
- `config_snapshots` — configuration reload history
- `schema_version` — migration tracking

### Initialization

```bash
# Create the database and apply all migrations
kairos init-db

# Specify a custom path
kairos init-db --db-path /var/lib/kairos/production.db
```

### Migration

Migrations are forward-only, embedded in the binary, and run automatically on daemon startup or `init-db`. Re-running `init-db` on an existing database is safe (idempotent).

### Retention

The daemon automatically prunes old data:

| Data | Default Retention | Config Key |
|------|-------------------|------------|
| Runs (+ jobs, steps, logs) | 90 days | `kairos.persistence.retention_days` |
| Watch samples | 10 epochs per group | (hardcoded) |
| Watch events | Same as runs | `kairos.persistence.retention_days` |
| Metrics snapshots | 7 days | `kairos.telemetry.metrics_retention_days` |

---

## 7. Filesystem Monitoring

Kairos supports three watch modes:

| Mode | Mechanism | Tradeoff |
|------|-----------|----------|
| `native` | inotify (Linux) / FSEvents (macOS) / ReadDirectoryChangesW (Windows) | Real-time but may miss events on queue overflow |
| `sample` | Periodic directory scan + diff | Reliable but not real-time; configurable interval |
| `hybrid` (default) | Native events + periodic sample verification | Best of both: real-time with correctness guarantee |

### Quick Test

```bash
# List watch groups
kairos watches list

# Show a specific group's configuration
kairos watches show my-group

# One-shot scan (no daemon needed)
kairos watches scan-once

# View recent events
kairos events list --watch-group my-group -n 20
```

---

## 8. MCP Agent Interface

Kairos provides a Model Context Protocol (MCP) server for AI agent integration. The MCP server communicates over stdio using JSON-RPC 2.0.

### Starting the MCP Server

```bash
kairos mcp --config kairos.toml
```

### Agent Usage Example

An AI agent (e.g., Claude) can use Kairos as a tool:

```json
{"jsonrpc":"2.0","method":"tools/call","params":{"name":"kairos.runWorkflow","arguments":{"workflow_id":"data-pipeline","follow":true}},"id":1}
```

The MCP server responds with structured results and can stream log notifications when `follow: true`.

---

## 9. Scripted Output (JSON Mode)

Every command supports `--json` for machine consumption:

```bash
# Version as JSON
kairos --json version
# {"version":"2.0.0"}

# Runs as JSON array
kairos --json runs list
# [{"run_id":"...","target_name":"...","status":"SUCCESS",...}]

# Status with metrics history
kairos --json status --history
# {"version":"2.0.0","daemon_running":true,...,"metrics_history":[...]}
```

When `--json` is active, human messages go to stderr and structured data goes to stdout, so you can safely pipe:

```bash
# Parse the most recent run ID
kairos --json runs list -n 1 | jq -r '.[0].run_id'

# Check if last run succeeded
kairos --json runs list -n 1 | jq -r '.[0].status'

# Get workflow details
kairos --json workflows show data-pipeline | jq '.dag_levels'
```

Auto-detection: when stdout is not a TTY (e.g., piped), Kairos automatically uses JSON format for logging and disables colors.

---

## 10. Logging

### Log Formats

| Mode | Format | When Used |
|------|--------|-----------|
| JSON | `{"ts":"…","level":"info","msg":"…"}` | Daemon mode, piped output, `--json` flag |
| Text | `[2026-03-02 14:30:05] [INFO] [kairos] message` | Interactive terminal |

The `auto` format (default) selects JSON when stdout is not a TTY, text when it is.

### Log Levels

From most to least verbose: `trace`, `debug`, `info`, `warn`, `error`, `critical`.

```bash
# Override log level from CLI
kairos start --log-level trace

# Override via environment
KAIROS_LOGGING_LEVEL=debug kairos start
```

### File Logging

Enable in config to write rotated JSON log files:

```toml
[kairos.logging]
file             = "/var/log/kairos/kairos.log"
max_file_size_mb = 100
max_files        = 5
```

### Run Logs

To view the captured stdout/stderr from a specific run:

```bash
# View logs
kairos logs <run_id>

# Follow live (polls at 200ms)
kairos logs <run_id> --follow
```

---

## 11. Environment Variables

### Kairos-Specific

| Variable | Purpose |
|----------|---------|
| `KAIROS_CONFIG_FILE` | Override config file path |
| `KAIROS_*` | Override any `kairos.*` config key (see §2) |
| `NO_COLOR` | Disable ANSI color output (respected by CLI) |

### Injected Into Jobs

These variables are available inside every running job/step:

| Variable | Value |
|----------|-------|
| `KAIROS_RUN_ID` | UUID of the current run |
| `KAIROS_JOB_ID` | ID of the current job |
| `KAIROS_STEP_ID` | ID of the current step |
| `KAIROS_WORKFLOW_ID` | ID of the parent workflow |
| `KAIROS_TRIGGER_TYPE` | `schedule`, `watch`, `manual` |
| `KAIROS_CORRELATION_ID` | Trace correlation ID |
| `KAIROS_DATA_DIR` | Configured data directory path |
| `KAIROS_DB_PATH` | Path to the SQLite database |

---

## 12. Build Scripts

Kairos includes cross-platform build and test scripts under `scripts/`:

### Build

```bash
# Linux / macOS
./scripts/build.sh                       # Default: debug build
./scripts/build.sh --release             # Release build
./scripts/build.sh --profile san         # Debug + ASan + UBSan
./scripts/build.sh --clean --release     # Clean rebuild, release
./scripts/build.sh --compiler clang      # Use Clang
./scripts/build.sh --help                # Full option reference
```

```powershell
# Windows
.\scripts\build.ps1                      # Default: debug build
.\scripts\build.ps1 -Release             # Release build
.\scripts\build.ps1 -Profile san         # Debug + ASan
.\scripts\build.ps1 -Clean -Release      # Clean rebuild, release
.\scripts\build.ps1 -ShowHelp            # Full option reference
```

### Test

```bash
# Linux / macOS
./scripts/test.sh                        # Run all tests (~1128 tests)
./scripts/test.sh --filter "Sha256"      # Filter by name
./scripts/test.sh --list                 # List without running
./scripts/test.sh --verbose              # Full output
./scripts/test.sh --repeat 5             # Stress test
./scripts/test.sh --rerun-failed         # Rerun only failed tests
./scripts/test.sh --help                 # Full option reference
```

```powershell
# Windows
.\scripts\test.ps1                       # Run all tests
.\scripts\test.ps1 -Filter "Sha256"      # Filter by name
.\scripts\test.ps1 -List                 # List without running
.\scripts\test.ps1 -Verbose              # Full output
.\scripts\test.ps1 -ShowHelp             # Full option reference
```

---

## 13. Examples

### Quick Development Setup

```bash
# 1. Build
./scripts/build.sh

# 2. Create minimal config with a workflow
mkdir -p /tmp/kairos-dev/workflows
cat > /tmp/kairos-dev/kairos.toml << 'EOF'
[kairos]
data_dir = "/tmp/kairos-dev/data"
db_path  = "/tmp/kairos-dev/data/kairos.db"
workflows_dir = "workflows"

[kairos.logging]
level = "debug"
format = "text"
EOF

cat > /tmp/kairos-dev/workflows/hello.yaml << 'EOF'
name: hello-world
jobs:
  - name: greet
    steps:
      - name: say-hello
        command: echo "Hello from Kairos!"
      - name: show-env
        command: env | grep KAIROS_
EOF

# 3. Initialize database
./build/debug/kairos init-db --config /tmp/kairos-dev/kairos.toml

# 4. List workflows
./build/debug/kairos workflows list --config /tmp/kairos-dev/kairos.toml

# 5. Run the workflow
./build/debug/kairos workflows run hello-world --follow \
    --config /tmp/kairos-dev/kairos.toml
```

### Inspecting a Failed Run

```bash
# Find the failed run
kairos runs list --status FAILURE -n 5

# Get full detail
kairos runs show run-abc123def456

# View the error output
kairos logs run-abc123def456
```

### CI Pipeline Integration

```bash
#!/usr/bin/env bash
set -euo pipefail

# Build
./scripts/build.sh --profile ci

# Test (returns non-zero on failure)
./scripts/test.sh --stop-on-fail

# Verify binary
./build/debug/kairos version
```

### Legacy Tool Migration

Kairos replaces three legacy Python tools. Here are the command equivalents:

| Legacy Command | Kairos Equivalent |
|----------------|-------------------|
| `avscheduler start` | `kairos start` |
| `avscheduler list-jobs` | `kairos jobs list` |
| `avscheduler run-job <id>` | `kairos jobs run <id>` |
| `avscheduler show-logs <id>` | `kairos logs <run_id>` |
| `avscheduler reload-config` | `kairos config reload` |
| `localflow run <workflow>` | `kairos workflows run <name>` |
| `localflow list` | `kairos workflows list` |
| `eventwatcher status` | `kairos watches list` |
| `eventwatcher list-events` | `kairos events list` |
| `eventwatcher monitor-once` | `kairos watches scan-once` |

### Docker Quick Test

```bash
# One-liner: build and test in a container
docker run --rm -v "$(pwd):/src" -w /src ubuntu:24.04 bash -c '
  apt-get update -qq && apt-get install -y -qq cmake g++ libsqlite3-dev git
  cmake -B build -DCMAKE_BUILD_TYPE=Debug -DKAIROS_BUILD_TESTS=ON
  cmake --build build -j$(nproc)
  cd build && ctest --output-on-failure
'
```

---

## 14. Troubleshooting

### "Another Kairos instance is already running"

The lock file was not cleaned up (e.g., after a crash):

```bash
# Check if kairos is actually running
pgrep -a kairos

# If not running, remove the stale lock
rm ~/.local/share/kairos/kairos.lock
```

### "Missing mandatory configuration keys"

The config file is missing `kairos.data_dir` or `kairos.db_path`. Either set them in your TOML file or via environment:

```bash
export KAIROS_DATA__DIR=/tmp/kairos
export KAIROS_DB__PATH=/tmp/kairos/kairos.db
kairos start
```

### "Config parse error"

Your `kairos.toml` has syntax issues. Validate it:

```bash
kairos config validate --config ./kairos.toml
```

### "Run 'kairos init-db' first"

CLI commands that query the database (runs, logs, events, status) will fail if the database doesn't exist yet:

```bash
kairos init-db
```

### "Workflow not found" / "Job not found"

The workflow or job ID/name doesn't match any definition. Check what's available:

```bash
kairos workflows list
kairos jobs list
```

### "Daemon not running"

Commands like `config reload` require a running daemon. Start it first:

```bash
kairos start &
kairos config reload
```

### No Colors in Terminal

Kairos respects the `NO_COLOR` environment variable. If colors are disabled unexpectedly, check:

```bash
# Remove NO_COLOR if set
unset NO_COLOR

# Ensure stdout is a TTY (colors are disabled when piping)
kairos runs list          # colored
kairos runs list | cat    # no colors (piped)
```

### Build Fails Fetching Dependencies

CMake FetchContent downloads dependencies from GitHub. If behind a firewall:

```bash
# Pre-clone dependencies
git clone https://github.com/araray/confy-cpp.git /tmp/deps/confy-cpp
cmake -B build -DFETCHCONTENT_SOURCE_DIR_CONFY-CPP=/tmp/deps/confy-cpp
```

### Tests Fail with "database is locked"

Multiple test processes are sharing the same database file. Tests use temporary directories by default — if you see this, check for stale test processes:

```bash
pkill -f test_persist
```

---

## References

- [Kairos Design Specification](docs/) — Parts 1–7
- [confy-cpp Documentation](https://github.com/araray/confy-cpp)
- [spdlog Documentation](https://github.com/gabime/spdlog/wiki)
- [CLI11 Documentation](https://cliutils.github.io/CLI11/book/)
- [SQLite WAL Mode](https://www.sqlite.org/wal.html)
- [Model Context Protocol (MCP)](https://modelcontextprotocol.io/)
- [NO_COLOR Convention](https://no-color.org/)
