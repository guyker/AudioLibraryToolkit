import { strict as assert } from "node:assert";
import { test } from "node:test";
import { subscribeToLogs } from "../renderer/src/logStream.mjs";

function createBridge() {
  let handler;
  let resolve;
  let reject;
  const snapshot = new Promise((ok, fail) => { resolve = ok; reject = fail; });
  let subscribed = false;
  return {
    bridge: {
      getLogs: () => snapshot,
      onLogUpdate: (callback) => {
        handler = callback;
        subscribed = true;
        return () => { subscribed = false; };
      },
    },
    emit: (update) => handler(update),
    resolve,
    reject,
    isSubscribed: () => subscribed,
  };
}

test("reconciles events arriving before the initial snapshot without duplicates", async () => {
  const source = createBridge();
  const values = [];
  const errors = [];
  const stop = subscribeToLogs(source.bridge, (value) => values.push(value), (error) => errors.push(error));
  source.emit({ revision: 1, reset: false, text: "A" });
  source.emit({ revision: 2, reset: false, text: "B" });
  source.resolve({ revision: 1, text: "A" });
  await new Promise(setImmediate);
  assert.deepEqual(values, ["AB"]);
  source.emit({ revision: 3, reset: false, text: "C" });
  assert.equal(values.at(-1), "ABC");
  assert.deepEqual(errors, []);
  stop();
  assert.equal(source.isSubscribed(), false);
});

test("honors reset and bounds logs even when updates precede the snapshot", async () => {
  const source = createBridge();
  const values = [];
  subscribeToLogs(source.bridge, (value) => values.push(value), assert.fail);
  source.emit({ revision: 2, reset: true, text: "new" });
  source.emit({ revision: 3, reset: false, text: "x".repeat(200_001) });
  source.resolve({ revision: 1, text: "old" });
  await new Promise(setImmediate);
  assert.equal(values[0], "x".repeat(200_000));
  source.emit({ revision: 4, reset: true, text: "next run" });
  assert.equal(values.at(-1), "next run");
});

test("reports snapshot failures and unsubscribes", async () => {
  const source = createBridge();
  const errors = [];
  subscribeToLogs(source.bridge, assert.fail, (error) => errors.push(error));
  source.reject(new Error("IPC unavailable"));
  await new Promise(setImmediate);
  assert.match(errors[0].message, /IPC unavailable/);
  assert.equal(source.isSubscribed(), false);
});

test("does not deliver a snapshot after the viewer closes", async () => {
  const source = createBridge();
  const values = [];
  const stop = subscribeToLogs(source.bridge, (value) => values.push(value), assert.fail);
  stop();
  source.resolve({ revision: 1, text: "late" });
  await new Promise(setImmediate);
  assert.deepEqual(values, []);
  assert.equal(source.isSubscribed(), false);
});
