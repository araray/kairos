# Kairos Web UI

Templates and static assets for the Kairos web dashboard (Roadmap §3.1).

## Layout

```
web/
├── templates/            # inja (Jinja2-compatible) templates, two-pass rendered
│   ├── base.html         # shared layout: navbar, search bar, theme toggle, footer
│   ├── dashboard.html
│   ├── error.html
│   ├── workflows/        # list.html, detail.html
│   ├── jobs/             # list.html, detail.html
│   ├── runs/             # list.html, detail.html (SSE live logs)
│   ├── events/           # list.html, detail.html
│   ├── watches/          # list.html, detail.html
│   └── triggers/         # list.html
└── static/
    ├── css/kairos.css    # theme variables (blue/dark/light) + component styles
    ├── js/kairos.js      # theme toggle, table search filter, SSE log streaming
    ├── img/kairos-logo.svg
    └── vendor/           # vendored third-party assets (Bootstrap 5.3.3, MIT)
```

## How templates are loaded

1. **External override** — if `kairos.http.web_root` is set (or
   `<data_dir>/web/` exists), templates and static files are read from
   there on every request. Edit files, refresh, see changes — no rebuild.
2. **Embedded fallback** — everything under `web/` is compiled into the
   binary by `cmake/KairosWebResources.cmake` at build time, so the
   single-binary distribution always works with zero external files.

## Rendering model

Pages are rendered in two passes: the page template produces an HTML
*content* block; `base.html` is then rendered with `{ title, active,
content, version }` and wraps it. There are no inja `include`s — the
wrapper approach keeps external/embedded loading identical.

## Themes

Three themes, all defined by CSS custom properties in `kairos.css`
(`[data-theme="blue|dark|light"]`), mirrored to Bootstrap 5 color modes
(`data-bs-theme`). The user's choice persists in `localStorage`
(`kairos-theme`); first visit follows `prefers-color-scheme`.

## Contributing

- Keep templates free of inline styles where a class/variable exists.
- All new pages must work in all three themes and support the navbar
  search filter (plain tables — the JS filters `tbody tr` automatically).
- Any new asset file must stay ASCII-safe (the CMake resource generator
  escapes and embeds file contents into a C++ source).
