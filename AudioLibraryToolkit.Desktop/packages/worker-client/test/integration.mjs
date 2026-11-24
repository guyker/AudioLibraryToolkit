import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { existsSync, mkdtempSync, mkdirSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { WorkerClient } from "../dist/index.js";

const [executable, ffmpeg] = process.argv.slice(2);
assert.ok(executable && ffmpeg, "Usage: integration.mjs <installed toolkit executable> <ffmpeg executable>");
const root = mkdtempSync(path.join(tmpdir(), "audio-client-isolation-"));
try {
  const libraryPath = path.join(root, "library");
  mkdirSync(libraryPath);
  const track = path.join(libraryPath, "track.flac");
  const generated = spawnSync(ffmpeg, [
    "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=0.1",
    "-c:a", "flac", track,
  ], { encoding: "utf8", windowsHide: true });
  assert.equal(generated.status, 0, generated.error?.message ?? generated.stderr);
  const hash = () => createHash("sha256").update(readFileSync(track)).digest("hex");
  const original = hash();
  const configPath = path.join(root, "config.json");
  const outputDirectory = path.join(root, "output");
  const client = new WorkerClient(executable);
  let ready = false;
  const initialized = await client.run({
    operation: "initializeConfig", parameters: { libraryPath, outputDirectory, configPath },
  }, { onEvent(event) {
    if (event.type === "ready") {
      ready = true;
      assert.ok(event.capabilities.includes("repair"));
    }
  } });
  assert.ok(ready);
  assert.equal(initialized.status, "succeeded", initialized.stderr);
  assert.ok(existsSync(configPath));
  for (const request of [
    { operation: "repair", mode: "preview", parameters: { inputPath: libraryPath, configPath } },
    { operation: "repair", parameters: { inputPath: libraryPath, configPath, dryRun: true } },
  ]) {
    const result = await client.run(request);
    assert.equal(result.status, "succeeded", result.stderr);
  }
  assert.equal(hash(), original, "Preview/dry-run modified the source track.");
  assert.ok(!existsSync(path.join(outputDirectory, "repaired-metadata")));
  await assert.rejects(
    new WorkerClient(path.join(root, "missing-backend.exe")).run({
      operation: "repair", parameters: { inputPath: libraryPath },
    }),
    /Could not start backend/,
  );
  console.log("Installed-backend handshake, setup, preview/dry-run and launch failure passed; source unchanged.");
} finally {
  rmSync(root, { recursive: true, force: true });
}
