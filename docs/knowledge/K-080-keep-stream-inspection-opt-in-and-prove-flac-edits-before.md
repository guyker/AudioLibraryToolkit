---
id: K-080
kind: decision
title: "Keep stream inspection opt-in and prove FLAC edits before publication"
area: [AudioLibraryToolkit]
status: active
created: 2026-11-28
reviewed: 2026-11-28
cites:
  - path: AudioLibraryToolkit/src/repair/RepairIntegrity.cpp
    symbol: PrepareProof
  - path: AudioLibraryToolkit/src/repair/RepairIntegrity.cpp
    symbol: VerifyProof
  - test: ConversionTests.test_repair_integrity_proves_only_the_unframed_flac_suffix
---

**Fact:** Metadata-only success says nothing about frames. Option 7 leaves integrity and full decode off unless
selected. A requested FLAC stream edit is a proposal, not authority to write: the staged result must match the
frame walk, every unchanged byte range and the original decoded program. A failing source decode is admissible
only for an isolated post-frame fill with independent **original**, nonzero sample-count and PCM MD5 witnesses.

**Why it matters:** Requiring both original and repaired files to decode cleanly would make the demonstrated
suffix failure unrepairable. Accepting just a successful output decode would also accept genuine sample loss.
An MD5 filled during the repair cannot be used to authenticate its own input.

**Rejected:** Searching backwards for zeros or sync words can discard a real frame or its checksum. Re-encoding
or concealing errors changes the evidence. Always decoding would change the cost and meaning of legacy runs;
rewriting MP3 VBR counts or injecting a header can change gapless playback even when music frames are untouched.
