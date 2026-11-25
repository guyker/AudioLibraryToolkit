# F-012: Headless worker interface

Status: experimental

## Behaviour
External applications and automation launch the executable with `--worker` and exchange versioned UTF-8 JSON Lines.
The toolkit supports conversion, scan, duplicate detection/quarantine, SQLite import, repair, template-based default
configuration initialization, and revision-checked configuration read/validate/save. It validates requests and remains
authoritative for filesystem safety, audio verification and output
installation. It has no graphical, client-source or JavaScript runtime dependency.

Worker requests and CLI commands share typed application-service dispatch. Conversion and repair offer non-mutating
previews; execution re-prepares before writing. Clients supply explicit MP3 tag choices and album paths for quarantine.
Configuration initialization uses supplied paths or creates an editable blank config from its shipped template and
refuses to overwrite an existing config. Configuration
snapshots do not create missing files; patches are validated by the backend, preserve comments and unknown keys,
reject stale revisions, and are saved atomically with a recovery backup.

## Entry points
- `AudioLibraryToolkit --worker` → `AudioLibraryToolkit/src/app/AudioLibraryToolkitMain.cpp#RunWithArguments`
- `WorkerProtocol::Run` → `AudioLibraryToolkit/src/app/WorkerProtocol.cpp`
- `ApplicationServices::Run` → `AudioLibraryToolkit/src/app/ApplicationServices.cpp`

## Design and knowledge
- DESIGN: `AudioLibraryToolkit/DESIGN.md` → Headless worker boundary; Threading and process lifetime
- Protocol: `docs/api/worker-protocol-v1.md`
- Configuration: F-007; conversion and tag decisions: F-002; duplicate review: F-005

## Acceptance checks
- [x] The worker announces protocol version 1 and supported operations; malformed requests and incompatible versions
      return explicit errors without starting work.
- [x] Worker requests and CLI commands use the same typed application-service dispatch without synthesizing CLI arguments.
- [x] MP3 worker conversion validates explicit tag-issue choices; supported choices are applied, while invalid values
      return an explicit request error.
- [x] Windows CTest's `worker-protocol` test verifies request validation, initial config creation/refusal to overwrite,
      and successful dry-run repair.
- [x] Worker configuration read/validate/save verifies missing-file defaults without creation, validation failures,
      comment/unknown-key preservation, backup creation and stale-revision refusal.
- [x] Default configuration initialization preserves the shipped comments, leaves paths unset, and refuses overwrite.
- [x] Conversion and repair expose non-mutating previews; execution rebuilds its plan before writing.
- [x] Only explicitly selected album folders are quarantined through worker requests.
- [x] Windows conversion integration verifies tag resolution, preview non-mutation, and selected duplicate quarantine.
- [x] Windows configure/build/test/install work with client source absent.

## Known limitations
- Conversion and repair previews are textual summaries, not persisted plan tokens; execution re-runs preflight before
  writing. Duplicate quarantine recomputes the groups and verifies selections against current results.
- Duplicate quarantine requires source-library roots in `MediaDirectoryList`; catalog-only configurations without
  active library roots can still detect and report duplicates, but cannot move albums.
- Unspecified MP3 tag choices still obey `TagIssueResolution` (`"ask"` stops before writing; `"auto"` applies
  recommendations).
- The process protocol is not a C++ ABI. The internal static target is not an installed public SDK.
- Cancellation is process termination, not transactional rollback; already verified outputs can remain.
