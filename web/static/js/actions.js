/* ==========================================================================
 * Kairos Web UI — declarative action buttons (Roadmap §3.8.5)
 * --------------------------------------------------------------------------
 * Buttons carry data attributes; this file binds the behavior:
 *
 *   data-kairos-action="run-workflow"  data-name="wf-name"
 *       → POST /api/v1/workflows/{name}/run → open the run page
 *   data-kairos-action="cancel-run"    data-run="run-id"
 *       → POST /api/v1/runs/{id}/cancel (with confirm) → refresh
 *   data-kairos-action="delete-entity" data-kind="workflow|watch"
 *                                      data-name="x"
 *       → DELETE /api/v1/{workflows|watches}/{name} (with confirm)
 *         → redirect to the list page
 * ========================================================================== */
(function () {
  'use strict';

  function post(url) {
    return fetch(url, { method: 'POST' })
      .then(function (res) { return res.json(); });
  }

  document.addEventListener('DOMContentLoaded', function () {
    document.addEventListener('click', function (ev) {
      var btn = ev.target.closest('[data-kairos-action]');
      if (!btn) return;
      var action = btn.getAttribute('data-kairos-action');

      if (action === 'run-workflow') {
        var name = btn.getAttribute('data-name');
        btn.disabled = true;
        btn.textContent = 'starting…';
        post('/api/v1/workflows/' + encodeURIComponent(name) + '/run')
          .then(function (j) {
            if (j.run_id) {
              window.location.href = '/runs/' + j.run_id;
            } else {
              // Submitted but run id not resolved yet — show history.
              window.location.href = '/runs?workflow=' +
                encodeURIComponent(name);
            }
          })
          .catch(function () {
            btn.disabled = false;
            btn.textContent = 'Run now';
          });
      } else if (action === 'cancel-run') {
        var run = btn.getAttribute('data-run');
        if (!window.confirm('Cancel run ' + run + '?')) return;
        btn.disabled = true;
        post('/api/v1/runs/' + encodeURIComponent(run) + '/cancel')
          .then(function () { location.reload(); });
      } else if (action === 'delete-entity') {
        var kind = btn.getAttribute('data-kind'); // workflow | watch
        var target = btn.getAttribute('data-name');
        if (!window.confirm('Delete ' + kind + ' "' + target +
                            '"? A .bak backup is kept on disk.')) return;
        btn.disabled = true;
        fetch('/api/v1/' + (kind === 'watch' ? 'watches' : 'workflows') +
              '/' + encodeURIComponent(target), { method: 'DELETE' })
          .then(function () {
            window.location.href =
              '/' + (kind === 'watch' ? 'watches' : 'workflows');
          });
      }
    });

    // Clickable tag pills on list pages (§3.5): feed the global search
    // filter so the visible table narrows to that tag.
    document.addEventListener('click', function (ev) {
      var tag = ev.target.closest('.kairos-tag[data-filter-tag]');
      if (!tag) return;
      var input = document.getElementById('kairos-search-input');
      if (!input) return;
      input.value = tag.getAttribute('data-filter-tag');
      input.dispatchEvent(new Event('input', { bubbles: true }));
      input.focus();
    });
  });
})();
