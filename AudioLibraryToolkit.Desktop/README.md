# AudioLibraryToolkit desktop client

This is a separate Electron project. It communicates with the C++ executable through
the toolkit's [public worker protocol](../docs/api/worker-protocol-v1.md); it does not link to backend code or share a
C++ ABI. Its build requires no backend checkout or C++ compiler.

Read the [design](DESIGN.md), [features](docs/features/index.md), [setup guide](docs/guides/setup.md),
[development/packaging guide](docs/guides/development.md) and [knowledge](docs/knowledge/index.md).

## Development

From this directory:

```text
npm ci
npm test
npm run build
npm run dev
```

`npm run package:win` creates a Windows NSIS installer; `npm run package:linux` creates a Linux AppImage. These package
the UI and TypeScript worker client, not the C++ backend or FFmpeg.

In the app's Settings, choose a separately built `AudioLibraryToolkit` executable. Optionally select its config file
and set an output root. Settings can create a first-run configuration from chosen output/library paths without
overwriting an existing file. Conversion and repair show non-mutating previews before execution; MP3 conversion has
per-issue tag choices. Duplicate reports support explicit album selection and confirmed quarantine.

The UI owns interaction, not media-processing policy. It uses only a compatible installed executable at runtime;
backend configuration, protocol validation, source protection and verified output installation remain authoritative.
The workspace's Node client is frontend-framework-independent and is not part of the toolkit installation.
