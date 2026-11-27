# F-001: Convert a library to 16-bit / 44.1 kHz FLAC

Status: active

## Behaviour
Conversions use the bundled FFmpeg runtime by default, with explicit absolute encoder overrides only (F-013).
Encoder preflight runs before conversion-output creation; failure never falls back to PATH.
Converts every supported audio file (`.flac .ape .dsf .dff .dsd .wv .wav .m2ts .m4a`) under a source folder to
16-bit / 44.1 kHz FLAC with triangular dither, in a separate tree `<root>/normalized-flac/<source folder name>/`. Every written
folder is complete: other regular files there are copied unchanged, including unsupported audio such as MP3/AAC/Ogg
beside converted tracks, and folders without audio (artwork, scans, empty folders) are retained. A folder subtree
containing audio but nothing to convert is skipped entirely and listed in `conversion-audit.json`; a library without
conversions writes only that report. Folder names are never changed. Native FLAC sources get an original-metadata
sidecar when enabled by `MetadataSidecars` (binary by default, JSON optional; both target policies default to
disabled, F-007).
Convertible sources below 16-bit or 44.1 kHz are rejected; they are not silently dropped or copied as fallback.
AAC `.m4a` sources are exempt only from the bit-depth check because AAC does not expose a meaningful source PCM bit
depth; they still require a verified sample rate of at least 44.1 kHz and produce a warning that lossy-to-FLAC
conversion cannot restore lost quality.
Preparation rejects internal `.afc-convert-<pid>-<time>-<n>[.<suffix>]` temporary files anywhere in the source tree,
before any conversion output is written. The diagnostic identifies the file and asks for manual review outside the
source tree; it does not delete or silently skip potentially recoverable output. This also applies to worker previews
and MP3 conversion. Other dotfiles and similar names without the generated numeric structure remain normal inputs.
Without `--overwrite`, existing targets are never replaced. Exit code 0 only if every planned item was preserved.
If planned destinations already exist, the console identifies the output folder and conflict count, explains that
it may contain previous run results, and suggests a different output root or explicit overwrite. Duplicate output
paths from different source items are reported separately and cannot be fixed by overwrite. Full conflict paths
are in the session log, not listed on the console (F-010). Unrelated files or an empty existing output folder alone
do not block conversion.
The console reports conversion start, overall and current-track progress, and completion, along with meaningful warnings/errors. Before
the preview, preparation reports per-library `Conversion discovery` progress (entries visited and convertible audio
found, total unknown) and `Conversion planning` progress (destinations checked of the known total, with ETA), updated
every second on an interactive console and every five seconds when redirected. Execution uses the same shared
updating line: overall installed-file counts span all selected libraries; the current track shows an encoding bar
and percentage, followed by verification/install stages. Real encoding measurements update the interactive line
immediately, without waiting for the one-second phase timer. Until the first measurement, the track shows
`Starting encoder`, not an artificial 0%; very short encodes can finish between FFmpeg reports.
Companion copying and final verification are identified
separately. Track encoding at 100% is not counted as installed until verification and installation succeed.
Long names are clipped to the terminal width rather than wrapping; redirected output uses periodic snapshots.
Before
converting tracks in a different folder, it shows the full source album folder path; root-level tracks and disc
subfolders use their own containing folder. It also signals when audio conversion ends and unchanged files are being
copied. Per-file success details and expected generated encoder/vendor normalization are available at debug level in
the session log (F-010). Tag aliases with unchanged values and selected tag fixes are file-only warnings; other
preservation differences remain visible warnings, and `conversion-audit.json` retains every
check (F-003).

## Entry points
- `AudioLibraryToolkit convert <input> [--output-dir] [--overwrite]` → `app/AudioLibraryToolkitMain.cpp#RunWithArguments` →
  `conversion/FolderConvert.cpp#FolderConvert::ConvertAudioFolderToOutput`
- Menu option 1 → `app/AudioLibraryToolkitMain.cpp#ConvertFLACToFLAC` → `app/AudioLibraryToolkitMain.cpp#ConvertConfiguredLibraries`
- Per file → `conversion/MediaConversionTask.cpp#MediaConversionTask::ConvertFile`, `conversion/MediaConversionTask.cpp#conversionArguments`

## Design and knowledge
- DESIGN: `AudioLibraryToolkit/DESIGN.md` → Key flows / Conversion; Contracts and invariants
- K-041 install after success, K-048 codec-aware AAC quality gate, K-056 FFmpeg argument vector, K-038 sidecar policy,
  K-032 configured vs canonical paths, K-065 operation-based output layout, K-066 complete folders, K-058 source link rejection

## Acceptance checks
- [ ] Source tree byte-identical before and after (hash all files).
- [ ] Menu and CLI use `normalized-flac/<library-name>`; previous `FLAC` output remains untouched.
- [ ] Every output FLAC probes as 16-bit, 44100 Hz, same channel count, duration within 0.15 s.
- [ ] A 24-bit/96 kHz source converts; an 8-bit or 22.05 kHz source is rejected with a non-zero exit.
- [ ] AAC `.m4a` at 44.1 kHz converts with a lossy-quality warning; AAC below 44.1 kHz is rejected.
- [ ] Re-running without `--overwrite` on an existing output fails without changing it.
- [ ] Large existing-destination and duplicate-path lists produce only console summaries; every conflicting path
      is retained in the session log when error-level logging is enabled, even with `--log-level info`.
- [ ] If detailed file logging is disabled or unavailable, conflicts still block execution and the console says
      that full paths are unavailable instead of claiming they were logged.
- [ ] Non-ASCII and >260-character paths convert on Windows.
- [ ] A near-limit Windows output directory succeeds when marker and temporary filenames would exceed `MAX_PATH`.
- [ ] Menu conversion accepts extended-prefix output paths with an FFmpeg launcher shim (see DESIGN's output anchoring).
- [ ] Preparation logs `Conversion discovery` and `Conversion planning` phases ending in `[done]` before conversion starts.
- [ ] An extended-prefix output below a mapped drive succeeds when its target descendants do not yet exist.
- [ ] Replacing an output directory after validation cannot redirect writes into the source library.
- [ ] `conversion-audit.json` exists with `status: completed` (see F-003).
- [ ] Before converting a folder's tracks, the console identifies the current source album folder without a line per track.
- [ ] Console output stays concise while the session log retains detailed per-file and preservation diagnostics (F-010).
- [ ] Multi-library conversion has one overall total; a failed track does not advance installed-file counts or
      produce a successful progress completion.
- [ ] A written folder retains unsupported audio, extensionless files and audio-free/empty subfolders byte-for-byte.
- [ ] A folder with audio but no conversions is absent from output and listed once (topmost) with its counts.
- [ ] The preview shows skipped-folder counts without individual paths; those paths remain available in the
      debug-level session log, including when the preview is cancelled.
- [ ] Libraries without conversions write only the report (listing `"."`) without launching the encoder.
- [ ] Copy failure retains the failure marker/report and reports unattempted copies as not installed.
- [ ] Any source file or directory symlink/reparse point fails during preparation before output is written.
- [ ] Internal conversion temporaries at the source root or any nested depth fail CLI/worker preparation with recovery
      guidance and no output writes, even with overwrite; every source file remains unchanged.
- [ ] Similar legitimate track names and unrelated dotfiles are still converted or copied normally.
- [ ] Explicit `--overwrite` writes a formerly skipped folder completely once it gains a conversion.

## Known limitations
- Sequential; stops at the first error, earlier outputs remain (K-024).
- Lossy sources (`.m4a` AAC) are re-encoded to FLAC without quality gain; the source-rate gate still applies.
- Skipping is per folder subtree: an MP3-only `CD2` beside a converted `CD1` is skipped and only listed.
- Source file and directory symlinks/junctions, broken links and other reparse points fail explicitly, including
  inside subtrees that would otherwise be skipped. Links are neither followed nor materialized as file contents;
  this is not a link/attribute-preserving filesystem backup.
- Plans describe discovery-time source contents. Existing unrelated output files are retained, not synchronized away.
- On Ubuntu 24.04 with FFmpeg 6.1.1, some AAC/ALAC M4A files fail the preservation audit on a generated `vendor_id`
  field; sources remain unchanged (K-063).
- macOS has no build preset yet.
