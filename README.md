# AudioLibraryToolkit repository

Two independently built modules live here:

| Module | Purpose | Documentation |
|---|---|---|
| `AudioLibraryToolkit` | Headless C++ audio conversion, metadata and stream repair, catalog and duplicate management | [Toolkit guide](AudioLibraryToolkit/README.md), [design](AudioLibraryToolkit/DESIGN.md), [features](docs/features/index.md), [knowledge](docs/knowledge/index.md) |
| `AudioLibraryToolkit.Desktop` | Optional Electron desktop client | [Desktop guide](AudioLibraryToolkit.Desktop/README.md), [design](AudioLibraryToolkit.Desktop/DESIGN.md), [features](AudioLibraryToolkit.Desktop/docs/features/index.md), [knowledge](AudioLibraryToolkit.Desktop/docs/knowledge/index.md) |

The toolkit owns the [versioned worker interface](docs/api/worker-protocol-v1.md). The desktop consumes that interface
through a separately installed executable. Toolkit builds and installation never require the desktop, Node.js or
Electron; desktop builds never require toolkit source or a C++ compiler.

Native CMake commands run from this repository root. Desktop npm commands run from `AudioLibraryToolkit.Desktop`.
Each module has its own documentation and knowledge ownership; this page is repository navigation only.

See [CONTRIBUTING.md](CONTRIBUTING.md) and [AGENTS.md](AGENTS.md). Repository boundary checks run with
`python tools\check_module_boundaries.py`. Source is [GPL-3.0-or-later](LICENSE); see [third-party notices](THIRD_PARTY_NOTICES.md).
