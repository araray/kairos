# Kairos Usage Guide

> *Unified orchestration for scheduling, workflows, and filesystem monitoring.*

This guide covers installation, configuration, CLI usage, common workflows, and troubleshooting.

---

## Table of Contents

1. [Installation](#1-installation)
2. [Configuration](#2-configuration)
3. [CLI Reference](#3-cli-reference)
4. [Running the Daemon](#4-running-the-daemon)
5. [Database Management](#5-database-management)
6. [Scripted Output (JSON Mode)](#6-scripted-output-json-mode)
7. [Logging](#7-logging)
8. [Environment Variables](#8-environment-variables)
9. [Build Scripts](#9-build-scripts)
10. [Examples](#10-examples)
11. [Troubleshooting](#11-troubleshooting)

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

Kairos uses [confy-cpp](https://github.com/araray/confy-cpp) for layered configuration.  Sources are merged in this order (later overrides earlier):

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

Everything else has sensible defaults.  See `deploy/kairos.toml.example` for the complete reference with all 47 keys documented.

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

### Environment Variable Overrides

All config keys can be overridden via environment variables with the `KAIROS_` prefix.  Dots become underscores; underscores in key names use double underscore:

```bash
# kairos.logging.level = "debug"
export KAIROS_LOGGING_LEVEL=debug

# kairos.runners.worker_pool_size = 8
export KAIROS_RUNNERS_WORKER__POOL__SIZE=8

# kairos.http.listen_port = 9000
export KAIROS_HTTP_LISTEN__PORT=9000
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

### Subcommands (Phase 1)

**`kairos version`** — print version information:

```bash
$ kairos version
Kairos v2.0.0

$ kairos --json version
{"version":"2.0.0"}
```

**`kairos init-db`** — initialize the SQLite database:

```bash
# Use path from config
$ kairos init-db
Database initialized: /home/user/.local/share/kairos/kairos.db (schema v1)

# Override path
$ kairos init-db --db-path /tmp/test.db
Database initialized: /tmp/test.db (schema v1)
```

**`kairos start`** — start the daemon:

```bash
# Start with default config
$ kairos start

# Start with explicit config
$ kairos start --config /etc/kairos/kairos.toml

# Start with debug logging
$ kairos start --log-level debug

# Stop: send SIGTERM or press Ctrl+C
$ kill -TERM $(cat ~/.local/share/kairos/kairos.lock)
```

### Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Generic error |
| 2 | Configuration error (missing/invalid config, mandatory key missing) |
| 126 | Command not executable (permissions) |
| 127 | Command not found |
| 200 | Timeout exceeded (soft kill — SIGTERM) |
| 201 | Timeout exceeded (hard kill — SIGKILL) |
| 202 | Runner configuration error |
| 203 | Dependency failure (upstream job failed) |
| 204 | Condition evaluation error (KEL) |

### Future Subcommands (Phase 2–5)

These are registered but disabled in Phase 1.  They will be implemented in later phases:

```
workflows    Manage workflow definitions
jobs         Manage standalone jobs
runs         Query run history
logs         View/follow execution logs
events       View filesystem watch events
explain      Show execution plan without running
mcp          Start MCP stdio server (agent interface)
status       Show daemon status
reload       Reload configuration (hot-reload)
stop         Stop the running daemon
prune        Prune old records from the database
```

---

## 4. Running the Daemon

### Foreground (Development)

```bash
kairos start --config kairos.toml --log-level debug
```

Structured JSON logs go to stdout.  Press `Ctrl+C` to stop.

### Signals

| Signal | Effect |
|--------|--------|
| `SIGTERM` / `SIGINT` / `Ctrl+C` | Graceful shutdown |
| `SIGHUP` | Configuration reload (Phase 3+) |

### Single-Instance Enforcement

Kairos uses a lock file (`kairos.lock` in the data directory) to ensure only one daemon instance runs at a time:

```bash
$ kairos start &
# Kairos v2.0.0 started

$ kairos start
# Error: Another Kairos instance is already running (lock: …/kairos.lock)
```

The lock is automatically released on clean shutdown.

---

## 5. Database Management

### Schema

The database uses SQLite in WAL mode with 10 tables:

- `runs` — workflow and standalone job executions
- `job_runs` — per-job results within a workflow run
- `step_runs` — per-step results within a job run
- `log_chunks` — captured stdout/stderr from executions
- `watch_samples` — filesystem snapshot samples
- `watch_events` — detected file change events
- `trigger_history` — record of all trigger fires
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

Migrations are forward-only, embedded in the binary, and run automatically on daemon startup or `init-db`.  Re-running `init-db` on an existing database is safe (idempotent).

---

## 6. Scripted Output (JSON Mode)

Every command supports `--json` for machine consumption:

```bash
# Version as JSON
kairos --json version
# {"version":"2.0.0"}
```

When `--json` is active, human messages go to stderr and structured data goes to stdout, so you can safely pipe:

```bash
# Parse version with jq
kairos --json version | jq -r '.version'
# 2.0.0
```

---

## 7. Logging

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

---

## 8. Environment Variables

### Kairos-Specific

| Variable | Purpose |
|----------|---------|
| `KAIROS_CONFIG_FILE` | Override config file path |
| `KAIROS_*` | Override any `kairos.*` config key (see §2) |

### Injected Into Jobs (Phase 2+)

These variables are available inside every running job/step:

| Variable | Value |
|----------|-------|
| `KAIROS_RUN_ID` | UUID of the current run |
| `KAIROS_JOB_ID` | ID of the current job |
| `KAIROS_STEP_ID` | ID of the current step |
| `KAIROS_WORKFLOW_ID` | ID of the parent workflow |
| `KAIROS_TRIGGER_TYPE` | `schedule_tick`, `file_event`, `file_diff`, `manual_run` |
| `KAIROS_CORRELATION_ID` | Trace correlation ID |
| `KAIROS_DATA_DIR` | Configured data directory path |
| `KAIROS_DB_PATH` | Path to the SQLite database |

---

## 9. Build Scripts

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
./scripts/test.sh                        # Run all tests
./scripts/test.sh --filter "Sha256"      # Filter by name
./scripts/test.sh --list                 # List without running
./scripts/test.sh --verbose              # Full output
./scripts/test.sh --repeat 5             # Stress test
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

## 10. Examples

### Quick Development Setup

```bash
# 1. Build
./scripts/build.sh

# 2. Create minimal config
mkdir -p /tmp/kairos-dev
cat > /tmp/kairos-dev/kairos.toml << 'EOF'
[kairos]
data_dir = "/tmp/kairos-dev/data"
db_path  = "/tmp/kairos-dev/data/kairos.db"

[kairos.logging]
level = "debug"
format = "text"
EOF

# 3. Initialize database
./build/debug/kairos init-db --config /tmp/kairos-dev/kairos.toml

# 4. Start daemon
./build/debug/kairos start --config /tmp/kairos-dev/kairos.toml
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

## 11. Troubleshooting

### "Another Kairos instance is already running"

The lock file was not cleaned up (e.g., after a crash):

```bash
# Check if kairos is actually running
pgrep -a kairos

# If not running, remove the stale lock
rm ~/.local/share/kairos/kairos.lock
```

### "Missing mandatory configuration keys"

The config file is missing `kairos.data_dir` or `kairos.db_path`.  Either set them in your TOML file or via environment:

```bash
export KAIROS_DATA__DIR=/tmp/kairos
export KAIROS_DB__PATH=/tmp/kairos/kairos.db
kairos start
```

### "Config parse error"

Your `kairos.toml` has syntax issues.  Validate it:

```bash
# Check TOML syntax (requires a TOML validator)
python3 -c "import tomllib; tomllib.load(open('kairos.toml','rb'))"
```

### Build Fails Fetching Dependencies

CMake FetchContent downloads dependencies from GitHub.  If behind a firewall:

```bash
# Pre-clone dependencies
git clone https://github.com/araray/confy-cpp.git /tmp/deps/confy-cpp
cmake -B build -DFETCHCONTENT_SOURCE_DIR_CONFY-CPP=/tmp/deps/confy-cpp
```

### Tests Fail with "database is locked"

Multiple test processes are sharing the same database file.  Tests use in-memory databases or temporary directories by default — if you see this, check for stale test processes:

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
