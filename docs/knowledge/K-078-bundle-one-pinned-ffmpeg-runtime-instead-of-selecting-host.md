---
id: K-078
kind: decision
title: "Bundle one pinned FFmpeg runtime instead of selecting host installations"
area: ["External Dependencies"]
status: active
supersedes: [K-069]
created: 2026-11-26
reviewed: 2026-11-26
cites:
  - path: tools/prepare_ffmpeg.py
    symbol: prepare
  - path: AudioLibraryToolkit/src/platform/FFmpegRuntime.cpp
    symbol: ResolveEncoder
  - path: tools/package_application.py
    symbol: redistribution_sources
---

**Fact:** The build restores checksum-locked platform SDKs, and application packages carry the matching tools and
shared libraries. `ResolveEncoder` defaults to the sibling executable and never falls back to PATH; only an explicit
absolute override opts out of encoder-version consistency, with a warning. Acquisition occurs in developer/CI setup,
not on end-user machines. Normal packaging remains blocked until matching corresponding-source materials are reviewed.

**Why it matters:** Deploying only the application can silently combine one set of linked libraries with a different
host encoder, changing compatibility and conversion results. Pinning headers alone does not pin the runtime.
Keeping downloaded binaries out of Git does not remove redistribution/source obligations; archive hashes establish
identity, not legal clearance or continued upstream availability.

**Rejected:** A source submodule alone does not supply deployable tools or their dependency closure. Host discovery
and first-run downloads leave behaviour dependent on another machine or network state. A controlled FFmpeg source
build would provide stronger provenance but adds codec/toolchain ownership; reconsider it if the selected SDK's
complete required source materials cannot be established rather than silently changing the approved pin.
