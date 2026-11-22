---
id: K-059
kind: decision
title: "Store configuration in a per-user platform directory"
area: [FlacConvert]
status: superseded
superseded_by: K-060
created: 2026-11-22
reviewed: 2026-11-22
cites:
  - path: FlacConvert/src/PlatformUtils.cpp
    symbol: UserConfigDirectory
  - path: FlacConvert/src/AppSettings.cpp
    symbol: "AppSettingsJson::LoadForCommandLine"
---

**Fact:** `PlatformUtils::UserConfigDirectory` selects `%LOCALAPPDATA%\AudioLibraryToolkit` on Windows,
`$XDG_CONFIG_HOME/AudioLibraryToolkit` (falling back to `~/.config/AudioLibraryToolkit`) on Linux, and
`~/Library/Application Support/AudioLibraryToolkit` on macOS. CLI and menu settings use this location; if a legacy
config is found, the loader copies it there and removes the working-directory pointer only after the copy succeeds.

**Why it matters:** The working directory and executable directory vary across launch methods and install layouts.
Configuration belongs to the current user, and library paths in the settings are commonly machine-specific, so the
same directory convention is used across platforms without implying that one config file is portable between machines.

**Rejected:** The executable/install directory may be read-only or move during upgrades; the working directory is
unstable and can be shared by unrelated invocations. A synced shared configuration would incorrectly imply that
machine-specific media paths work unchanged on every platform.
