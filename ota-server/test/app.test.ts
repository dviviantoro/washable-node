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
import { fakeImage, multipart } from "./helpers.js";

let app: FastifyInstance;
let dir: string;
let token = "";
const auth = () => ({ authorization: `Bearer ${token}` });

before(async () => {
  dir = mkdtempSync(path.join(os.tmpdir(), "ota-test-"));
  const config = loadConfig({ JWT_SECRET: "x".repeat(40), ADMIN_PASSWORD: "correct horse", DATA_DIR: dir, ALLOWED_ORIGINS: "https://dash.example.com,https://*.vercel.app" });
  app = await buildApp(config, new Store(":memory:"));
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

test("before any release the manifest is inert and passes the device sanity checks", async () => {
  const r = await app.inject("/ota/manifest.json");
  assert.equal(r.statusCode, 200);
  const m = r.json();
  assert.equal(m.version, "0.0.0");
  assert.equal(m.sha256.length, 64);
  assert.ok(m.size >= 100_000);
  assert.equal((await app.inject("/ota/firmware.bin")).statusCode, 404);
});

test("upload validates, publishes, and serves exactly the uploaded bytes", async () => {
  const img = fakeImage("1.4.0");
  const up = multipart({ min_version: "1.1.0", notes: "test", activate: "true" }, { name: "firmware.bin", data: img });
  const res = await app.inject({ method: "POST", url: "/api/releases", payload: up.payload, headers: { ...up.headers, ...auth() } });
  assert.equal(res.statusCode, 201, res.body);
  assert.equal(res.json().version, "1.4.0");
  assert.equal(res.json().active, true);

  const m = (await app.inject("/ota/manifest.json")).json();
  assert.deepEqual(m, { version: "1.4.0", sha256: createHash("sha256").update(img).digest("hex"), size: img.length, min_version: "1.1.0" });

  const fw = await app.inject("/ota/firmware.bin");
  assert.equal(fw.statusCode, 200);
  assert.equal(fw.headers["content-length"], String(img.length));
  assert.ok(fw.rawPayload.equals(img));
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

test("activate, pause and delete rules", async () => {
  const up = multipart({}, { name: "firmware.bin", data: fakeImage("1.5.0") });
  const created = (await app.inject({ method: "POST", url: "/api/releases", payload: up.payload, headers: { ...up.headers, ...auth() } })).json();
  assert.equal(created.active, false);

  const list = (await app.inject({ url: "/api/releases", headers: auth() })).json();
  const active = list.find((r: { active: boolean }) => r.active);
  assert.equal(active.version, "1.4.0");
  assert.equal((await app.inject({ method: "DELETE", url: `/api/releases/${active.id}`, headers: auth() })).statusCode, 409);

  assert.equal((await app.inject({ method: "POST", url: `/api/releases/${created.id}/activate`, headers: auth() })).statusCode, 200);
  assert.equal((await app.inject("/ota/manifest.json")).json().version, "1.5.0");
  assert.equal((await app.inject({ method: "DELETE", url: `/api/releases/${active.id}`, headers: auth() })).statusCode, 204);

  await app.inject({ method: "POST", url: "/api/releases/deactivate", headers: auth() });
  assert.equal((await app.inject("/ota/manifest.json")).json().version, "0.0.0");
});

test("device checks are logged and summarized", async () => {
  await app.inject({ url: "/ota/manifest.json", headers: { "user-agent": "washable-ota/1.1.0", "cf-connecting-ip": "203.0.113.7" } });
  const ev = (await app.inject({ url: "/api/events?kind=manifest&limit=1", headers: auth() })).json();
  assert.equal(ev[0].device_version, "1.1.0");
  assert.equal(ev[0].ip, "203.0.113.7");
  const st = (await app.inject({ url: "/api/status", headers: auth() })).json();
  assert.ok(st.versions24h.some((v: { version: string }) => v.version === "1.1.0"));
  assert.equal(st.paused, true);
});

test("CORS only for the configured dashboard origins", async () => {
  const allowed = await app.inject({ method: "OPTIONS", url: "/api/releases", headers: { origin: "https://my-app-git-x.vercel.app", "access-control-request-method": "GET" } });
  assert.equal(allowed.headers["access-control-allow-origin"], "https://my-app-git-x.vercel.app");
  const denied = await app.inject({ method: "OPTIONS", url: "/api/releases", headers: { origin: "https://evil.example", "access-control-request-method": "GET" } });
  assert.equal(denied.headers["access-control-allow-origin"], undefined);
});
