---
name: toolkit-review
description: Review the native AudioLibraryToolkit part of a diff or PR for repository-specific conversion, filesystem, catalog and worker contracts. Supplements general C++ review; not for desktop-only changes.
---

# Code review (AudioLibraryToolkit)

Use the global **code-review** skill when available for general C++ correctness; if unavailable, review the diff
directly. This checklist adds the repository contract and
**overrides** the global style guide: repo names are PascalCase types/methods, `_camelCase` members, camelCase
locals; do not flag them. Ignore C#/WPF guidance.

## Read first

1. The requested PR or working changes, including staged and new files when in scope
2. `AudioLibraryToolkit/DESIGN.md` sections for every touched component
3. Look up affected contracts per the knowledge protocol; inspect active entries relevant to the diff, not the history of every caller
4. Affected `docs/features/F-xxx` entries
5. `.github/instructions/audio-cpp.instructions.md`

## Checklist

**Source safety and installation**
- [ ] No write, rename or delete can target the source tree; overlap checks still run before any write.
- [ ] Outputs are written to a unique temporary, verified, audited, then installed; no direct write to the final name.
- [ ] Without overwrite, an existing target is never replaced; sidecar/audio install order and rollback preserved.
- [ ] Reserved names, marker and owned-folder rename semantics unchanged unless DESIGN is updated.

**FFmpeg**
- [ ] FFmpeg/ffprobe run via argument vector (`runFFmpeg`); no new shell strings, `system`, or `popen`.
- [ ] Windows arguments pass through `quoteWindowsArgument` and `PlatformUtils::WindowsLongPath`.
- [ ] A new `AudioTarget` or option updates every site in DESIGN.md → Extension points (no binary `Mp3 ? : ` fallthrough).
- [ ] Output verification (codec, rate, depth/channels, duration) still covers new formats/options.
- [ ] libav resources freed on every path; log callback remains thread-safe.

**Paths, encoding, portability**
- [ ] Paths stay `std::filesystem::path`; UTF-8 conversions via `CommonUtils`/`PlatformUtils` helpers.
- [ ] Configured paths for I/O, canonical paths only for comparisons (K-032); no new filesystem probes on canonical paths.
- [ ] Cited K-entries are live (`status: active`); superseded ones are history, not rules.
- [ ] New platform code isolated in `_WIN32` / POSIX branches or `PlatformUtils`; no new Windows-only APIs in core.

**SQLite and JSON**
- [ ] Databases opened with the `win32-longpath` VFS on Windows; prepared statements with bound values only.
- [ ] Transactions rolled back on failure; statements finalised and handles closed on every path.
- [ ] Generated JSON still replaced via staging (`installGeneratedFiles`).

**Behaviour and docs**
- [ ] Trace affected CLI, menu and worker callers; check shared defaults, previews and failure paths.
- [ ] For wire changes, compare the exact field/event to `docs/api/worker-protocol-v1.md` and `tests/worker_protocol.py`.
      Use the root router for consumer review; a passing worker test does not exercise the client.
- [ ] DESIGN.md, the F-entry and README match materially changed behaviour; `kb.ps1 affected` entries handled.
      Do not require documentation churn for behaviour-preserving edits.
- [ ] Validation evidence uses generated fixtures; the CMake preset build and `ctest --preset <preset>` pass.

Report findings by severity with `path#Symbol` and the violated DESIGN contract or K-id.
