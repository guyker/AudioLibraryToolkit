---
id: K-058
kind: decision
title: "Fail source scans on symlinks and reparse points"
area: [AudioLibraryToolkit]
status: active
supersedes: [K-057]
created: 2026-11-22
reviewed: 2026-11-24
cites:
  - path: AudioLibraryToolkit/src/catalog/AlbumCollection.cpp
    symbol: "AlbumCollection::LoadAlbumsCo"
  - path: AudioLibraryToolkit/src/conversion/FolderConvert.cpp
    symbol: "FolderConvert::PrepareConversion"
  - path: AudioLibraryToolkit/src/repair/AudioRepair.cpp
    symbol: "AudioRepair::Prepare"
---

**Fact:** `FolderConvert::PrepareConversion`, `AlbumCollection::LoadAlbumsCo`, and `AudioRepair::Prepare` reject a
source root or discovered source entry that is a symlink or Windows reparse point, including regular-file links.
They fail during preparation instead of following a link, silently skipping it, or treating its target as copied
content. Option 2's separate `RecursionDirectorySearchDepth` policy still skips over-depth directories with a warning.

**Why it matters:** The same library should not expose different file inventories across conversion, catalog scan,
and repair, and no workflow should unexpectedly read content outside the configured source tree.

**Rejected:** Following links can escape the library or duplicate content; skipping links makes a run appear complete
while omitting files. Depth-limited scan folders remain a distinct, configured scan behavior rather than a link policy.
