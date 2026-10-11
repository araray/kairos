/* ==========================================================================
 * Kairos Web UI — KEL playground (Roadmap §3.8.7)
 * --------------------------------------------------------------------------
 * Evaluate KEL expressions against the daemon (job()/has_tag() bindings)
 * with a localStorage-backed history. Dependency-free.
 * ========================================================================== */
(function () {
  'use strict';

  var input = document.getElementById('kel-expr');
  if (!input) return; // not the playground page

  var runBtn = document.getElementById('kel-run');
  var clearBtn = document.getElementById('kel-clear');
  var resultEl = document.getElementById('kel-result');
  var statusEl = document.getElementById('kel-status');
  var historyEl = document.getElementById('kel-history');
  var KEY = 'kairos-kel-history';

  function loadHistory() {
    try { return JSON.parse(localStorage.getItem(KEY)) || []; }
    catch (e) { return []; }
  }
  function saveHistory(h) {
    try { localStorage.setItem(KEY, JSON.stringify(h.slice(0, 15))); }
    catch (e) { /* ignore */ }
  }
  function renderHistory() {
    if (!historyEl) return;
    historyEl.innerHTML = '';
    loadHistory().forEach(function (expr) {
      var li = document.createElement('li');
      li.className = 'mb-1';
      var a = document.createElement('a');
      a.href = '#';
      a.textContent = expr;
      a.addEventListener('click', function (ev) {
        ev.preventDefault();
        input.value = expr;
        input.focus();
      });
      li.appendChild(a);
      historyEl.appendChild(li);
    });
  }

  function showLine(text, cls) {
    var line = document.createElement('div');
    line.className = 'log-line ' + (cls || 'log-stdout');
    line.textContent = text;
    resultEl.appendChild(line);
    resultEl.scrollTop = resultEl.scrollHeight;
  }

  function evaluate() {
    var expr = input.value.trim();
    if (!expr) return;
    statusEl.textContent = 'evaluating…';
    fetch('/api/v1/kel/eval', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ expr: expr })
    }).then(function (res) { return res.json(); }).then(function (j) {
      statusEl.textContent = '';
      if (j.error) {
        showLine('✕ ' + j.error, 'log-stderr');
      } else {
        var val = typeof j.value === 'string'
          ? '"' + j.value + '"' : String(j.value);
        showLine('» ' + expr);
        showLine(val + '   (' + j.type + ')', 'log-stdout');
        var h = loadHistory().filter(function (e) { return e !== expr; });
        h.unshift(expr);
        saveHistory(h);
        renderHistory();
      }
    }).catch(function () {
      statusEl.textContent = '';
      showLine('request failed', 'log-stderr');
    });
  }

  runBtn.addEventListener('click', evaluate);
  clearBtn.addEventListener('click', function () {
    input.value = '';
    resultEl.innerHTML = '';
    input.focus();
  });
  input.addEventListener('keydown', function (ev) {
    if (ev.key === 'Enter' && (ev.ctrlKey || ev.metaKey)) {
      ev.preventDefault();
      evaluate();
    }
  });

  renderHistory();
  input.focus();
})();
