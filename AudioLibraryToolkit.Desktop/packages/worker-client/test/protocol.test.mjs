import test from "node:test";
import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { PassThrough, Writable } from "node:stream";

import {
  WorkerClient,
  WorkerClientError,
  WORKER_PROTOCOL_VERSION,
  parseWorkerEvent,
} from "../dist/index.js";

test("parses a compatible ready event and its advertised operations", () => {
  const event = parseWorkerEvent(JSON.stringify({
    protocolVersion: WORKER_PROTOCOL_VERSION,
    requestId: "",
    type: "ready",
    capabilities: ["convert", "scan", "initializeDefaultConfig"],
  }));
  assert.equal(event.type, "ready");
  assert.deepEqual(event.capabilities, ["convert", "scan", "initializeDefaultConfig"]);
});

test("rejects malformed, incompatible, and unknown events", () => {
  assert.throws(() => parseWorkerEvent("{"), WorkerClientError);
  assert.throws(() => parseWorkerEvent(JSON.stringify({
    protocolVersion: 99,
    requestId: "x",
    type: "ready",
    capabilities: [],
  })), /incompatible protocol event/);
  assert.throws(() => parseWorkerEvent(JSON.stringify({
    protocolVersion: 1,
    requestId: "x",
    type: "mystery",
  })), /Unknown backend event type/);
});

test("validates progress and completion payloads", () => {
  assert.equal(parseWorkerEvent(JSON.stringify({
    protocolVersion: 1,
    requestId: "x",
    type: "progress",
    message: "Scanning: 2/10 files (20%)",
  })).type, "progress");
  assert.throws(() => parseWorkerEvent(JSON.stringify({
    protocolVersion: 1,
    requestId: "x",
    type: "completed",
    exitCode: "0",
    status: "succeeded",
  })), /Invalid completion event/);
});

test("does not send an operation absent from the worker capability list", async () => {
  let requestText = "";
  const spawnWorker = () => {
    const child = new EventEmitter();
    child.pid = undefined;
    child.kill = () => true;
    child.stdout = new PassThrough();
    child.stderr = new PassThrough();
    child.stdin = new Writable({
      write(chunk, _encoding, callback) {
        requestText += chunk.toString();
        callback();
      },
    });
    child.stdin.on("finish", () => {
      setImmediate(() => {
        child.stdout.end();
        child.stderr.end();
        child.emit("close", 0, null);
      });
    });
    setImmediate(() => {
      child.stdout.write(JSON.stringify({
        protocolVersion: WORKER_PROTOCOL_VERSION,
        requestId: "",
        type: "ready",
        capabilities: ["readConfiguration"],
      }) + "\n");
      setImmediate(() => {
        child.stdout.end();
        child.stderr.end();
        child.emit("close", 0, null);
      });
    });
    return child;
  };
  const client = new WorkerClient("fixture-worker", process.env, spawnWorker);

  await assert.rejects(client.run({
    operation: "repair",
    parameters: { inputPath: "generated-fixture" },
  }), /does not advertise support for operation "repair"/);
  assert.equal(requestText, "");
});
