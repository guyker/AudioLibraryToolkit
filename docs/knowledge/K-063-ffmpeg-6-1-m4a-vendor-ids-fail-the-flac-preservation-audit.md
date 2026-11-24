---
id: K-063
kind: defect
title: "FFmpeg 6.1 M4A vendor IDs fail the FLAC preservation audit"
area: [AudioLibraryToolkit]
status: active
created: 2026-11-22
reviewed: 2026-11-24
cites:
  - test: ConversionTests.test_aac_m4a_at_cd_sample_rate_converts_with_lossy_warning
  - test: ConversionTests.test_m4a_default_stream_tags_do_not_fail_flac_preservation_audit
  - path: AudioLibraryToolkit/src/metadata/PreservationAudit.cpp
    symbol: isGeneratedMp4Tag
---

**Fact:** On Ubuntu 24.04 with FFmpeg 6.1.1, generated AAC and ALAC M4A fixtures expose a default `vendor_id`
as `[0][0][0][0]`. `isGeneratedMp4Tag` recognizes `0x0000` but not that representation, so the FLAC
preservation audit reports `MissingOrChanged` for `vendor_id` and conversion fails. This also occurs on the
unchanged baseline; source files remain unchanged.

**Why it matters:** Valid M4A inputs can fail on the documented Linux build despite passing the source-quality gate.
A fix must distinguish generated fields without weakening preservation checks for meaningful metadata.
