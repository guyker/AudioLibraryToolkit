---
id: K-011
kind: trap
title: "Use SQLite's long-path VFS for Windows database exports"
area: [AudioLibraryToolkit]
status: superseded
supersedes: [K-008]
superseded_by: K-042
created: 2026-10-10
reviewed: 2026-11-10
cites:
  - path: AudioLibraryToolkit/src/catalog/AlbumCollection.cpp
    symbol: "AlbumCollection::ExportToDatabase"
---

**Fact:** `AlbumCollection::ExportToDatabase` opens the UTF-8 database path through SQLite's `win32-longpath` VFS on Windows; other platforms use SQLite's UTF-8 open API.

**Why it matters:** Keep Unicode database names intact and preserve long-path support when changing SQLite's platform-specific file access.
