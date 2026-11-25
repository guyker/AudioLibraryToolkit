# AudioLibraryToolkit worker protocol v1

The worker is a headless process interface for external applications and automation. Start the executable with
`AudioLibraryToolkit --worker`, then exchange UTF-8 JSON Lines on stdin/stdout. Standard output contains
protocol events only; diagnostics and session logging go to stderr and the normal application log directory.
The worker executes requests sequentially and remains available until stdin closes.

## Request

Each request has `protocolVersion: 1`, a non-empty `requestId`, `type: "run"` or `"preview"`, an `operation`, and an
object of operation-specific `parameters`. Preview requests are supported for conversion and repair; they prepare and
report a plan without writing outputs. Execution always prepares the operation again to catch changes since preview.
Paths are UTF-8 strings. The worker validates the request into typed
`CommandLineOptions` and invokes the shared application-service dispatcher used by CLI commands. Configuration
checks, collision handling, and output safety rules remain authoritative.

| Operation | Required parameters | Optional parameters |
|---|---|---|
| `convert` | `inputPath` | `target` (`flac` or `mp3`), `mp3Quality` (0-9), `downmixToStereo`, `overwrite`, `noMetadataSidecar`, `tagIssueResolutions`, `outputDirectory`, `configPath` |
| `scan` | `inputPath` | `fullRescan`, `tier`, `archiveFlacMetadata`, `outputDirectory`, `configPath` |
| `duplicates` | `jsonDirectory` | `openFolders`, `reportPath`, `duplicateQuarantinePaths` (string array), `outputDirectory`, `configPath` |
| `databaseImport` | `jsonDirectory` | `outputDirectory`, `configPath` |
| `repair` | `inputPath` | `overwrite`, `dryRun`, `removeId3v1`, `jobs` (1-16), `rules` (string array), `outputDirectory`, `configPath` |
| `initializeConfig` | `libraryPath`, `outputDirectory` | `configPath` |
| `initializeDefaultConfig` | none | `configPath` |
| `readConfiguration` | none | `configPath` |
| `validateConfiguration` | `configRevision`, `configurationPatch` (JSON object encoded as a string) | `configPath` |
| `saveConfiguration` | `configRevision`, `configurationPatch` (JSON object encoded as a string) | `configPath` |

Example:

```json
{"protocolVersion":1,"requestId":"c7f7","type":"run","operation":"scan","parameters":{"inputPath":"D:\\Music","outputDirectory":"E:\\AudioOutput","tier":"catalog"}}
```

For MP3 conversion, `tagIssueResolutions` is an optional object keyed by `emptyValues`, `numericGenre`,
`embeddedNul`, `flacOnlyBlocks`, and `replayGain`. Values are `apply`, `alternative`, `skipFiles`, or `stop`,
subject to the choices supported by that issue type. Unspecified detected issues remain unresolved: the configured
`TagIssueResolution: "auto"` policy applies recommended choices; otherwise the operation stops before writing.

`initializeConfig` creates a new configuration from the backend defaults/template using the selected output and
library paths. It refuses to overwrite an existing file; when `configPath` is omitted, the backend's per-user
configuration path is used.
`initializeDefaultConfig` creates an editable configuration without preset library/output paths, using the shipped
commented template when valid and built-in defaults otherwise. It also refuses to overwrite an existing file.

Configuration operations target that same per-user file unless `configPath` selects another path. A read returns
`schemaVersion`, the resolved absolute path, `exists`, a content `revision`, and the parsed configuration. A missing
file returns built-in defaults with revision `"missing"` and is not created by reading or validation. Patches contain
only top-level setting values. Validation applies the patch to the loaded JSONC file and runs backend validation
without writing. Saving requires the exact revision returned by the latest read; changed files fail with a conflict
and must be reloaded. Successful saves preserve comments and unknown keys, keep unchanged text, atomically replace
the file, and retain the prior file as `<config>.bak`. The backend owns the serialization and validation; clients
should not write the file directly.

Duplicate review is a two-request flow: run `duplicates` with a new `reportPath`, inspect the schema-v1 report, then
send selected album paths in `duplicateQuarantinePaths`. The backend recomputes the groups and validates the choices
against the current scan. Quarantine requires `outputDirectory` (or configured `OutDirectory`) and at least one active
source library in `MediaDirectoryList`. Moves retain the existing copy-verify-before-removal safety behavior; catalog
JSON and databases are not updated.

## Events

The worker first emits a `ready` event with `protocolVersion` and supported operation names. For each accepted request
it emits `started`, zero or more `progress` and `output` events, then `completed`. Invalid or unsupported requests
emit `error` with a message and do not start an operation. Every subsequent event carries the request's `requestId`.
`completed.status` is `succeeded` or `failed`, with the command's `exitCode`.

Progress messages are informational text; clients must not parse them as stable fields. Output text is captured
stdout from the existing CLI operation. Error and warning diagnostics are emitted on stderr and also recorded in the
session log.

## Cancellation and safety

Clients can stop a worker process. The worker is contained with its FFmpeg child process (Windows Job Object or POSIX
process group), so process termination also stops the child encoder. Cancellation may leave already-installed,
verified outputs and crash-marker/temporary artifacts for inspection; it does not roll back completed work. The
backend remains responsible for source protection, validation, destination conflicts, and install-after-verification.
Clients must treat termination without a `completed` event as cancelled/indeterminate, never as success.

## Compatibility

Clients must check `protocolVersion` and capabilities at worker startup. Unsupported versions, malformed messages,
launch failures, stale configuration revisions, and exits without a completion event are errors. Protocol v1 is not a C++ ABI and exposes no
backend implementation headers.
