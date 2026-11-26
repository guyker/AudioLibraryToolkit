---
id: K-065
kind: decision
title: "Separate outputs by operation while preserving catalog workflows"
area: [AudioLibraryToolkit]
status: active
supersedes: [K-025, K-026, K-031]
created: 2026-11-23
reviewed: 2026-11-26
cites:
  - path: AudioLibraryToolkit/include/common/OutputLayout.h
    symbol: "OutputLayout::Catalog"
  - path: AudioLibraryToolkit/src/app/AudioLibraryToolkitMain.cpp
    symbol: configuredCatalogDirectory
  - path: AudioLibraryToolkit/src/app/AudioLibraryToolkitMain.cpp
    symbol: ConfigureManualMp3
---

**Fact:** `OutputLayout` gives menu and CLI operations separate output categories while retaining library names and
relative paths. `configuredCatalogDirectory` makes menu duplicate review and SQLite import read the new scan
location, falling back to the previous per-library scan directory with a warning only when the new directory is
absent. Current empty, corrupt or inaccessible catalogs never trigger fallback; batch sets are not mixed.
Existing outputs and databases are not moved or migrated. Conversion remains source-preserving; manual MP3 quality,
downmix and overwrite still default to 0/NO/NO, are prompted per operation and are not persisted.

**Why it matters:** Format-only folders do not distinguish normalized audio, metadata repair and catalogs.
Options 3 and 4 depend on option 2's complete batch set: renaming only writers would disconnect those workflows,
while falling back from a corrupt current catalog could silently process stale data.

**Rejected:** Removing operation grouping mixes different outputs. Menu-number folder names couple stored data to
UI numbering. Automatically renaming old outputs or migrating old databases mutates existing data; merging catalogs
or silently choosing an older valid scan hides current errors. Per-run timestamps for ordinary output would bypass
the existing destination-conflict workflow; only quarantine retains its unique per-album destination policy.
