<p align="center">
  <img src="docs/kairos-logo.svg" alt="Kairos" width="200"/>
</p>

<h1 align="center">Kairos</h1>

<p align="center">
  <strong>Unified local orchestration daemon — scheduling, workflows, and filesystem monitoring in a single C++20 binary.</strong>
</p>

<p align="center">
  <a href="#quick-start">Quick Start</a> •
  <a href="#features">Features</a> •
  <a href="#architecture">Architecture</a> •
  <a href="#installation">Installation</a> •
  <a href="#documentation">Documentation</a> •
  <a href="#license">License</a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue?logo=cplusplus" alt="C++20"/>
  <img src="https://img.shields.io/badge/platforms-Linux%20|%20macOS%20|%20Windows-green" alt="Platforms"/>
  <img src="https://img.shields.io/badge/license-MIT-blue" alt="License"/>
  <img src="https://img.shields.io/badge/tests-1100+-brightgreen" alt="Tests"/>
</p>

---

> *"Καιρός (Kairos) — the ancient Greek concept of the opportune, decisive moment."*

Kairos replaces three legacy Python tools — **LocalFlow** (workflow/DAG execution), **AVScheduler** (job scheduling), and **EventWatcher** (filesystem monitoring) — with a single, production-grade C++20 daemon. It runs on developer machines like a production service: deterministic, observable, secure, and agent-ready.

---

## Features

### Unified Pipeline

Every trigger — schedule tick, filesystem change, manual run, or MCP agent call — flows through the same pipeline:

```
Trigger → Evaluate Conditions (KEL) → Plan (DAG) → Execute → Persist → Emit Observability
```

### Scheduling Engine

- **Cron triggers** — standard 5-field cron expressions in wall-clock time (`system_clock`).
- **Interval triggers** — drift-immune periodic execution via `steady_clock`.
- **Date triggers** — fire-once at a specific wall-clock time.
- **Misfire detection** — coalesce, skip, or run-all-missed after daemon restart.
- **Max instances** — prevent concurrent overlapping runs of the same target.

### Workflow & DAG Engine

- **YAML-defined workflows** — jobs with dependencies, conditions, and multi-step commands.
- **Topological DAG execution** — Kahn's algorithm with level-by-level parallelism.
- **Conditional execution** — KEL expressions gate jobs based on prior run history.
- **Standalone jobs** — simple scheduled tasks without the full workflow overhead.
- **Execution plans** — `kairos explain` shows exactly what would run and why.

### Filesystem Watch Engine

- **Three watch modes** — `native` (inotify/FSEvents/RDCW), `sample` (periodic diff), or `hybrid` (both).
- **KEL-powered rules** — flexible rule conditions with aggregation and previous-sample comparison.
- **Bounded scanning** — max depth, max files, time budget, exclude globs, symlink cycle detection.
- **Content hashing** — configurable `mtime+size` (fast) or `sha256` (precise) hash policy.

### KEL — Kairos Expression Language

A small, safe, deterministic expression language for all condition evaluation:

```
job("build").last_success and job("test").finished_within(30m)
aggregate(data, "*.log", "size", "sum") > 10 * 1024 * 1024 * 1024
event.type == "content_changed" and matches(event.path, ".*\\.conf$")
```

- **Not Turing-complete** — no loops, no recursion, guaranteed termination.
- **Sandboxed** — no I/O, no eval, no system calls. 30+ whitelisted functions only.
- **Bounded** — max AST depth, max eval time (100ms), max string/list size.

### Cross-Platform

- **Linux** — inotify, systemd service, DEB/RPM packages.
- **macOS** — FSEvents, launchd plist, Homebrew formula.
- **Windows** — ReadDirectoryChangesW, Windows Service (SCM), MSVC build.
- UTF-8/UTF-16 path handling, `std::filesystem` throughout.

### Agent-Ready (MCP)

14 JSON-RPC 2.0 tools over stdio transport for AI agent integration:

```
kairos.listWorkflows    kairos.runWorkflow     kairos.explainPlan
kairos.listJobs         kairos.runJob          kairos.getMetrics
kairos.queryRuns        kairos.getRunDetail    kairos.getRunLogs
kairos.getStepOutput    kairos.listWatchGroups kairos.getEvents
kairos.reloadConfig     kairos.watchScanOnce
```

### Observability

- **Structured logs** — JSON or text format, with `run_id`/`job_id`/`step_id` correlation.
- **Metrics** — counters, gauges, histograms. Periodic SQLite snapshots. Optional OpenTelemetry export.
- **Tracing** — spans for run/job/step/scan operations.
- **Secret masking** — all output paths automatically redact secret values.

### Persistence

- **SQLite in WAL mode** — concurrent reads, durable writes, queryable run history.
- **Retention policies** — automatic pruning of old runs, events, and metric snapshots.
- **Migration tool** — `kairos migrate-config` and `kairos migrate-db` from legacy tools.

### Security

- **No eval** — KEL replaces all `eval()` from legacy tools with a sandboxed parser + AST + evaluator.
- **Ansible Vault** — optional encrypted secret store (`${{ secrets.key }}` syntax).
- **Secret masking** — resolved values never appear in logs or persisted output.
- **Content-addressable IDs** — deterministic SHA-256 identifiers for all entities.

---

## Quick Start

### Build from Source

```bash
# Linux (Ubuntu/Debian)
sudo apt-get install -y cmake g++ libsqlite3-dev
git clone https://github.com/araray/kairos.git && cd kairos
./scripts/build.sh --release

# macOS
brew install cmake sqlite3
git clone https://github.com/araray/kairos.git && cd kairos
./scripts/build.sh --release

# Windows (PowerShell, requires VS Build Tools)
git clone https://github.com/araray/kairos.git; cd kairos
.\scripts\build.ps1 -Release
```

### Run

```bash
# Verify the build
./build/release/kairos version

# Create a minimal workspace
mkdir -p /tmp/kairos/{workflows,watch_groups,data}

cat > /tmp/kairos/kairos.toml << 'EOF'
[kairos]
data_dir      = "/tmp/kairos/data"
db_path       = "/tmp/kairos/data/kairos.db"
workflows_dir = "workflows"
[kairos.logging]
level = "info"
EOF

cat > /tmp/kairos/workflows/hello.yaml << 'EOF'
name: hello-world
jobs:
  - name: greet
    steps:
      - name: say-hello
        command: echo "Hello from Kairos!"
      - name: show-env
        command: env | grep KAIROS_
EOF

# Initialize database
./build/release/kairos init-db --config /tmp/kairos/kairos.toml

# Run the workflow
./build/release/kairos workflows run hello-world --follow --config /tmp/kairos/kairos.toml

# Start the daemon
./build/release/kairos start --config /tmp/kairos/kairos.toml
```

### Install

```bash
# System install
sudo cmake --install build/release

# Or via package
./scripts/build.sh --profile package    # generates DEB + RPM + ZIP
sudo dpkg -i build/release/kairos_*.deb # Debian/Ubuntu
sudo rpm -i build/release/kairos-*.rpm  # RHEL/Fedora

# Or Homebrew (macOS)
brew install --formula deploy/homebrew/kairos.rb

# Or Docker
docker build -t kairos -f deploy/docker/Dockerfile .
docker run -v /path/to/config:/etc/kairos kairos
```

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          INTERFACE LAYER                                 │
│                                                                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌───────────────────────┐  │
│  │ CLI      │  │ MCP      │  │ HTTP API │  │ Service Integration   │  │
│  │ (CLI11)  │  │ (stdio)  │  │ (opt.)   │  │ systemd/launchd/SCM  │  │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └──────────┬────────────┘  │
│       └──────────────┴─────────────┴───────────────────┘               │
├─────────────────────────────────────────────────────────────────────────┤
│                          DAEMON CORE                                    │
│                                                                         │
│  ┌────────────────────┐       ┌───────────────────────────────────────┐│
│  │   Trigger Bus       │──────│  Pipeline (DAG Planner + Dispatcher)  ││
│  │   (bounded queue)   │      │  • Topological sort (Kahn's)          ││
│  └──┬──────────┬───────┘      │  • Level-by-level parallel execution  ││
│     │          │              │  • KEL condition evaluation            ││
│     │          │              │  • Execution plan generation           ││
│     │          │              └──────────────────┬────────────────────┘│
│  ┌──┴────┐ ┌──┴──────────┐                      │                     │
│  │Sched- │ │Watch Engine │               ┌──────┴──────────┐          │
│  │uler   │ │             │               │ Runner Pool      │          │
│  │       │ │• inotify    │               │                  │          │
│  │• cron │ │• FSEvents   │               │• LocalShellRunner│          │
│  │• intv │ │• RDCW       │               │• DockerRunner    │          │
│  │• date │ │• sample+diff│               │• (Ansible opt.)  │          │
│  └───────┘ └─────────────┘               └─────────────────┘          │
├─────────────────────────────────────────────────────────────────────────┤
│                          ENGINE LAYER                                   │
│                                                                         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────────┐ │
│  │ KEL Evaluator│  │ Config Store │  │ Workflow Registry             │ │
│  │ parser → AST │  │ (confy-cpp)  │  │ (immutable, atomic reload)   │ │
│  │ → evaluator  │  │ layered conf │  │ workflows + jobs + triggers  │ │
│  └──────────────┘  └──────────────┘  └──────────────────────────────┘ │
├─────────────────────────────────────────────────────────────────────────┤
│                       INFRASTRUCTURE LAYER                              │
│                                                                         │
│  ┌──────────────┐  ┌──────────────┐  ┌────────────────┐  ┌──────────┐│
│  │ SQLite Store │  │ Logging      │  │ Metrics        │  │ Tracer   ││
│  │ (WAL mode)   │  │ (spdlog)     │  │ counters/gauge │  │ (OTel    ││
│  │ runs/steps/  │  │ JSON + text  │  │ periodic snap  │  │  opt.)   ││
│  │ events/state │  │ secret mask  │  │ retention prune│  │          ││
│  └──────────────┘  └──────────────┘  └────────────────┘  └──────────┘│
└─────────────────────────────────────────────────────────────────────────┘
```

### Threading Model

Kairos uses `std::jthread` with cooperative cancellation (`std::stop_token`). No unbounded queues, no detached threads.

| Thread | Role |
|--------|------|
| Main / Scheduler | Timer heap evaluation, trigger emission |
| Pipeline | Consumes TriggerEvents, plans DAGs, dispatches work |
| Runner Pool (N threads) | Process execution with timeout enforcement |
| Watch Engine Coordinator | Manages watch groups, sample scheduling |
| Watch Native (per backend) | inotify/FSEvents/RDCW event loop |
| DB Writer | Async batched SQLite persistence |
| MCP Server (optional) | stdio JSON-RPC 2.0 server |

---

## CLI

```
kairos [OPTIONS] SUBCOMMAND

Options:
  -c, --config <path>    Path to kairos.toml
  --log-level <level>    Override log level
  --json                 Force JSON output for scripting

Subcommands:
  version                Print version information
  start                  Start the daemon
  status                 Show daemon status and statistics
  mcp                    Start MCP stdio server

  workflows list         List all workflows
  workflows show <id>    Show workflow detail with DAG
  workflows run <id>     Trigger a workflow run [-f/--follow]

  jobs list              List standalone jobs
  jobs show <id>         Show job detail
  jobs run <id>          Run a standalone job [-f/--follow]

  runs list              List run history [--status, --workflow, --since, -n]
  runs show <run_id>     Show run detail with jobs and steps

  logs <run_id>          View logs for a run [-f/--follow]

  watches list           List watch groups and status
  watches show <name>    Show watch group detail
  watches scan-once      Run a single scan cycle

  events list            List recent watch events [--watch-group, -n]

  config show            Show effective configuration
  config validate        Validate configuration files
  config reload          Reload config (SIGHUP to daemon)

  init-db                Initialize SQLite database

  migrate-config         Migrate legacy tool configuration
  migrate-db             Migrate legacy tool database
```

Every command supports `--json` for scripting:

```bash
kairos --json runs list -n 1 | jq -r '.[0].run_id'
```

---

## Configuration

Kairos uses [confy-cpp](https://github.com/araray/confy-cpp) for layered configuration with this precedence (lowest → highest):

1. **Hardcoded defaults** (47+ keys, all documented)
2. **Config file** (`kairos.toml`)
3. **`.env` file**
4. **Environment variables** (`KAIROS_` prefix)
5. **CLI overrides**

See [`deploy/kairos.toml.example`](deploy/kairos.toml.example) for the complete reference.

---

## Documentation

| Document | Description |
|----------|-------------|
| [`USAGE.md`](USAGE.md) | Complete CLI reference, examples, and operational guide |
| [`BUILD.md`](BUILD.md) | Build system, options, cross-platform instructions, and testing |
| [`AUTHORING.md`](AUTHORING.md) | Workflow, watch group, and KEL authoring guide |
| [`deploy/kairos.toml.example`](deploy/kairos.toml.example) | Annotated default configuration reference |
| [`docs/DESIGN_SPEC_PART1-7.md`](docs/) | Complete design and architecture specification |

---

## Project Structure

```
kairos/
├── CMakeLists.txt                  # Root build system
├── CMakePresets.json               # Build presets (dev, san, release, ci)
├── cmake/
│   ├── KairosOptions.cmake         # Feature flags
│   ├── KairosDependencies.cmake    # FetchContent dependencies
│   ├── KairosInstall.cmake         # Install targets
│   └── KairosCPack.cmake           # DEB/RPM/ZIP packaging
├── include/kairos/                 # Public headers
│   ├── cli/                        #   CLI application + table rendering
│   ├── config/                     #   ConfigStore + YAML loader
│   ├── core/                       #   Version, IDs, exit codes, bounded queue
│   ├── daemon/                     #   Daemon lifecycle + Win32 service
│   ├── engine/                     #   DAG, pipeline, scheduler, triggers
│   ├── exec/                       #   Process handles, runner pool, Docker
│   ├── http/                       #   HTTP server (optional)
│   ├── kel/                        #   Expression language (AST, evaluator)
│   ├── mcp/                        #   MCP handler + transport
│   ├── migration/                  #   Legacy tool migration
│   ├── observability/              #   Logging, metrics, tracing
│   ├── persist/                    #   SQLite database, writer, queries
│   ├── platform/                   #   Paths, signals, terminal, threading
│   ├── security/                   #   Secret store (Vault)
│   ├── testing/                    #   Fake clock, filesystem, process
│   └── watch/                      #   Watch engine, scanners, debounce
├── src/                            # Implementation sources
├── tests/                          # Test suite (Google Test)
│   ├── unit/                       #   ~1100 unit tests
│   ├── integration/                #   Config reload, HTTP, YAML wiring
│   ├── e2e/                        #   Daemon lifecycle end-to-end
│   └── helpers/                    #   Test utilities
├── deploy/
│   ├── kairos.toml.example         # Annotated configuration reference
│   ├── systemd/kairos.service      # systemd unit (Type=notify)
│   ├── launchd/com.kairos.daemon.plist
│   ├── deb/postinst, prerm         # Debian package scripts
│   ├── rpm/postinst.sh, prerm.sh   # RPM package scripts
│   ├── homebrew/kairos.rb          # Homebrew formula
│   └── docker/Dockerfile           # Multi-stage Docker image
├── scripts/
│   ├── build.sh / build.ps1       # Cross-platform build scripts
│   └── test.sh / test.ps1         # Cross-platform test scripts
└── .github/workflows/ci.yml       # CI: GCC+ASan, Clang, AppleClang, MSVC
```

---

## Dependencies

All dependencies are fetched automatically via CMake FetchContent.

| Library | Version | License | Purpose |
|---------|---------|---------|---------|
| [confy-cpp](https://github.com/araray/confy-cpp) | main | MIT | Layered configuration management |
| [SQLiteCpp](https://github.com/SRombauts/SQLiteCpp) | 3.3.1 | MIT | SQLite RAII wrapper |
| [spdlog](https://github.com/gabime/spdlog) | 1.13.0 | MIT | Structured logging |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | MIT | JSON handling |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | 0.8.0 | MIT | YAML parsing |
| [CLI11](https://github.com/CLIUtils/CLI11) | 2.4.1 | BSD-3 | CLI argument parsing |
| [croncpp](https://github.com/mariusbancila/croncpp) | 2023.03.30 | MIT | Cron expression parsing |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.15.3 | MIT | HTTP server (optional, `KAIROS_HTTP`) |
| [inja](https://github.com/pantor/inja) | 3.4.0 | MIT | HTML templating (optional, `KAIROS_HTTP`) |
| [Google Test](https://github.com/google/googletest) | 1.14.0 | BSD-3 | Testing (dev only) |

Optional dependencies (guarded by build flags):

| Flag | Extra Dependency | Purpose |
|------|-----------------|---------|
| `KAIROS_OTEL` | opentelemetry-cpp | Distributed tracing (OTLP export) |
| `KAIROS_VAULT` | OpenSSL | Ansible Vault decryption |
| `KAIROS_DOCKER` | (none — shells out to `docker` CLI) | Container execution |
| `KAIROS_TUI` | FTXUI | Terminal UI dashboard |

---

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Generic error |
| 2 | Configuration error |
| 4 | Not found (workflow/job/run) |
| 5 | Already running (daemon) |
| 6 | Not running (daemon) |
| 126 | Command not executable (permissions) |
| 127 | Command not found |
| 200 | Timeout exceeded (soft kill — SIGTERM) |
| 201 | Timeout exceeded (hard kill — SIGKILL) |
| 202 | Runner configuration error |
| 203 | Dependency failure (upstream job failed) |
| 204 | Condition evaluation error (KEL) |

---

## Legacy Migration

Kairos includes a built-in migration tool for transitioning from the three legacy Python tools:

```bash
# Migrate AVScheduler config and database
kairos migrate-config --source avscheduler --source-config config.toml --output-dir ./kairos/
kairos migrate-db --source avscheduler --source-db jobs.db --target-db kairos.db

# Migrate EventWatcher
kairos migrate-config --source eventwatcher --source-config config.toml \
    --source-watches watch_groups.yaml --output-dir ./kairos/

# Migrate LocalFlow workflows
kairos migrate-config --source localflow --source-workflows ./workflows/ --output-dir ./kairos/
```

---

## License

MIT

---

## Acknowledgments

Kairos consolidates patterns and learnings from:

- **LocalFlow** — YAML workflow definitions with DAG execution
- **AVScheduler** — Cron/interval scheduling with job dependency conditions
- **EventWatcher** — Filesystem monitoring with rule-based event detection
