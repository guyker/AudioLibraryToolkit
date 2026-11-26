---
id: UI-K-002
kind: decision
title: "Duplicate review reports first; quarantine is a separate confirmed copy"
area: [Desktop]
status: active
created: 2026-11-26
reviewed: 2026-11-26
cites:
  - path: apps/desktop/renderer/src/duplicateReview.mjs
    symbol: duplicateFindParameters
  - path: apps/desktop/desktop/album-folders.mjs
    symbol: createAlbumFolderGate
---

**Fact:** Duplicate review is two steps. **Find duplicate albums** sends no `duplicateQuarantinePaths`
(`duplicateFindParameters`), so it only writes a report; the worker treats a non-empty list as a copy-only
quarantine request. The report stays visible afterwards, including after a quarantine, and each album can be opened in
the file manager (`createAlbumFolderGate` allows only albums of the latest validated report). Quarantine is a separate,
confirmed request. The app never deletes albums; the user deletes originals manually.

**Why it matters:** The intended workflow is: find, open each matching album's folder, then optionally delete an album
manually or copy it to quarantine. Earlier, the Find button re-sent the report's checked albums as a quarantine request
under a "sources are not modified" confirmation, and the report disappeared after a quarantine, so the albums could no
longer be reviewed or opened.

**Rejected:** A single combined Find-and-quarantine action (a stale selection could copy albums unprompted). Opening
every album folder during the run (dozens of file-manager windows; kept only as an explicit option). Letting the
renderer open any path through the shell (an arbitrary-path launcher).
