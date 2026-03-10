# Building & Testing Kairos

> Complete guide to building Kairos from source, with all configuration options, cross-platform instructions, and testing procedures.

---

## Table of Contents

1. [Prerequisites](#1-prerequisites)
2. [Quick Build](#2-quick-build)
3. [Build Scripts (Recommended)](#3-build-scripts-recommended)
4. [Manual CMake Build](#4-manual-cmake-build)
5. [CMake Feature Flags](#5-cmake-feature-flags)
6. [Build Profiles](#6-build-profiles)
7. [CMake Presets](#7-cmake-presets)
8. [Sanitizer Builds](#8-sanitizer-builds)
9. [Testing](#9-testing)
10. [Packaging](#10-packaging)
11. [Docker Build](#11-docker-build)
12. [CI/CD Pipeline](#12-cicd-pipeline)
13. [IDE Integration](#13-ide-integration)
14. [Troubleshooting](#14-troubleshooting)

---

## 1. Prerequisites

### Linux (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install -y \
    cmake           \   # >= 3.21
    g++             \   # >= 12 (C++20 support)
    libsqlite3-dev  \   # SQLite development headers
    git                 # For FetchContent downloads

# Optional: for packaging
sudo apt-get install -y dpkg-dev rpm  # DEB + RPM
```

### Linux (Fedora/RHEL)

```bash
sudo dnf install cmake gcc-c++ sqlite-devel git
# Optional: rpm-build for RPM packaging
```

### macOS

```bash
# Xcode Command Line Tools (provides AppleClang)
xcode-select --install

# Dependencies via Homebrew
brew install cmake sqlite3
```

### Windows

1. Install [Visual Studio 2022](https://visualstudio.microsoft.com/) (Community is free) with the "Desktop development with C++" workload.
2. Alternatively, install [Build Tools for Visual Studio 2022](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022).
3. CMake is included with VS, or install standalone from [cmake.org](https://cmake.org/download/).
4. Git for Windows from [git-scm.com](https://git-scm.com/).

Open a **Developer PowerShell for VS 2022** or **x64 Native Tools Command Prompt** for builds.

### Optional Dependencies

| Feature | Dependency | Install |
|---------|-----------|---------|
| HTTP server (`KAIROS_HTTP`) | (bundled via FetchContent) | No extra install needed |
| OpenTelemetry (`KAIROS_OTEL`) | opentelemetry-cpp | System install or FetchContent |
| Vault secrets (`KAIROS_VAULT`) | OpenSSL >= 1.1 | `apt install libssl-dev` / `brew install openssl` |
| Docker runner (`KAIROS_DOCKER`) | Docker CLI on PATH | Install Docker Desktop or Docker Engine |
| TUI dashboard (`KAIROS_TUI`) | FTXUI | FetchContent (automatic) |

---

## 2. Quick Build

The fastest path from clone to running binary:

```bash
# Clone
git clone https://github.com/araray/kairos.git
cd kairos

# Build (debug, with tests)
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DKAIROS_BUILD_TESTS=ON
cmake --build build -j$(nproc)    # Linux/macOS
cmake --build build -j            # Windows (auto-detects cores)

# Test
cd build && ctest --output-on-failure

# Run
./build/kairos version
```

---

## 3. Build Scripts (Recommended)

The build scripts provide a richer experience with colored output, profiles, sanitizer support, and packaging.

### Linux / macOS (`scripts/build.sh`)

```bash
# Default debug build with tests
./scripts/build.sh

# Release build
./scripts/build.sh --release

# Clean rebuild
./scripts/build.sh --clean --release

# All features enabled
./scripts/build.sh --profile full

# Sanitizer build (ASan + UBSan)
./scripts/build.sh --profile san

# Use Clang instead of GCC
./scripts/build.sh --compiler clang

# Build + generate DEB package
./scripts/build.sh --release --http --package-deb

# Dry run (show CMake commands without executing)
./scripts/build.sh --release --http --dry-run

# Full option reference
./scripts/build.sh --help
```

### Windows (`scripts/build.ps1`)

```powershell
# Default debug build
.\scripts\build.ps1

# Release build
.\scripts\build.ps1 -Release

# Sanitizer build (ASan)
.\scripts\build.ps1 -Profile san

# Full-featured build
.\scripts\build.ps1 -Profile full

# Clean rebuild
.\scripts\build.ps1 -Clean -Release

# Help
.\scripts\build.ps1 -ShowHelp
```

### Build Script Options Reference

| Option | Description | Default |
|--------|-------------|---------|
| `--debug` / `--release` / `--relwithdebinfo` / `--minsizerel` | Build type | `Debug` |
| `--tests` / `--no-tests` | Build test suite | `ON` |
| `--http` | Enable HTTP server + Web UI | `OFF` |
| `--otel` | Enable OpenTelemetry tracing | `OFF` |
| `--vault` | Enable Ansible Vault support (requires OpenSSL) | `OFF` |
| `--docker` | Enable Docker runner | `OFF` |
| `--tui` | Enable TUI dashboard (FTXUI) | `OFF` |
| `--asan` / `--ubsan` / `--tsan` | Individual sanitizers | `OFF` |
| `--sanitizers` | ASan + UBSan together | `OFF` |
| `--compiler gcc` / `--compiler clang` | Select compiler | System default |
| `--clean` | Remove build directory before configuring | `false` |
| `--build-dir <path>` | Custom build directory | `build/<type>` |
| `--prefix <path>` | Install prefix | `/usr/local` |
| `--install` | Build then install | `false` |
| `--configure-only` | Configure without building | `false` |
| `--dry-run` | Print commands without executing | `false` |
| `--verbose` | Verbose build output | `false` |
| `-j <N>` / `--jobs <N>` | Parallel build jobs | Auto (`nproc`) |
| `--package` | Generate all packages (DEB + RPM + ZIP) | `false` |
| `--package-deb` / `--package-rpm` / `--package-zip` | Specific package | `false` |
| `-- <args...>` | Pass extra arguments to CMake | |

---

## 4. Manual CMake Build

For full control over the build process.

### Linux / macOS

```bash
# Configure
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DKAIROS_BUILD_TESTS=ON \
    -DKAIROS_HTTP=ON \
    -DKAIROS_VAULT=ON \
    -DKAIROS_DOCKER=ON

# Build
cmake --build build -j$(nproc)

# Install
sudo cmake --install build --prefix /usr/local
```

### Windows (MSVC)

```powershell
# Configure (Visual Studio generator)
cmake -B build -G "Visual Studio 17 2022" -A x64 `
    -DKAIROS_BUILD_TESTS=ON `
    -DKAIROS_HTTP=ON

# Build
cmake --build build --config Release -j

# Install
cmake --install build --config Release --prefix C:\kairos
```

### Windows (Ninja + MSVC)

```powershell
# From Developer PowerShell
cmake -B build -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DKAIROS_BUILD_TESTS=ON

cmake --build build -j
```

---

## 5. CMake Feature Flags

These are defined in `cmake/KairosOptions.cmake`:

| Flag | Description | Default | Extra Dependencies |
|------|-------------|---------|-------------------|
| `KAIROS_BUILD_TESTS` | Build test suite (Google Test) | `ON` | None (fetched) |
| `KAIROS_HTTP` | Build HTTP server and Web UI | `OFF` | cpp-httplib, inja (fetched) |
| `KAIROS_OTEL` | Build with OpenTelemetry tracing | `OFF` | opentelemetry-cpp (system) |
| `KAIROS_VAULT` | Build with Ansible Vault decryption | `OFF` | OpenSSL >= 1.1 (system) |
| `KAIROS_DOCKER` | Build with Docker runner support | `OFF` | None (shells to Docker CLI) |
| `KAIROS_TUI` | Build TUI dashboard (FTXUI) | `OFF` | FTXUI (fetched) |
| `KAIROS_USE_SYSTEM_DEPS` | Prefer system-installed libs over FetchContent | `OFF` | Varies |
| `KAIROS_EMBED_ASSETS` | Embed web assets into binary | `ON` | None |

Enable a flag by passing `-D<FLAG>=ON` to CMake:

```bash
cmake -B build -DKAIROS_HTTP=ON -DKAIROS_VAULT=ON -DKAIROS_DOCKER=ON
```

### Dependency Fetching

By default, all dependencies are fetched via CMake FetchContent at configure time. This requires internet access during the first build. Subsequent builds use cached downloads.

To use pre-cloned or system-installed dependencies:

```bash
# Point to a pre-cloned confy-cpp
cmake -B build -DFETCHCONTENT_SOURCE_DIR_CONFY-CPP=/path/to/confy-cpp

# Use system-installed libraries
cmake -B build -DKAIROS_USE_SYSTEM_DEPS=ON
```

### Conditional Compilation

Feature flags control which source files are compiled:

| Flag | Compiled Sources | Stub Sources (when OFF) |
|------|-----------------|------------------------|
| `KAIROS_HTTP` | `src/http/http_server.cpp` | (excluded entirely) |
| `KAIROS_VAULT` | `src/security/secret_store.cpp` | `src/security/secret_store_stub.cpp` |
| `KAIROS_DOCKER` | `src/exec/docker_process_handle.cpp` | `src/exec/docker_process_handle_stub.cpp` |
| `KAIROS_OTEL` | `src/observability/otel_tracer.cpp` | `src/observability/null_tracer.cpp` |

Platform-specific sources are automatically selected:

| Platform | Process Execution | Instance Lock | Paths | Signals | Terminal |
|----------|------------------|---------------|-------|---------|----------|
| POSIX | `process_handle_posix.cpp` | `instance_lock_posix.cpp` | `paths_posix.cpp` | `signals_posix.cpp` | `terminal_posix.cpp` |
| Windows | (future: `process_handle_win32.cpp`) | (future) | (future) | (future) | (future) |

---

## 6. Build Profiles

Profiles are predefined bundles of options. Use `--profile <name>` with the build scripts.

| Profile | Build Type | Tests | Features | Sanitizers |
|---------|-----------|-------|----------|-----------|
| `dev` | Debug | ON | None | None |
| `san` | Debug | ON | None | ASan + UBSan |
| `release` | Release | OFF | None | None |
| `ci` | Debug | ON | None | ASan + UBSan |
| `full` | Release | ON | HTTP, OTel, Vault, Docker | None |
| `full-debug` | Debug | ON | HTTP, OTel, Vault, Docker | None |
| `full-san-debug` | Debug | ON | HTTP, OTel, Vault, Docker | ASan + UBSan |
| `package` | Release | OFF | HTTP, Vault, Docker | None + DEB/RPM/ZIP |

---

## 7. CMake Presets

`CMakePresets.json` provides named presets for common configurations:

```bash
# List available presets
cmake --list-presets

# Use a preset
cmake --preset debug
cmake --build --preset debug

cmake --preset release
cmake --build --preset release
```

---

## 8. Sanitizer Builds

Sanitizers detect memory errors, undefined behavior, and data races at runtime.

### AddressSanitizer (ASan) — Memory Errors

```bash
./scripts/build.sh --asan
# or manually:
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
```

Detects: use-after-free, buffer overflows, stack overflows, memory leaks.

### UndefinedBehaviorSanitizer (UBSan)

```bash
./scripts/build.sh --ubsan
```

Detects: signed integer overflow, null pointer dereference, misaligned access, shift errors.

### ThreadSanitizer (TSan) — Data Races

```bash
./scripts/build.sh --tsan
```

Detects: data races between threads. Cannot be combined with ASan.

### Combined ASan + UBSan

```bash
./scripts/build.sh --sanitizers
# or:
./scripts/build.sh --profile san
```

This is the recommended sanitizer configuration for development.

### Windows Sanitizers

MSVC supports ASan via `/fsanitize=address`:

```powershell
.\scripts\build.ps1 -Profile san
```

UBSan and TSan are not available on MSVC. Use GCC or Clang for full sanitizer coverage.

---

## 9. Testing

### Test Suite Overview

Kairos includes over 1100 tests organized by module:

| Category | Location | Description |
|----------|----------|-------------|
| Unit tests | `tests/unit/` | Per-module tests (KEL, DAG, scheduler, config, persistence, etc.) |
| Integration tests | `tests/integration/` | Config reload, HTTP API, YAML wiring, watch integration |
| End-to-end tests | `tests/e2e/` | Full daemon lifecycle tests |

### Running Tests

#### Via Build Script (Recommended)

```bash
# Linux / macOS
./scripts/test.sh                       # Run all tests
./scripts/test.sh --filter "Kel"        # Filter by test name
./scripts/test.sh --list                # List all tests without running
./scripts/test.sh --verbose             # Full output
./scripts/test.sh --repeat 5            # Stress test (run 5 times)
./scripts/test.sh --rerun-failed        # Rerun only previously failed tests
./scripts/test.sh --stop-on-fail        # Stop at first failure
./scripts/test.sh --help                # Full option reference

# Windows
.\scripts\test.ps1
.\scripts\test.ps1 -Filter "Kel"
.\scripts\test.ps1 -List
.\scripts\test.ps1 -Verbose
```

#### Via CTest

```bash
cd build
ctest --output-on-failure                              # All tests
ctest --output-on-failure -R "kel"                     # Filter by regex
ctest --output-on-failure -j$(nproc)                   # Parallel execution
ctest --output-on-failure --timeout 120                # Per-test timeout
ctest --output-on-failure --rerun-failed               # Rerun failures
ctest --output-on-failure -L unit                      # Only unit tests
ctest --output-on-failure -L integration               # Only integration tests
```

#### Direct Google Test Binary Execution

```bash
# Run a specific test binary
./build/tests/unit/test_kel_evaluator

# With Google Test flags
./build/tests/unit/test_kel_evaluator --gtest_filter="*Duration*"
./build/tests/unit/test_kel_evaluator --gtest_list_tests
./build/tests/unit/test_kel_evaluator --gtest_repeat=10
./build/tests/unit/test_kel_evaluator --gtest_shuffle
```

### Test Organization

Test binaries are organized by module:

```
tests/
├── unit/
│   ├── cli/                    # CLI table rendering, completions
│   ├── config/                 # ConfigStore, validation, YAML loader
│   ├── core/                   # ID generator, exit codes
│   ├── daemon/                 # Command reader
│   ├── engine/                 # DAG, pipeline, scheduler, triggers, explain
│   ├── exec/                   # Process handle, runner pool, env builder, Docker
│   ├── http/                   # HTTP server
│   ├── kel/                    # Lexer, parser, evaluator (most comprehensive)
│   ├── mcp/                    # MCP handler, transport, log streaming
│   ├── migration/              # Legacy tool migration (17 tests)
│   ├── observability/          # JSON formatter, metrics, tracer
│   ├── persist/                # Schema migration, DB writer, queries, retention
│   ├── platform/               # Path normalization
│   ├── security/               # Secret store
│   ├── testing/                # Fake clock
│   └── watch/                  # Watch engine, scanner, debounce, hash, rules
├── integration/                # Cross-module integration tests
├── e2e/                        # Full daemon lifecycle tests (7 tests)
└── helpers/                    # Test utilities (test_helpers.hpp)
```

### Writing Tests

Tests use Google Test and follow these conventions:

- Each source file has a corresponding `_test.cpp` in the matching test directory.
- Tests use temporary directories (via `std::filesystem::temp_directory_path()`) to avoid filesystem conflicts.
- Time-sensitive tests use `kairos::testing::FakeClock` instead of real clocks.
- Database tests create in-memory SQLite databases (`:memory:`) or temporary files.

Example test structure:

```cpp
#include <gtest/gtest.h>
#include "kairos/kel/evaluator.hpp"

class KelEvaluatorTest : public ::testing::Test {
protected:
    kairos::kel::EvalContext ctx_;
    kairos::kel::EvalLimits limits_;

    void SetUp() override {
        ctx_ = kairos::kel::make_default_context();
    }
};

TEST_F(KelEvaluatorTest, BooleanLiteral) {
    auto result = kairos::kel::eval_expression("true", ctx_, limits_);
    EXPECT_TRUE(result.is_bool());
    EXPECT_TRUE(result.as_bool());
}
```

---

## 10. Packaging

### CPack (DEB / RPM / ZIP)

After building, generate packages using CPack:

```bash
# Build first
./scripts/build.sh --release --http --no-tests

# Generate all packages
cd build/release
cpack                               # All generators
cpack -G DEB                        # Debian package only
cpack -G RPM                        # RPM package only (needs rpmbuild)
cpack -G ZIP                        # ZIP archive only

# Or use the build script
./scripts/build.sh --profile package # Release + all packages
./scripts/build.sh --release --package-deb
```

Generated packages:

| Generator | Output | Contents |
|-----------|--------|----------|
| DEB | `kairos_0.1.0_amd64.deb` | Binary, headers, service files, completions, example config |
| RPM | `kairos-0.1.0-1.x86_64.rpm` | Same as DEB |
| ZIP | `kairos-0.1.0-Linux-x86_64.zip` | Binary + headers + example config |

### DEB Package Post-Install

The DEB `postinst` script automatically:

1. Creates a `kairos` system user.
2. Creates `/var/lib/kairos` and `/var/log/kairos` directories.
3. Enables the systemd service (`systemctl enable kairos`).
4. Installs the default configuration if none exists.

### Install Targets

`cmake --install` installs:

| Component | Destination |
|-----------|-------------|
| `kairos` binary | `${PREFIX}/bin/kairos` |
| Public headers | `${PREFIX}/include/kairos/` |
| systemd unit | `${PREFIX}/lib/systemd/system/kairos.service` |
| launchd plist | `${PREFIX}/Library/LaunchDaemons/com.kairos.daemon.plist` |
| Example config | `${PREFIX}/share/kairos/kairos.toml.example` |
| Shell completions | `${PREFIX}/share/bash-completion/completions/kairos` etc. |

### Homebrew (macOS)

```bash
brew install --formula deploy/homebrew/kairos.rb
```

The formula builds from source with CMake, installs completions, and creates the Homebrew service block.

---

## 11. Docker Build

### Multi-Stage Dockerfile

```bash
# Build the image
docker build -t kairos:latest -f deploy/docker/Dockerfile .

# Run with config mount
docker run -d \
    -v /path/to/config:/etc/kairos \
    -v kairos-data:/var/lib/kairos \
    --name kairos \
    kairos:latest

# Health check
docker exec kairos kairos status
```

The Dockerfile uses a two-stage build:

1. **Builder** (ubuntu:24.04): installs cmake, build-essential, libsqlite3-dev; builds release binary.
2. **Runtime** (ubuntu:24.04): copies binary, installs libsqlite3-0 and ca-certificates only. Runs as non-root.

### Docker Quick Test

Build and test in a container without installing anything locally:

```bash
docker run --rm -v "$(pwd):/src" -w /src ubuntu:24.04 bash -c '
  apt-get update -qq && apt-get install -y -qq cmake g++ libsqlite3-dev git
  cmake -B build -DCMAKE_BUILD_TYPE=Debug -DKAIROS_BUILD_TESTS=ON
  cmake --build build -j$(nproc)
  cd build && ctest --output-on-failure
'
```

---

## 12. CI/CD Pipeline

The GitHub Actions pipeline (`.github/workflows/ci.yml`) runs on every push and PR:

### Build Matrix

| Compiler | Platform | Sanitizers | Purpose |
|----------|----------|-----------|---------|
| GCC 12+ | Ubuntu 24.04 | ASan + UBSan | Memory safety, UB detection |
| Clang 16+ | Ubuntu 24.04 | None | Fast build for PR feedback |
| AppleClang | macOS 14 | None | macOS compatibility |
| MSVC 17 | Windows Server 2022 | None | Windows compatibility |

### Pipeline Stages

1. **Build** — configure and compile on all 4 compilers.
2. **Test** — run full test suite on all platforms.
3. **Package** — generate DEB + RPM on `main` branch (Ubuntu only).
4. **Docker** — build, test, and push to GHCR on release tags.

---

## 13. IDE Integration

### Visual Studio Code

Recommended extensions: C/C++ (ms-vscode), CMake Tools (ms-vscode.cmake-tools), clangd.

`.vscode/settings.json`:

```json
{
    "cmake.buildDirectory": "${workspaceFolder}/build/debug",
    "cmake.configureArgs": ["-DKAIROS_BUILD_TESTS=ON"],
    "cmake.debugConfig": {
        "args": ["start", "--config", "deploy/kairos.toml.example", "--log-level", "debug"]
    }
}
```

### CLion

Open the project root. CLion auto-detects CMake. Configure feature flags in Settings → Build → CMake → CMake options.

### Visual Studio

Open the project with "Open a local folder" and select the project root. VS auto-detects `CMakeLists.txt` and `CMakePresets.json`.

---

## 14. Troubleshooting

### "CMake version too old"

Kairos requires CMake >= 3.21. On older Ubuntu:

```bash
pip install cmake --upgrade
# or install from Kitware APT repo:
# https://apt.kitware.com/
```

### "C++ compiler does not support C++20"

Requires GCC >= 12, Clang >= 16, AppleClang >= 15, or MSVC >= 19.30 (VS 2022).

```bash
# Check your compiler version
g++ --version
clang++ --version
```

### Build Fails Fetching Dependencies

CMake FetchContent downloads from GitHub. If behind a firewall:

```bash
# Pre-clone dependencies
git clone https://github.com/araray/confy-cpp.git /tmp/deps/confy-cpp
cmake -B build -DFETCHCONTENT_SOURCE_DIR_CONFY-CPP=/tmp/deps/confy-cpp
```

Or set a proxy:

```bash
export http_proxy=http://proxy:port
export https_proxy=http://proxy:port
```

### "SQLite not found"

```bash
# Ubuntu/Debian
sudo apt-get install libsqlite3-dev

# Fedora/RHEL
sudo dnf install sqlite-devel

# macOS
brew install sqlite3
```

### "OpenSSL not found" (when `KAIROS_VAULT=ON`)

```bash
# Ubuntu/Debian
sudo apt-get install libssl-dev

# macOS
brew install openssl
cmake -B build -DOPENSSL_ROOT_DIR=$(brew --prefix openssl)
```

### Tests Fail with "database is locked"

Multiple test processes are sharing the same database file. Tests use temporary directories by default. Check for stale test processes:

```bash
pkill -f test_persist
```

### Windows: "The system cannot find the path specified"

Ensure you are running from a Developer PowerShell or x64 Native Tools Command Prompt, not a regular PowerShell window. The MSVC toolchain must be on PATH.

### Ninja vs Make

On Linux, CMake defaults to Unix Makefiles. For faster builds:

```bash
sudo apt-get install ninja-build
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

---

## References

- [CMake Documentation](https://cmake.org/cmake/help/latest/)
- [CMake FetchContent](https://cmake.org/cmake/help/latest/module/FetchContent.html)
- [CPack Documentation](https://cmake.org/cmake/help/latest/module/CPack.html)
- [Google Test User's Guide](https://google.github.io/googletest/)
- [AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html)
- [confy-cpp](https://github.com/araray/confy-cpp)
