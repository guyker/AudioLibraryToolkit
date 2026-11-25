import { strict as assert } from "node:assert";
import { test } from "node:test";
import path from "node:path";
import { mkdtemp, readdir, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { getOperationPaths } from "../desktop/operation-paths.mjs";

test("catalog inputs point to the worker scan output, not the audio library", () => {
  const library = path.resolve("library", "Music");
  const output = path.resolve("output");
  const defaults = getOperationPaths(`${library}${path.sep}`, output);
  assert.equal(defaults.catalogDirectory, path.join(output, "catalog-json", "Music"));
  assert.equal(path.dirname(defaults.reportPath), output);
  assert.match(path.basename(defaults.reportPath), /^duplicates-[0-9a-f-]+\.json$/);
  assert.notEqual(defaults.reportPath, getOperationPaths(library, output).reportPath);
});

test("missing output is not replaced with the source or current directory", () => {
  assert.deepEqual(getOperationPaths(path.resolve("library"), "  "), {
    catalogDirectory: "", reportPath: "",
  });
  assert.equal(getOperationPaths("", path.resolve("output")).catalogDirectory, "");
  assert.equal(getOperationPaths(path.parse(process.cwd()).root, "output").catalogDirectory, "");
  assert.throws(() => getOperationPaths(null, "output"), /must be strings/);
});

test("suggesting paths creates no directories or files", async () => {
  const root = await mkdtemp(path.join(tmpdir(), "audio-ui-paths-"));
  try {
    const defaults = getOperationPaths(path.join(root, "source"), path.join(root, "output"));
    assert.equal(defaults.catalogDirectory, path.join(root, "output", "catalog-json", "source"));
    assert.deepEqual(await readdir(root), []);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});
