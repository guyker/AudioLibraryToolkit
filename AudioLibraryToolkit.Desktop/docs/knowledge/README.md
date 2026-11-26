# Desktop knowledge

This knowledge base owns durable desktop/Node-client decisions, not toolkit implementation facts.
Use module-relative paths and `UI-K-` IDs. Do not copy the global knowledge tooling.

From the module root:

```powershell
& "$HOME\.copilot\skills\project-memory\scripts\kb.ps1" search "confirmation" -Root "$PWD"
& "$HOME\.copilot\skills\project-memory\scripts\kb.ps1" new -Kind decision -Title "<fact>" -Area Desktop -Cite "apps/desktop/renderer/src/App.tsx#App" -Root "$PWD"
& "$HOME\.copilot\skills\project-memory\scripts\kb.ps1" affected -Root "$PWD"
& "$HOME\.copilot\skills\project-memory\scripts\kb.ps1" check -Root "$PWD"
```

Append facts; never rewrite their meaning. Supersede changed decisions, repair moved citations and maintain the
generated index. Record reasons/risks, not progress or ordinary refactors. Use the public protocol for backend
contracts rather than backend private source. No sensitive data or media-library details.

The [archive](archive/legacy-K-073-treat-explicit-desktop-album-selections-as-quarantine.md) retains the original
mixed-scope repository K-073 fact with repository-relative citations and lifecycle metadata as migration provenance
only. Its historical `K-055` relationship refers to the
[historical toolkit record](https://github.com/guyker/AudioLibraryToolkit/blob/main/docs/knowledge/K-055-keep-duplicate-review-groups-directly-connected-when-moving.md),
not this module's ID namespace. Current backend guarantees belong to
[toolkit K-077](https://github.com/guyker/AudioLibraryToolkit/blob/main/docs/knowledge/K-077-quarantine-only-explicitly-selected-albums-while-keeping.md),
which supersedes K-074; desktop confirmation belongs to
[UI-K-001](UI-K-001-confirm-individual-quarantine-selections.md).
