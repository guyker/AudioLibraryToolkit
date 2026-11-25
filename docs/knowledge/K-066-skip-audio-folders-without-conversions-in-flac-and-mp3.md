---
id: K-066
kind: decision
title: "Skip audio folders without conversions in FLAC and MP3 output"
area: [AudioLibraryToolkit]
status: active
supersedes: [K-036]
created: 2026-11-23
reviewed: 2026-11-25
cites:
  - path: AudioLibraryToolkit/src/conversion/FolderConvert.cpp
    symbol: FolderConvert.PrepareConversion
  - path: AudioLibraryToolkit/src/conversion/FolderConvert.cpp
    symbol: writeAudit
  - path: tests/audio_library_integration.py
    symbol: ConversionTests.test_flac_skips_unchanged_audio_folders_and_keeps_written_folders_complete
  - path: tests/audio_library_integration.py
    symbol: ConversionTests.test_mp3_omits_folders_with_audio_but_no_flac
---

**Fact:** `FolderConvert::PrepareConversion` counts files, audio and conversions per source subtree for both
targets (FLAC: every convertible file; MP3: `.flac` files only). The topmost subtrees that contain audio but no
conversions are omitted from output with all their files and recorded in `skippedUnchangedDirectories` of
`conversion-audit.json`. FLAC without any conversion writes only its report (`"."`); MP3 without FLAC writes
nothing. In FLAC every other folder is written complete: unsupported audio beside conversions and audio-free
folders (artwork, scans, empty folders) are copied unchanged. In MP3 non-audio companions outside skipped subtrees
are copied and non-FLAC audio is never copied. Folder names are never changed. The cited integration cases verify
output inventories, skipped lists and unchanged sources.

**Why it matters:** The user does not want album folders in the output that contain no converted audio (for example
MP3-only albums appearing in MP3 output with only artwork), nor marker files inside the library. Untouched albums
are absent and discoverable through the report. The rule is audio-based so `Album\Artwork` and other audio-free
subfolders stay with a converted album. Known limitation: an MP3-only `CD2` next to a converted `CD1` is skipped
and only listed.

**Rejected:** `COPIED_UNCHANGED.txt` markers inside copied folders (clutter); copying unchanged folders into a
separate tree (splits libraries); renaming/prefixing folders (breaks cue/playlist references); skipping every
subtree without conversions (drops artwork and empty folders of converted albums); silently dropping unsupported
audio beside FLAC conversions (incomplete albums); copying companions of MP3-only folders in MP3 output (user
rejected albums without converted audio).
