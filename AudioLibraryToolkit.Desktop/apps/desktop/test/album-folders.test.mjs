import { strict as assert } from "node:assert";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { test } from "node:test";
import { createAlbumFolderGate } from "../desktop/album-folders.mjs";

async function withLibrary(run) {
  const root = await mkdtemp(path.join(tmpdir(), "audio-ui-albums-"));
  try {
    await run(root);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
}

const reportOf = (...albums) => ({ groups: [{ albums }] });

test("no folder can be opened before a duplicate report was read", async () => {
  await withLibrary(async (root) => {
    const album = path.join(root, "album-a");
    await mkdir(album);
    await assert.rejects(createAlbumFolderGate().resolve(album), /current duplicate report/);
  });
});

test("a folder listed by the current report can be opened", async () => {
  await withLibrary(async (root) => {
    const album = path.join(root, "album-a");
    await mkdir(album);
    const gate = createAlbumFolderGate();
    gate.remember(reportOf(album));
    assert.equal(await gate.resolve(album), album);
  });
});

test("unlisted, traversing, relative and non-string paths are rejected", async () => {
  await withLibrary(async (root) => {
    const listed = path.join(root, "album-a");
    const sibling = path.join(root, "album-b");
    await mkdir(listed);
    await mkdir(sibling);
    const gate = createAlbumFolderGate();
    gate.remember(reportOf(listed, "album-a"));

    for (const candidate of [
      sibling,
      root,
      `${listed}${path.sep}..${path.sep}album-b`,
      "album-a",
      "",
      undefined,
      null,
      42,
    ]) {
      await assert.rejects(gate.resolve(candidate), /current duplicate report/, String(candidate));
    }
  });
});

test("a listed path that is a file or no longer exists is rejected", async () => {
  await withLibrary(async (root) => {
    const file = path.join(root, "not-a-folder");
    const missing = path.join(root, "gone");
    await writeFile(file, "x");
    const gate = createAlbumFolderGate();
    gate.remember(reportOf(file, missing));
    await assert.rejects(gate.resolve(file), /not a folder/);
    await assert.rejects(gate.resolve(missing), /not available/);
  });
});

test("a newer report replaces the list of openable folders", async () => {
  await withLibrary(async (root) => {
    const first = path.join(root, "first");
    const second = path.join(root, "second");
    await mkdir(first);
    await mkdir(second);
    const gate = createAlbumFolderGate();
    gate.remember(reportOf(first));
    gate.remember(reportOf(second));
    await assert.rejects(gate.resolve(first), /current duplicate report/);
    assert.equal(await gate.resolve(second), second);
  });
});
