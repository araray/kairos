/* ==========================================================================
 * Kairos Web UI — client behaviors
 * --------------------------------------------------------------------------
 *  - Theme system (§3.2): blue / dark / light, persisted in localStorage,
 *    first visit follows prefers-color-scheme, mirrored to Bootstrap's
 *    data-bs-theme for component compatibility.
 *  - Search filter (§3.3): the navbar search box filters rows of every
 *    table on the current page. Plain text matches case-insensitively;
 *    wrapping the query in slashes (/regex/) switches to regex mode.
 *    Press "/" anywhere to focus the search box.
 *  - Live logs (§3.6): if a <div data-kairos-sse="/sse/logs/..."> exists,
 *    stream SSE output into the nearest .log-panel.
 * ========================================================================== */
(function () {
  'use strict';

  /* ---- Theme ----------------------------------------------------------- */

  var THEMES = ['blue', 'dark', 'light'];
  var STORAGE_KEY = 'kairos-theme';

  function applyTheme(theme) {
    if (THEMES.indexOf(theme) === -1) theme = 'blue';
    var html = document.documentElement;
    html.setAttribute('data-theme', theme);
    // Bootstrap 5.3 color modes: blue/dark are dark-ish, light is light.
    html.setAttribute('data-bs-theme', theme === 'light' ? 'light' : 'dark');
    var btn = document.getElementById('theme-toggle');
    if (btn) btn.textContent = '◐ ' + theme;
  }

  function currentTheme() {
    var t = document.documentElement.getAttribute('data-theme');
    return THEMES.indexOf(t) === -1 ? 'blue' : t;
  }

  function initTheme() {
    var saved = null;
    try { saved = localStorage.getItem(STORAGE_KEY); } catch (e) { /* private mode */ }
    if (saved) {
      applyTheme(saved);
    } else if (window.matchMedia &&
               window.matchMedia('(prefers-color-scheme: light)').matches) {
      applyTheme('light');
    } else {
      applyTheme('blue');
    }
    var btn = document.getElementById('theme-toggle');
    if (btn) {
      btn.addEventListener('click', function () {
        var next = THEMES[(THEMES.indexOf(currentTheme()) + 1) % THEMES.length];
        try { localStorage.setItem(STORAGE_KEY, next); } catch (e) { /* ignore */ }
        applyTheme(next);
      });
    }
  }

  /* ---- Table search filter ---------------------------------------------- */

  var FILTER_TIMEOUT = null;
  var FILTER_DELAY_MS = 150; // debounce (§3.3)

  function compileFilter(query) {
    query = query.trim();
    if (!query) return null;
    if (query.length > 1 && query.charAt(0) === '/' &&
        query.charAt(query.length - 1) === '/') {
      try {
        var re = new RegExp(query.slice(1, -1), 'i');
        return { test: function (s) { return re.test(s); }, mode: 'regex' };
      } catch (e) {
        return { test: function () { return true; }, mode: 'invalid' };
      }
    }
    var needle = query.toLowerCase();
    return {
      test: function (s) { return s.toLowerCase().indexOf(needle) !== -1; },
      mode: 'text'
    };
  }

  function applyFilter() {
    var input = document.getElementById('kairos-search-input');
    if (!input) return;
    var filter = compileFilter(input.value);
    var shown = 0, total = 0;

    document.querySelectorAll('table').forEach(function (table) {
      if (table.closest('[data-kairos-nofilter]')) return;
      var rows = table.querySelectorAll('tbody tr');
      rows.forEach(function (row) {
        if (row.querySelector('td[colspan]')) return; // placeholder rows
        total++;
        var text = row.textContent || '';
        var visible = !filter || filter.test(text);
        row.style.display = visible ? '' : 'none';
        if (visible) shown++;
      });
    });

    var count = document.getElementById('kairos-search-count');
    if (count) {
      if (filter && filter.mode === 'invalid') {
        count.textContent = 'invalid regex';
      } else if (filter) {
        count.textContent = shown + '/' + total;
      } else {
        count.textContent = '';
      }
    }
  }

  function initSearch() {
    var input = document.getElementById('kairos-search-input');
    if (!input) return;
    input.addEventListener('input', function () {
      if (FILTER_TIMEOUT) clearTimeout(FILTER_TIMEOUT);
      FILTER_TIMEOUT = setTimeout(applyFilter, FILTER_DELAY_MS);
    });
    // Press "/" anywhere (outside a text field) to jump to search.
    document.addEventListener('keydown', function (ev) {
      if (ev.key === '/' && document.activeElement !== input &&
          !/^(INPUT|TEXTAREA|SELECT)$/.test(document.activeElement.tagName)) {
        ev.preventDefault();
        input.focus();
      }
      if (ev.key === 'Escape' && document.activeElement === input) {
        input.value = '';
        applyFilter();
        input.blur();
      }
    });
  }

  /* ---- Live log streaming (SSE) ------------------------------------------ */

  function appendLogLine(panel, meta, stream, content) {
    var line = document.createElement('div');
    line.className = 'log-line ' + (stream === 'stderr' ? 'log-stderr' : 'log-stdout');
    if (meta) {
      var m = document.createElement('span');
      m.className = 'log-meta';
      m.textContent = '[' + meta + '] ';
      line.appendChild(m);
    }
    line.appendChild(document.createTextNode(content));
    panel.appendChild(line);
  }

  function initSSE() {
    var marker = document.querySelector('[data-kairos-sse]');
    if (!marker || !window.EventSource) return;
    var url = marker.getAttribute('data-kairos-sse');
    var panel = marker.querySelector('.log-panel') ||
                document.querySelector('.log-panel');
    if (!panel) return;

    var es = new EventSource(url);
    es.onmessage = function (ev) {
      try {
        var chunk = JSON.parse(ev.data);
        appendLogLine(panel, chunk.job_id || '', chunk.stream || 'stdout',
                      chunk.content || '');
        panel.scrollTop = panel.scrollHeight;
      } catch (e) { /* malformed chunk — ignore */ }
    };
    es.addEventListener('complete', function (ev) {
      try {
        var d = JSON.parse(ev.data);
        appendLogLine(panel, '', 'stderr', '— run ' + (d.status || 'completed') + ' —');
      } catch (e) { /* ignore */ }
      es.close();
    });
    es.onerror = function () { es.close(); };
  }

  /* ---- Boot -------------------------------------------------------------- */

  document.addEventListener('DOMContentLoaded', function () {
    initTheme();
    initSearch();
    initSSE();
  });
})();
