---
id: K-072
kind: decision
title: "Preserve menu numbers when removing unused option 5"
area: [AudioLibraryToolkit]
status: active
supersedes: [K-049]
created: 2026-11-24
reviewed: 2026-11-24
cites:
  - path: AudioLibraryToolkit/src/app/AudioLibraryToolkitMain.cpp
    symbol: GetUserAction
  - test: ConversionTests.test_menu_descriptions_exit_eof_and_invalid_selections
---

**Fact:** The legacy menu omits option 5 and rejects it as invalid. Options 1-4, 6 (MP3 conversion), and
7 (metadata repair) retain their established numbers; `?` displays only available actions. Manual actions
still require the existing preview and confirmation flow.

**Why it matters:** Removing an unused choice should not silently change the numbers users already use for
MP3 conversion and metadata repair.

**Rejected:** Renumbering options 6 and 7 would change established menu selections. Reusing option 5 for
another action would turn a removed selection into a different operation.
