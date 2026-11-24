---
id: K-067
kind: decision
title: "Write and verify mapped MP3 tags through TagLib"
area: [AudioLibraryToolkit]
status: active
created: 2026-11-23
reviewed: 2026-11-24
cites:
  - test: ConversionTests.test_mp3_taglib_preserves_repeated_date_values_without_sidecar
  - path: AudioLibraryToolkit/src/metadata/Mp3TagMetadata.cpp
    symbol: Mp3TagMetadata.WriteFromFlac
  - path: AudioLibraryToolkit/src/metadata/Mp3TagMetadata.cpp
    symbol: Mp3TagMetadata.Read
  - path: AudioLibraryToolkit/src/metadata/PreservationAudit.cpp
    symbol: PreservationAudit.Compare
---

**Fact:** `Mp3TagMetadata::WriteFromFlac` maps FLAC comments into ID3v2.4 frames and `Mp3TagMetadata::Read` reads those
frames back for `PreservationAudit::Compare`. Repeated `DATE`/`YEAR` values are combined into a verifiable date property;
the sidecar setting remains independent, and metadata not represented in ID3 still requires a verified sidecar.

**Why it matters:** FFmpeg's simplified metadata dictionary cannot establish ID3 frame multiplicity. Keeping the
TagLib read/write stage allows representable repeated tags to pass verification without forcing sidecar creation,
while fail-closed checks still protect non-transferable FLAC metadata.

**Rejected:** Relying on FFmpeg dictionaries alone leaves repeated ID3 values unverified. Requiring sidecars for every
MP3 would change the existing opt-in sidecar policy and create archives even when all relevant tags are represented
in ID3.
