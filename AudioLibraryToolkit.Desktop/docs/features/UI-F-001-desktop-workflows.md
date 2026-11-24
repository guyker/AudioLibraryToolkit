# UI-F-001: Independent desktop workflows

Status: experimental

## Behaviour
Forms expose conversion, scan, duplicate review, database import and repair through a separately selected executable.
Settings can request new configuration creation. Conversion/repair display previews and require confirmation;
MP3 offers per-issue tag choices. Duplicate reports expose individual album selection and a quarantine confirmation.
Progress, output and errors are displayed; users can stop the worker.

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
- [ ] Conversion/repair require preview then execute; quarantine sends only confirmed selected album paths.
- [ ] Stopping/failed work never displays unverified success or promises rollback.

## Known limitations
- Experimental client; packaging and window smoke checks depend on a supported host.
- Backend/native prerequisites must be installed separately.
- Electron's bundled Chromium media-codec runtime is separate from the toolkit's FFmpeg SDK/tools.
- Textual previews are not persisted plan tokens.
