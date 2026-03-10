# Authoring Guide — Workflows, Watch Groups, and KEL

> A meticulous reference for writing Kairos workflow definitions, watch groups, scheduling triggers, and KEL conditions.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Workflow Definitions](#2-workflow-definitions)
3. [Jobs and Steps](#3-jobs-and-steps)
4. [DAG Dependencies](#4-dag-dependencies)
5. [Triggers (Scheduling)](#5-triggers-scheduling)
6. [Standalone Jobs](#6-standalone-jobs)
7. [Watch Group Definitions](#7-watch-group-definitions)
8. [Watch Rules](#8-watch-rules)
9. [KEL — Kairos Expression Language](#9-kel--kairos-expression-language)
10. [Environment Variables and Secrets](#10-environment-variables-and-secrets)
11. [Runner Types](#11-runner-types)
12. [Content-Addressable IDs](#12-content-addressable-ids)
13. [Validation and Debugging](#13-validation-and-debugging)
14. [Worked Examples](#14-worked-examples)
15. [Legacy Migration Patterns](#15-legacy-migration-patterns)

---

## 1. Overview

Kairos definitions are written in YAML. There are two top-level definition types:

| Definition | Directory | Extension | Purpose |
|-----------|-----------|-----------|---------|
| **Workflow** | `workflows/` | `.yaml`, `.yml` | DAG of jobs with steps, triggers, and conditions |
| **Watch Group** | `watch_groups/` | `.yaml`, `.yml` | Filesystem monitoring with rules |

Both directories are relative to the config file location (or configured via `kairos.workflows_dir` and `kairos.watch_groups_dir`).

Kairos scans these directories at startup and on config reload. Every `.yaml`/`.yml` file in the directory is loaded. Subdirectories are not scanned.

---

## 2. Workflow Definitions

A workflow is a directed acyclic graph (DAG) of jobs. Each job contains an ordered sequence of steps (shell commands).

### Minimal Workflow

```yaml
# workflows/hello.yaml
name: hello-world
jobs:
  - name: greet
    steps:
      - name: say-hello
        command: echo "Hello from Kairos!"
```

### Complete Workflow Schema

```yaml
# workflows/deploy-pipeline.yaml

# ─── Metadata ──────────────────────────────────────────────
name: deploy-pipeline                  # Required. Human-readable name.
                                       #   Must be unique across all workflows.
description: "Build, test, deploy"     # Optional. Documentation.
version: "2.1.0"                       # Optional. Informational only.
author: "team@example.com"             # Optional.
tags: [deploy, production]             # Optional. For filtering.

# ─── Workflow-level environment ────────────────────────────
env:                                   # Optional. Inherited by all jobs/steps.
  APP_NAME: "myapp"
  BUILD_TYPE: "release"

# ─── Triggers ──────────────────────────────────────────────
triggers:                              # Optional. When omitted, manual-only.
  - type: cron
    spec: "0 2 * * 1-5"               # Weekdays at 2 AM
  - type: interval
    every: 4h                          # Every 4 hours
  - type: watch_group
    name: "repo-monitor"               # Trigger on watch group events
    event_types: [content_changed]
  - type: manual                       # Always allow manual runs

# ─── Settings ──────────────────────────────────────────────
settings:                              # Optional. Per-workflow overrides.
  timeout_seconds: 3600                # Max total workflow duration
  max_concurrent_jobs: 4               # Parallelism limit
  default_shell: "/bin/bash"           # Shell for command execution

# ─── Jobs ──────────────────────────────────────────────────
jobs:
  - name: build
    steps:
      - name: compile
        command: cmake --build build/
        working_dir: /src/myapp
        timeout: 300                   # Per-step timeout (seconds)

  - name: test
    needs: [build]                     # DAG dependency
    condition: 'job("build").last_success'
    steps:
      - name: unit-tests
        command: ctest --output-on-failure
        working_dir: /src/myapp/build

  - name: deploy
    needs: [test]
    condition: 'job("test").last_success and job("test").finished_within(30m)'
    env:
      DEPLOY_TARGET: "prod-01"
      API_KEY: "${{ secrets.deploy_key }}"
    steps:
      - name: push
        command: ./scripts/deploy.sh $DEPLOY_TARGET
        timeout: 120
```

### Workflow Fields Reference

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `name` | string | **yes** | — | Unique human-readable name |
| `description` | string | no | `""` | Documentation |
| `version` | string | no | `""` | Informational version string |
| `author` | string | no | `""` | Author attribution |
| `tags` | list[string] | no | `[]` | Tags for filtering |
| `env` | map[string, string] | no | `{}` | Workflow-level environment variables |
| `triggers` | list[trigger] | no | `[]` | Scheduling triggers |
| `settings` | map | no | `{}` | Workflow-level setting overrides |
| `jobs` | list[job] | **yes** | — | Job definitions |

---

## 3. Jobs and Steps

### Job Schema

```yaml
jobs:
  - name: my-job                       # Required. Unique within workflow.
    needs: [other-job]                  # Optional. DAG predecessor job names.
    condition: 'expression'             # Optional. KEL expression (must be truthy to run).
    continue_on_error: false            # Optional. Continue workflow if this job fails.
    working_dir: /opt/app               # Optional. Default working directory for all steps.
    env:                                # Optional. Job-level env vars.
      KEY: "value"
    timeout_seconds: 600               # Optional. Max total job duration.
    steps:                              # Required. Ordered list of steps.
      - name: step-1
        command: echo "hello"
```

### Step Schema

```yaml
steps:
  - name: my-step                      # Required. Unique within job.
    command: echo "hello world"         # Required. Shell command to execute.
    working_dir: /tmp                   # Optional. Overrides job working_dir.
    env:                                # Optional. Step-level env vars.
      EXTRA: "value"
    timeout: 300                        # Optional. Per-step timeout (seconds).
    use_shell: true                     # Optional. Default true. Execute via shell.
```

### Step Fields Reference

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `name` | string | **yes** | — | Unique name within the job |
| `command` | string | **yes** | — | Shell command to execute |
| `working_dir` | string | no | Inherit from job/workflow | Working directory |
| `env` | map[string, string] | no | `{}` | Step-level environment variables |
| `timeout` | int | no | `kairos.runners.default_timeout_s` | Timeout in seconds |
| `use_shell` | bool | no | `true` | Execute via shell (`/bin/sh -c` / `cmd.exe /c`) |

### Multi-Line Commands

Use YAML block scalars for multi-line commands:

```yaml
steps:
  - name: multi-step
    command: |
      set -euo pipefail
      echo "Step 1: building"
      cmake --build build/ --target all
      echo "Step 2: testing"
      cd build && ctest --output-on-failure
```

The entire block is passed to the shell as a single script.

### Step Execution Semantics

1. Steps execute **sequentially** within a job, in definition order.
2. If a step fails (non-zero exit code), the job fails and remaining steps are skipped.
3. The job's `continue_on_error` flag controls whether the workflow continues after a job failure.
4. Each step's stdout and stderr are captured and persisted in the database.
5. Steps respect the `timeout` setting. On timeout: SIGTERM is sent first, then SIGKILL after `kill_timeout_s` (default: 10s).

---

## 4. DAG Dependencies

The `needs` field creates edges in the workflow's directed acyclic graph.

### Parallel Execution

Jobs with no dependencies (or whose dependencies are satisfied) run **in parallel**, level-by-level:

```yaml
jobs:
  # Level 0: runs first (parallel)
  - name: fetch-data
    steps:
      - name: download
        command: curl -o data.csv https://api.example.com/data

  - name: fetch-config
    steps:
      - name: download
        command: curl -o config.json https://api.example.com/config

  # Level 1: runs after both Level 0 jobs complete (parallel)
  - name: transform
    needs: [fetch-data, fetch-config]
    steps:
      - name: process
        command: python3 transform.py data.csv config.json

  - name: validate
    needs: [fetch-data]
    steps:
      - name: check
        command: python3 validate.py data.csv

  # Level 2: runs after all Level 1 jobs complete
  - name: load
    needs: [transform, validate]
    steps:
      - name: insert
        command: psql -f load.sql
```

This produces the DAG:

```
Level 0:  fetch-data    fetch-config
              ↓   ↘          ↓
Level 1:  validate   transform
              ↓       ↓
Level 2:      load
```

### Cycle Detection

Kairos validates that the job dependency graph is acyclic at load time. If a cycle is detected, the workflow is rejected with an error:

```
YAML error in workflows/broken.yaml:
  jobs: Cycle detected: A → B → C → A
```

### Skip Propagation

When a job is skipped (condition evaluates to false or a `needs` dependency failed), its dependents' behavior depends on context:

| Upstream Status | Downstream Behavior |
|----------------|---------------------|
| SUCCESS | Normal execution |
| FAILURE | Downstream is SKIPPED (exit code 203 — dependency failure) |
| SKIPPED | Downstream is SKIPPED (cascading) |
| CANCELLED | Downstream is CANCELLED |

Exception: if the upstream job has `continue_on_error: true` and fails, downstream jobs still run.

---

## 5. Triggers (Scheduling)

Triggers define when a workflow runs automatically. Without triggers, a workflow can only be run manually.

### Cron Trigger

Standard 5-field cron expressions in wall-clock time:

```yaml
triggers:
  - type: cron
    spec: "0 2 * * *"        # Every day at 2:00 AM
  - type: cron
    spec: "*/15 * * * *"     # Every 15 minutes
  - type: cron
    spec: "0 9 * * 1-5"      # Weekdays at 9:00 AM
  - type: cron
    spec: "0 0 1 * *"        # First day of every month at midnight
```

Cron field format: `minute hour day-of-month month day-of-week`

| Field | Range | Special Characters |
|-------|-------|-------------------|
| Minute | 0–59 | `*`, `/`, `,`, `-` |
| Hour | 0–23 | `*`, `/`, `,`, `-` |
| Day of Month | 1–31 | `*`, `/`, `,`, `-` |
| Month | 1–12 | `*`, `/`, `,`, `-` |
| Day of Week | 0–6 (Sun=0) | `*`, `/`, `,`, `-` |

### Interval Trigger

Fixed-frequency execution using `steady_clock` (drift-immune):

```yaml
triggers:
  - type: interval
    every: 30m                  # Every 30 minutes
  - type: interval
    every: 4h                   # Every 4 hours
    align_to_start: true        # Fire immediately at daemon start
```

Duration format: `Xd`, `Xh`, `Xm`, `Xs`, `Xms` (e.g., `2h30m`, `90s`, `500ms`).

### Date Trigger

Fire once at a specific time (removed from the scheduler after firing):

```yaml
triggers:
  - type: date
    at: "2026-06-15T14:00:00Z"  # ISO 8601 timestamp
```

### Watch Group Trigger

Trigger when a watch group detects matching events:

```yaml
triggers:
  - type: watch_group
    name: "repo-monitor"
    event_types: [content_changed, file_created]
```

### Manual Trigger

Always allow manual triggering via CLI or MCP:

```yaml
triggers:
  - type: manual
```

### Trigger Options

```yaml
triggers:
  - type: cron
    spec: "0 2 * * *"
    misfire_policy: coalesce     # coalesce (default) | skip | run_all
    max_instances: 1             # Max concurrent runs from this trigger
    enabled: true                # Can be disabled without removing
```

**Misfire policies:**

| Policy | Behavior After Downtime |
|--------|------------------------|
| `coalesce` | Fire once on restart, skip all missed occurrences (default) |
| `skip` | Skip all missed fires entirely |
| `run_all` | Fire once for each missed occurrence |

---

## 6. Standalone Jobs

A standalone job is a single job without the workflow DAG overhead. It is defined in the `workflows/` directory with the `standalone: true` flag.

```yaml
# workflows/backup.yaml
name: daily-backup
standalone: true

# Optional condition (evaluated before every run)
condition: 'job("cleanup").last_success'

# Triggers
triggers:
  - type: cron
    spec: "0 2 * * *"

steps:
  - name: dump-db
    command: pg_dump -Fc mydb > /tmp/backup.dump
    working_dir: /opt/backups
  - name: upload
    command: aws s3 cp /tmp/backup.dump s3://backups/$(date +%Y%m%d).dump
    env:
      AWS_REGION: us-east-1
```

Standalone jobs are the equivalent of AVScheduler's `[jobs.X]` sections. They are listed under `kairos jobs list` (not `kairos workflows list`).

---

## 7. Watch Group Definitions

Watch groups define filesystem monitoring targets and rules. They live in the `watch_groups/` directory.

### Minimal Watch Group

```yaml
# watch_groups/logs.yaml
watch_groups:
  - name: log-monitor
    watch_items:
      - /var/log/myapp/
    rules:
      - name: large-log
        condition: 'aggregate(data, "*.log", "size", "sum") > 1073741824'
        severity: warning
```

### Complete Watch Group Schema

```yaml
# watch_groups/config-monitor.yaml
watch_groups:
  - name: config-monitor               # Required. Unique name.
    enabled: true                       # Optional. Default: true.

    # ─── What to watch ─────────────────────────────────────
    watch_items:                        # Required. Paths or glob patterns.
      - /etc/myapp/
      - /opt/myapp/config/*.conf
      - /opt/myapp/secrets/

    # ─── How to watch ──────────────────────────────────────
    mode: hybrid                        # native | sample | hybrid (default)

    # ─── Scanning parameters ───────────────────────────────
    sample_rate: 120                    # Seconds between scans (default: 300)
    max_depth: 5                        # Max directory recursion depth (default: 10)
    max_files: 10000                    # Max files per scan (default: 100000)
    timeout_seconds: 30                 # Max time for single scan

    # ─── Hash policy ───────────────────────────────────────
    hash_policy: "mtime+size"           # mtime+size (fast) | sha256 (precise)

    # ─── Filtering ─────────────────────────────────────────
    exclude_globs:                      # Glob patterns to skip
      - "*.tmp"
      - "*.swp"
      - "*.bak"
      - ".git/**"

    # ─── Symlink handling ──────────────────────────────────
    symlink_policy: follow              # follow (default) | no_follow

    # ─── Content detection ─────────────────────────────────
    pattern: "ERROR|FATAL|CRITICAL"     # Optional regex for pattern_found events

    # ─── Rules ─────────────────────────────────────────────
    rules:
      - name: config-changed
        condition: 'event.type == "file_modified"'
        severity: warning
        description: "A configuration file was modified"
        event_types: [file_modified]    # Optional filter
        trigger_target: deploy-pipeline # Optional: trigger a workflow
        trigger_is_workflow: true       # true (default) or false for standalone job

      - name: secret-rotation
        condition: 'event.type == "file_created" and matches(event.path, ".*\\.key$")'
        severity: critical

      - name: log-growth
        condition: |
          aggregate(data, "*.log", "size", "sum") >
          1.5 * previous("config-monitor", "*.log", "size")
        severity: warning
```

### Watch Group Fields Reference

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `name` | string | **yes** | — | Unique name |
| `enabled` | bool | no | `true` | Whether the group is active |
| `watch_items` | list[string] | **yes** | — | Paths or glob patterns to watch |
| `mode` | string | no | `"hybrid"` | `native`, `sample`, or `hybrid` |
| `sample_rate` | int | no | 300 | Seconds between sample scans |
| `max_depth` | int | no | 10 | Max directory recursion depth |
| `max_files` | int | no | 100000 | Max files per scan |
| `timeout_seconds` | int | no | 30 | Max time for a single scan |
| `hash_policy` | string | no | `"mtime+size"` | `mtime+size` or `sha256` |
| `exclude_globs` | list[string] | no | `[]` | Glob patterns to exclude |
| `symlink_policy` | string | no | `"follow"` | `follow` or `no_follow` |
| `pattern` | string | no | — | Regex for `pattern_found` detection |
| `rules` | list[rule] | no | `[]` | KEL rules for event detection |

### Watch Modes Explained

| Mode | Mechanism | Tradeoff |
|------|-----------|----------|
| `native` | inotify (Linux) / FSEvents (macOS) / ReadDirectoryChangesW (Windows) | Real-time but may miss events on queue overflow |
| `sample` | Periodic directory scan + content diff | Reliable but not real-time |
| `hybrid` | Native events for real-time + periodic verification scan | Best of both: real-time with correctness guarantee |

**Hybrid mode (default)** works as follows:

1. Native events provide real-time notifications.
2. When a native event arrives, a targeted re-scan of the affected path confirms the change.
3. Periodically (at `sample_rate`), a full scan runs to catch anything the native watcher missed.
4. Events are deduplicated: if a native event and a sample scan detect the same change, only one event is emitted.

### First-Scan Behavior

The first scan after daemon startup establishes a baseline. No events are produced from the first scan — this matches EventWatcher's behavior. Events begin from the second scan onward.

---

## 8. Watch Rules

Rules are KEL expressions evaluated against each changed file in a scan diff.

### Rule Fields Reference

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `name` | string | **yes** | — | Unique name within the watch group |
| `condition` | string | **yes** | — | KEL expression (must evaluate to truthy) |
| `severity` | string | no | `"info"` | `info`, `warning`, or `critical` |
| `description` | string | no | `""` | Human-readable description |
| `event_types` | list[string] | no | `[]` (all) | Filter: only evaluate for these event types |
| `trigger_target` | string | no | — | Workflow or job to trigger when rule fires |
| `trigger_is_workflow` | bool | no | `true` | Whether `trigger_target` is a workflow (true) or standalone job (false) |

### Event Types

The watch engine detects these event types:

| Event Type | Description |
|-----------|-------------|
| `file_created` | New file appeared |
| `file_deleted` | File was removed |
| `file_modified` | File content or metadata changed |
| `content_changed` | File content specifically changed (not just mtime) |
| `size_changed` | File size changed |
| `permission_changed` | File permissions changed |
| `pattern_found` | Regex `pattern` matched in file content |

### Rule Evaluation Context

Within a watch rule's KEL expression, these variables are available:

| Variable | Type | Description |
|----------|------|-------------|
| `data` | (special) | Current sample data map — first argument to `aggregate()` |
| `event` | (special) | The event being evaluated |
| `event.type` | string | Event type string (e.g., `"file_modified"`) |
| `event.path` | string | Affected file path |
| `event.size` | int | Current file size in bytes |
| `event.mtime` | int | File modification time (Unix timestamp) |
| `event.severity` | string | Event severity |
| `watch_group` | string | Name of the current watch group |
| `file` | (special) | Current file metrics |
| `prev_file` | (special) | Previous file metrics (from last sample) |

All KEL functions are available in this context, including `aggregate()`, `previous()`, and `job()` history queries.

---

## 9. KEL — Kairos Expression Language

KEL is a small, safe, deterministic expression language used for all condition evaluation in Kairos.

### Design Principles

- **Not Turing-complete** — no loops, no recursion, no user-defined functions.
- **Sandboxed** — no I/O, no system calls, no eval.
- **Bounded** — max AST depth (32), max eval time (100ms), max string/list size.
- **Deterministic** — same inputs always produce the same output.
- **Familiar** — syntax resembles Python/JavaScript boolean expressions.

### Type System

KEL has six types. There is no `null` — functions return typed defaults instead.

| Type | C++ Mapping | Literal Syntax | Examples |
|------|------------|----------------|---------|
| `bool` | `bool` | `true`, `false` | `true`, `false` |
| `int` | `int64_t` | Decimal, hex, binary | `42`, `-7`, `0xFF`, `0b1010` |
| `float` | `double` | Decimal with `.` or `e` | `3.14`, `-0.5`, `1e10` |
| `string` | `std::string` | Double or single quotes | `"hello"`, `'world'` |
| `duration` | `chrono::milliseconds` | Number + unit suffix | `2h`, `30m`, `90s`, `1d`, `500ms` |
| `list` | `vector<KelValue>` | Square brackets | `[1, 2, 3]`, `["a", "b"]` |

**Duration units:**

| Suffix | Meaning | Example | Milliseconds |
|--------|---------|---------|-------------|
| `ms` | Milliseconds | `500ms` | 500 |
| `s` | Seconds | `90s` | 90000 |
| `m` | Minutes | `30m` | 1800000 |
| `h` | Hours | `2h` | 7200000 |
| `d` | Days | `1d` | 86400000 |

**Type promotion:** `int + float → float` (widening). No implicit bool↔int or string↔numeric conversion.

### Operators

Sorted by precedence (highest first):

| Precedence | Operator | Description | Associativity |
|-----------|----------|-------------|---------------|
| 1 | `not`, unary `-` | Logical NOT, numeric negation | Right |
| 2 | `*`, `/`, `%` | Multiply, divide, modulo | Left |
| 3 | `+`, `-` | Add/concatenate, subtract | Left |
| 4 | `<`, `>`, `<=`, `>=` | Ordered comparison | Left |
| 5 | `==`, `!=` | Equality | Left |
| 6 | `in` | List membership | Left |
| 7 | `and` | Logical AND (short-circuit) | Left |
| 8 | `or` | Logical OR (short-circuit) | Left |

**Short-circuit evaluation:** `and` and `or` evaluate the right operand only if necessary. This means `job("x").last_success and job("x").finished_within(1h)` skips the database query if the first condition is false.

**Cross-type comparison:** `==` and `!=` between incompatible types return `false` (not error). `<`, `>`, `<=`, `>=` between incompatible types raise a runtime error.

### Built-in Functions

All functions are whitelisted. Unknown function names produce an error.

#### Category 1 — General Purpose

| Function | Signature | Description |
|----------|-----------|-------------|
| `min(a, b)` | `(numeric, numeric) → numeric` | Minimum of two values |
| `max(a, b)` | `(numeric, numeric) → numeric` | Maximum of two values |
| `abs(x)` | `(numeric) → numeric` | Absolute value |
| `len(x)` | `(string\|list) → int` | Length of string or list |
| `sum(xs)` | `(list[numeric]) → numeric` | Sum of list elements |
| `avg(xs)` | `(list[numeric]) → float` | Average of list elements |
| `round(x)` | `(float) → int` | Round to nearest integer |
| `floor(x)` | `(float) → int` | Round down |
| `ceil(x)` | `(float) → int` | Round up |
| `int(x)` | `(float\|string\|bool) → int` | Convert to integer |
| `float(x)` | `(int\|string) → float` | Convert to float |
| `str(x)` | `(any) → string` | Convert to string |
| `bool(x)` | `(any) → bool` | Truthiness conversion |

#### Category 2 — String Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `starts_with(s, prefix)` | `(string, string) → bool` | Test if `s` starts with `prefix` |
| `ends_with(s, suffix)` | `(string, string) → bool` | Test if `s` ends with `suffix` |
| `contains(s, substr)` | `(string, string) → bool` | Test if `s` contains `substr` |
| `matches(s, pattern)` | `(string, string) → bool` | Regex match (ECMAScript syntax) |
| `lower(s)` | `(string) → string` | Convert to lowercase (ASCII) |
| `upper(s)` | `(string) → string` | Convert to uppercase (ASCII) |
| `trim(s)` | `(string) → string` | Strip leading/trailing whitespace |
| `replace(s, old, new)` | `(string, string, string) → string` | Replace first occurrence |

**`matches()` safety:** Regex compilation and matching are bounded to 10ms total. Pathological patterns (ReDoS) return `false` with a warning logged. This is a hardcoded limit.

#### Category 3 — Aggregation Functions (Watch Rules)

These are only available in watch-rule evaluation contexts.

| Function | Signature | Description |
|----------|-----------|-------------|
| `aggregate(data, glob, metric, func)` | `(map, string, string, string) → numeric` | Aggregate metric values for files matching glob |
| `previous(group, glob, metric)` | `(string, string, string) → numeric` | Get previous sample's aggregated metric value |

**`aggregate()` parameters:**

- `data` — the current sample data map (automatically bound).
- `glob` — filename pattern (e.g., `"*.log"`, `"config.*"`).
- `metric` — one of: `"size"`, `"mtime"`, `"hash"`, `"pattern_found"`.
- `func` — one of: `"sum"`, `"min"`, `"max"`, `"avg"`, `"count"`.

**`previous()` parameters:**

- `group` — watch group name.
- `glob` — filename pattern.
- `metric` — one of: `"size"`, `"mtime"`, etc.

`previous()` queries the SQLite database for the most recent prior sample. It returns 0 if no prior sample exists. This is the only KEL function that performs I/O (read-only database query), bounded by the evaluation timeout.

#### Category 4 — Job History Functions

These query the SQLite run history. They work via the `job()` reference pattern.

| Expression | Return Type | Description |
|-----------|-------------|-------------|
| `job("id")` | JobRef | Create a reference to a job by name or ID |
| `job("id").last_success` | bool | True if last completed run was SUCCESS |
| `job("id").last_status` | string | Status of last run: `"success"`, `"failed"`, `"timed_out"`, `"skipped"`, `"cancelled"` |
| `job("id").last_exit_code` | int | Exit code of last run (-1 if never run) |
| `job("id").last_run_at` | duration | Time elapsed since last run |
| `job("id").finished_within(dur)` | bool | True if last successful run completed within `dur` of now |
| `job("id").run_count` | int | Total number of completed runs |
| `job("id").success_rate` | float | Ratio of successful to total runs (0.0–1.0) |
| `job("id").has_run` | bool | True if the job has ever run |

The `job("name")` argument is the job's human-readable name (from YAML) or its content-addressable ID.

#### Category 5 — Time Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `now()` | `→ duration` | Current wall-clock time as duration since Unix epoch |
| `duration_seconds(d)` | `(duration) → int` | Convert duration to whole seconds |
| `duration_minutes(d)` | `(duration) → int` | Convert duration to whole minutes |
| `duration_hours(d)` | `(duration) → int` | Convert duration to whole hours |

**Determinism of `now()`:** Within a single pipeline evaluation (one TriggerEvent), `now()` is captured once and reused. All jobs in the same workflow run see the same timestamp.

### Evaluation Contexts

Different evaluation sites provide different variable bindings:

**Context 1 — Workflow Job Condition** (`jobs[].condition`):

| Variable | Type | Description |
|----------|------|-------------|
| `workflow` | string | Current workflow name |
| `trigger` | string | Trigger type (`"schedule"`, `"watch"`, `"manual"`, `"mcp"`) |
| `run_id` | string | Current run UUID |

Available functions: all Category 1, 2, 4, 5. **Not available:** `aggregate()`, `previous()`, `event`, `data`.

**Context 2 — Watch Group Rule Condition** (`rules[].condition`):

| Variable | Type | Description |
|----------|------|-------------|
| `data` | (special) | Current sample data map |
| `event` | (special) | Current event with `.type`, `.path`, `.size`, `.mtime` members |
| `watch_group` | string | Watch group name |
| `file` | (special) | Current file metrics |
| `prev_file` | (special) | Previous file metrics |

Available functions: all Categories 1–5.

**Context 3 — Standalone Job Condition** (`condition` on standalone jobs):

| Variable | Type | Description |
|----------|------|-------------|
| `trigger` | string | Always `"schedule"` for standalone jobs |

Available functions: all Category 1, 2, 4, 5. **Not available:** watch-specific variables and aggregation functions.

### Safety Limits

| Limit | Default | Config Key | Effect on Violation |
|-------|---------|-----------|---------------------|
| Max AST nodes | 1024 | `kairos.kel.max_ast_nodes` | Parse error |
| Max AST depth | 32 | `kairos.kel.max_ast_depth` | Parse error |
| Max evaluation time | 100ms | `kairos.kel.max_eval_time_ms` | `KelLimitError` |
| Max string result | 64 KB | `kairos.kel.max_string_length` | `KelLimitError` |
| Max list result | 10,000 | `kairos.kel.max_list_length` | `KelLimitError` |
| Max function args | 16 | (hardcoded) | Parse error |
| Max regex compile | 10ms | (hardcoded) | `matches()` returns false |

### Error Messages

KEL provides detailed, human-friendly error messages:

**Parse error:**

```
KEL parse error at offset 23: expected ')' after function arguments, got ','
  Expression: job('build').finished_within(30m, 2h)
                                              ^
```

**Evaluation error:**

```
KEL evaluation error: type mismatch in '>' operator
  Left operand: "hello" (string)
  Right operand: 42 (int)
  Ordered comparison requires matching numeric, string, or duration types
```

---

## 10. Environment Variables and Secrets

### Environment Variable Layering

Environment variables are merged in this order (later overrides earlier):

1. System environment (inherited from daemon process)
2. Global defaults (`kairos.env` in config)
3. Workflow-level `env` (YAML)
4. Job-level `env` (YAML)
5. Step-level `env` (YAML)
6. Secret injection (`${{ secrets.key }}`)
7. Kairos-injected variables (always present)

### Kairos-Injected Variables

Every running step has access to these variables:

| Variable | Description |
|----------|-------------|
| `KAIROS_RUN_ID` | UUID of the current run |
| `KAIROS_JOB_ID` | Content-addressable ID of the current job |
| `KAIROS_STEP_ID` | Content-addressable ID of the current step |
| `KAIROS_WORKFLOW_ID` | ID of the parent workflow (empty for standalone jobs) |
| `KAIROS_TRIGGER_TYPE` | `schedule_tick`, `file_event`, `file_diff`, `manual_run` |
| `KAIROS_CORRELATION_ID` | Trace correlation ID for observability |
| `KAIROS_DATA_DIR` | Configured data directory path |
| `KAIROS_DB_PATH` | Path to the SQLite database |

### Secrets

Secrets use `${{ secrets.key_name }}` syntax and are resolved at step execution time (never at load time):

```yaml
jobs:
  - name: deploy
    steps:
      - name: push
        command: ./deploy.sh
        env:
          API_KEY: "${{ secrets.deploy_api_key }}"
          DB_PASSWORD: "${{ secrets.db_password }}"
```

Secrets require `KAIROS_VAULT=ON` at build time and a configured Ansible Vault file. Resolved values are never logged or persisted. Unresolved references cause the step to fail with exit code 204.

---

## 11. Runner Types

### Local Shell Runner (Default)

Executes commands via the system shell:

- **POSIX:** `/bin/sh -c "<command>"` (or `default_shell` from config)
- **Windows:** `cmd.exe /c "<command>"`

### Docker Runner (`KAIROS_DOCKER=ON`)

Executes commands inside Docker containers:

```yaml
jobs:
  - name: build-in-container
    runner:
      type: docker
      image: ubuntu:22.04
      volumes:
        - /src:/workspace
    steps:
      - name: compile
        command: gcc -o app main.c
        working_dir: /workspace
```

Requires the Docker CLI on PATH. The Docker runner shells out to `docker run` with appropriate flags for timeout enforcement, volume mounts, and environment propagation.

---

## 12. Content-Addressable IDs

Kairos generates deterministic IDs from content hashing (SHA-256). This ensures IDs are stable across machines for identical definitions.

| Entity | Prefix | Hash Input |
|--------|--------|-----------|
| Workflow | `wfl-` | `name + sorted(job_names)` |
| Job | `job-` | `name + sorted(step_names) + condition` |
| Step | `stp-` | `name + command + working_dir` |
| Trigger | `trg-` | `target_id + spec_type + spec_value` |
| Watch Group | `wg-` | `name + sorted(watch_items)` |
| Run | `run-` | UUID v4 (not content-addressed — each run is unique) |

You can refer to entities by either their human-readable name or their content-addressable ID in CLI commands and KEL expressions.

---

## 13. Validation and Debugging

### Validate Configuration

```bash
# Validate the TOML config
kairos config validate --config kairos.toml

# View effective configuration (with all layers merged)
kairos config show
```

### Validate Workflows

Workflows are validated at load time. Errors are reported per-file with YAML path context:

```bash
# Start with debug logging to see validation output
kairos start --config kairos.toml --log-level debug 2>&1 | grep -i "yaml\|error\|warning"
```

### Explain Execution Plan

The `explain` command shows exactly what would happen without executing:

```bash
kairos explain data-pipeline

  Execution Plan: data-pipeline
  ═══════════════════════════════

  Level 0:
    ● extract — WOULD RUN (no condition)

  Level 1:
    ● transform — WOULD RUN if job("extract").last_success → evaluates to: true
    ● load — WOULD RUN (no condition, depends on: extract)

  3 job(s) would run, 0 would skip
```

### Test a Watch Group

Run a single scan cycle without starting the daemon:

```bash
# Scan all watch groups
kairos watches scan-once --config kairos.toml

# Scan a specific group
kairos watches scan-once config-monitor --config kairos.toml
```

### Test KEL Expressions

While there is no standalone KEL REPL, you can test expressions via the MCP `explainPlan` tool:

```json
{"jsonrpc":"2.0","method":"tools/call","params":{
  "name":"kairos.explainPlan",
  "arguments":{"workflow_id":"my-workflow"}
},"id":1}
```

---

## 14. Worked Examples

### Example 1: CI/CD Pipeline

```yaml
# workflows/ci-pipeline.yaml
name: ci-pipeline
triggers:
  - type: watch_group
    name: source-code
    event_types: [content_changed]
  - type: manual

jobs:
  - name: lint
    steps:
      - name: run-linter
        command: flake8 src/ --max-line-length 120

  - name: build
    needs: [lint]
    steps:
      - name: compile
        command: |
          cmake --preset release
          cmake --build build/release -j$(nproc)
        timeout: 600

  - name: test
    needs: [build]
    steps:
      - name: unit-tests
        command: cd build/release && ctest --output-on-failure -j4
      - name: integration-tests
        command: ./scripts/run-integration-tests.sh

  - name: deploy-staging
    needs: [test]
    condition: 'job("test").last_success and job("test").finished_within(30m)'
    env:
      ENVIRONMENT: staging
    steps:
      - name: deploy
        command: ./scripts/deploy.sh staging
```

### Example 2: Data Pipeline with Schedule

```yaml
# workflows/etl-daily.yaml
name: etl-daily
triggers:
  - type: cron
    spec: "0 3 * * *"                 # 3 AM daily

jobs:
  - name: extract
    steps:
      - name: fetch-api
        command: python3 extract.py --output /data/raw/$(date +%Y%m%d).json
        timeout: 1800

  - name: transform
    needs: [extract]
    steps:
      - name: clean
        command: python3 transform.py --input /data/raw/ --output /data/clean/

  - name: load
    needs: [transform]
    condition: 'job("transform").last_success'
    steps:
      - name: insert
        command: python3 load.py --source /data/clean/ --target production_db
        env:
          DB_PASSWORD: "${{ secrets.prod_db_password }}"
```

### Example 3: Log Monitoring with Size Alert

```yaml
# watch_groups/log-monitor.yaml
watch_groups:
  - name: app-logs
    mode: hybrid
    sample_rate: 60
    watch_items:
      - /var/log/myapp/
    exclude_globs:
      - "*.gz"
      - "*.old"
    hash_policy: "mtime+size"
    pattern: "FATAL|PANIC|OOM"

    rules:
      - name: fatal-detected
        condition: 'event.type == "pattern_found"'
        severity: critical
        trigger_target: alert-pipeline
        trigger_is_workflow: true

      - name: log-explosion
        condition: 'aggregate(data, "*.log", "size", "sum") > 5368709120'
        severity: warning
        description: "Total log size exceeds 5 GB"
        trigger_target: log-rotate
        trigger_is_workflow: false

      - name: rapid-growth
        condition: |
          aggregate(data, "*.log", "size", "sum") >
          2.0 * previous("app-logs", "*.log", "size")
        severity: warning
        description: "Logs growing at >2x rate"
```

### Example 4: Standalone Backup Job with Dependency

```yaml
# workflows/backup.yaml
name: nightly-backup
standalone: true
condition: 'job("health-check").last_success and job("health-check").finished_within(1h)'
triggers:
  - type: cron
    spec: "0 1 * * *"               # 1 AM nightly

steps:
  - name: dump
    command: pg_dump -Fc production > /backups/$(date +%Y%m%d).dump
    timeout: 3600
  - name: upload
    command: |
      aws s3 cp /backups/$(date +%Y%m%d).dump \
        s3://company-backups/postgres/$(date +%Y%m%d).dump
    env:
      AWS_REGION: us-east-1
  - name: cleanup
    command: find /backups -name "*.dump" -mtime +7 -delete
```

---

## 15. Legacy Migration Patterns

### AVScheduler → Kairos

| AVScheduler Pattern | Kairos Equivalent |
|--------------------|-------------------|
| `condition = "job_A.last_run_successful"` | `condition: 'job("job_A").last_success'` |
| `condition = "job_A.finished_within(2h)"` | `condition: 'job("job_A").finished_within(2h)'` |
| `schedule_type = "cron"` + `schedule = "0 2 * * *"` | `triggers: [{type: cron, spec: "0 2 * * *"}]` |
| `schedule_type = "interval"` + `interval_seconds = 3600` | `triggers: [{type: interval, every: 1h}]` |
| `type = "PYTHON"` + `command = "script.py"` | `command: python3 script.py` |

### EventWatcher → Kairos

| EventWatcher Pattern | Kairos Equivalent |
|--------------------|-------------------|
| `eval("aggregate(data, '*.log', 'size', sum)")` | `condition: 'aggregate(data, "*.log", "size", "sum")'` |
| `eval("get_previous_metric(db, 'grp', '*.log', 'size')")` | `condition: 'previous("grp", "*.log", "size")'` |
| `sample_rate: 120` | `sample_rate: 120` (unchanged) |
| `watch_items: ["/path"]` | `watch_items: ["/path"]` (unchanged) |

### LocalFlow → Kairos

| LocalFlow Pattern | Kairos Equivalent |
|------------------|-------------------|
| `condition: "job_id"` (string) | `condition: 'job("job_id").last_success'` |
| `needs: [dep_A, dep_B]` | `needs: [dep_A, dep_B]` (unchanged) |
| `eval_condition: "expression"` | `condition: 'expression'` (KEL syntax) |

Use `kairos migrate-config` to automate most of these translations:

```bash
kairos migrate-config --source avscheduler --source-config config.toml --output-dir ./kairos/
kairos migrate-config --source localflow --source-workflows ./workflows/ --output-dir ./kairos/
kairos migrate-config --source eventwatcher --source-config config.toml \
    --source-watches watch_groups.yaml --output-dir ./kairos/
```

The migration tool flags any expressions that require manual review (e.g., Python `eval()` patterns that cannot be directly translated to KEL).

---

## References

- [Kairos Design Specification](docs/) — Parts 1–7
- [KEL Grammar (EBNF)](docs/DESIGN_SPEC_PART2.md#74-grammar)
- [confy-cpp Documentation](https://github.com/araray/confy-cpp)
- [Cron Expression Reference](https://crontab.guru/)
- [YAML Specification](https://yaml.org/spec/1.2.2/)
- [Model Context Protocol (MCP)](https://modelcontextprotocol.io/)
