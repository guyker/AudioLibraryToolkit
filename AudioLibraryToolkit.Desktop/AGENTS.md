# Desktop module

This npm workspace owns the Electron desktop client and Node worker-client package. Read `README.md`, `DESIGN.md`,
and `docs/features/index.md` before changes. Its own guides and knowledge live under this module's `docs/`.

## Boundaries

- Consume the toolkit only through the published worker protocol and an explicitly selected executable.
- Do not import backend source/headers, invoke CMake from npm, or make the toolkit discover this module.
- Keep rendering, settings, dialogs, confirmation and packaging here. Backend safety remains authoritative.
- Keep the Node client package independent of React/Electron. Expose only narrow preload APIs to the renderer.
- Backend API/schema changes belong to the toolkit; client-only changes update only this module's documentation.
- Root C++ feature instructions are toolkit-specific, not desktop naming/build conventions.

## Build and knowledge

From this module: `npm test`, `npm run build`, `npm run dev`. See `docs/guides/development.md` for installation and
packaging. Never test with a user's media library.

Use the installed Project Memory tool, not a copied script. From this module:

```powershell
& "$HOME\.copilot\skills\project-memory\scripts\kb.ps1" search "worker" -Root "$PWD"
& "$HOME\.copilot\skills\project-memory\scripts\kb.ps1" affected -Root "$PWD"
& "$HOME\.copilot\skills\project-memory\scripts\kb.ps1" check -Root "$PWD"
```

`-Root` is required: otherwise Git selects the repository/toolkit knowledge base. New entries use `UI-K-` IDs,
module-relative citations and areas from this module's `kb.config.json`. Preserve entry facts/history and lifecycle;
record only durable decisions/risks. Do not stage or commit unless asked.
