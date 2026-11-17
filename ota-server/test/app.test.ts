import test, { before, after } from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdtempSync, rmSync } from "node:fs";
import os from "node:os";
import path from "node:path";
process.env.NODE_ENV = "test";
const { buildApp } = await import("../src/app.js");
const { Store } = await import("../src/db.js");
const { loadConfig } = await import("../src/config.js");
import type { FastifyInstance } from "fastify";
import type { AddressInfo } from "node:net";
import { fakeImage, multipart } from "./helpers.js";

let app: FastifyInstance;
let dir: string;
let token = "";
const auth = () => ({ authorization: `Bearer ${token}` });

before(async () => {
  dir = mkdtempSync(path.join(os.tmpdir(), "ota-test-"));
  const config = loadConfig({ JWT_SECRET: "x".repeat(40), ADMIN_PASSWORD: "correct horse", DATA_DIR: dir, REPO_DIR: path.join(dir, "repo"), ALLOWED_ORIGINS: "https://dash.example.com,https://*.vercel.app" });
  app = await buildApp(config, new Store(":memory:"));
  await app.listen({ port: 0, host: "127.0.0.1" });
});
after(async () => {
  await app.close();
  rmSync(dir, { recursive: true, force: true });
});

test("admin API needs a login", async () => {
  assert.equal((await app.inject("/api/releases")).statusCode, 401);
  assert.equal((await app.inject({ method: "POST", url: "/api/login", payload: { password: "nope" } })).statusCode, 401);
  const ok = await app.inject({ method: "POST", url: "/api/login", payload: { password: "correct horse" } });
  assert.equal(ok.statusCode, 200);
  token = ok.json().token;
  assert.equal((await app.inject({ url: "/api/releases", headers: auth() })).statusCode, 200);
});

const DEV = { "x-device-id": "aabbccddeeff", "x-machine-id": "03", "x-fw-version": "1.1.0", "cf-connecting-ip": "203.0.113.7" };
const upload = async (version: string, fields: Record<string, string> = {}) => {
  const up = multipart(fields, { name: "firmware.bin", data: fakeImage(version) });
  return app.inject({ method: "POST", url: "/api/releases", payload: up.payload, headers: { ...up.headers, ...auth() } });
};

test("a kiosk with nothing assigned gets the inert manifest, which passes the device sanity checks", async () => {
  const r = await app.inject({ url: "/ota/manifest.json", headers: DEV });
  assert.equal(r.statusCode, 200);
  const m = r.json();
  assert.equal(m.version, "0.0.0");
  assert.equal(m.sha256.length, 64);
  assert.ok(m.size >= 100_000);
  assert.equal((await app.inject({ url: "/ota/firmware.bin", headers: DEV })).statusCode, 404);
});

test("the first contact registers the node", async () => {
  const nodes = (await app.inject({ url: "/api/nodes", headers: auth() })).json();
  assert.equal(nodes.length, 1);
  assert.equal(nodes[0].deviceId, "aabbccddeeff");
  assert.equal(nodes[0].machineId, "03");
  assert.equal(nodes[0].fwVersion, "1.1.0");
  assert.equal(nodes[0].target, null);
});

test("upload validates and stores a release without publishing it to anybody", async () => {
  const img = fakeImage("1.4.0");
  const up = multipart({ min_version: "1.1.0", notes: "test" }, { name: "firmware.bin", data: img });
  const res = await app.inject({ method: "POST", url: "/api/releases", payload: up.payload, headers: { ...up.headers, ...auth() } });
  assert.equal(res.statusCode, 201, res.body);
  assert.equal(res.json().version, "1.4.0");
  assert.equal(res.json().nodes, 0);
  assert.equal((await app.inject({ url: "/ota/manifest.json", headers: DEV })).json().version, "0.0.0"); // not broadcast
  assert.equal((await app.inject("/ota/manifest.json")).json().version, "0.0.0"); // old firmware without an id: nothing either
});

test("rejects version mismatch, duplicates, bad min_version and bad images", async () => {
  const send = async (fields: Record<string, string>, data: Buffer) => {
    const up = multipart(fields, { name: "firmware.bin", data });
    return app.inject({ method: "POST", url: "/api/releases", payload: up.payload, headers: { ...up.headers, ...auth() } });
  };
  assert.equal((await send({ version: "9.9.9" }, fakeImage("1.5.0"))).statusCode, 422); // label != binary
  assert.equal((await send({}, fakeImage("1.4.0"))).statusCode, 409); // duplicate version
  assert.equal((await send({ min_version: "2.0.0" }, fakeImage("1.5.0"))).statusCode, 422); // min > version
  assert.equal((await send({}, fakeImage("1.5.0", { marker: false }))).statusCode, 422);
  assert.equal((await send({}, fakeImage("1.5.0", { size: 2_100_000 }))).statusCode, 413);
  const noFile = multipart({ version: "1.5.0" });
  assert.equal((await app.inject({ method: "POST", url: "/api/releases", payload: noFile.payload, headers: { ...noFile.headers, ...auth() } })).statusCode, 400);
});

test("targeting: only the chosen node sees the release; another node and old firmware do not", async () => {
  const rels = (await app.inject({ url: "/api/releases", headers: auth() })).json();
  const rel = rels.find((r: { version: string }) => r.version === "1.4.0");
  const other = { ...DEV, "x-device-id": "112233445566", "x-machine-id": "04" };
  await app.inject({ url: "/ota/manifest.json", headers: other }); // second kiosk shows up

  const t = await app.inject({ method: "POST", url: "/api/nodes/target", headers: auth(), payload: { releaseId: rel.id, deviceIds: ["aabbccddeeff"] } });
  assert.equal(t.statusCode, 200);
  assert.deepEqual(t.json().results, [{ deviceId: "aabbccddeeff", ok: true }]);

  const m = (await app.inject({ url: "/ota/manifest.json", headers: DEV })).json();
  assert.deepEqual(m, { version: "1.4.0", sha256: rel.sha256, size: rel.size, min_version: "1.1.0" });
  assert.equal((await app.inject({ url: "/ota/manifest.json", headers: other })).json().version, "0.0.0");
  assert.equal((await app.inject("/ota/manifest.json")).json().version, "0.0.0");

  // over a real socket: this is the path a kiosk takes (and the one that logs "download" when the connection closes)
  const base = `http://127.0.0.1:${(app.server.address() as AddressInfo).port}`;
  const fw = await fetch(`${base}/ota/firmware.bin`, { headers: DEV });
  assert.equal(fw.status, 200);
  assert.equal(fw.headers.get("content-length"), String(rel.size));
  assert.equal(createHash("sha256").update(Buffer.from(await fw.arrayBuffer())).digest("hex"), rel.sha256);
  assert.equal((await app.inject({ url: "/ota/firmware.bin", headers: other })).statusCode, 404);
  assert.equal((await app.inject("/ota/firmware.bin")).statusCode, 404);

  // the download is logged when the connection closes, which can land a moment after inject() resolves
  let node = (await app.inject({ url: "/api/nodes/aabbccddeeff", headers: auth() })).json();
  for (let i = 0; i < 40 && !node.log.some((l: { kind: string }) => l.kind === "download"); i++) {
    await new Promise((r) => setTimeout(r, 25));
    node = (await app.inject({ url: "/api/nodes/aabbccddeeff", headers: auth() })).json();
  }
  assert.equal(node.node.update, "pending");
  assert.ok(node.log.some((l: { kind: string }) => l.kind === "download"));
  assert.ok(node.log.some((l: { kind: string }) => l.kind === "assigned"));
});

test("targeting rules: no downgrade, min_version, unknown nodes, deletion guard, unassign", async () => {
  const rels = (await app.inject({ url: "/api/releases", headers: auth() })).json();
  const rel = rels.find((r: { version: string }) => r.version === "1.4.0");
  const post = (payload: unknown) => app.inject({ method: "POST", url: "/api/nodes/target", headers: auth(), payload });

  // a node that already runs 1.4.0 (reports it via heartbeat) cannot be assigned 1.4.0 again
  const beat = await app.inject({ method: "POST", url: "/ota/heartbeat", headers: { ...DEV, "x-fw-version": "1.4.0" }, payload: { fw: "1.4.0", up: 500, heap: 120000, maxblk: 90000, rssi: -60, rst: 1, relay: 1, relayfw: "0.2.0", relayrst: "power", jobs: 0, scr: 0, ota: 1, latest: "1.4.0" } });
  assert.equal(beat.statusCode, 204, beat.body);
  const again = (await post({ releaseId: rel.id, deviceIds: ["aabbccddeeff"] })).json();
  assert.equal(again.results[0].ok, false);
  assert.match(again.results[0].error, /downgrade|reinstall/);
  assert.equal((await app.inject({ url: "/api/nodes/aabbccddeeff", headers: auth() })).json().node.update, "done");

  assert.equal((await post({ releaseId: rel.id, deviceIds: ["000000000000"] })).json().results[0].ok, false); // unknown node
  assert.equal((await post({ releaseId: rel.id, deviceIds: [] })).statusCode, 400); // no "everybody" shortcut
  assert.equal((await post({ releaseId: 9999, deviceIds: ["aabbccddeeff"] })).statusCode, 404);

  assert.equal((await app.inject({ method: "DELETE", url: `/api/releases/${rel.id}`, headers: auth() })).statusCode, 409); // still assigned
  assert.equal((await post({ releaseId: null, deviceIds: ["aabbccddeeff"] })).json().results[0].ok, true);
  assert.equal((await app.inject({ url: "/ota/manifest.json", headers: DEV })).json().version, "0.0.0");
  assert.equal((await app.inject({ method: "DELETE", url: `/api/releases/${rel.id}`, headers: auth() })).statusCode, 204);
});

test("heartbeats: health is stored sanitised, reboots are detected, flooding is limited", async () => {
  const dev = { ...DEV, "x-device-id": "0a0b0c0d0e0f", "x-machine-id": "07", "x-fw-version": "1.4.0" };
  const body = { fw: "1.4.0", up: 900, heap: 100000, maxblk: 40000, rssi: -90, rst: 1, relay: 0, relayfw: "0.2.0<script>", relayrst: "iwdg", jobs: 1, scr: 5, ota: 1, latest: "1.4.0" };
  assert.equal((await app.inject({ method: "POST", url: "/ota/heartbeat", headers: dev, payload: body })).statusCode, 204);
  assert.equal((await app.inject({ method: "POST", url: "/ota/heartbeat", headers: dev, payload: body })).statusCode, 429);
  assert.equal((await app.inject({ method: "POST", url: "/ota/heartbeat", headers: { ...dev, "x-device-id": "nothex" }, payload: body })).statusCode, 400);
  assert.equal((await app.inject({ method: "POST", url: "/ota/heartbeat", headers: { ...dev, "x-device-id": "1a0b0c0d0e0f" }, payload: { fw: "evil" } })).statusCode, 400);

  const n = (await app.inject({ url: "/api/nodes/0a0b0c0d0e0f", headers: auth() })).json();
  assert.equal(n.node.online, true);
  assert.equal(n.node.health.relayfw, "0.2.0script"); // markup characters stripped
  assert.equal(n.node.health.activity, "wash cycle running");
  assert.equal(n.node.health.screen, "running");
  assert.equal(n.node.level, "bad"); // controller link down
  assert.ok(n.node.issues.some((i: string) => /relay/.test(i)));
  assert.ok(n.node.issues.some((i: string) => /Wi-Fi/.test(i)));
  assert.ok(n.node.issues.some((i: string) => /memory/.test(i)));

  const st = (await app.inject({ url: "/api/status", headers: auth() })).json();
  assert.equal(st.nodes.total, 3);
  assert.equal(st.nodes.online, 3);
  assert.equal(st.nodes.attention, 1);
});

test("device checks are logged and summarized", async () => {
  await app.inject({ url: "/ota/manifest.json", headers: { "user-agent": "washable-ota/1.1.0", "cf-connecting-ip": "203.0.113.9" } });
  const ev = (await app.inject({ url: "/api/events?kind=manifest&limit=1", headers: auth() })).json();
  assert.equal(ev[0].device_version, "1.1.0");
  assert.equal(ev[0].ip, "203.0.113.9");
  const st = (await app.inject({ url: "/api/status", headers: auth() })).json();
  assert.ok(st.versions24h.some((v: { version: string }) => v.version === "1.1.0"));
});

test("CORS only for the configured dashboard origins", async () => {
  const allowed = await app.inject({ method: "OPTIONS", url: "/api/releases", headers: { origin: "https://my-app-git-x.vercel.app", "access-control-request-method": "GET" } });
  assert.equal(allowed.headers["access-control-allow-origin"], "https://my-app-git-x.vercel.app");
  const denied = await app.inject({ method: "OPTIONS", url: "/api/releases", headers: { origin: "https://evil.example", "access-control-request-method": "GET" } });
  assert.equal(denied.headers["access-control-allow-origin"], undefined);
});
