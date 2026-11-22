---
id: K-060
kind: decision
title: "Use per-user configuration without legacy discovery"
area: [FlacConvert]
status: superseded
supersedes: [K-059]
superseded_by: K-061
created: 2026-11-22
reviewed: 2026-11-22
cites:
  - path: FlacConvert/src/AppSettings.cpp
    symbol: "AppSettingsJson::LoadForCommandLine"
  - path: FlacConvert/src/PlatformUtils.cpp
    symbol: UserConfigDirectory
---

**Fact:** CLI and menu configuration use `PlatformUtils::UserConfigDirectory`: `%LOCALAPPDATA%\AudioLibraryToolkit`
on Windows, `$XDG_CONFIG_HOME/AudioLibraryToolkit` (or `~/.config/AudioLibraryToolkit`) on Linux, and
`~/Library/Application Support/AudioLibraryToolkit` on macOS. CLI commands can override this with `--config`;
without it they use built-in defaults when the per-user file is absent. Neither entry point discovers or migrates
configurations from the working directory.

**Why it matters:** The working and executable directories depend on how the app is launched or installed.
Per-user configuration avoids that instability, and media paths remain specific to the machine using them.

**Rejected:** A working-directory pointer and automatic migration add legacy behavior that is not needed. Sharing
one synced configuration across machines would incorrectly assume their media paths are interchangeable.
