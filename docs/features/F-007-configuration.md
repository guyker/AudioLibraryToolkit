# F-007: Configuration loading

Status: active

## Behaviour
Settings come from `config.json` in the per-user configuration directory: `%LOCALAPPDATA%\AudioLibraryToolkit` on
Windows, `$XDG_CONFIG_HOME/AudioLibraryToolkit` (or `~/.config/AudioLibraryToolkit`) on Linux, and
`~/Library/Application Support/AudioLibraryToolkit` on macOS, or an explicitly selected CLI `--config <file>`.
Startup logs the absolute path and notes that it can be opened and edited. The working, executable and old shared directories
are not searched. On a first operational run with interactive stdin and stdout, setup asks for an output directory and
an existing input library, saves absolute paths, then creates the file; without an interactive terminal or when input
ends before setup is complete, it creates an empty template and exits with editing instructions. The created file is a
copy of the commented `config.default.json` shipped beside the executable or in `share/AudioLibraryToolkit`, with
setup answers filled in; if that template is missing or unusable, a warning is shown and built-in defaults are written
without comments. Help/version print the
path without creating a config.
Existing files are not overwritten by setup. Defaults contain neither an output path nor private libraries; the menu
requires an output root and an active library. CLI commands use their positional input and need either `OutDirectory`
or `--output-dir`, except `duplicates`, which does not need an output root.
`OutDirectory` and `--output-dir` are base roots; operations append the categories in the
[output layout](../../AudioLibraryToolkit/README.md#output-layout). No configuration keys are added for the layout.
Invalid `LogLevel` (outside 0–6), `AlbumsSplitThreshold <= 0`, or a negative/non-integer
`MetadataScanMaxThreads` fails. `MetadataScanMaxThreads` defaults to `0`, which uses hardware concurrency (fallback 4);
a positive value caps concurrent metadata workers per album. `LogLevel` sets the detailed session-file threshold for
both CLI and menu; CLI `--log-level` overrides it without changing console verbosity (F-010).
`MetadataSidecars` sets the conversion sidecar
`Format` (`binary`|`json`, default `binary`) and per-target policies `ConvertToFlac` / `ConvertToMp3`
(`disabled`|`optional`|`required`, default `disabled`); other values fail loading. The default template uses
JSON comments to explain these options. `disabled` never writes a sidecar; `optional` writes by default and can
be disabled for a run; `required` always writes and rejects disable requests. The menu and CLI apply the same
built-in defaults and validation to a found file (`AppSettingsJson::LoadWithDefaults`). In `MediaDirectoryList`,
`isActive` must be boolean and `mediaPath` a string when present, and active
entries require a non-empty `mediaPath`; otherwise loading fails.
`TagIssueResolution` (`ask`|`auto`, default `ask`; other values fail loading) controls MP3 tag issues (F-002): `ask`
prompts in the menu and fails the CLI before writing; `auto` applies the recommended fixes.
`RepairCategories` sets option 7 defaults per member: `metadata=fix`, `integrity=off`, `container=off`,
`decode=off`. A run can override one member without replacing the others; see F-011 for valid modes and their cost.
`UseAsyncMetadataReads` controls parallel TagLib reads during scan; the previous `UseAsyncFFmpegCalls` key remains
accepted as an alias when the new key is absent. `CatalogExtensions` defaults to `.flac` and `.mp3`; additional supported TagLib formats can be
enabled explicitly. `UseFFmpegLibraryAPI` is deprecated, ignored with a warning, and omitted when configuration is
saved. `ScanFlacMetadataArchives` is a boolean, default `false`, setting the default for optional native FLAC scan
archives independently of conversion `MetadataSidecars`; menu and CLI choices can override it for one scan.
`JsonBatchTargetBytes` is a non-negative integer, default `0` (disabled); a positive value sets a hard JSON batch
size limit. Large albums split into track fragments; a track that cannot fit fails the scan.

`FLACSettings.ffmpeg_exe_name` defaults to `"bundled"`. Omitted/empty values and legacy `"ffmpeg"` / `"ffmpeg.exe"`
values select the app-relative encoder, not PATH. Absolute external paths remain supported with a warning;
other bare names, relative paths, embedded NUL and non-string values fail validation. `FLACSettings` itself must
be an object. Loaded aliases normalize in memory; existing configuration files are not automatically rewritten.
The same rules apply to worker read/validate/save (F-013).

The worker configuration API reads the same per-user file or explicit `configPath`. Read and validate operations
return defaults for a missing file without creating it. A save applies only supplied top-level keys: an object value
such as `MetadataSidecars` is merged member by member, while any other value, including `MediaDirectoryList` and
`CatalogExtensions`, replaces the existing value. It validates the merged JSONC with backend rules, and requires the
unchanged content revision returned by the last read. A conflict requires reloading rather than overwriting external
edits. Saving from a missing snapshot creates a config from built-in defaults plus the patch; there is no prior file
to back up. Replacing an existing file preserves comments and unknown keys (except inside a replaced list), creates
a `<config>.bak` recovery copy, and atomically replaces the file. Desktop code does not write backend config files
directly. The explicit worker `initializeDefaultConfig` operation instead creates a blank editable config from the
shipped commented template when valid, or built-in defaults otherwise; it never presets library/output paths or
overwrites an existing file.

## Entry points
- `app/AppSettings.cpp#AppSettingsJson::LoadForCommandLine` (CLI), `app/AppSettings.cpp#AppSettingsJson::AppSetting` (menu),
  both through `app/AppSettings.cpp#AppSettingsJson::LoadWithDefaults`
- `include/app/AppSettings.h#AppSettingsJson::GetDefaultSettings`, `app/AppSettings.cpp#AppSettingsJson::CreateInitialConfiguration`,
  `app/AppSettings.cpp#AppSettingsJson::FindDefaultConfigTemplate`, `AudioLibraryToolkit/config.default.json`,
  `include/app/AppSettings.h#MediaDirectoryElement`

## Design and knowledge
- DESIGN: `AudioLibraryToolkit/DESIGN.md` → Configuration
- K-061 per-user configuration and explicit required paths

## Acceptance checks
- [ ] `--config missing.json` exits non-zero with a clear message.
- [ ] First interactive setup asks for an output directory and existing input library and saves them.
- [ ] Unicode and long paths entered on Windows are saved without corruption.
- [ ] A library path entered during setup or edited in the config with a trailing separator keeps its folder name as
      the default `mediaName`.
- [ ] Unattended or interrupted first setup creates an empty editable template and exits non-zero.
- [ ] First setup copies the commented default template; interactive answers are filled in with comments preserved;
      a missing or invalid template falls back to built-in defaults with a warning.
- [ ] The menu rejects an existing config without an output root or active library without changing the file.
- [ ] A CLI command can use an explicit input and `--output-dir` with a config containing no saved libraries.
- [ ] Output-producing CLI commands without a configured or explicit output root fail clearly.
- [ ] A config in the working directory is ignored unless selected with `--config`.
- [ ] `--config` takes precedence over the per-user config; an invalid per-user config fails instead of using defaults.
- [ ] `--help` and `--version` print the configuration path without creating a file.
- [ ] `AlbumsSplitThreshold: 0` is rejected.
- [ ] Negative or non-integer `JsonBatchTargetBytes` is rejected.
- [ ] `MediaDirectoryList` must contain objects, and a closed input stream exits the configuration prompt.
- [ ] An active `MediaDirectoryList` entry with a missing, empty or non-string `mediaPath`, or a non-boolean
      `isActive`, fails loading; an inactive entry without a path is accepted.
- [ ] `MetadataScanMaxThreads: -1` is rejected; `0` selects automatic concurrency.
- [ ] `CatalogExtensions` accepts only supported extensions and matching is case-insensitive.
- [ ] `ScanFlacMetadataArchives` defaults to `false`, round-trips through config, and rejects non-boolean values.
- [ ] A legacy `UseFFmpegLibraryAPI` setting is ignored and not written back.
- [ ] `MetadataSidecars` config comments load; omitting its target policies defaults both to `disabled`.
- [ ] `MetadataSidecars.Format: "xml"` is rejected; `--no-metadata-sidecar` fails when the target policy is
      `required` and succeeds without a sidecar when it is `optional`.
- [ ] `mediaName` containing a path separator is rejected by scan/import.
- [x] Worker reads/validation of a missing config do not create a file; explicit save creates it from built-in defaults
      without a backup, while `initializeDefaultConfig` uses the shipped template when valid and refuses overwrite.
- [x] `initializeDefaultConfig` creates a blank config from the shipped template/defaults and refuses overwrite.
- [ ] Worker config validation rejects invalid values without changing the existing file.
- [ ] Bundled aliases normalize without changing an existing file; absolute Unicode paths round-trip, while relative
      paths, non-string values and embedded NUL fail validation.
- [ ] Config saves preserve JSON comments and unknown keys, including inside a partly patched `FLACSettings` or
      `MetadataSidecars`, create a recovery backup when replacing an existing file, and reject stale revisions.

## Known limitations
- `DatabaseFileName`, `resultPath`, `dbPath` and most `FLACSettings` strings are read but unused.
