import test from "node:test";
import assert from "node:assert/strict";
import { Store } from "../src/db.js";
import { nodeView, recordContact, recordHeartbeat, sanitizeHeartbeat } from "../src/nodes.js";

const beat = (over: Record<string, unknown> = {}) =>
  sanitizeHeartbeat({ fw: "1.3.0", up: 1000, heap: 120000, maxblk: 90000, rssi: -55, rst: 1, relay: 1, relayfw: "0.2.0", relayrst: "power", jobs: 0, scr: 0, ota: 1, latest: "", ...over })!;

test("nodes: reboot and crash detection, controller loss, offline", () => {
  const store = new Store(":memory:");
  recordContact(store, { id: "aabbccddeeff", machine: "03", fw: "1.3.0", ip: "1.2.3.4" });
  let node = store.getNode("aabbccddeeff")!;
  recordHeartbeat(store, node, beat({ up: 1000 }), "1.2.3.4");
  node = store.getNode("aabbccddeeff")!;
  recordHeartbeat(store, node, beat({ up: 60, rst: 6 }), "1.2.3.4"); // uptime went backwards after a task watchdog reset
  node = store.getNode("aabbccddeeff")!;
  assert.equal(node.boots, 1);
  assert.ok(store.nodeLog("aabbccddeeff", 20).some((l) => l.kind === "crash" && /task watchdog/.test(l.detail)));
  let v = nodeView(node, new Map());
  assert.equal(v.level, "warn");
  assert.ok(v.issues.some((i) => /task watchdog/.test(i)));

  recordHeartbeat(store, node, beat({ up: 400, relay: 0 }), "1.2.3.4");
  node = store.getNode("aabbccddeeff")!;
  v = nodeView(node, new Map());
  assert.equal(v.level, "bad");
  assert.ok(store.nodeLog("aabbccddeeff", 20).some((l) => l.kind === "relay" && /lost/.test(l.detail)));

  assert.equal(nodeView(node, new Map(), Date.now() + 30 * 60_000).online, false);
  assert.equal(nodeView(node, new Map(), Date.now() + 30 * 60_000).level, "offline");
});

test("nodes: sanitising clamps numbers and strips markup", () => {
  const h = sanitizeHeartbeat({ fw: "1.3.0", up: -5, heap: 1e99, rssi: 40, rst: 99, relay: "yes", relayfw: "<b>0.2</b>", relayrst: "x".repeat(50), jobs: 9, scr: -1, ota: 77, latest: "9.9.9;drop" })!;
  assert.equal(h.up, 0);
  assert.equal(h.heap, 8_000_000);
  assert.equal(h.rssi, 0);
  assert.equal(h.rst, 20);
  assert.equal(h.relay, false);
  assert.equal(h.relayfw, "b0.2b");
  assert.equal(h.relayrst.length, 7);
  assert.equal(h.jobs, 3);
  assert.equal(h.ota, 15);
  assert.equal(h.latest, "9.9.9drop");
  assert.equal(sanitizeHeartbeat(null), null);
  assert.equal(sanitizeHeartbeat({ fw: "not-a-version" }), null);
});
