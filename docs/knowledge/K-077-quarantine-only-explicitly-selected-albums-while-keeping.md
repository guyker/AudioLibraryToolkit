---
id: K-077
kind: decision
title: "Quarantine only explicitly selected albums while keeping survivors connected"
area: [AudioLibraryToolkit]
status: active
supersedes: [K-074]
created: 2026-11-25
reviewed: 2026-11-25
cites:
  - path: AudioLibraryToolkit/src/app/AudioLibraryToolkitMain.cpp
    symbol: ScanFolderProcessJSONAndFindDuplicates
  - path: AudioLibraryToolkit/src/duplicates/DuplicateReview.cpp
    symbol: "DuplicateReview::ReviewGroup"
---

**Fact:** The headless duplicate operation accepts individual album paths, recomputes current groups, and refuses
selections that quarantine the last album or disconnect the surviving direct-match graph. Each accepted path goes
through `DuplicateReview::ReviewGroup` and a verified copy to quarantine; the source is never changed (K-076).
Detection alone authorizes nothing. The interactive adapter requires exact `COPY`; other clients supply explicit paths.

**Why it matters:** Connected duplicate components are not cliques, and a candidate match does not establish which
edition should be kept. All adapters need the same backend safety independently of their confirmation interaction.

**Rejected:** Implicit whole-group quarantine can select an unselected edition. Allowing bridge selection disconnects
unrelated survivors; requiring a clique would reject otherwise safe selections.
