/* ==========================================================================
 * Kairos Web UI — YAML editor behaviors (Roadmap §3.8.3)
 * --------------------------------------------------------------------------
 * Dependency-free editor over a <textarea>: live validation (debounced),
 * save via the write API, revert. CodeMirror-style highlighting can be
 * layered on later; everything here works offline with zero assets.
 *
 * Expected DOM (server-rendered by edit.html):
 *   #yaml-editor          — the textarea, carrying data attributes:
 *                           data-validate-url, data-save-url,
 *                           data-save-method, data-kind, data-name
 *   #btn-validate         — explicit validate button
 *   #btn-save             — save button
 *   #btn-revert           — revert button
 *   #editor-status        — status line (ok / errors)
 *   #editor-errors        — validation error list
 * ========================================================================== */
(function () {
  'use strict';

  var editor = document.getElementById('yaml-editor');
  if (!editor) return; // Not an editor page.

  var statusEl = document.getElementById('editor-status');
  var errorsEl = document.getElementById('editor-errors');
  var validateUrl = editor.getAttribute('data-validate-url');
  var saveUrl = editor.getAttribute('data-save-url');
  var saveMethod = editor.getAttribute('data-save-method') || 'POST';
  var kind = editor.getAttribute('data-kind') || 'workflow';

  function setStatus(text, cls) {
    if (!statusEl) return;
    statusEl.textContent = text;
    statusEl.className = 'small ' + (cls || 'text-muted');
  }

  function renderErrors(errors) {
    if (!errorsEl) return;
    errorsEl.innerHTML = '';
    (errors || []).forEach(function (e) {
      var li = document.createElement('li');
      li.className = 'text-muted';
      li.textContent = (e.path ? e.path + ': ' : '') + e.message;
      errorsEl.appendChild(li);
    });
  }

  function request(url, method, body) {
    return fetch(url, {
      method: method,
      headers: { 'Content-Type': 'text/yaml' },
      body: body
    }).then(function (res) {
      return res.json().then(function (j) {
        return { ok: res.ok, status: res.status, json: j };
      }).catch(function () {
        return { ok: res.ok, status: res.status, json: {} };
      });
    });
  }

  function validate() {
    if (!validateUrl) return Promise.resolve({ ok: true, json: {} });
    setStatus('validating…', 'text-muted');
    return request(validateUrl + '?kind=' + kind, 'POST', editor.value)
      .then(function (r) {
        if (r.ok && r.json.valid) {
          setStatus('valid ✓', 'sev-info');
          renderErrors([]);
          return r;
        }
        setStatus('invalid', 'sev-critical');
        renderErrors(r.json.errors || [{ message: r.json.error || 'invalid' }]);
        return r;
      })
      .catch(function () {
        setStatus('validation request failed', 'sev-critical');
        return { ok: false, json: {} };
      });
  }

  function save() {
    validate().then(function (r) {
      if (!r.ok) return;
      setStatus('saving…', 'text-muted');
      request(saveUrl, saveMethod, editor.value).then(function (res) {
        if (res.status === 201 || res.ok) {
          setStatus('saved ✓', 'sev-info');
          renderErrors([]);
          if (res.status === 201 && res.json.name) {
            // Newly created → jump to the entity's detail page.
            window.location.href =
              '/' + (kind === 'watch' ? 'watches' : 'workflows') +
              '/' + encodeURIComponent(res.json.name);
          } else {
            window.location.href =
              '/' + (kind === 'watch' ? 'watches' : 'workflows') +
              '/' + encodeURIComponent(
                  editor.getAttribute('data-name') || '');
          }
        } else {
          setStatus('save failed (' + res.status + ')', 'sev-critical');
          renderErrors(res.json.errors ||
                       [{ message: res.json.error || 'save failed' }]);
        }
      }).catch(function () {
        setStatus('save request failed', 'sev-critical');
      });
    });
  }

  var btnV = document.getElementById('btn-validate');
  if (btnV) btnV.addEventListener('click', validate);
  var btnS = document.getElementById('btn-save');
  if (btnS) btnS.addEventListener('click', save);
  var btnR = document.getElementById('btn-revert');
  if (btnR) btnR.addEventListener('click', function () { location.reload(); });

  // Debounced live validation while typing.
  var t = null;
  editor.addEventListener('input', function () {
    if (t) clearTimeout(t);
    t = setTimeout(validate, 800);
  });

  // Ctrl/Cmd+S saves.
  document.addEventListener('keydown', function (ev) {
    if ((ev.ctrlKey || ev.metaKey) && ev.key === 's') {
      ev.preventDefault();
      save();
    }
  });
})();
