# Kairos — Unified Orchestration Daemon

> *"Καιρός (Kairos) — the ancient Greek concept of the opportune, decisive moment."*

Kairos is a unified, local-first orchestration daemon built in C++20. It consolidates workflow execution (LocalFlow), time-based scheduling (AVScheduler), and filesystem monitoring (EventWatcher) into a single, high-performance system.

## Status: Phase 1 — Skeleton

Phase 1 delivers the foundational infrastructure:

| Deliverable | Status |
|------------|--------|
| CMake build system with FetchContent, presets, feature flags | ✅ |
| confy-cpp integration (ConfigStore, defaults, validation) | ✅ |
| Platform abstraction (paths, terminal, signals, threading, instance lock) | ✅ |
| SQLite schema v1 with migration system (10 tables, 14 indexes) | ✅ |
| Structured logging (spdlog, JSON + text formatters, async) | ✅ |
| Content-hash ID generation (SHA-256, UUID v4) | ✅ |
| CLI skeleton (`kairos version`, `start`, `init-db`) | ✅ |
| GitHub Actions CI (4-platform matrix) | ✅ |
| Unit tests (90 Kairos-specific + 203 confy-cpp = 293 total) | ✅ |

## Quick Start

### Build

```bash
# Linux/macOS
sudo apt-get install -y cmake g++ libsqlite3-dev   # Ubuntu
# or: brew install cmake sqlite3                     # macOS

cmake -B build -DCMAKE_BUILD_TYPE=Debug -DKAIROS_BUILD_TESTS=ON
cmake --build build -j$(nproc)
```

### Test

```bash
cd build && ctest --output-on-failure
```

### Run

```bash
# Print version
./build/kairos version
./build/kairos version --json

# Initialize database
./build/kairos init-db --db-path ~/.local/share/kairos/kairos.db

# Start daemon (stops on Ctrl+C / SIGTERM)
./build/kairos start --config deploy/kairos.toml.example

# With custom log level
./build/kairos start --config deploy/kairos.toml.example --log-level debug
```

### CLI Reference (Phase 1)

```
kairos [OPTIONS] SUBCOMMAND

Options:
  -c, --config TEXT       Path to kairos.toml config file
  --log-level TEXT [info] Log level (trace|debug|info|warn|error|critical)
  --json                  Force JSON output (for scripting)

Subcommands:
  version                 Print version information
  start                   Start the Kairos daemon
  init-db                 Initialize the SQLite database
```

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         CLI (CLI11)                              │
├─────────────────────────────────────────────────────────────────┤
│  Config (confy-cpp)  │  Logging (spdlog)  │  Platform Layer    │
├─────────────────────────────────────────────────────────────────┤
│                     Core Library                                │
│  ┌──────────┐ ┌──────────────┐ ┌──────────────┐               │
│  │ ID Gen   │ │ Exit Codes   │ │ Version      │               │
│  │ (SHA-256)│ │ (200-204)    │ │              │               │
│  └──────────┘ └──────────────┘ └──────────────┘               │
├─────────────────────────────────────────────────────────────────┤
│           Persistence (SQLite, WAL mode, migrations)            │
└─────────────────────────────────────────────────────────────────┘
```

## Configuration

Kairos uses [confy-cpp](https://github.com/araray/confy-cpp) for layered configuration with this precedence (low → high):

1. **Hardcoded defaults** (47 keys, all documented)
2. **Config file** (`kairos.toml`)
3. **`.env` file**
4. **Environment variables** (`KAIROS_` prefix)
5. **CLI overrides**

See `deploy/kairos.toml.example` for the complete configuration reference.

## Project Structure

```
kairos/
├── CMakeLists.txt              # Build system
├── CMakePresets.json           # Build presets
├── cmake/                      # CMake modules
├── include/kairos/             # Public headers
│   ├── cli/                    #   CLI application
│   ├── config/                 #   Configuration store
│   ├── core/                   #   Version, IDs, exit codes
│   ├── daemon/                 #   Daemon lifecycle
│   ├── observability/          #   Logging, JSON formatter
│   ├── persist/                #   SQLite, migrations
│   └── platform/               #   Cross-platform abstractions
├── src/                        # Implementation
├── tests/                      # Test suite (Google Test)
│   ├── helpers/                #   Test utilities
│   └── unit/                   #   Unit tests by module
├── deploy/                     # Deployment files
│   └── kairos.toml.example     #   Example configuration
├── .github/workflows/          # CI pipeline
└── docs/                       # Documentation
```

## Dependencies

| Library | Version | License | Purpose |
|---------|---------|---------|---------|
| confy-cpp | main | MIT | Configuration management |
| SQLiteCpp | 3.3.1 | MIT | SQLite RAII wrapper |
| spdlog | 1.13.0 | MIT | Structured logging |
| nlohmann/json | 3.11.3 | MIT | JSON handling |
| yaml-cpp | 0.8.0 | MIT | YAML parsing |
| CLI11 | 2.4.1 | BSD-3 | CLI argument parsing |
| croncpp | 2023.03.30 | MIT | Cron expression parsing |
| Google Test | 1.14.0 | BSD-3 | Testing (dev only) |

All dependencies are fetched automatically via CMake FetchContent.

## License

MIT
