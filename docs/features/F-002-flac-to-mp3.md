# F-002: Convert FLAC to MP3

Status: active

## Behaviour
Converts `.flac` files under a source folder to LAME VBR MP3 (`-q:a 0`–`9`, default 0) in
`<root>/converted-mp3/<source folder name>/`, preserving mapped tags in ID3v2.4 (no ID3v1), cover art and chapters.
TagLib writes FLAC comments into ID3 and reads the saved ID3 properties for preservation verification. 44.1/48 kHz
sources keep their rate; others are resampled to 48 kHz. Multichannel sources are rejected unless downmix is enabled.
A `.flac-metadata` sidecar beside each MP3 preserves the original FLAC metadata byte-for-byte when enabled;
`MetadataSidecars` can select a readable `.flac-metadata.json` form or make it optional/disabled (disabled by
default; F-007). Sidecars remain necessary for FLAC metadata that ID3 cannot represent; without a verified sidecar,
loss of such metadata still fails the conversion.
Non-FLAC audio is reported as skipped; companions are copied, except that the topmost folders containing audio
but no FLAC (for example MP3-only albums) are omitted with all their files and listed in
`conversion-audit.json` (`skippedUnchangedDirectories`), as in F-001. Preparation reports the same discovery and planning
progress as FLAC conversion (F-001).
Menu and CLI destination conflicts use the same concise summaries and file-only path details as FLAC conversion
(F-001, F-010); enabling overwrite never bypasses duplicate output-path conflicts.

## Entry points
- `AudioLibraryToolkit convert <input> --to mp3 [--mp3-quality N] [--downmix-to-stereo] [--overwrite]` →
  `FlacConvertMain.cpp#RunWithArguments`
- Menu option 6 → `FlacConvertMain.cpp#ConvertFLACToMP3` (prompts in `FlacConvertMain.cpp#ConfigureManualMp3`)
- `include/ConversionOptions.h#ConversionOptions`, `MediaConvertionTask.cpp#conversionArguments`,
  `FlacMetadata.cpp#FlacMetadata::Save`, `Mp3TagMetadata.cpp#Mp3TagMetadata::WriteFromFlac`,
  `Mp3TagMetadata.cpp#Mp3TagMetadata::Read`

## Design and knowledge
- DESIGN: `FlacConvert/DESIGN.md` → Conversion; Files and schemas (sidecar)
- K-038 metadata sidecar policy, K-016 channel policy, K-017 MP3 encoding settings, K-065 output layout and retained menu defaults,
  K-029 preservation audit, K-066 skipped audio folders without FLAC, K-067 TagLib MP3 tag writing and verification

## Acceptance checks
- [ ] `ctest --preset windows-debug -R mp3-conversion-integration` (or the matching Linux preset) passes.
- [ ] Menu and CLI use `converted-mp3/<library-name>`; previous `MP3` output remains untouched.
- [ ] Output probes as `mp3`, rate 44100/48000, channels = min(source, 2) only when downmix is set.
- [ ] Sidecar passes `FlacMetadata::Verify` against the source.
- [ ] With `MetadataSidecars.Format: "json"` the `.mp3.flac-metadata.json` archive decodes to the original bytes.
- [ ] With sidecars disabled, a FLAC with application blocks fails instead of losing them.
- [ ] With sidecars disabled, repeated `DATE`/`YEAR` comments are written to ID3v2.4 and verified directly through TagLib.
- [ ] An MP3-only or WAV-only album folder is absent from the output and listed in `skippedUnchangedDirectories`.
- [ ] 5.1 source without downmix fails; with downmix produces stereo.
- [ ] `--mp3-quality 10` is rejected by the CLI (`CommandLine.cpp#ParseCommandLine`).

## Known limitations
- Requires an `ffmpeg` build with `libmp3lame`.
- Only FLAC input; other lossless formats are skipped, not converted.
