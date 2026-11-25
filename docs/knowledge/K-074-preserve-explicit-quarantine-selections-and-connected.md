---
id: K-074
kind: decision
title: "Preserve explicit quarantine selections and connected survivors for all clients"
area: [AudioLibraryToolkit]
status: superseded
supersedes: [K-055]
superseded_by: K-077
created: 2026-11-24
reviewed: 2026-11-24
cites:
  - path: AudioLibraryToolkit/src/app/AudioLibraryToolkitMain.cpp
    symbol: ScanFolderProcessJSONAndFindDuplicates
  - path: AudioLibraryToolkit/src/duplicates/DuplicateReview.cpp
    symbol: "DuplicateReview::ReviewGroup"
---

**Fact:** The headless duplicate operation accepts individual album paths, recomputes current groups, and refuses
selections that move the last album or disconnect the surviving direct-match graph. Each accepted path goes through
`DuplicateReview::ReviewGroup` and verified copy-before-removal. Detection alone authorizes no relocation.
The interactive adapter retains its exact `MOVE` prompt; other clients supply explicit paths through the public API.

**Why it matters:** Connected duplicate components are not cliques, and a candidate match does not establish which
edition should be kept. All adapters need the same backend safety independently of their confirmation interaction.

**Rejected:** Implicit whole-group moves can relocate an unselected edition. Allowing bridge removal disconnects
unrelated survivors; requiring a clique would reject otherwise safe removals.
