# F-002: Convert FLAC to MP3

Status: active

## Behaviour
Converts `.flac` files under a source folder to LAME VBR MP3 (`-q:a 0`–`9`, default 0) in
`<root>/converted-mp3/<source folder name>/`, preserving mapped tags in ID3v2.4 (no ID3v1), cover art and chapters.
TagLib replaces the encoded MP3's text properties from the FLAC comments, preventing FFmpeg-generated alias frames
from duplicating tag values, and reads the saved ID3 properties for preservation verification. 44.1/48 kHz
sources keep their rate; others are resampled to 48 kHz. Multichannel sources are rejected unless downmix is enabled.
A `.flac-metadata` sidecar beside each MP3 preserves the original FLAC metadata byte-for-byte when enabled;
`MetadataSidecars` can select a readable `.flac-metadata.json` form or make it optional/disabled (disabled by
default; F-007). Sidecars remain necessary for FLAC metadata that ID3 cannot represent; without a verified sidecar,
loss of such metadata still fails the conversion.
Non-FLAC audio is reported as skipped; companions are copied, except that the topmost folders containing audio
but no FLAC (for example MP3-only albums) are omitted with all their files and listed in
`conversion-audit.json` (`skippedUnchangedDirectories`), as in F-001. Preparation reports the same discovery and planning
progress as FLAC conversion (F-001). Execution also shares F-001's overall installed-file and current-track progress:
one updating line across libraries, with the track's encoding percentage and verification/install stages.
Real encoding measurements trigger immediate interactive redraws. Startup shows `Starting encoder` until the
first measurement; very short encodes may finish without an intermediate percentage.
Audio-specific encoder timestamps keep the percentage moving even when FFmpeg 7.1 reports no mux-wide time for
files with cover art; these are real measurements, not estimated animation.
Unchanged tag-key aliases (for example `COMMENT`/`DESCRIPTION`, `DATE`/`YEAR`) are not console warnings;
they remain in the session log at warning level and in the preservation audit.
Menu and CLI destination conflicts use the same concise summaries and file-only path details as FLAC conversion
(F-001, F-010); enabling overwrite never bypasses duplicate output-path conflicts.

**Tag issues.** Preparation detects tag issues that MP3 cannot carry unchanged: empty values, numeric (ID3v1) genres,
values containing NUL, FLAC-only blocks (application, cuesheet, seektable, reserved) and ReplayGain tags. Empty
values and FLAC-only blocks are not reported when a verified sidecar is enabled. Each issue type offers choices with
the app's recommendation first (for example drop empty values, map genre numbers to names, split NUL values,
discard FLAC-only blocks, keep ReplayGain); every type can also skip the affected files or stop.
- Menu: after the preview, one prompt per issue type; Enter takes the recommendation. The choice applies to every
  affected file; the file list goes to the session log. Skipped files are omitted, and a folder left without audio
  is omitted with its companions.
- CLI and `TagIssueResolution: "ask"` (default): the run fails before writing anything, with a per-type summary and a
  hint. `"auto"` applies the recommendations in both menu and CLI.
- Applied resolutions are audited as `Normalized` (or `SourceOnly` for discarded blocks) instead of failing, and are
  listed in `conversion-audit.json` (`tagIssueResolutions`, `skippedByTagIssue`). Without a resolution, the audit
  remains fail-closed.

## Entry points
- `AudioLibraryToolkit convert <input> --to mp3 [--mp3-quality N] [--downmix-to-stereo] [--overwrite]` →
  `app/AudioLibraryToolkitMain.cpp#RunWithArguments`
- Menu option 6 → `app/AudioLibraryToolkitMain.cpp#ConvertFLACToMP3` (prompts in `app/AudioLibraryToolkitMain.cpp#ConfigureManualMp3`)
- Worker `convert` with `target: "mp3"` → `app/WorkerProtocol.cpp#makeOptions` → `app/ApplicationServices.cpp#ApplicationServices::Run` (F-012).
- `include/conversion/ConversionOptions.h#ConversionOptions`, `conversion/MediaConversionTask.cpp#conversionArguments`,
  `metadata/FlacMetadata.cpp#FlacMetadata::Save`, `metadata/Mp3TagMetadata.cpp#Mp3TagMetadata::WriteFromFlac`,
  `metadata/Mp3TagMetadata.cpp#Mp3TagMetadata::Read`, `metadata/Mp3TagIssues.cpp#Mp3TagIssues::Detect`,
  `metadata/Mp3TagIssues.cpp#Mp3TagIssues::Resolve`, `conversion/FolderConvert.cpp#ApplyTagIssueResolutions`,
  `app/AudioLibraryToolkitMain.cpp#ResolveMenuTagIssues`

## Design and knowledge
- DESIGN: `AudioLibraryToolkit/DESIGN.md` → Conversion; Files and schemas (sidecar)
- K-038 metadata sidecar policy, K-016 MP3 quality and channel policy, K-017 FLAC-only CD profile, K-065 output layout and retained menu defaults,
  K-029 preservation audit, K-066 skipped audio folders without FLAC, K-067 TagLib MP3 tag writing and verification

## Acceptance checks
- [ ] `ctest --preset windows-debug -R mp3-conversion-integration` (or the matching Linux preset) passes.
- [ ] Menu and CLI use `converted-mp3/<library-name>`; previous `MP3` output remains untouched.
- [ ] Overall progress spans all selected libraries and counts only installed files; per-track encoding progress
      does not bypass verification, and failure finishes with `[stopped]`.
- [ ] Intermediate track measurements redraw before the one-second timer; encoder startup does not claim 0%.
- [ ] Benign tag-key aliases and selected tag fixes stay out of the console, including with file logging disabled;
      aliases remain in the session log at `info` and in `conversion-audit.json`.
- [ ] Output probes as `mp3`, rate 44100/48000, channels = min(source, 2) only when downmix is set.
- [ ] Sidecar passes `FlacMetadata::Verify` against the source.
- [ ] With `MetadataSidecars.Format: "json"` the `.mp3.flac-metadata.json` archive decodes to the original bytes.
- [ ] With sidecars disabled, a FLAC with application blocks stops for a tag-issue choice (CLI `ask` fails) instead of
      silently losing them; `TagIssueResolution: "auto"` discards them with an audited `SourceOnly` entry.
- [ ] CLI `ask` with an empty value or numeric genre writes nothing and names `TagIssueResolution`; `auto` drops the empty
      value, writes `GENRE=17` as `Rock`, splits NUL values and keeps ReplayGain.
- [ ] Menu tag-issue Skip omits the affected files and their folder; Stop starts no action.
- [ ] With sidecars disabled, repeated `DATE`/`YEAR` comments are written to ID3v2.4 and verified directly through TagLib.
- [ ] `COMMENT`/`DESCRIPTION`, `ENCODEDBY` and `LABEL` values, including repeated identical values, pass verification
      without sidecars or extra FFmpeg alias occurrences; artwork and chapters remain intact.
- [ ] An MP3-only or WAV-only album folder is absent from the output and listed in `skippedUnchangedDirectories`.
- [ ] 5.1 source without downmix fails; with downmix produces stereo.
- [ ] `--mp3-quality 10` is rejected by the CLI (`app/CommandLine.cpp#ParseCommandLine`).

## Known limitations
- Requires an `ffmpeg` build with `libmp3lame`.
- Only FLAC input; other lossless formats are skipped, not converted.
