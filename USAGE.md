# Kairos Usage Guide

> *Unified orchestration for scheduling, workflows, and filesystem monitoring.*

This guide covers installation, configuration, CLI usage, common workflows, and troubleshooting for the complete Kairos system.

---

## Table of Contents

1. [Installation](#1-installation)
2. [Configuration](#2-configuration)
3. [CLI Reference](#3-cli-reference)
4. [Running the Daemon](#4-running-the-daemon)
5. [Workflow Definitions](#5-workflow-definitions)
6. [Standalone Jobs](#6-standalone-jobs)
7. [Filesystem Monitoring](#7-filesystem-monitoring)
8. [Database Management](#8-database-management)
9. [MCP Agent Interface](#9-mcp-agent-interface)
10. [Scripted Output (JSON Mode)](#10-scripted-output-json-mode)
11. [Logging and Observability](#11-logging-and-observability)
12. [Service Integration](#12-service-integration)
13. [Legacy Migration](#13-legacy-migration)
14. [Environment Variables](#14-environment-variables)
15. [Build and Test Scripts](#15-build-and-test-scripts)
16. [Examples](#16-examples)
17. [Troubleshooting](#17-troubleshooting)

---

## 1. Installation

### From Source (Linux / macOS)

```bash
# Install prerequisites
sudo apt-get install -y cmake g++ libsqlite3-dev    # Ubuntu/Debian
# or: brew install cmake sqlite3                      # macOS

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

# Optional: install system-wide
sudo cmake --install build
```

### From Source (Windows)

```powershell
# Requires Visual Studio 2022 with C++ workload
git clone https://github.com/araray/kairos.git
cd kairos

.\scripts\build.ps1 -Release
.\scripts\test.ps1
```

### From Packages

```bash
# Debian/Ubuntu (.deb)
sudo dpkg -i kairos_0.1.0_amd64.deb

# Fedora/RHEL (.rpm)
sudo rpm -i kairos-0.1.0-1.x86_64.rpm

# macOS (Homebrew)
brew install --formula deploy/homebrew/kairos.rb

# Docker
docker run -v /path/to/config:/etc/kairos ghcr.io/araray/kairos:latest
```

### Verify Installation

```bash
kairos version
# Kairos v0.1.0

kairos --json version
# {"version":"0.1.0","major":0,"minor":1,"patch":0}
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
| 5 (highest) | CLI overrides | `--log-level trace` |

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
mkdir -p ~/.config/kairos
cp deploy/kairos.toml.example ~/.config/kairos/kairos.toml
```

### Minimal Config

Only two keys are mandatory — `data_dir` and `db_path`:

```toml
[kairos]
data_dir = "~/.local/share/kairos"
db_path  = "~/.local/share/kairos/kairos.db"
```

Everything else has sensible defaults. See `deploy/kairos.toml.example` for the full reference with all keys documented.

### Key Configuration Sections

**Logging** — control verbosity, format, and file output:

```toml
[kairos.logging]
level  = "info"      # trace | debug | info | warn | error | critical
format = "auto"      # auto | json | text  ("auto" = JSON when piped, text in TTY)
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
default_mode      = "hybrid"       # native | sample | hybrid
sample_interval_s = 30
max_depth         = 10
hash_policy       = "mtime+size"   # mtime+size | sha256
exclude_patterns  = [".git", "node_modules", "__pycache__", ".DS_Store"]
```

**Persistence** — database and retention:

```toml
[kairos.persistence]
retention_days = 90
wal_mode       = true
```

**KEL safety** — expression language limits:

```toml
[kairos.kel]
max_ast_nodes    = 1024
max_ast_depth    = 32
max_eval_time_ms = 100
```

### Validating Config

```bash
kairos config validate
# Configuration is valid.

kairos config validate --config ./kairos.toml
kairos --json config validate
# {"valid": true}
```

### Viewing Effective Config

```bash
kairos config show
# Config file: /home/user/.config/kairos/kairos.toml
# Data dir:    /home/user/.local/share/kairos
# DB path:     /home/user/.local/share/kairos/kairos.db
# ...
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
├── status                   Show daemon and engine status
│   └── --history            Include metric trends
├── mcp                      Start MCP stdio server for agent integration
│
├── workflows                Workflow management
│   ├── list                 List all workflows
│   ├── show <id>            Show workflow detail (DAG, jobs, steps)
│   ├── run <id>             Trigger a workflow run
│   │   └── -f, --follow     Follow log output until completion
│   └── explain <id>         Explain execution plan (dry run)
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
│   └── cancel <run_id>      Cancel a running run
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
├── init-db                  Initialize the SQLite database
│   └── --db-path            Override database path
│
├── migrate-config           Migrate legacy tool configuration
│   ├── --source             Source tool (avscheduler|eventwatcher|localflow)
│   ├── --source-config      Path to source config file
│   ├── --source-watches     Path to EventWatcher watch groups YAML
│   ├── --source-workflows   Path to LocalFlow workflows directory
│   ├── --output-dir         Output directory for migrated files
│   └── --dry-run            Show what would be generated
│
├── migrate-db               Migrate legacy tool database
│   ├── --source             Source tool (avscheduler|eventwatcher)
│   ├── --source-db          Path to source SQLite database
│   └── --target-db          Path to Kairos SQLite database
│
├── explain <id>             Explain execution plan (alias)
├── stop                     Stop the daemon
└── prune                    Prune old records
    └── --older-than <days>  Override retention threshold
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

### `kairos version`

```bash
$ kairos version
Kairos v0.1.0

$ kairos --json version
{"version":"0.1.0","major":0,"minor":1,"patch":0}
```

### `kairos init-db`

Initialize the SQLite database (create tables, run migrations). Safe to run repeatedly (idempotent).

```bash
$ kairos init-db
Database initialized: /home/user/.local/share/kairos/kairos.db (schema v1)

$ kairos init-db --db-path /tmp/test.db
Database initialized: /tmp/test.db (schema v1)
```

### `kairos start`

Start the daemon. Runs the scheduler, watch engine, and pipeline.

```bash
$ kairos start
$ kairos start --config /etc/kairos/kairos.toml
$ kairos start --log-level debug
```

Stop with `Ctrl+C`, `SIGTERM`, or `kairos stop`.

### `kairos status`

```bash
$ kairos status

  KAIROS v0.1.0
  Status: RUNNING  (PID 12345)
  Config: /home/user/.config/kairos/kairos.toml
  Data:   /home/user/.local/share/kairos

  Workflows:    3    Watch Groups: 2
  DB Size:      1.2 MB    Total Runs:   142
  Runs Today:   12    Failures:     1

$ kairos status --history    # includes metric trends
```

### `kairos workflows list`

```bash
$ kairos workflows list
WORKFLOW                        ID            JOBS
──────────────────────────────  ────────────  ────────────────────────
data-pipeline                   wfl-a1b2c3d4  extract, transform, load
deploy-staging                  wfl-e5f6g7h8  build, test, deploy
```

### `kairos workflows run <id> [--follow]`

```bash
$ kairos workflows run data-pipeline
[ok] Workflow 'data-pipeline' completed: SUCCESS
  Run ID: run-abc123def456

$ kairos workflows run data-pipeline --follow
   | Fetching data from API...
   | 200 OK (1.2MB)
   | Running ETL pipeline...
[ok] Workflow 'data-pipeline' completed: SUCCESS

$ kairos --json workflows run data-pipeline
{"run_id":"run-abc123def456","status":"SUCCESS","workflow":"data-pipeline"}
```

### `kairos runs list`

```bash
$ kairos runs list
RUN ID          TARGET                STATUS              TRIGGER   STARTED              DURATION
──────────────  ────────────────────  ──────────────────  ────────  ───────────────────  ────────
run-abc123de..  data-pipeline         ✓ SUCCESS           schedule  2026-03-08T21:00:00  1.2s
run-def456gh..  deploy-staging        ✗ FAILURE           manual    2026-03-08T20:45:00  3.5s

$ kairos runs list --status FAILURE
$ kairos runs list --workflow deploy -n 5
$ kairos runs list --since 2026-03-08T00:00:00Z
```

### `kairos runs show <run_id>`

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

### `kairos logs <run_id> [--follow]`

```bash
$ kairos logs run-abc123def456
   | Fetching data from API...
   | 200 OK (1.2MB)
ERR| Warning: deprecated API version
   | Processing complete.

$ kairos logs run-abc123def456 --follow    # polls at 200ms until complete
```

### `kairos watches list`

```bash
$ kairos watches list
GROUP                MODE     PATHS  FILES  LAST SCAN              STATUS
───────────────────  ───────  ─────  ─────  ─────────────────────  ──────
config-files         hybrid   3      42     2026-03-08T21:30:00Z   active
log-rotation         sample   1      128    2026-03-08T21:29:30Z   active
```

### `kairos watches scan-once`

```bash
$ kairos watches scan-once
Group: config-files — 42 files scanned (+0 -0 ~2) in 15ms
  Triggered: config-changed (file_modified, 2 files)
Group: log-rotation — 128 files scanned (+3 -1 ~0) in 28ms

$ kairos watches scan-once config-files
Group: config-files — 42 files scanned (+0 -0 ~0) in 12ms
```

### `kairos events list`

```bash
$ kairos events list
GROUP                RULE            TYPE            SEVERITY   CREATED
───────────────────  ──────────────  ──────────────  ─────────  ───────────────────
config-files         config-changed  file_modified   warning    2026-03-08T21:30:00
log-rotation         size-exceeded   threshold       critical   2026-03-08T21:15:00

$ kairos events list --watch-group config-files -n 10
```

### `kairos config reload`

Signal the running daemon to reload configuration (sends `SIGHUP`):

```bash
$ kairos config reload
Reload signal sent to daemon (PID 12345)
```

### `kairos explain <id>`

Dry-run: explain what would happen if a workflow were triggered now:

```bash
$ kairos explain data-pipeline

  Execution Plan: data-pipeline
  ═══════════════════════════════

  Level 0:
    ● extract — WOULD RUN (no condition)

  Level 1:
    ● transform — WOULD RUN if job("extract").last_success → true
    ● load — WOULD RUN (no condition, depends on: extract)

  3 job(s) would run, 0 would skip
```

---

## 4. Running the Daemon

### Foreground (Development)

```bash
kairos start --config kairos.toml --log-level debug
```

Press `Ctrl+C` for graceful shutdown.

### Signals

| Signal | Effect |
|--------|--------|
| `SIGTERM` / `SIGINT` / `Ctrl+C` | Graceful shutdown (drain in-flight runs) |
| `SIGHUP` | Configuration reload (parse-then-swap, atomic) |

### Single-Instance Enforcement

A lock file (`kairos.lock` in the data directory) ensures only one daemon runs:

```bash
$ kairos start &
# Kairos started

$ kairos start
# Error: Another Kairos instance is already running (lock: …/kairos.lock)
```

### Daemon Subsystems

On startup, the daemon initializes and runs:

1. **Scheduler** — timer heap for cron/interval/date triggers
2. **Watch Engine** — filesystem monitoring with native + sample backends
3. **Pipeline** — consumes TriggerEvents, plans DAGs, dispatches work
4. **Runner Pool** — executes process steps with timeout enforcement
5. **DB Writer** — async batched SQLite persistence
6. **Metrics** — periodic snapshots to SQLite (default: every 60s)

---

## 5. Workflow Definitions

See [AUTHORING.md](AUTHORING.md) for the complete authoring guide with schema reference.

Quick example:

```yaml
# workflows/data-pipeline.yaml
name: data-pipeline
triggers:
  - type: cron
    spec: "0 3 * * *"
jobs:
  - name: extract
    steps:
      - name: fetch
        command: curl -o /tmp/data.csv https://api.example.com/data
  - name: transform
    needs: [extract]
    condition: 'job("extract").last_success'
    steps:
      - name: etl
        command: python3 etl.py /tmp/data.csv
```

---

## 6. Standalone Jobs

```yaml
# workflows/backup.yaml
name: daily-backup
standalone: true
condition: 'job("health-check").last_success'
triggers:
  - type: cron
    spec: "0 2 * * *"
steps:
  - name: dump
    command: pg_dump -Fc mydb > /tmp/backup.dump
```

---

## 7. Filesystem Monitoring

See [AUTHORING.md § Watch Group Definitions](AUTHORING.md#7-watch-group-definitions) for the complete schema.

Three watch modes:

| Mode | Mechanism | Best For |
|------|-----------|----------|
| `native` | inotify / FSEvents / RDCW | Real-time alerting |
| `sample` | Periodic scan + diff | Reliability on network drives or where native isn't available |
| `hybrid` | Native + periodic verification | General use (recommended default) |

Quick test:

```bash
kairos watches list
kairos watches scan-once
kairos events list -n 20
```

---

## 8. Database Management

### Schema

SQLite in WAL mode with these tables: `runs`, `run_jobs`, `run_steps`, `log_chunks`, `watch_samples`, `watch_events`, `trigger_state`, `metrics_snapshots`, `config_snapshots`, `schema_version`.

### Initialization

```bash
kairos init-db
kairos init-db --db-path /var/lib/kairos/production.db
```

Migrations are embedded in the binary and run automatically. Re-running `init-db` is always safe.

### Retention

| Data | Default Retention | Config Key |
|------|-------------------|------------|
| Runs, jobs, steps, logs | 90 days | `kairos.persistence.retention_days` |
| Watch samples | 10 epochs per group | (hardcoded) |
| Watch events | Same as runs | `kairos.persistence.retention_days` |
| Metrics snapshots | 7 days | `kairos.telemetry.metrics_retention_days` |

The daemon prunes automatically. To manually prune:

```bash
kairos prune --older-than 30
```

---

## 9. MCP Agent Interface

14 tools over stdio JSON-RPC 2.0 for AI agent integration:

```bash
kairos mcp --config kairos.toml
# stdin/stdout become JSON-RPC 2.0 transport
# Logging goes to stderr
```

### Available Tools

| Tool | Description |
|------|-------------|
| `kairos.listWorkflows` | List all configured workflows |
| `kairos.getWorkflow` | Get workflow details + DAG |
| `kairos.runWorkflow` | Trigger a workflow (optional follow) |
| `kairos.explainPlan` | Dry-run: what would happen? |
| `kairos.listJobs` | List standalone jobs |
| `kairos.runJob` | Trigger a standalone job |
| `kairos.queryRuns` | Query run history |
| `kairos.getRunDetail` | Full run detail with jobs/steps |
| `kairos.getRunLogs` | Get log chunks for a run |
| `kairos.getStepOutput` | Get stdout/stderr for a step |
| `kairos.listWatchGroups` | List watch groups |
| `kairos.getEvents` | Get watch events |
| `kairos.reloadConfig` | Reload configuration |
| `kairos.getMetrics` | Get metrics snapshot |
| `kairos.watchScanOnce` | One-shot filesystem scan |

### Example Agent Interaction

```json
{"jsonrpc":"2.0","method":"tools/call","params":{"name":"kairos.runWorkflow","arguments":{"workflow_id":"data-pipeline","follow":true}},"id":1}
```

When `follow: true`, the MCP server streams log chunks as JSON-RPC notifications (`notifications/log_chunk`) and sends `notifications/run_complete` when done.

---

## 10. Scripted Output (JSON Mode)

Every command supports `--json` for machine consumption:

```bash
kairos --json version
kairos --json runs list -n 1 | jq -r '.[0].run_id'
kairos --json status --history | jq '.metrics_history'
kairos --json workflows show data-pipeline | jq '.dag_levels'
```

When `--json` is active, human messages go to stderr and structured data goes to stdout.

Auto-detection: when stdout is not a TTY, Kairos automatically uses JSON format for logging and disables colors.

---

## 11. Logging and Observability

### Log Formats

| Mode | When Used | Format |
|------|-----------|--------|
| JSON | Daemon, piped output, `--json` | `{"ts":"…","level":"info","msg":"…","run_id":"…"}` |
| Text | Interactive terminal | `[2026-03-08 14:30:05] [INFO] [kairos] message` |
| Auto | Default | JSON when not TTY, text when TTY |

### Log Levels

From most to least verbose: `trace`, `debug`, `info`, `warn`, `error`, `critical`.

### File Logging

```toml
[kairos.logging]
file             = "/var/log/kairos/kairos.log"
max_file_size_mb = 100
max_files        = 5
```

### Secret Masking

All output paths (logs, DB, MCP) automatically redact resolved secret values with `***REDACTED***`.

### Metrics

Kairos tracks these metrics internally:

| Metric | Type | Description |
|--------|------|-------------|
| `kairos_runs_total` | counter | Total completed runs |
| `kairos_runs_active` | gauge | Currently in-flight runs |
| `kairos_runs_failed` | counter | Failed runs |
| `kairos_uptime_seconds` | gauge | Daemon uptime |
| `kairos_scheduler_fires_total` | counter | Triggers fired |
| `kairos_watch_scan_duration_ms` | gauge | Last scan duration |
| `kairos_db_write_batch_ms` | gauge | DB write batch latency |

Metrics are persisted to SQLite every 60s (configurable) and pruned after 7 days.

### OpenTelemetry (Optional)

Build with `KAIROS_OTEL=ON` for distributed tracing. Spans are emitted for run/job/step/scan operations via OTLP export.

---

## 12. Service Integration

### systemd (Linux)

```bash
# Installed automatically by DEB/RPM package
sudo systemctl enable kairos
sudo systemctl start kairos
sudo systemctl status kairos
sudo journalctl -u kairos -f
```

Unit file features: `Type=notify`, `WatchdogSec=60`, security hardening (`NoNewPrivileges`, `ProtectSystem`).

### launchd (macOS)

```bash
# Installed by Homebrew or manual copy
cp deploy/launchd/com.kairos.daemon.plist ~/Library/LaunchDaemons/
launchctl load ~/Library/LaunchDaemons/com.kairos.daemon.plist
```

### Windows Service

```powershell
# Install as Windows Service
kairos --service install

# Start/stop
net start kairos
net stop kairos

# Uninstall
kairos --service uninstall
```

---

## 13. Legacy Migration

### Migrate AVScheduler

```bash
# Config: TOML → Kairos TOML + workflow YAML
kairos migrate-config --source avscheduler \
    --source-config /path/to/config.toml \
    --output-dir ~/.config/kairos/

# Database: execution logs → Kairos runs table
kairos migrate-db --source avscheduler \
    --source-db /path/to/jobs.db \
    --target-db ~/.local/share/kairos/kairos.db
```

### Migrate EventWatcher

```bash
kairos migrate-config --source eventwatcher \
    --source-config /path/to/config.toml \
    --source-watches /path/to/watch_groups.yaml \
    --output-dir ~/.config/kairos/

kairos migrate-db --source eventwatcher \
    --source-db /path/to/ew.db \
    --target-db ~/.local/share/kairos/kairos.db
```

### Migrate LocalFlow

```bash
kairos migrate-config --source localflow \
    --source-workflows /path/to/workflows/ \
    --output-dir ~/.config/kairos/
# LocalFlow has no database — migrate-db is a no-op.
```

### Migration Safety

- **Read-only** on source data: never modifies legacy configs or databases.
- **Duplicate-safe**: uses `INSERT OR IGNORE` for database migration.
- **Auditable**: migrated records are tagged with `migrated_from` metadata.
- **Dry-run mode**: `--dry-run` shows what would be generated without writing files.
- **Warnings**: expressions using Python `eval()` patterns are flagged for manual review.

### Command Equivalents

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

---

## 14. Environment Variables

### Kairos-Specific

| Variable | Purpose |
|----------|---------|
| `KAIROS_CONFIG_FILE` | Override config file path |
| `KAIROS_*` | Override any `kairos.*` config key (see §2) |
| `NO_COLOR` | Disable ANSI color output ([no-color.org](https://no-color.org/)) |

### Injected Into Jobs

These variables are available inside every running job/step:

| Variable | Value |
|----------|-------|
| `KAIROS_RUN_ID` | UUID of the current run |
| `KAIROS_JOB_ID` | ID of the current job |
| `KAIROS_STEP_ID` | ID of the current step |
| `KAIROS_WORKFLOW_ID` | ID of the parent workflow (empty for standalone jobs) |
| `KAIROS_TRIGGER_TYPE` | `schedule_tick`, `file_event`, `file_diff`, `manual_run` |
| `KAIROS_CORRELATION_ID` | Trace correlation ID |
| `KAIROS_DATA_DIR` | Configured data directory path |
| `KAIROS_DB_PATH` | Path to the SQLite database |

---

## 15. Build and Test Scripts

### Build

```bash
# Linux / macOS
./scripts/build.sh                       # Default: debug build
./scripts/build.sh --release             # Release build
./scripts/build.sh --profile san         # Debug + ASan + UBSan
./scripts/build.sh --profile full        # All features, release
./scripts/build.sh --clean --release     # Clean rebuild
./scripts/build.sh --compiler clang      # Use Clang
./scripts/build.sh --package             # Build + DEB + RPM + ZIP
./scripts/build.sh --help                # Full option reference
```

```powershell
# Windows
.\scripts\build.ps1                      # Default: debug build
.\scripts\build.ps1 -Release             # Release build
.\scripts\build.ps1 -Profile san         # Debug + ASan
.\scripts\build.ps1 -Clean -Release      # Clean rebuild
```

### Test

```bash
# Linux / macOS
./scripts/test.sh                        # Run all tests (~1100 tests)
./scripts/test.sh --filter "Kel"         # Filter by name
./scripts/test.sh --list                 # List without running
./scripts/test.sh --verbose              # Full output
./scripts/test.sh --repeat 5             # Stress test
./scripts/test.sh --rerun-failed         # Rerun failed
```

```powershell
# Windows
.\scripts\test.ps1
.\scripts\test.ps1 -Filter "Kel"
.\scripts\test.ps1 -List
```

See [BUILD.md](BUILD.md) for the complete build guide.

---

## 16. Examples

### Quick Development Setup

```bash
# 1. Build
./scripts/build.sh

# 2. Create workspace
mkdir -p /tmp/kairos-dev/{workflows,watch_groups,data}

cat > /tmp/kairos-dev/kairos.toml << 'EOF'
[kairos]
data_dir = "/tmp/kairos-dev/data"
db_path  = "/tmp/kairos-dev/data/kairos.db"
workflows_dir = "workflows"
watch_groups_dir = "watch_groups"
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

# 3. Initialize and run
./build/debug/kairos init-db --config /tmp/kairos-dev/kairos.toml
./build/debug/kairos workflows list --config /tmp/kairos-dev/kairos.toml
./build/debug/kairos workflows run hello-world --follow --config /tmp/kairos-dev/kairos.toml
```

### Inspecting a Failed Run

```bash
kairos runs list --status FAILURE -n 5
kairos runs show run-abc123def456
kairos logs run-abc123def456
```

### Scripting with JSON

```bash
#!/usr/bin/env bash
set -euo pipefail

# Run workflow and capture run ID
RUN_ID=$(kairos --json workflows run data-pipeline | jq -r '.run_id')
echo "Started run: $RUN_ID"

# Wait and check status
sleep 5
STATUS=$(kairos --json runs show "$RUN_ID" | jq -r '.status')
if [[ "$STATUS" != "SUCCESS" ]]; then
    echo "Run failed with status: $STATUS"
    kairos logs "$RUN_ID"
    exit 1
fi
echo "Run succeeded"
```

### Docker Quick Test

```bash
docker run --rm -v "$(pwd):/src" -w /src ubuntu:24.04 bash -c '
  apt-get update -qq && apt-get install -y -qq cmake g++ libsqlite3-dev git
  cmake -B build -DCMAKE_BUILD_TYPE=Debug -DKAIROS_BUILD_TESTS=ON
  cmake --build build -j$(nproc)
  cd build && ctest --output-on-failure
'
```

---

## 17. Troubleshooting

### "Another Kairos instance is already running"

```bash
# Check if kairos is actually running
pgrep -a kairos

# If not running, remove the stale lock
rm ~/.local/share/kairos/kairos.lock
```

### "Missing mandatory configuration keys"

Set `kairos.data_dir` and `kairos.db_path` in your TOML file or via environment:

```bash
export KAIROS_DATA__DIR=/tmp/kairos
export KAIROS_DB__PATH=/tmp/kairos/kairos.db
kairos start
```

### "Config parse error"

Validate your TOML syntax:

```bash
kairos config validate --config ./kairos.toml
```

### "Run 'kairos init-db' first"

The database doesn't exist yet:

```bash
kairos init-db
```

### "Workflow not found" / "Job not found"

```bash
kairos workflows list
kairos jobs list
```

### No Colors in Terminal

Kairos respects the `NO_COLOR` environment variable. Colors are also disabled when piping:

```bash
unset NO_COLOR
kairos runs list          # colored
kairos runs list | cat    # no colors (piped)
```

### Build Fails Fetching Dependencies

Pre-clone dependencies if behind a firewall:

```bash
git clone https://github.com/araray/confy-cpp.git /tmp/deps/confy-cpp
cmake -B build -DFETCHCONTENT_SOURCE_DIR_CONFY-CPP=/tmp/deps/confy-cpp
```

### "database is locked"

Multiple processes sharing the same database. Check for stale test processes:

```bash
pkill -f test_persist
```

---

## References

- [BUILD.md](BUILD.md) — Complete build system guide
- [AUTHORING.md](AUTHORING.md) — Workflow, watch group, and KEL authoring guide
- [`kairos.toml.example`](deploy/kairos.toml.example) — Full configuration reference
- [Kairos Design Specification](docs/) — Parts 1–7
- [confy-cpp](https://github.com/araray/confy-cpp) — Configuration library
- [Model Context Protocol (MCP)](https://modelcontextprotocol.io/)
- [NO_COLOR Convention](https://no-color.org/)
