---
id: K-071
kind: defect
title: "Untyped Windows FFmpeg SDK paths can break Ninja builds"
area: ["External Dependencies"]
status: active
created: 2026-11-24
reviewed: 2026-11-24
cites:
  - path: cmake/platform/Windows.cmake
    symbol: FFmpeg_ROOT
---

**Fact:** On Windows, configuring an untyped `-DFFmpeg_ROOT=<path>` with native backslashes can generate
an invalid escape in Ninja's `VerifyGlobs.cmake`, causing the first build to fail. This reproduces on the
unchanged baseline. Passing `-DFFmpeg_ROOT:PATH=<path>` normalizes the path and the glob check succeeds.

**Why it matters:** The documented custom SDK setting can prevent a Windows build even when the SDK is valid.
