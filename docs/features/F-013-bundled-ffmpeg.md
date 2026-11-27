# F-013: Bundled FFmpeg runtime and offline packages

Status: experimental

## Behaviour
Windows x64 and Ubuntu 24.04 x86_64 packages carry a checksum-pinned FFmpeg runtime: encoder, probe tool and matching
shared libraries. Developer/CI setup downloads and verifies the SDK before CMake configuration. End users extract
the whole application package; there is no runtime download, separate FFmpeg installation or normal PATH lookup.
SDK binaries and caches stay outside Git history; a submodule is not used for the binary distribution.

Startup validates linked library versions and private locations. Conversion defaults to the encoder beside the
actual executable, irrespective of working directory or competing host tools. Legacy default config values select
the bundle. An explicit absolute encoder override warns and bypasses bundled encoder-version consistency only;
relative/bare-name overrides fail validation. Required encoder-statistics options and FLAC/MP3 encoder support are
checked before conversion output is created. Failure never silently falls back to another encoder.
CLI, menu and worker share this policy; previews and no-encode operations do not launch encoder probes.

The package script produces Windows ZIP/Linux tar.gz archives from a clean runtime-component install, with a file-hash
manifest. Normal packaging requires Release mode and reviewed, matching source companion archives.
**Redistribution is currently blocked** because the complete corresponding-source/licensing provenance has not been
established. `--development` produces explicitly marked local/test artifacts, not a compliance approval.

## Entry points
- Developer/CI restore: `tools/prepare_ffmpeg.py#prepare`; verification: `cmake/FFmpegSDK.cmake`
- Startup: `AudioLibraryToolkit/src/platform/FFmpegRuntime.cpp#ValidateLibraries`
- CLI/menu/worker conversion: `AudioLibraryToolkit/src/conversion/FolderConvert.cpp#ExecuteConversion` ->
  `AudioLibraryToolkit/src/platform/FFmpegRuntime.cpp#ResolveEncoder`
- Packaging: `tools/package_application.py#package`, `cmake/InstallLinuxRuntime.cmake.in`

## Design and knowledge
- DESIGN: `AudioLibraryToolkit/DESIGN.md` -> Build and external dependencies; Bundled runtime and packages
- Configuration: F-007; conversion: F-001/F-002; preflight failure outputs: F-003; worker boundary: F-012
- Build/package commands: `AudioLibraryToolkit/README.md`; redistribution gate: `THIRD_PARTY_NOTICES.md`
- K-078: bundled runtime instead of host discovery or a source-only submodule

## Acceptance checks
- [ ] `ffmpeg-sdk-setup`: cached/offline restore works; modified payloads, wrong hashes and unsafe archives fail.
- [ ] Configure consumes the locked SDK and rejects an unrelated/missing SDK, including stale cache selections.
- [ ] `ffmpeg-bundled-runtime`: a relocated Unicode/space-containing tree converts with no system FFmpeg.
- [ ] Competing working-directory/PATH encoders are ignored; a missing bundled encoder fails before output creation.
- [ ] Absolute overrides warn; a non-encoder override is rejected before creating outputs.
- [ ] Worker stdout remains JSON Lines while a real default conversion succeeds; menu defaults also use the bundle.
- [ ] Missing libraries fail startup; even a matching-version library loaded from outside the bundle is rejected.
- [ ] Packaged/installed relocation works independently of the build directory and SDK, with private Linux RUNPATHs.
- [ ] Normal packaging refuses pending/missing/changed source materials; local artifacts are labelled development-only.
- [ ] Source fixture bytes remain unchanged on success and failure.

## Known limitations
- Linux compatibility is qualified against Ubuntu 24.04 x86_64, not every distribution/glibc baseline.
- macOS and other architectures are not packaged.
- Missing mandatory libraries may cause an OS-loader failure before an in-app diagnostic is possible.
- Explicit external encoders and deliberate OS-loader overrides are outside the normal bundled guarantee.
- Archive checksums establish identity, not indefinite upstream retention or legal compliance.
- The same revision across platforms does not promise bit-identical encoded output across OS/CPU implementations.
