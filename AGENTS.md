# AGENTS.md

<!-- Router for AI agents and new developers. Keep it short; put detail in the files it points to. -->

## Project

AudioLibraryToolkit is a C++ audio conversion and library-management utility. The application sources are under
`AudioLibraryToolkit/`; conversions run through the FFmpeg executable, and metadata and audio analysis also use FFmpeg
libraries. See `AudioLibraryToolkit/README.md` for the toolkit feature overview.
`AudioLibraryToolkit.Desktop/` is an optional, independently built client with its own `AGENTS.md` and documentation.
Dependency direction is strictly client to the toolkit's public worker protocol, never toolkit to client.

The core C++ sources must remain portable across Windows, Linux and macOS. CMake is the only build definition; Visual
Studio is used through its CMake "Open Folder" support, not a hand-maintained solution. Isolate
platform-specific code behind narrow platform abstractions or conditional implementations.

**Absolute rule: source library folders are read-only.** No option, command, worker request or failure path may
create, modify, rename or delete anything inside a source library. Temporary and generated files belong only in the
output tree or system temp. See `.github/instructions/audio-cpp.instructions.md`.

## Map

| Folder / file | What it is | Read first |
|---|---|---|
| `CMakeLists.txt`, `CMakePresets.json` | Headless Windows/Linux build | `AudioLibraryToolkit/README.md`, then platform CMake files |
| `AudioLibraryToolkit/` | The application: components, flows, contracts, schemas | `AudioLibraryToolkit/DESIGN.md` |
| `AudioLibraryToolkit/src/`, `AudioLibraryToolkit/include/` | Implementation and headers grouped by responsibility (`app`, `catalog`, `common`, `conversion`, `duplicates`, `media`, `metadata`, `platform`, `repair`); vendored RapidJSON stays under `include/rapidjson/` | `AudioLibraryToolkit/DESIGN.md` component map, then the file |
| `docs/features/` | Toolkit features (F-xxx): behaviour, entry points, acceptance checks | `docs/features/index.md` |
| `AudioLibraryToolkit.Desktop/` | Independent desktop workspace, build, docs and knowledge | `AudioLibraryToolkit.Desktop/AGENTS.md` |
| `tests/` | Native regression and Python integration tests, registered with CTest | `tests/CMakeLists.txt` |
| `external/` | Third-party source/SDK dependencies | `CMakeLists.txt` and `cmake/platform/` for expected paths |

## Skills

| Task | Repo skill |
|---|---|
| Toolkit feature or fix | `.github/skills/feature-development` |
| Toolkit diff or PR review | Global `code-review` for C++; `.github/skills/toolkit-review` adds repository contracts |
| Trace toolkit behaviour or impact | `.github/skills/change-impact` |
| Check toolkit docs after a change | `.github/skills/doc-sync` |
| Desktop feature, fix or review | [Desktop router](AudioLibraryToolkit.Desktop/AGENTS.md); no C++ checklist |

For a change crossing the worker boundary, start with the affected operation/field/event in the
[public protocol](docs/api/worker-protocol-v1.md). Trace its producer and consumer separately using their own
routers, knowledge roots and tests. Search the literal wire name as well as symbol references: JSON and IPC calls
do not form a single static call graph. A change inside one module does not by itself require changing the other.
Use `python tools\check_module_boundaries.py` from the root to check dependency direction, not wire compatibility.

## Build and test

The shared CMake presets use Ninja. Initialize submodules (`git submodule update --init --recursive`) and
install CMake 3.28+, Ninja, Python 3.12+ and a C++20 toolchain first. Restore the locked SDK, then configure, build and run CTest with the
platform preset (`windows-debug`, `windows-release`, `linux-debug` or `linux-release`):

```text
python tools\prepare_ffmpeg.py
cmake --preset windows-debug
cmake --build --preset windows-debug --parallel
ctest --preset windows-debug
```

Windows x64 and Ubuntu 24.04 x86_64 use checksum-pinned shared SDKs from `cmake/ffmpeg-lock.json`, restored under
ignored `external/ffmpeg/<build>/<platform>`. `FFmpeg_ROOT` can select another verified restore location, not
another FFmpeg version. Linux also requires `patchelf` and pkg-config. Installation bundles the tools and private
libraries; it does not rely on a host FFmpeg installation. Redistribution remains gated on reviewed matching source
materials; see `AudioLibraryToolkit/README.md`. SQLite amalgamation, CLI11 and spdlog are supplied in the
repository/submodules. Build artifacts live under ignored `out/build/`.
In Visual Studio, use File > Open > Folder on the repository root and pick a Windows preset; there is no `.sln`.
CTest builds and runs the native regression executables and `tests/audio_library_integration.py` against disposable
generated fixtures. See `.github/instructions/audio-cpp.instructions.md` for audio validation and source safety.

## Where knowledge lives

| Kind | Where | Changes how |
|---|---|---|
| What the system is (design, flows, contracts) | `AudioLibraryToolkit/DESIGN.md` | Kept aligned with behaviour changes |
| What users get (features, acceptance checks) | `docs/features/F-xxx-*.md` | Edited when important behaviour changes |
| Rules for changing it | `.github/instructions/*.instructions.md` (scoped by `applyTo`) | Rewritten |
| Why: decisions, traps, constraints, preferences, domain facts, known defects | `docs/knowledge/` (one file per entry; start at `docs/knowledge/index.md`) | Appended; the fact is never rewritten, only its status, review date and citations |
| How-to procedures | `.github/skills/*/SKILL.md` | Versioned with the tooling |
| Issues and change history | Work items, commits, pull requests | Closed, not edited |

Use the globally installed Project Memory skill and its `kb.ps1` script; do not copy the skill or script into this
repository. Toolkit knowledge starts at `docs/knowledge/index.md`. Desktop knowledge starts at
`AudioLibraryToolkit.Desktop/docs/knowledge/index.md`; use its explicit module root, not Git's default root.
Desktop-only changes must not create toolkit knowledge entries. Repository navigation/automation may know both modules;
toolkit code, build and product documentation must not depend on desktop files or implementation details.

Read applicable scoped instructions and owning design documents before edits. Follow
`.github/instructions/knowledge-protocol.instructions.md` for knowledge lookup, recording and post-change maintenance.
For code changes, assess whether important behaviour, design, contracts, schemas or risks changed; update the
relevant DESIGN and feature entries only when needed. Do not create change-history entries for routine edits.
For material or uncertain impact, `python tools\doc_impact.py --details` helps locate candidate documents; it is
advisory, not a substitute for review.
