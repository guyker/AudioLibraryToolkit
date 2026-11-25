---
id: K-076
kind: decision
title: "Treat source libraries as strictly read-only; duplicate quarantine copies only"
area: [AudioLibraryToolkit]
status: active
supersedes: [K-052]
created: 2026-11-25
reviewed: 2026-11-25
cites:
  - path: AudioLibraryToolkit/src/duplicates/DuplicateReview.cpp
    symbol: copyAlbumToQuarantine
  - path: AudioLibraryToolkit/src/duplicates/DuplicateReview.cpp
    symbol: "DuplicateReview::ReviewPair"
  - path: AudioLibraryToolkit/include/platform/PlatformUtils.h
    symbol: "ScopedCurrentDirectory::EnterValidatedDirectory"
---

**Fact:** The user requires that no option, command, worker request or failure path creates, modifies, renames or
deletes anything inside a source library. Temporary and generated files go only to the output tree or system temp,
after overlap validation. Duplicate quarantine (`copyAlbumToQuarantine`, reached from `DuplicateReview::ReviewPair`
and `ReviewGroup`) therefore copies the whole album to `OutDirectory/quarantine`, verifies it byte-for-byte, rechecks
the source, and never removes originals; confirmation is exact `COPY` per album. Tests assert source snapshots are
unchanged. A reported `.afc-convert-*` file and `.conversion-in-progress` folder were in the conversion OUTPUT of a
hard-killed desktop worker, not in the source.

**Why it matters:** Source libraries are the user's only authoritative copies. Any write (even a temporary or
marker) breaks the trust contract and can corrupt or clutter archives; deletion after a candidate match risks losing
an edition the user wanted.

**Rejected:** Verified copy-then-remove quarantine (K-052) still deletes source content. Writing temporaries or
markers beside source files was rejected; output or system temp is always available.
