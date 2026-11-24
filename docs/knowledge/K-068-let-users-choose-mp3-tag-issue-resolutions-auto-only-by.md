---
id: K-068
kind: decision
title: "Let users choose MP3 tag-issue resolutions, auto only by configuration"
area: [AudioLibraryToolkit]
status: active
created: 2026-11-23
reviewed: 2026-11-24
cites:
  - test: ConversionTests.test_cli_ask_tag_issues_stop_before_writing
  - path: AudioLibraryToolkit/src/metadata/Mp3TagIssues.cpp
    symbol: Mp3TagIssues.Resolve
  - path: AudioLibraryToolkit/src/app/AudioLibraryToolkitMain.cpp
    symbol: ResolveMenuTagIssues
  - path: AudioLibraryToolkit/src/conversion/FolderConvert.cpp
    symbol: FolderConvert.ConvertAudioFolderToOutput
  - path: AudioLibraryToolkit/src/metadata/PreservationAudit.cpp
    symbol: PreservationAudit.Compare
---

**Fact:** For MP3 conversion, `Mp3TagIssues::Detect` finds tag issues (empty values, numeric genres, embedded NUL, FLAC-only blocks, ReplayGain) during planning; empty values and FLAC-only blocks are ignored when a sidecar is written. With `TagIssueResolution: "ask"` (default) the menu (`ResolveMenuTagIssues`) asks once per issue type after the preview, recommendation first, and applies the choice to every affected file; the CLI cannot ask, so `ConvertAudioFolderToOutput` fails before writing with a summary. `"auto"` applies the recommendations. `PreservationAudit::Compare` treats only differences explained by a selected resolution as `Normalized`/`SourceOnly`; unresolved differences stay fatal.

**Why it matters:** Several issues have more than one valid fix (map or keep genre numbers, split or join NUL values, keep or remove ReplayGain), and the user explicitly wants to choose them. Silently fixing would change tags without consent; failing every such file made option 6 unusable on real libraries.

**Rejected:** Always auto-fixing (no user consent). Prompting per file (unusable on large libraries). A CLI default of auto (unattended runs would change tags silently). Relaxing the audit globally (would hide real losses; K-029 stays fail-closed for unresolved differences).
