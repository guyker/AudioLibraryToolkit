# UI-F-001: Independent desktop workflows

Status: experimental

## Behaviour
Forms expose conversion, scan, duplicate review, database import and repair through a separately selected executable.
First-run Settings supports bounded executable discovery and choosing whether to load an existing backend
configuration or create an editable configuration from the backend's shipped template/defaults. Desktop connection preferences remain separate from the backend's
per-user or explicitly selected configuration file. Grouped settings edit backend-serialized values through
revision-checked worker operations; the backend preserves comments/unknown keys and atomically saves validated
changes. Operation forms use backend library/output defaults without sending them as overrides, and catalog inputs
remain distinct from audio-library paths. Duplicate review and SQLite import prefill the catalog directory produced
by a worker scan of the selected source.
Duplicate review also suggests a fresh report filename under the output root for each run. Manual path edits are
preserved; suggesting paths writes nothing.
Conversion/repair display previews and require confirmation; MP3 offers per-issue tag choices.
Duplicate reports expose individual album selection and a quarantine confirmation.
Progress, the latest output line, **View logs** and the stop action (while running) stay in a compact status strip.
Logs open on demand in a same-window overlay or a separate log window; long lines scroll horizontally at a readable
monospace size. Both views show the latest 200,000 characters for the current or most recent operation.
Logs never open automatically; previews and failures point to them from the status message.

## Entry points
- `apps/desktop/desktop/main.mjs` launches/manages the Node client.
- `apps/desktop/renderer/src/App.tsx` owns user interaction.
- `packages/worker-client/src/index.ts` owns process/protocol communication.

## Design and knowledge
- [DESIGN](../../DESIGN.md): interaction contracts and independent delivery.
- [UI-K-001](../knowledge/UI-K-001-confirm-individual-quarantine-selections.md): explicit selection/confirmation.
- [Public worker protocol](../../../docs/api/worker-protocol-v1.md): external capabilities, not private backend code.

## Acceptance checks
- [x] `npm test` and `npm run build` succeed without backend source.
- [x] An unpacked Windows package includes the renderer and Node client but no native toolkit executable or
      toolkit FFmpeg SDK/tools.
- [ ] Setup selects a separately installed executable; missing/incompatible executables are explicit errors.
- [ ] Discovery accepts only executables that answer the supported worker handshake and never scans the full disk.
- [ ] Existing config loads without copying; missing config is not created until the user explicitly creates it.
- [ ] Default creation uses the backend template, leaves library/output paths unset, and refuses to overwrite a file.
- [ ] Invalid edits and stale revisions leave the backend configuration unchanged; comments and unknown keys survive saves.
- [ ] Backend settings are grouped and validated by the worker; catalog and audio-library source paths stay separate.
- [x] Duplicate/import catalog inputs default to the selected source's scan output; duplicate reports get unique
      output-root filenames without creating files. Refreshing defaults does not replace manually edited paths.
- [ ] Conversion/repair require preview then execute; quarantine sends only confirmed selected album paths.
- [ ] Stopping/failed work never displays unverified success or promises rollback.
- [x] No docked console; the status strip shows progress, the latest output line, **View logs** and **Stop worker**.
- [x] **View logs** opens an overlay (Escape/backdrop close, copy, follow latest); **Open in new window** shows the same
      operation log in a single auxiliary window that closes with the main window. Logs never open automatically.

## Known limitations
- Experimental client; packaging and window smoke checks depend on a supported host.
- Backend/native prerequisites must be installed separately.
- Electron's bundled Chromium media-codec runtime is separate from the toolkit's FFmpeg SDK/tools.
- Textual previews are not persisted plan tokens.
