---
id: K-064
kind: decision
title: "Seed first-run configuration from a shipped commented template"
area: [AudioLibraryToolkit]
status: active
created: 2026-11-22
reviewed: 2026-11-26
cites:
  - path: AudioLibraryToolkit/src/app/AppSettings.cpp
    symbol: "AppSettingsJson::FindDefaultConfigTemplate"
---

**Fact:** When the per-user configuration is missing, `AppSettingsJson::CreateInitialConfiguration` copies
`config.default.json`, found by `AppSettingsJson::FindDefaultConfigTemplate` beside the executable or in
`<exe>/../share/AudioLibraryToolkit`. Interactive setup answers replace only the template's empty `OutDirectory` and
`MediaDirectoryList` values. A missing or invalid template, or one with preset paths, falls back with a warning to
uncommented JSON generated from built-in defaults, so first run never depends on an installed file. The user chose
this behaviour.

**Why it matters:** The comments in the template are the main in-file documentation for optional settings that users
are told to open and edit. Removing the template from build staging or install rules silently degrades first run to
an uncommented file; a template with preset paths would reintroduce machine-specific locations rejected by K-061.

**Rejected:** Built-in generation only (loses explanatory comments); embedding the template text in the executable
(duplicates the source of truth and complicates edits); requiring the template to exist (an incomplete install or a
copied bare executable could not start).
