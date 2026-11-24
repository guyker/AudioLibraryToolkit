---
id: K-073
kind: decision
title: "Treat explicit desktop album selections as quarantine authorization"
area: [AudioLibraryToolkit]
status: archived
supersedes: [K-055]
created: 2026-11-24
reviewed: 2026-11-24
cites:
  - path: AudioLibraryToolkit/src/app/AudioLibraryToolkitMain.cpp
    symbol: ScanFolderProcessJSONAndFindDuplicates
  - path: AudioLibraryToolkit/src/app/WorkerProtocol.cpp
    symbol: "WorkerProtocol::Run"
  - path: AudioLibraryToolkit.Desktop/apps/desktop/renderer/src/App.tsx
    symbol: quarantineSelectedAlbums
---

**Fact:** The desktop renderer requires explicit album selection and a confirmation dialog before sending quarantine
paths. `WorkerProtocol::Run` accepts only an array of album paths; the duplicate flow recomputes current groups,
validates each selected album, preserves at least one album and direct-match connectivity, then delegates each
selected move to `DuplicateReview::ReviewGroup` for verified copy-before-removal. Unspecified albums are never moved.
The menu continues to require the exact `MOVE` confirmation.

**Why it matters:** Desktop clients need a native selection and confirmation interaction rather than terminal text,
without weakening the rule that each moved album must be explicitly chosen and revalidated.

**Rejected:** A blanket approval to move a whole group could quarantine an album the user meant to keep. Requiring
every desktop client to synthesize or prompt for the menu's exact terminal word would couple the UI to the menu
interaction instead of its own explicit album-selection control.
