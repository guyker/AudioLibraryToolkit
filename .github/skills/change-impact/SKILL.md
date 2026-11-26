---
name: change-impact
description: Trace native AudioLibraryToolkit behaviour and change impact from CLI, menu or worker entry points into code, contracts, consumers and tests. For desktop-only questions use the desktop module guide.
---

# Change impact and onboarding (AudioLibraryToolkit)

Answer from the documentation first, then confirm in code. Read-only: do not edit files.

## Procedure

1. **Locate** – `docs/features/index.md` → the F-entry for the feature; `AudioLibraryToolkit/DESIGN.md` → component map,
   flow and contracts.
2. **Why** – `kb.ps1 search <concept>` and `kb.ps1 related -Path <file>` (global project-memory skill). Note live
   decisions, constraints, traps and defects.
3. **Confirm in code** – go to the cited `path#Symbol` (LSP `goToDefinition`, `findReferences`, `incomingCalls`).
   Code is current behaviour; if docs disagree, report the drift.
4. **Impact** (for "what breaks") – list:
   - applicable callers: CLI, menu and `--worker` are distinct entry modes (DESIGN → Entry points);
     check `WorkerProtocol.cpp#makeOptions` and the shared `ApplicationServices::Run` for CLI/worker dispatch.
     For a wire field, search its literal name in `docs/api/worker-protocol-v1.md` and the producer/consumer;
     use the repository router for cross-module questions;
   - DESIGN contracts and K-entries the change would touch;
   - output files/schemas affected (audit report, sidecar, album JSON v2, `catalog-v2.sqlite`) and their readers;
   - relevant platform branches and exact CTest names from `tests/CMakeLists.txt`; distinguish what the
     test asserts from paths that lack coverage;
   - F-entry acceptance checks that must be re-run.

## Answer format

- One-paragraph answer, then evidence as `path#Symbol` and F-/K-ids.
- For impact questions: a table of affected item → why → check to run.
- State what is unverified.
