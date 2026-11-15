# Villa integration contract, version 1

Kairos ships a small integration surface so that Villa can prove what the
engine has applied, keep interval timers stable while unrelated workflows
come and go, and match every scheduled occurrence to a run. The
authoritative requirements live in the Villa spec (§7.2.3, §7.8). This
page records what is implemented and the decisions taken where the spec
leaves room.

Baseline: `6391343c7b606aa2b187e43c9d6334c8fb0bf886`. Both endpoints use
the same bearer-token auth as every other `/api/v1` route.

## `GET /api/v1/integration/villa/status`

```json
{"contract_version":1,
 "baseline_commit":"6391343c7b606aa2b187e43c9d6334c8fb0bf886",
 "daemon_start_id":"ds_<32hex>",
 "loaded_registry_digest":"<hex>",
 "scheduler_applied_digest":"<hex>|null",
 "pipeline_applied_digest":"<hex>",
 "active_workflow_count":2, "queue_depth":0,
 "timer_continuity":"preserve_unchanged_v1",
 "timers":[{"workflow":"villa.schedule.sch_<32hex>.g1","trigger_id":"trg-…",
            "trigger_type":"interval","schedule_id":"sch_<32hex>",
            "schedule_generation":1,"trigger_digest":"<hex>",
            "timer_epoch":"te_<32hex>","next_due_at":"2026-11-14T03:08:33.006Z"}]}
```

- `loaded_registry_digest` is the registry the daemon most recently built
  from disk. The scheduler and the pipeline each publish the digest of the
  registry they swapped in, from their own threads, after the swap. The
  scheduler publishes its digest together with its timer list in one
  snapshot. `scheduler_applied_digest` is `null` only before the
  scheduler's first build. The pipeline applies a pending registry
  between runs, so a run in progress delays its acknowledgment.
- `timers` lists every live timer, sorted by `(workflow, trigger_id)`.
  `schedule_id` and `schedule_generation` are `null` for workflow names
  that do not match `^villa\.schedule\.(sch_[0-9a-f]{32})\.g([0-9]+)$`.
  `trigger_type` is an extra field.

## Digests

- **Registry digest**: SHA-256 hex of the compact JSON array
  `[["<workflow_name>","<sha256_hex(source bytes)>"],…]`, sorted by name
  (ties broken by hash), with no whitespace and UTF-8 passed through
  unescaped (Python: `json.dumps(pairs, separators=(",",":"),
  ensure_ascii=False)`). Source bytes are the exact file bytes the loader
  parsed, which are the bytes `GET /api/v1/workflows/{name}/yaml` returns.
  An empty registry hashes `[]`, which gives
  `4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945`.
- **Trigger digest**: SHA-256 hex of the canonical trigger object. The
  object has sorted keys, no whitespace, and timing fields only.
  `enabled`, `max_instances`, `misfire_policy` and all workflow content
  are excluded:
  - cron: `{"expression":"<expr>","type":"cron"}`
  - interval: `{"interval_ms":<int>,"type":"interval"}` (so `60s` and
    `1m` hash the same)
  - date: `{"at":"<RFC3339 UTC ms>","type":"date"}`

## Timer continuity across reloads

Every timer has a continuity key:

- Villa schedules use `schedule_id + trigger_digest`.
- Other workflows use `trigger_id + trigger_digest`.
- Cron triggers also include the global timezone delta in the key.
- When one workflow has several identical triggers, an ordinal is added.

On an ordinary reload, a timer whose key is unchanged keeps its monotonic
deadline, its wall projection (`next_due_at`), its `timer_epoch` and its
one-shot consumed state. This holds even when the workflow name changes
to a new generation. Adding or removing unrelated workflows never moves a
timer.

The following start a new epoch with initial semantics: engine restart,
a changed trigger digest, a timer that was disabled or absent and comes
back, and a changed cron timezone. Under initial semantics an interval
first fires one interval from now, and a cron trigger fires at its next
match after now.

**Superseded generations.** When several generations of one schedule are
registered at once (for example the intermediate registry during a
generation switch), only the highest generation has a timer. It inherits
the state described above. Lower generations stay registered, so
manual/queued runs can still resolve them, but they never fire.

Intervals reschedule by phase: the next deadline is the previous deadline
plus the interval. Slots that have already passed are skipped, never
replayed.

## Durable occurrences

When a timer fires, Kairos first inserts a row into
`scheduled_occurrences` (SQLite migration 7, `PRAGMA synchronous=FULL` on
a dedicated connection). Only then does the event enter the in-memory
queue. The row holds:

- `occurrence_id`: `occ_<32hex>`, random
- `seq`: AUTOINCREMENT, never reused
- `workflow`, `schedule_id` and `trigger_id`
- the scheduler-applied registry digest
- `scheduled_for`: the timer's original due instant
- `disposition` and `created_at`

If the insert fails, the occurrence is not delivered.

| Disposition | When |
|---|---|
| `emitted` | Queued for execution. `engine_run_id` is bound when the pipeline starts the run, before any step is spawned. |
| `skipped` | Kairos itself skipped it: `max_instances` was reached; or the daemon restarted while the row was still `emitted` without a run; or a one-shot `date` passed while the engine was down (recorded once). |
| `abandoned` | The trigger bus rejected the event, or the pipeline could not resolve the target in its applied registry (the workflow was removed before the event was dequeued). |

Kairos never redelivers persisted occurrences, so restarts never mint new
IDs for them. Rows older than 7 days are pruned at daemon start and at
most hourly while running.

### Trusted step environment

For a scheduled run, every step receives these variables:

- `VILLA_KAIROS_OCCURRENCE_ID`
- `VILLA_KAIROS_SCHEDULED_FOR` (RFC 3339 UTC ms)
- `VILLA_KAIROS_CONFIG_HASH` (registry digest at emission)

They are set after all env layers are merged. Any `VILLA_KAIROS_*` from
the daemon environment, `env_file`, job env or step env is removed first,
so manual runs never carry them. The existing `KAIROS_RUN_ID`,
`KAIROS_JOB_ID`, `KAIROS_STEP_ID` and `KAIROS_CORRELATION_ID` are
unchanged.

## `GET /api/v1/integration/villa/occurrences`

`?cursor=<seq>&limit=<1..500>`. The defaults are `cursor=0` and
`limit=100`. Items with `seq > cursor` are returned in ascending `seq`
order:

```json
{"items":[{"seq":1,"rev":2,"occurrence_id":"occ_…","workflow":"…",
           "scheduled_for":"…","registry_digest":"…","disposition":"emitted",
           "engine_run_id":"run-…","created_at":"…"}],
 "next_cursor":1,"has_more":false}
```

- `next_cursor` is the last returned `seq`, or `null` when the page is
  empty. When it is `null`, keep your cursor.
- `has_more` means the page was full.
- Extension: `?changed_since=<rev>` (exclusive with `cursor`) returns rows
  in ascending `rev` order. `rev` is a durable revision counter that is
  bumped on every insert and update: run binding, abandon, and the
  restart skip. Use it to observe disposition changes after first
  ingestion. In this mode `next_cursor` is the last `rev`.
- Bad parameters return 400.

## Related fixes in this branch

- `use_shell: false` steps are tokenized into argv without a shell:
  - split on whitespace;
  - `'…'` is taken literally;
  - `"…"` honours `\"` and `\\`;
  - a backslash escapes the next character;
  - nothing is expanded.

  Before this fix, every non-shell step failed to spawn. `argv[0]` must be
  an absolute path, because the process layer uses `execve`.
- `date` triggers accept RFC 3339 fractional seconds and a `Z` or `±HH:MM`
  suffix. A zoned value is absolute; without a zone the legacy local-time
  reading is kept.
- Config reloads are serialized.

## Out of scope

The following are not part of this contract:

- concurrent top-level workflows;
- misfire catch-up and coalescing (`misfire_policy` is parsed, but
  behaviour is skip-only);
- per-schedule timezones;
- a PostgreSQL backend.
