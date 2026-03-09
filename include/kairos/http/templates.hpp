/// include/kairos/http/templates.hpp
// ╔════════════════════════════════════════════════════════════════════════════╗
// ║  Embedded inja templates for the Kairos web dashboard                    ║
// ║                                                                          ║
// ║  Templates are stored as constexpr string_view constants, compiled      ║
// ║  into the binary for zero-dependency deployment ("single binary").      ║
// ║                                                                          ║
// ║  Template syntax: inja (Jinja2-compatible)                              ║
// ║    {{ variable }}         — interpolation                               ║
// ║    {% for x in list %}    — iteration                                   ║
// ║    {% if condition %}     — conditionals                                ║
// ║    {% include "partial" %} — not used (we inline partials)              ║
// ║                                                                          ║
// ║  Spec reference: §26.5, §26.6                                          ║
// ╚════════════════════════════════════════════════════════════════════════════╝
#pragma once

#include <string_view>

namespace kairos::http::templates {

// ── Base layout (shared head/nav/footer) ────────────────────────────────
//
// All page templates include this inline. The base provides:
//   - Bootstrap 5 CDN CSS
//   - Dark theme styling
//   - Navigation bar with branding
//   - Footer with version

/// Common <head> content — CSS, meta tags, favicon.
inline constexpr std::string_view kHeadFragment = R"html(
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.2/dist/css/bootstrap.min.css"
      rel="stylesheet" crossorigin="anonymous">
<style>
  body { background: #1a1a2e; color: #e0e0e0; font-family: 'Segoe UI', system-ui, sans-serif; }
  .card { background: #16213e; border: 1px solid #0f3460; border-radius: 8px; }
  .table { color: #e0e0e0; }
  .table th { border-color: #0f3460; font-weight: 600; }
  .table td { border-color: #0f3460; }
  .table-hover tbody tr:hover { background-color: rgba(0, 212, 255, 0.06); }
  .navbar { background: #0f3460 !important; }
  .navbar-brand { font-weight: 700; letter-spacing: 1px; }
  code { color: #00d4ff; font-size: 0.85em; }
  a { color: #00d4ff; text-decoration: none; }
  a:hover { color: #33e0ff; }
  .stat-value { font-size: 2rem; font-weight: 700; color: #00d4ff; }
  .stat-label { font-size: 0.8rem; color: #888; text-transform: uppercase;
                letter-spacing: 0.5px; margin-top: 4px; }
  .badge-success { background-color: #198754 !important; }
  .badge-danger { background-color: #dc3545 !important; }
  .badge-primary { background-color: #0d6efd !important; }
  .badge-warning { background-color: #ffc107 !important; color: #000 !important; }
  .badge-secondary { background-color: #6c757d !important; }
  .page-header { border-bottom: 1px solid #0f3460; padding-bottom: 12px;
                 margin-bottom: 24px; }
  .breadcrumb { background: transparent; padding: 0; }
  .breadcrumb-item a { color: #00d4ff; }
  .breadcrumb-item.active { color: #888; }
  .log-panel { background: #0d1117; border: 1px solid #30363d;
               border-radius: 6px; padding: 12px; font-family: monospace;
               font-size: 0.85rem; max-height: 400px; overflow-y: auto; }
  .log-panel .log-line { padding: 1px 0; }
  .log-panel .log-stderr { color: #ff6b6b; }
  .log-panel .log-stdout { color: #adbac7; }
</style>
)html";

/// Navigation bar.
inline constexpr std::string_view kNavFragment = R"html(
<nav class="navbar navbar-dark mb-4">
  <div class="container-fluid">
    <a class="navbar-brand" href="/">⏱ Kairos</a>
    <div>
      <a href="/" class="text-light me-3">Dashboard</a>
      <a href="/workflows" class="text-light me-3">Workflows</a>
      <a href="/runs" class="text-light me-3">Runs</a>
      <a href="/events" class="text-light me-3">Events</a>
      <a href="/metrics" class="text-light me-3 small">Metrics</a>
    </div>
  </div>
</nav>
)html";

// ── Dashboard page (/) ──────────────────────────────────────────────────

inline constexpr std::string_view kDashboardTemplate = R"html(<!DOCTYPE html>
<html lang="en">
<head>
  <title>Kairos — Dashboard</title>
  {{ head }}
</head>
<body>
  {{ nav }}
  <div class="container-fluid">
    <!-- Summary cards -->
    <div class="row mb-4">
      <div class="col-md-3">
        <div class="card p-3 text-center">
          <div class="stat-value">{{ total_runs }}</div>
          <div class="stat-label">Total Runs</div>
        </div>
      </div>
      <div class="col-md-3">
        <div class="card p-3 text-center">
          <div class="stat-value">{{ active_runs }}</div>
          <div class="stat-label">Active Runs</div>
        </div>
      </div>
      <div class="col-md-3">
        <div class="card p-3 text-center">
          <div class="stat-value">{{ runs_today }}</div>
          <div class="stat-label">Runs Today</div>
        </div>
      </div>
      <div class="col-md-3">
        <div class="card p-3 text-center">
          <div class="stat-value" style="color: {% if failures_today > 0 %}#ff6b6b{% else %}#00d4ff{% endif %}">{{ failures_today }}</div>
          <div class="stat-label">Failures Today</div>
        </div>
      </div>
    </div>

    <!-- Uptime + Quick Actions -->
    <div class="row mb-4">
      <div class="col-md-6">
        <div class="card p-3">
          <h5>Uptime</h5>
          <p class="mb-0" style="color: #00d4ff; font-size: 1.2rem;">
            {{ uptime_hours }}h {{ uptime_minutes }}m
          </p>
        </div>
      </div>
      <div class="col-md-6">
        <div class="card p-3">
          <h5>Quick Actions</h5>
          <a href="/api/v1/workflows" class="btn btn-outline-info btn-sm me-2">Workflows API</a>
          <a href="/api/v1/runs" class="btn btn-outline-info btn-sm me-2">Runs API</a>
          <a href="/metrics" class="btn btn-outline-info btn-sm me-2">Metrics</a>
          <a href="/health" class="btn btn-outline-info btn-sm">Health</a>
        </div>
      </div>
    </div>

    <!-- Recent runs table -->
    <div class="card p-3">
      <div class="d-flex justify-content-between align-items-center mb-3">
        <h5 class="mb-0">Recent Runs</h5>
        <a href="/runs" class="btn btn-outline-info btn-sm">View All</a>
      </div>
      <table class="table table-sm table-hover mb-0">
        <thead>
          <tr>
            <th>Run ID</th><th>Target</th><th>Status</th>
            <th>Trigger</th><th>Started</th><th>Duration</th>
          </tr>
        </thead>
        <tbody>
{% for run in recent_runs %}
          <tr>
            <td><code>{{ run.run_id_short }}</code></td>
            <td>{{ run.target_name }}</td>
            <td><span class="badge badge-{{ run.badge }}">{{ run.status }}</span></td>
            <td>{{ run.trigger_type }}</td>
            <td>{{ run.start_ts }}</td>
            <td>{{ run.duration_ms }}ms</td>
          </tr>
{% endfor %}
{% if length(recent_runs) == 0 %}
          <tr>
            <td colspan="6" class="text-center text-muted">No runs recorded yet</td>
          </tr>
{% endif %}
        </tbody>
      </table>
    </div>
  </div>
</body>
</html>)html";

// ── Workflows page (/workflows) ─────────────────────────────────────────

inline constexpr std::string_view kWorkflowsTemplate = R"html(<!DOCTYPE html>
<html lang="en">
<head>
  <title>Kairos — Workflows</title>
  {{ head }}
</head>
<body>
  {{ nav }}
  <div class="container-fluid">
    <h4 class="page-header">Workflows</h4>
    <div class="card p-3">
      <table class="table table-sm table-hover mb-0">
        <thead>
          <tr><th>Name</th><th>Jobs</th><th>Trigger</th></tr>
        </thead>
        <tbody>
{% for wf in workflows %}
          <tr>
            <td><a href="/workflows/{{ wf.name }}">{{ wf.name }}</a></td>
            <td>{{ wf.job_count }}</td>
            <td>{{ wf.trigger_type }}</td>
          </tr>
{% endfor %}
{% if length(workflows) == 0 %}
          <tr>
            <td colspan="3" class="text-center text-muted">No workflows loaded</td>
          </tr>
{% endif %}
        </tbody>
      </table>
    </div>
  </div>
</body>
</html>)html";

// ── Runs page (/runs) ───────────────────────────────────────────────────

inline constexpr std::string_view kRunsTemplate = R"html(<!DOCTYPE html>
<html lang="en">
<head>
  <title>Kairos — Runs</title>
  {{ head }}
</head>
<body>
  {{ nav }}
  <div class="container-fluid">
    <h4 class="page-header">Run History</h4>
    <div class="card p-3">
      <table class="table table-sm table-hover mb-0">
        <thead>
          <tr>
            <th>Run ID</th><th>Target</th><th>Type</th><th>Status</th>
            <th>Trigger</th><th>Started</th><th>Duration</th>
          </tr>
        </thead>
        <tbody>
{% for run in runs %}
          <tr>
            <td><a href="/runs/{{ run.run_id }}"><code>{{ run.run_id_short }}</code></a></td>
            <td>{{ run.target_name }}</td>
            <td>{{ run.target_type }}</td>
            <td><span class="badge badge-{{ run.badge }}">{{ run.status }}</span></td>
            <td>{{ run.trigger_type }}</td>
            <td>{{ run.start_ts }}</td>
            <td>{{ run.duration_ms }}ms</td>
          </tr>
{% endfor %}
{% if length(runs) == 0 %}
          <tr>
            <td colspan="7" class="text-center text-muted">No runs recorded yet</td>
          </tr>
{% endif %}
        </tbody>
      </table>
    </div>
  </div>
</body>
</html>)html";

// ── Run detail page (/runs/:id) ─────────────────────────────────────────

inline constexpr std::string_view kRunDetailTemplate = R"html(<!DOCTYPE html>
<html lang="en">
<head>
  <title>Kairos — Run {{ run_id_short }}</title>
  {{ head }}
</head>
<body>
  {{ nav }}
  <div class="container-fluid">
    <nav aria-label="breadcrumb">
      <ol class="breadcrumb">
        <li class="breadcrumb-item"><a href="/runs">Runs</a></li>
        <li class="breadcrumb-item active">{{ run_id_short }}</li>
      </ol>
    </nav>

    <div class="row mb-4">
      <div class="col-md-8">
        <div class="card p-3">
          <h5>Run Detail</h5>
          <table class="table table-sm mb-0">
            <tr><td class="text-muted" style="width:140px">Run ID</td><td><code>{{ run_id }}</code></td></tr>
            <tr><td class="text-muted">Target</td><td>{{ target_name }}</td></tr>
            <tr><td class="text-muted">Status</td><td><span class="badge badge-{{ badge }}">{{ status }}</span></td></tr>
            <tr><td class="text-muted">Trigger</td><td>{{ trigger_type }}</td></tr>
            <tr><td class="text-muted">Started</td><td>{{ start_ts }}</td></tr>
            <tr><td class="text-muted">Ended</td><td>{{ end_ts }}</td></tr>
            <tr><td class="text-muted">Duration</td><td>{{ duration_ms }}ms</td></tr>
          </table>
        </div>
      </div>
      <div class="col-md-4">
        <div class="card p-3">
          <h5>Actions</h5>
          <a href="/api/v1/runs/{{ run_id }}" class="btn btn-outline-info btn-sm me-2">JSON</a>
          <a href="/api/v1/runs/{{ run_id }}/logs" class="btn btn-outline-info btn-sm me-2">Logs API</a>
{% if status == "RUNNING" %}
          <a href="/sse/logs/{{ run_id }}" class="btn btn-outline-warning btn-sm">Live SSE</a>
{% endif %}
        </div>
      </div>
    </div>

    <!-- Jobs -->
    <div class="card p-3 mb-4">
      <h5>Jobs</h5>
      <table class="table table-sm table-hover mb-0">
        <thead>
          <tr><th>Job</th><th>Status</th><th>Exit Code</th><th>Duration</th></tr>
        </thead>
        <tbody>
{% for job in jobs %}
          <tr>
            <td>{{ job.job_name }}</td>
            <td><span class="badge badge-{{ job.badge }}">{{ job.status }}</span></td>
            <td><code>{{ job.exit_code }}</code></td>
            <td>{{ job.duration_ms }}ms</td>
          </tr>
{% for step in job.steps %}
          <tr style="font-size: 0.85em;">
            <td style="padding-left: 2em;">↳ {{ step.step_name }}</td>
            <td><span class="badge badge-{{ step.badge }}">{{ step.status }}</span></td>
            <td><code>{{ step.exit_code }}</code></td>
            <td><code>{{ step.command }}</code></td>
          </tr>
{% endfor %}
{% endfor %}
        </tbody>
      </table>
    </div>
  </div>
</body>
</html>)html";

// ── Events page (/events) ───────────────────────────────────────────────

inline constexpr std::string_view kEventsTemplate = R"html(<!DOCTYPE html>
<html lang="en">
<head>
  <title>Kairos — Events</title>
  {{ head }}
</head>
<body>
  {{ nav }}
  <div class="container-fluid">
    <h4 class="page-header">Watch Events</h4>
    <div class="card p-3">
      <table class="table table-sm table-hover mb-0">
        <thead>
          <tr>
            <th>Event ID</th><th>Watch Group</th><th>Rule</th>
            <th>Type</th><th>Severity</th><th>Time</th>
          </tr>
        </thead>
        <tbody>
{% for event in events %}
          <tr>
            <td><code>{{ event.event_uid_short }}</code></td>
            <td>{{ event.watch_group }}</td>
            <td>{{ event.rule_name }}</td>
            <td>{{ event.event_type }}</td>
            <td>{{ event.severity }}</td>
            <td>{{ event.created_at }}</td>
          </tr>
{% endfor %}
{% if length(events) == 0 %}
          <tr>
            <td colspan="6" class="text-center text-muted">No events recorded yet</td>
          </tr>
{% endif %}
        </tbody>
      </table>
    </div>
  </div>
</body>
</html>)html";

// ── Error page (500) ────────────────────────────────────────────────────

inline constexpr std::string_view kErrorTemplate = R"html(<!DOCTYPE html>
<html lang="en">
<head>
  <title>Kairos — Error</title>
  {{ head }}
</head>
<body>
  {{ nav }}
  <div class="container-fluid">
    <div class="card p-4 text-center">
      <h3 style="color: #ff6b6b;">{{ error_title }}</h3>
      <p class="text-muted">{{ error_message }}</p>
      <a href="/" class="btn btn-outline-info btn-sm">Return to Dashboard</a>
    </div>
  </div>
</body>
</html>)html";

// ── Helpers ─────────────────────────────────────────────────────────────

/// Map run status to Bootstrap badge class suffix.
/// Returns one of: success, danger, primary, warning, secondary.
inline std::string status_to_badge(const std::string& status) {
    if (status == "SUCCESS") return "success";
    if (status == "FAILURE") return "danger";
    if (status == "RUNNING") return "primary";
    if (status == "CANCELLED") return "warning";
    return "secondary";
}

/// Safely truncate a string to max_len characters.
inline std::string truncate(const std::string& s, size_t max_len = 12) {
    if (s.size() <= max_len) return s;
    return s.substr(0, max_len);
}

}  // namespace kairos::http::templates
