---
id: K-070
kind: defect
title: "Windows Release duplicate matching can lose artist evidence"
area: [AudioLibraryToolkit]
status: active
created: 2026-11-24
reviewed: 2026-11-24
cites:
  - path: AudioLibraryToolkit/src/duplicates/DuplicateMatcher.cpp
    symbol: "DuplicateMatcher::BuildProfile"
  - path: tests/duplicate_matcher.cpp
    symbol: nameConsensus
  - path: tests/duplicate_matcher.cpp
    symbol: identifiers
---

**Fact:** `DuplicateMatcher::BuildProfile` iterates nested `std::initializer_list` keys after their backing
arrays have expired. In Windows Release with MSVC 19.52, the unchanged baseline loses track-artist data:
the `nameConsensus` fixture is only a probable match, and the same-name, different-artist case in
`identifiers` can be reported as a duplicate.

**Why it matters:** Missing artist evidence can produce false duplicate candidates. Quarantine still requires
an explicit per-album choice and confirmation, but users may be asked to review unrelated albums.
