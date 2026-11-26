---
name: feature-development
description: Implement a new feature or behaviour change in AudioLibraryToolkit (C++20, FFmpeg, SQLite) - e.g. "add an output format", "add a CLI option", "change how conversion/scan/duplicates/DB import works", "fix a bug". Use before editing AudioLibraryToolkit/src or AudioLibraryToolkit/include.
---

# Feature development (AudioLibraryToolkit)

Use the global **project-memory** skill for every knowledge-base step (`kb.ps1`); do not copy it here.

## Read first

1. `AGENTS.md`, `.github/instructions/audio-cpp.instructions.md`, `.github/instructions/knowledge-protocol.instructions.md`
2. `AudioLibraryToolkit/DESIGN.md` – the component, flow and contract you will touch
3. `docs/features/index.md` and the affected `F-xxx` entry (or the template for a new one)
4. Use the knowledge protocol's `related` and concept search; read the active entries bearing on the behaviour or
   risk, not every hit on a shared file. Fixed/superseded entries are not current requirements.

If a live K-entry or a DESIGN contract conflicts with the request, stop and tell the user before coding.
If intent is still ambiguous, ask one question (ask_user).

## Plan

Write a short plan covering:
- the applicable CLI, menu and worker entry points (`path#Symbol`), shared implementation, affected readers,
  defaults and failure paths (DESIGN → Entry points and the owning flow);
- source-safety and install-after-success impact; temporary names; overwrite semantics;
- platform impact (`_WIN32` branches, long paths, UTF-8);
- validation fixtures (generated, never the user's library).

## Implement

- Follow existing naming: PascalCase types/methods, `_camelCase` members, camelCase locals; int/bool returns logged
  with spdlog. Repo conventions override the global code-review style guide.
- For conversion options, use DESIGN → Extension points. For other operations follow their own typed options and
  applicable entry points, not the MP3 menu. For worker-exposed options, trace the wire name from the public
  protocol through `app/WorkerProtocol.cpp#makeOptions` and `app/ApplicationServices.cpp#ApplicationServices::Run`.
  Do not expose a new option on every surface without a requirement.
- New FFmpeg arguments go into the argument vector (`conversion/MediaConversionTask.cpp#conversionArguments`); never a shell string.
- New outputs go through a temporary + verification + `installFile`; never write into the source tree.

## Build and validate

```text
cmake --preset windows-debug
cmake --build --preset windows-debug --parallel
ctest --preset windows-debug
```
Use the matching `linux-*` preset on Linux. New source files go in `AudioLibraryToolkit/CMakeLists.txt` (`AudioLibraryToolkitCore`);
new regression tests are registered in `tests/CMakeLists.txt`. Then validate per `audio-cpp.instructions.md`
(generated fixtures, probe outputs, hash sources). For conversion work extend `tests\audio_library_integration.py` (CTest
`mp3-conversion-integration`) for new behaviour. Run the feature entry's
acceptance checks.

## Finish (same change)

- [ ] Assess whether the change materially affects behaviour, design, contracts, configuration, schemas or risks.
      Update `AudioLibraryToolkit/DESIGN.md` and the affected F-entry only when their current content needs to change; create
      a new indexed F-entry for an important new feature, not for routine edits.
- [ ] `AudioLibraryToolkit/README.md` updated when a material CLI, menu or configuration change makes it inaccurate
- [ ] `kb.ps1 affected`: review each entry (`reviewed` / `cite` / supersede); record new decisions per the protocol
- [ ] Follow the **doc-sync** procedure, even if the skill is not auto-discovered. Use
      `python tools\doc_impact.py --details` when candidate documents need identifying.
- [ ] Do not create per-change history or knowledge entries for formatting or behaviour-preserving refactors
- [ ] Do not commit unless the user asks
