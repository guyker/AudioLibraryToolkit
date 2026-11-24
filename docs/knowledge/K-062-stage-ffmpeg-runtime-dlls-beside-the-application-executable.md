---
id: K-062
kind: domain
title: "Stage FFmpeg runtime DLLs beside the application executable"
area: ["External Dependencies"]
status: active
supersedes: [K-044]
created: 2026-11-22
reviewed: 2026-11-24
cites:
  - path: cmake/platform/Windows.cmake
    symbol: afc_copy_windows_ffmpeg_runtime
---

**Fact:** On Windows, `afc_copy_windows_ffmpeg_runtime` copies the selected SDK's DLLs beside the application
and native test executables after build; installation places the DLLs beside the installed application.

**Why it matters:** Without the runtime DLLs, the Windows loader cannot start the application or its native tests.
