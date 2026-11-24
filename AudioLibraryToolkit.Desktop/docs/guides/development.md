# Development and packaging

From `AudioLibraryToolkit.Desktop`, using a supported Node.js/npm installation:

```text
npm ci
npm test
npm run build
npm run dev
```

Run `npm ci` again after moving this workspace so npm recreates its workspace links at the new location.
The root commands forward to module-local workspaces. Vite listens on `127.0.0.1:5173`; build before launching Electron.
To explicitly load that development URL in Electron, set `VITE_DEV_SERVER_URL` to `http://127.0.0.1:5173`.
The toolkit is not built by these commands. Select a separately installed executable in Settings for runtime checks.

```text
npm run package:win
npm run package:linux
```

These produce NSIS/AppImage output below `apps/desktop/release` on a supported packaging host. To check unpacked
Windows packaging without creating an installer:

```text
npm run package:dir
```

Builds go to `apps/desktop/build` and `packages/worker-client/dist`; dependencies and generated outputs are ignored.
The Node client package has no renderer/Electron dependency. Its own tests run through the module's `npm test`.
Use generated fixtures and a separately staged backend for integration; never point tests at a user's library.

The optional client integration check accepts executable paths, not backend source. It creates and removes its own
short FLAC fixture, checks setup/handshake and non-mutating repair requests, and verifies launch errors:

```text
npm run test:integration --workspace @audio-library/worker-client -- "C:\Toolkit\AudioLibraryToolkit.exe" "C:\FFmpeg\ffmpeg.exe"
```

Before finishing changes, check this module's links, feature acceptance and knowledge using `AGENTS.md`.
The repository-level boundary checker is optional coordination tooling, not a dependency of this module's build.
