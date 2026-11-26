import test from "node:test";
import assert from "node:assert/strict";
import path from "node:path";

import { getUnpackagedToolkitCandidate } from "../desktop/worker-candidates.mjs";

test("packaged discovery never probes the repository build output", () => {
  assert.equal(getUnpackagedToolkitCandidate(true, "C:\\project\\apps\\desktop", "win32"), null);
});

test("unpackaged Windows discovery can probe its repository build output", () => {
  assert.equal(
    getUnpackagedToolkitCandidate(false, "C:\\project\\AudioLibraryToolkit.Desktop\\apps\\desktop\\desktop", "win32"),
    path.resolve("C:\\project\\AudioLibraryToolkit.Desktop\\apps\\desktop\\desktop",
      "..", "..", "..", "..", "out", "build", "windows-debug", "AudioLibraryToolkit",
      "AudioLibraryToolkit.exe"),
  );
});
