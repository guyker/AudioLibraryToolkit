# F-012: Headless worker interface

Status: experimental

## Behaviour
External applications and automation launch the executable with `--worker` and exchange versioned UTF-8 JSON Lines.
The toolkit supports conversion, scan, duplicate detection/quarantine, SQLite import, repair, template-based default
configuration initialization, and revision-checked configuration read/validate/save. It validates requests and remains
authoritative for filesystem safety, audio verification and output
installation. It has no graphical, client-source or JavaScript runtime dependency.
The worker uses the same bundled FFmpeg policy as CLI/menu (F-013); startup library diagnostics stay on stderr.
No protocol fields are added: explicit encoder overrides remain configuration settings subject to F-007 validation.

Worker requests and CLI commands share typed application-service dispatch. Conversion and repair offer non-mutating
previews; execution re-prepares before writing. Clients supply explicit MP3 tag choices and album paths for quarantine.
Configuration initialization uses supplied paths or creates an editable blank config from its shipped template and
refuses to overwrite an existing config. Configuration snapshots and validation do not create missing files. Saving
edits from a missing snapshot creates a file from built-in defaults without a backup; explicit default initialization
instead uses the shipped commented template when valid and refuses to overwrite. Patches are validated by the backend,
merge object settings member by member (lists are replaced whole), preserve comments and unknown keys outside replaced
values, reject stale revisions, and atomically replace the file. Replacing an existing file retains a recovery backup.

## Entry points
- `AudioLibraryToolkit --worker` → `AudioLibraryToolkit/src/app/AudioLibraryToolkitMain.cpp#RunWithArguments`
- Request validation and events → `AudioLibraryToolkit/src/app/WorkerProtocol.cpp#WorkerProtocol::Run`
- Shared CLI/worker dispatch → `AudioLibraryToolkit/src/app/ApplicationServices.cpp#ApplicationServices::Run`

## Design and knowledge
- DESIGN: `AudioLibraryToolkit/DESIGN.md` → Headless worker boundary; Threading and process lifetime
- Protocol: `docs/api/worker-protocol-v1.md`; duplicate reports: `docs/schemas/duplicate-report-v1.schema.json`
- Configuration: F-007; conversion and tag decisions: F-002; duplicate review: F-005

## Acceptance checks
- [x] The worker announces protocol version 1 and supported operations; clients reject unadvertised operations before
      sending requests. Malformed requests and incompatible versions return explicit errors without starting work.
- [x] Worker requests and CLI commands use the same typed application-service dispatch without synthesizing CLI arguments.
- [x] MP3 worker conversion validates explicit tag-issue choices; supported choices are applied, while invalid values
      return an explicit request error.
- [x] CTest `worker-protocol` (`tests/worker_protocol.py`) verifies request validation, initial config creation/refusal to overwrite,
      and successful dry-run repair.
- [x] Worker configuration read/validate/save verifies missing-file defaults without creation, validation failures,
      comment/unknown-key preservation (including inside merged nested objects), backup creation when replacing an
      existing file, and stale-revision refusal.
- [x] Default configuration initialization preserves the shipped comments, leaves paths unset, and refuses overwrite.
- [x] Conversion and repair expose non-mutating previews; execution rebuilds its plan before writing.
- [x] Only explicitly selected album folders are quarantined through worker requests.
- [x] Windows conversion integration verifies tag resolution, preview non-mutation, and selected duplicate quarantine.
- [x] Windows configure/build/test/install work with client source absent.

## Known limitations
- Conversion and repair previews are textual summaries, not persisted plan tokens; execution re-runs preflight before
  writing. Duplicate quarantine recomputes the groups and verifies selections against current results.
- Duplicate quarantine requires source-library roots in `MediaDirectoryList`; catalog-only configurations without
  active library roots can still detect and report duplicates, but cannot quarantine albums.
- Unspecified MP3 tag choices still obey `TagIssueResolution` (`"ask"` stops before writing; `"auto"` applies
  recommendations).
- The process protocol is not a C++ ABI. The internal static target is not an installed public SDK.
- Cancellation is process termination, not transactional rollback; already verified outputs can remain.
