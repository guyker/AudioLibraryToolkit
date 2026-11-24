import test from "node:test";
import assert from "node:assert/strict";

import {
  WorkerClientError,
  WORKER_PROTOCOL_VERSION,
  parseWorkerEvent,
} from "../dist/index.js";

test("parses a compatible ready event and its advertised operations", () => {
  const event = parseWorkerEvent(JSON.stringify({
    protocolVersion: WORKER_PROTOCOL_VERSION,
    requestId: "",
    type: "ready",
    capabilities: ["convert", "scan"],
  }));
  assert.equal(event.type, "ready");
  assert.deepEqual(event.capabilities, ["convert", "scan"]);
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
