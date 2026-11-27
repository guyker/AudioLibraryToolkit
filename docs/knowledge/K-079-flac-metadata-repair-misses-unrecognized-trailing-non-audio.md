---
id: K-079
kind: defect
title: "FLAC metadata repair misses unrecognized trailing non-audio bytes"
area: [AudioLibraryToolkit]
status: active
created: 2026-11-28
reviewed: 2026-11-28
cites:
  - path: AudioLibraryToolkit/src/repair/RepairRules.cpp
    symbol: analyzeFlac
  - path: AudioLibraryToolkit/src/conversion/MediaConversionTask.cpp
    symbol: runFFmpeg
---

**Fact:** `analyzeFlac` takes the audio range through EOF, excluding only recognized trailing tags; it does not
parse FLAC frames. A native FLAC with zero bytes appended after its complete, CRC-valid final frame can therefore
produce no findings and `no-repairs-needed`, while `runFFmpeg` conversion fails under `-xerror` with invalid sync
code/frame header (exit code -1094995529). Direct reproduction showed that all declared samples and the stored
STREAMINFO PCM MD5 can still match despite that failure. Removing only the independently verified non-audio
suffix from a separate copy restored strict decoding without changing metadata or compressed audio bytes.

**Why it matters:** Metadata-repair success is not a FLAC integrity certificate. Report-publication fixes and
clearing a retained output marker do not address this input defect. Do not remove trailing zeros heuristically:
valid frame payloads and checksums can contain zeros. Automated recovery needs a proven frame boundary and full
audio-integrity verification; disabling conversion's strict decoder errors can conceal real audio damage.
