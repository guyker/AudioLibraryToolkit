# F-010: Per-request session logging

Status: active

## Behaviour
Each process invocation (CLI command or interactive-menu session) writes a unique `.log` file under
`%LOCALAPPDATA%\AudioLibraryToolkit\logs` on Windows, `$XDG_STATE_HOME/AudioLibraryToolkit/logs` or
`$HOME/.local/state/AudioLibraryToolkit/logs` elsewhere, falling back to the system temporary directory when the
platform-specific location is unavailable. The console remains concise: informational progress (including the current
source album folder during FLAC conversion), completion, application warnings, and errors; per-file details and
expected generated-field normalization are reserved for the session log. Redirected output contains no animated
progress control sequences. Scan folder discovery and track collection report counts, discovered folders/albums,
elapsed time and (for track collection) an ETA on one updating line, with periodic snapshots when redirected; scan
metadata progress spans all batches of a library on one updating line. Intermediate metadata batches do not
print completion lines; only the final metadata batch logs completion, separate from catalog installation.
Before any menu action prepares work, it logs all active source library paths. CLI commands log the input folder,
explicitly labeled as catalog JSON for duplicates/database import. Conversion and scan library transitions and
repair preparation identify the current source folder, including before long preprocessing begins. Every invocation
prints the absolute configuration path and notes that it can be opened and edited, including help and version requests.

Conversion and repair destination conflicts show the affected output folder, counts and recovery guidance instead
of listing files or directories on the console. Every conflicting path is written only to the session log at error
level, so `--log-level info` retains it. The summary points to the log; if error-level file logging is disabled or
the log is unavailable, that limitation is explicit. Conversion previews also reserve individual skipped-folder
paths for debug-level logging.

The config `LogLevel` (integer spdlog level 0–6) controls the detailed file log; CLI commands can override it with
`--log-level info|debug|trace`. Start and finish entries are always written when the log is available. The file
threshold does not suppress console application warnings or errors. FFmpeg warnings are file-only at warning
level, including messages without file context and diagnostics from worker threads; linked-FFmpeg errors still
appear on the console. FFmpeg child-process output is also file-only, while unsuccessful exits are reported as
console errors. TagLib debug messages remain file-only at debug level. File-only messages respect the configured
threshold; warnings are not echoed to the console when their file logging is disabled or unavailable.
Conversion audit and scan-report JSON remain the authoritative structured records; session logs supplement them. TagLib and FFmpeg diagnostics are
logged with the file path when known. FFmpeg warnings without file context remain explicitly unattributed rather than
being assigned to a track.

Inactive application-owned logs older than 30 days are removed; the logger also prunes oldest inactive logs when their combined size exceeds
1 GiB. Active requests are not pruned. If a session log cannot be created, the request continues with console logging
and reports the failure.

## Entry points
- CLI commands → `FlacConvertMain.cpp#RunWithArguments` → `RunLogging.cpp#RunLogging::Start` / `RunLogging::Finish`
- Menu option 1 → `FolderConvert.cpp#FolderConvert::ExecuteConversion`
- Menu option 2 → `FlacConvertMain.cpp#ScanFolderAndCreateJSON`, `TagLibReader.cpp#TagLibReader::Read`
- FFmpeg warnings → `ffmpeg.cpp#FFmpeg::scoped_ffmpeg_log_callback` → `RunLogging.cpp#RunLogging::RecordWarning`
- Detailed log override → `CommandLine.cpp#add_config_option`, `FlacConvertMain.cpp#RunWithArgumentsImpl`

## Design and knowledge
- DESIGN: `FlacConvert/DESIGN.md` → Run logging; Configuration
- Structured reports: F-003 (`conversion-audit.json`), F-004 (`scan-report.json`)

## Acceptance checks
- [ ] Each CLI invocation creates a distinct session log with start and finish status.
- [ ] Menu, CLI, help and version output show the configuration path and editing guidance.
- [ ] The menu uses the same logger and configured file threshold.
- [ ] `--log-level info|debug|trace` overrides the file threshold; console application warnings and errors remain visible.
- [ ] Conversion console output omits per-file `Processing:` messages, FFmpeg progress stats, and expected generated-field normalization.
- [ ] Large menu and CLI conflict lists stay out of console output while all paths are retained in the session
      log at `info` or a more verbose threshold; disabled/unavailable logging is reported truthfully.
- [ ] FFmpeg child diagnostics and TagLib debug messages are associated with their source file in the detailed log.
- [ ] MP3 scans with many estimated-duration warnings do not print those warnings; every path-linked warning
      remains in the session log at `info` or a more verbose threshold and in `scan-report.json`.
- [ ] Scoped, worker-thread and unattributed FFmpeg warnings are file-only; corresponding errors remain visible,
      and disabling file logging does not turn warnings into console output.
- [ ] Scan warnings/errors remain in `scan-report.json`; unscoped FFmpeg entries remain unattributed and are not lost from the session log.
- [ ] Redirected scan output contains no terminal control sequences.
- [ ] Multi-batch metadata scans update one shared line, without per-batch completion lines or cursor-up sequences;
      errors remain readable and only the final metadata batch reports metadata completion.
- [ ] Log-file creation failure is explicit and does not prevent the requested operation.
- [ ] Retention preserves active logs and prunes expired or oldest inactive logs within the configured bounds.

## Known limitations
- TagLib debug output depends on how TagLib was built; release builds may omit those messages.
- Forced termination may leave an active marker; stale markers are retained when process identity cannot be verified.
