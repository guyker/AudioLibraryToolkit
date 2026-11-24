---
id: K-075
kind: decision
title: "Deliver the headless toolkit independently through a client-neutral process API"
area: [AudioLibraryToolkit]
status: active
created: 2026-11-24
reviewed: 2026-11-24
cites:
  - path: AudioLibraryToolkit/src/app/WorkerProtocol.cpp
    symbol: "WorkerProtocol::Run"
  - path: CMakeLists.txt
---

**Fact:** The toolkit's native build and installation contain no client source/runtime dependency.
`WorkerProtocol::Run` exposes application operations through a versioned process contract usable by external
applications or automation. Client implementations own their interaction and consume that contract, never the
reverse. The internal static target is not a public SDK or stable library ABI.

**Why it matters:** Headless delivery must remain usable without installing or building an optional graphical client.
Different consumers need the same application guarantees without imposing their runtime or interaction choices on
the toolkit.

**Rejected:** Building/loading a client from the toolkit would make headless delivery depend on that client's
technology stack. Treating an internal native target as a published SDK would promise an ABI and packaging contract
that have not been designed.
