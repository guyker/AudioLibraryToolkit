---
id: K-069
kind: constraint
title: "Windows builds select a matching shared FFmpeg SDK"
area: ["External Dependencies"]
status: superseded
supersedes: [K-043]
superseded_by: K-078
created: 2026-11-24
reviewed: 2026-11-24
cites:
  - path: cmake/platform/Windows.cmake
    symbol: FFmpeg_ROOT
  - path: cmake/platform/Windows.cmake
    symbol: FFMPEG_EXECUTABLE
  - path: cmake/platform/Linux.cmake
    symbol: pkg_check_modules
---

**Fact:** `cmake/platform/Windows.cmake` selects `FFmpeg_ROOT` from the CMake cache, the environment or the
repository's default SDK path. It requires the FFmpeg headers, five import libraries and runtime DLLs there.
`ffmpeg.exe` and `ffprobe.exe` are required separately; the SDK's `bin` directory is a search hint, not an
exclusive location. Linux discovers FFmpeg libraries through pkg-config.

**Why it matters:** Mismatched headers, import libraries and DLLs can link but fail at runtime. Check the selected
command-line tools as well when multiple FFmpeg versions are installed.
