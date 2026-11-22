---
id: K-061
kind: decision
title: "Require explicit setup for per-user configuration"
area: [FlacConvert]
status: active
supersedes: [K-007, K-060]
created: 2026-11-22
reviewed: 2026-11-22
cites:
  - path: FlacConvert/src/AppSettings.cpp
    symbol: "AppSettingsJson::CreateInitialConfiguration"
  - path: FlacConvert/src/AppSettings.cpp
    symbol: "AppSettingsJson::LoadForCommandLine"
  - path: FlacConvert/src/PlatformUtils.cpp
    symbol: UserConfigDirectory
---

**Fact:** Operational invocations use a per-user configuration, or an explicit CLI `--config`, without legacy
directory discovery. First-time setup requires a user-selected output directory and input library; an unattended
first run creates an editable empty template and fails rather than guessing paths. The menu requires both settings;
explicit CLI input and `--output-dir` can supply paths for an operation.

**Why it matters:** Machine-specific sample paths or a silent temporary output default can direct work to the
wrong library or location. An unattended run must not wait for input or silently select either path.

**Rejected:** Implicit working-directory discovery, a persistent legacy pointer, built-in private library paths,
and an automatic temporary output location.
