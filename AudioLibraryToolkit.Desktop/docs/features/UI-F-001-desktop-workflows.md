# UI-F-001: Independent desktop workflows

Status: experimental

## Behaviour
Forms expose conversion, scan, duplicate review, database import and repair through a separately selected executable.
First-run Settings supports bounded executable discovery and choosing whether to load an existing backend
configuration or create an editable configuration from the backend's shipped template/defaults. Desktop connection preferences remain separate from the backend's
per-user or explicitly selected configuration file. Grouped settings edit backend-serialized values through
revision-checked worker operations; a save sends only the settings the user changed, and the backend preserves
comments/unknown keys (also inside nested objects; an edited list is replaced whole) and atomically saves validated
changes. Operation forms prefill and send the selected input path, including a configured default; catalog inputs
remain distinct from audio-library paths. An unchanged output root is omitted from ordinary requests, while a changed
output override is sent. Duplicate review and SQLite import prefill the catalog directory produced
by a worker scan of the selected source.
Duplicate review also suggests a fresh report filename under the output root for each run. Manual path edits are
preserved; suggesting paths writes nothing.
Conversion/repair display previews and require confirmation; MP3 offers per-issue tag choices.
Duplicate review is report-first. **Find duplicate albums** only writes a new report file: it never copies, moves or
deletes albums and never re-sends earlier checkbox selections. The report stays on screen and each matching album has
**Open folder** (each group **Open all folders**) to compare editions in the file manager; only albums of the current
report can be opened. The app never deletes albums: the user deletes unwanted editions manually, or selects them
individually (at least one album per group stays unmarked) and confirms **Quarantine**, a verified copy to
`<output root>/quarantine`. Copied albums are marked **Copied to quarantine** and the report remains visible.
Without an output root no report path is suggested; choose a report file to review the matches in the app.
Progress, the latest output line, **View logs** and the stop action (while running) stay in a compact status strip.
Logs open on demand in a same-window overlay or a separate log window; long lines scroll horizontally at a readable
monospace size. Both views show the latest 200,000 characters for the current or most recent operation.
Logs never open automatically; previews and failures point to them from the status message.

## Entry points
- `apps/desktop/desktop/main.mjs` launches/manages the Node client.
- `apps/desktop/renderer/src/App.tsx` owns user interaction.
- `packages/worker-client/src/index.ts` owns process/protocol communication.
- `apps/desktop/desktop/album-folders.mjs` allows opening only albums of the latest validated duplicate report.
- `apps/desktop/renderer/src/duplicateReview.mjs` and `configurationEditor.mjs` hold the duplicate request/selection
  rules and the settings patch logic as UI-independent, unit-tested modules.

## Design and knowledge
- [DESIGN](../../DESIGN.md): interaction contracts and independent delivery.
- [UI-K-001](../knowledge/UI-K-001-confirm-individual-quarantine-selections.md): explicit selection/confirmation.
- [UI-K-002](../knowledge/UI-K-002-duplicate-review-reports-first-quarantine-is-a-separate.md): Find only reports;
  quarantine is a separate confirmed copy.
- [Public worker protocol v1](https://github.com/guyker/AudioLibraryToolkit/blob/main/docs/api/worker-protocol-v1.md):
  external capabilities, not private backend code.

## Acceptance checks
- [x] `npm test` and `npm run build` succeed without backend source.
- [x] An unpacked Windows package includes the renderer and Node client but no native toolkit executable or
      toolkit FFmpeg SDK/tools.
- [ ] Setup selects a separately installed executable; missing/incompatible executables are explicit errors.
- [ ] Discovery accepts only executables that answer the supported worker handshake and never scans the full disk.
- [x] Packaged discovery never probes the repository build output; that candidate is limited to unpackaged development.
- [ ] Reading or validating a missing config creates nothing; saving edits from its `"missing"` snapshot creates a
      built-in-default config without a backup, while explicit Create uses the template when available and refuses overwrite.
- [ ] Default creation uses the backend template, leaves library/output paths unset, and refuses to overwrite a file.
- [ ] Invalid edits and stale revisions leave the backend configuration unchanged; comments and unknown keys survive saves.
- [ ] Backend settings are grouped and validated by the worker; catalog and audio-library source paths stay separate.
- [x] Duplicate/import catalog inputs default to the selected source's scan output; duplicate reports get unique
      output-root filenames without creating files. Refreshing defaults does not replace manually edited paths.
- [ ] Conversion/repair require preview then execute; quarantine sends only confirmed selected album paths.
- [x] **Find duplicate albums** sends no quarantine paths and its confirmation says no album is copied, moved or deleted;
      the report stays visible after a run and after a quarantine.
- [x] **Open folder**/**Open all folders** open only existing directories of albums in the latest validated report;
      other paths are refused. Nothing in the app deletes an album.
- [x] Quarantine names `<output root>/quarantine`, needs an output root, keeps one album per group (albums already
      copied count as marked) and marks copied albums.
- [x] After **Create with backend defaults** every JSON editor shows the file's value; a save sends only changed keys and
      leaves comments, unknown keys and layout unchanged.
- [ ] Stopping/failed work never displays unverified success or promises rollback.
- [x] No docked console; the status strip shows progress, the latest output line, **View logs** and **Stop worker**.
- [x] **View logs** opens an overlay (Escape/backdrop close, copy, follow latest); **Open in new window** shows the same
      operation log in a single auxiliary window that closes with the main window. Logs never open automatically.

## Known limitations
- Experimental client; packaging and window smoke checks depend on a supported host.
- Backend/native prerequisites must be installed separately.
- Electron's bundled Chromium media-codec runtime is separate from the toolkit's FFmpeg SDK/tools.
- Textual previews are not persisted plan tokens.
- Quarantine copies only and does not update catalog JSON or databases; rebuild the catalog after deleting originals.
