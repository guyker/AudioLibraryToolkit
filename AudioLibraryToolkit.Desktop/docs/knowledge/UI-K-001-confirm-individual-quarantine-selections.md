---
id: UI-K-001
kind: decision
title: "Confirm individual quarantine selections"
area: [Desktop]
status: active
created: 2026-11-24
reviewed: 2026-11-26
cites:
  - path: apps/desktop/renderer/src/App.tsx
    symbol: quarantineSelectedAlbums
---

**Fact:** `quarantineSelectedAlbums` requires a non-empty user-selected album list and a confirmation dialog before
passing those paths to the external worker. No default selection or automatic preferred-edition choice is made.

**Why it matters:** Duplicate candidates are not authorization to relocate an edition. Desktop-native confirmation
must preserve individual selection without coupling the client to a terminal prompt.

**Rejected:** Blanket group approval or automatic quality ranking can move an album the user intended to keep.
