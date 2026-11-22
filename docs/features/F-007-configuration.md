# F-007: Configuration loading

Status: active

## Behaviour
Settings come from `config.json` in the per-user configuration directory: `%LOCALAPPDATA%\AudioLibraryToolkit` on
Windows, `$XDG_CONFIG_HOME/AudioLibraryToolkit` (or `~/.config/AudioLibraryToolkit`) on Linux, and
`~/Library/Application Support/AudioLibraryToolkit` on macOS. CLI order: `--config <file>` → per-user config →
built-in defaults with no libraries. The working directory is not searched automatically. CLI use of built-in
defaults does not create a config file. Invalid `LogLevel` (outside 0–6), `AlbumsSplitThreshold <= 0`, or a negative/non-integer
`MetadataScanMaxThreads` fails. `MetadataScanMaxThreads` defaults to `0`, which uses hardware concurrency (fallback 4);
a positive value caps concurrent metadata workers per album. `LogLevel` sets the detailed session-file threshold for
both CLI and menu; CLI `--log-level` overrides it without changing console verbosity (F-010).
`MetadataSidecars` sets the conversion sidecar
`Format` (`binary`|`json`, default `binary`) and per-target policies `ConvertToFlac` / `ConvertToMp3`
(`disabled`|`optional`|`required`, default `disabled`); other values fail loading. The sample config uses
JSON comments to explain these options. `disabled` never writes a sidecar; `optional` writes by default and can
be disabled for a run; `required` always writes and rejects disable requests. The menu offers to create a default file in the per-user configuration directory. The menu and CLI apply the same
built-in defaults and validation to a
found file (`AppSettingsJson::LoadWithDefaults`), so an absent or empty `OutDirectory` uses the platform default
(K-007) in both. In `MediaDirectoryList`, `isActive` must be boolean and `mediaPath` a string when present, and active
entries require a non-empty `mediaPath`; otherwise loading fails.
`UseAsyncMetadataReads` controls parallel TagLib reads during scan; the previous `UseAsyncFFmpegCalls` key remains
accepted as an alias when the new key is absent. `CatalogExtensions` defaults to `.flac` and `.mp3`; additional supported TagLib formats can be
enabled explicitly. `UseFFmpegLibraryAPI` is deprecated, ignored with a warning, and omitted when configuration is
saved. `ScanFlacMetadataArchives` is a boolean, default `false`, setting the default for optional native FLAC scan
archives independently of conversion `MetadataSidecars`; menu and CLI choices can override it for one scan.
`JsonBatchTargetBytes` is a non-negative integer, default `0` (disabled); a positive value sets a hard JSON batch
size limit. Large albums split into track fragments; a track that cannot fit fails the scan.

## Entry points
- `AppSettings.cpp#AppSettingsJson::LoadForCommandLine` (CLI), `AppSettings.cpp#AppSettingsJson::AppSetting` (menu),
  both through `AppSettings.cpp#AppSettingsJson::LoadWithDefaults`
- `AppSettings.cpp#AppSettingsJson::GetDefaultSettings`, `include/AppSettings.h#MediaDirectoryElement`

## Design and knowledge
- DESIGN: `FlacConvert/DESIGN.md` → Configuration
- K-007 default output directory
- K-060 per-user platform configuration directory

## Acceptance checks
- [ ] `--config missing.json` exits non-zero with a clear message.
- [ ] With no config file, `convert` works using defaults.
- [ ] A config in the working directory is ignored unless selected with `--config`.
- [ ] `--config` takes precedence over the per-user config; an invalid per-user config fails instead of using defaults.
- [ ] Missing implicit config uses built-in defaults without creating a file.
- [ ] `AlbumsSplitThreshold: 0` is rejected.
- [ ] Negative or non-integer `JsonBatchTargetBytes` is rejected.
- [ ] `MediaDirectoryList` must contain objects, and a closed input stream exits the configuration prompt.
- [ ] An active `MediaDirectoryList` entry with a missing, empty or non-string `mediaPath`, or a non-boolean
      `isActive`, fails loading; an inactive entry without a path is accepted.
- [ ] A menu config without `OutDirectory` uses the same platform default as the CLI.
- [ ] `MetadataScanMaxThreads: -1` is rejected; `0` selects automatic concurrency.
- [ ] `CatalogExtensions` accepts only supported extensions and matching is case-insensitive.
- [ ] `ScanFlacMetadataArchives` defaults to `false`, round-trips through config, and rejects non-boolean values.
- [ ] A legacy `UseFFmpegLibraryAPI` setting is ignored and not written back.
- [ ] `MetadataSidecars` config comments load; omitting its target policies defaults both to `disabled`.
- [ ] `MetadataSidecars.Format: "xml"` is rejected; `--no-metadata-sidecar` fails when the target policy is
      `required` and succeeds without a sidecar when it is `optional`.
- [ ] `mediaName` containing a path separator is rejected by scan/import.

## Known limitations
- `DatabaseFileName`, `resultPath`, `dbPath` and most `FLACSettings` strings are read but unused.
