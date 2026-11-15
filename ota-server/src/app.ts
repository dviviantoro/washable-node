import Fastify, { type FastifyInstance, type FastifyRequest } from "fastify";
import cors from "@fastify/cors";
import jwt from "@fastify/jwt";
import multipart from "@fastify/multipart";
import rateLimit from "@fastify/rate-limit";
import { createHash, timingSafeEqual } from "node:crypto";
import { createReadStream, existsSync, mkdirSync } from "node:fs";
import { rename, rm, writeFile } from "node:fs/promises";
import path from "node:path";
import type { Config } from "./config.js";
import { Store, type EventKind, type Release } from "./db.js";
import { DEVICE_ID_RE, MIN_BEAT_GAP_MS, nodeView, recordContact, recordHeartbeat, sanitizeHeartbeat } from "./nodes.js";
import { JobManager } from "./jobs.js";
import { Workspace } from "./workspace.js";
import { registerStudio } from "./studio.js";
import { ImageError, checkMinVersion, inspectFirmware } from "./image.js";
import { cmpVer, isSemver } from "./version.js";

declare module "fastify" {
  interface FastifyInstance {
    authenticate: (req: FastifyRequest) => Promise<void>;
  }
}

const SERVER_VERSION = "2.0.0";

/**
 * Served to a kiosk that has no release assigned (and to old firmware that does not identify itself). ota.cpp compares versions and sees
 * "0.0.0 <= current" -> "terbaru" (up to date), so kiosks stay quiet instead of showing a red error.
 * sha256/size only have to pass the device's sanity checks (64 chars, size >= 100000); it is never downloaded.
 */
const INERT_MANIFEST = { version: "0.0.0", sha256: "0".repeat(64), size: 1_000_000 };

function manifestFor(rel: Release | undefined): Record<string, unknown> {
  if (!rel) return INERT_MANIFEST;
  // Keep it tiny and in this key order: the firmware uses a naive string-search JSON reader and caps the manifest at 4 KB.
  const m: Record<string, unknown> = { version: rel.version, sha256: rel.sha256, size: rel.size };
  if (rel.min_version) m.min_version = rel.min_version;
  return m;
}

function originAllowed(origin: string, allowed: string[]): boolean {
  return allowed.some((a) => {
    if (!a.includes("*")) return a === origin;
    const re = new RegExp("^" + a.split("*").map((p) => p.replace(/[.+?^${}()|[\]\\]/g, "\\$&")).join("[a-z0-9-]+(?:\\.[a-z0-9-]+)*") + "$", "i");
    return re.test(origin);
  });
}

function sameSecret(a: string, b: string): boolean {
  const ha = createHash("sha256").update(a).digest();
  const hb = createHash("sha256").update(b).digest();
  return timingSafeEqual(ha, hb);
}

function clientIp(req: FastifyRequest): string {
  const cf = req.headers["cf-connecting-ip"];
  return (Array.isArray(cf) ? cf[0] : cf) ?? req.ip;
}

function deviceVersion(req: FastifyRequest): string {
  const m = /^washable-ota\/(\d+\.\d+\.\d+)/.exec(req.headers["user-agent"] ?? "");
  return m ? m[1]! : "unknown";
}

function header(req: FastifyRequest, name: string): string {
  const v = req.headers[name];
  return String((Array.isArray(v) ? v[0] : v) ?? "").trim();
}

/** The kiosk's identity from its request headers, or undefined for old firmware / browsers. */
function deviceOf(req: FastifyRequest): { id: string; machine: string; fw: string } | undefined {
  const id = header(req, "x-device-id").toLowerCase();
  if (!DEVICE_ID_RE.test(id)) return undefined;
  const fw = header(req, "x-fw-version");
  return { id, machine: header(req, "x-machine-id"), fw: isSemver(fw) ? fw : deviceVersion(req) };
}

export async function buildApp(config: Config, store: Store): Promise<FastifyInstance> {
  const fwDir = path.join(config.dataDir, "firmware");
  mkdirSync(fwDir, { recursive: true });

  const app = Fastify({ logger: process.env.NODE_ENV === "test" ? false : { level: "info" }, trustProxy: true });

  await app.register(cors, {
    origin: (origin, cb) => {
      if (!origin || originAllowed(origin, config.allowedOrigins)) cb(null, true);
      else cb(null, false);
    },
    methods: ["GET", "HEAD", "POST", "PUT", "DELETE", "OPTIONS"],
    allowedHeaders: ["Authorization", "Content-Type"],
    maxAge: 600,
  });
  await app.register(rateLimit, { global: false });
  await app.register(jwt, { secret: config.jwtSecret, sign: { expiresIn: config.tokenTtl } });
  await app.register(multipart, { limits: { fileSize: config.maxFirmwareBytes + 1, files: 1, fields: 8, fieldSize: 2_000 } });

  app.decorate("authenticate", async (req: FastifyRequest) => {
    try {
      await req.jwtVerify();
    } catch {
      throw Object.assign(new Error("Not signed in"), { statusCode: 401 });
    }
  });

  app.setErrorHandler((err: Error & { statusCode?: number; code?: string }, _req, reply) => {
    if (err instanceof ImageError) return reply.code(422).send({ error: err.message });
    if (err.code === "FST_REQ_FILE_TOO_LARGE") {
      return reply.code(413).send({ error: `File is larger than the ${config.maxFirmwareBytes} byte OTA slot.` });
    }
    const status = err.statusCode && err.statusCode >= 400 ? err.statusCode : 500;
    if (status >= 500) app.log.error(err);
    return reply.code(status).send({ error: status >= 500 ? "Internal error" : err.message });
  });

  app.get("/healthz", async () => ({ ok: true, version: SERVER_VERSION }));

  // ------------------------------------------------------------------ device endpoints (public)
  // The kiosk's "OTA URL" is https://<tunnel-host>/ota/manifest.json ; firmware.bin and heartbeat live in the same folder.
  // Updates are never broadcast: a kiosk only sees a release that the dashboard assigned to its device id.

  /** The release assigned to this kiosk (undefined = nothing to install). */
  const releaseFor = (id: string | undefined): Release | undefined => {
    if (!id) return undefined;
    const t = store.getNode(id)?.target_release_id;
    return t != null ? store.getRelease(t) : undefined;
  };

  app.get("/ota/manifest.json", async (req, reply) => {
    const dev = deviceOf(req);
    const ip = clientIp(req);
    const node = dev ? recordContact(store, { ...dev, ip }) : undefined;
    const rel = releaseFor(node?.device_id);
    if (req.method === "GET") {
      store.logEvent({ kind: "manifest", ip, device_version: dev?.fw ?? deviceVersion(req), served_version: rel?.version ?? "none", status: 200, bytes: 0, complete: 1, device_id: dev?.id ?? "" });
    }
    return reply
      .header("Cache-Control", "no-store")
      .type("application/json; charset=utf-8")
      .send(JSON.stringify(manifestFor(rel), null, 2) + "\n");
  });

  app.get("/ota/firmware.bin", async (req, reply) => {
    const dev = deviceOf(req);
    const ip = clientIp(req);
    const node = dev ? recordContact(store, { ...dev, ip }) : undefined;
    const rel = releaseFor(node?.device_id);
    const file = rel ? path.join(fwDir, rel.filename) : "";
    const log = (status: number, bytes: number, complete: boolean) => {
      if (req.method !== "GET") return;
      store.logEvent({ kind: "firmware", ip, device_version: dev?.fw ?? deviceVersion(req), served_version: rel?.version ?? "none", status, bytes, complete: complete ? 1 : 0, device_id: dev?.id ?? "" });
      if (dev && rel && status === 200) store.logNode(dev.id, complete ? "download" : "download-failed", `${rel.version}: ${bytes} of ${rel.size} bytes`);
    };
    if (!rel || !existsSync(file)) {
      log(404, 0, false);
      return reply.code(404).header("Cache-Control", "no-store").send({ error: "No firmware is assigned to this device" });
    }
    const stream = createReadStream(file);
    let sent = 0;
    stream.on("data", (c) => (sent += c.length));
    reply.raw.on("close", () => log(200, sent, reply.raw.writableFinished && sent === rel.size));
    // An explicit Content-Length (no chunked encoding): ota.cpp insists http.getSize() == manifest size.
    return reply
      .header("Cache-Control", "no-store")
      .header("Content-Length", String(rel.size))
      .type("application/octet-stream")
      .send(stream);
  });

  const lastBeat = new Map<string, number>();
  app.post("/ota/heartbeat", { bodyLimit: 2048 }, async (req, reply) => {
    const dev = deviceOf(req);
    if (!dev) return reply.code(400).send({ error: "X-Device-Id header required" });
    const h = sanitizeHeartbeat(req.body);
    if (!h) return reply.code(400).send({ error: "Bad heartbeat" });
    const now = Date.now();
    if (now - (lastBeat.get(dev.id) ?? 0) < MIN_BEAT_GAP_MS) return reply.code(429).send({ error: "Too many heartbeats" });
    lastBeat.set(dev.id, now);
    if (lastBeat.size > 5000) lastBeat.clear();
    const ip = clientIp(req);
    const node = recordContact(store, { id: dev.id, machine: dev.machine, fw: h.fw, ip });
    if (!node) return reply.code(403).send({ error: "Node limit reached" });
    recordHeartbeat(store, store.getNode(dev.id)!, h, ip);
    return reply.code(204).send();
  });

  // ------------------------------------------------------------------ admin API
  app.post(
    "/api/login",
    { config: { rateLimit: { max: 8, timeWindow: "1 minute" } } },
    async (req, reply) => {
      const body = (req.body ?? {}) as { password?: unknown };
      if (typeof body.password !== "string" || !sameSecret(body.password, config.adminPassword)) {
        return reply.code(401).send({ error: "Wrong password" });
      }
      const token = app.jwt.sign({ role: "admin" });
      return { token, expiresIn: config.tokenTtl };
    },
  );

  /** Validate an image and store it as a new release (used by the manual upload and by "release this build"). */
  async function publishRelease(buf: Buffer, fields: { version?: string; min_version?: string; notes?: string }): Promise<Release> {
    const info = inspectFirmware(buf, { min: config.minFirmwareBytes, max: config.maxFirmwareBytes });
    const version = fields.version || info.detectedVersion;
    if (!isSemver(version)) throw new ImageError("Version must look like 1.2.0 (no leading v).");
    if (version !== info.detectedVersion) {
      throw new ImageError(`Version mismatch: you typed ${version} but this binary reports ${info.detectedVersion}. Kiosks would reinstall it forever. Rebuild with FW_VERSION ${version} (tools/release.py ${version}).`);
    }
    const minVersion = fields.min_version ?? "";
    if (minVersion) checkMinVersion(version, minVersion);
    const notes = (fields.notes ?? "").slice(0, 500);
    if (store.getReleaseByVersion(version)) throw Object.assign(new Error(`Version ${version} already exists. Bump FW_VERSION and rebuild.`), { statusCode: 409 });

    const filename = `${version}-${info.sha256.slice(0, 8)}.bin`;
    const tmp = path.join(fwDir, `.${filename}.part`);
    await writeFile(tmp, buf);
    await rename(tmp, path.join(fwDir, filename));
    return store.insertRelease({ version, sha256: info.sha256, size: info.size, min_version: minVersion, notes, filename });
  }

  const workspace = new Workspace(config.repoDir, config.dataDir);
  const jobs = new JobManager(path.join(config.dataDir, "jobs"));

  await app.register(async (admin) => {
    admin.addHook("preHandler", app.authenticate);

    const publicBase = (req: FastifyRequest) => config.publicUrl || `${req.protocol}://${req.headers.host ?? "localhost"}`;
    const releaseMap = () => new Map(store.listReleases().map((r) => [r.id, r]));
    const relView = (r: Release) => ({ ...r, nodes: store.nodesTargeting(r.id) });

    admin.get("/api/status", async (req) => {
      const week = new Date(Date.now() - 7 * 864e5).toISOString();
      const day = new Date(Date.now() - 864e5).toISOString();
      const rels = releaseMap();
      const nodes = store.listNodes().map((n) => nodeView(n, rels));
      return {
        serverVersion: SERVER_VERSION,
        manifestUrl: `${publicBase(req)}/ota/manifest.json`,
        releaseCount: rels.size,
        limits: { maxBytes: config.maxFirmwareBytes, minBytes: config.minFirmwareBytes },
        nodes: {
          total: nodes.length,
          online: nodes.filter((n) => n.online).length,
          attention: nodes.filter((n) => n.level === "warn" || n.level === "bad").length,
          offline: nodes.filter((n) => !n.online).length,
          pendingUpdates: nodes.filter((n) => n.update === "pending").length,
        },
        versions24h: store.versionStats(day),
        versions7d: store.versionStats(week),
        daily: store.dailyChecks(new Date(Date.now() - 14 * 864e5).toISOString()),
      };
    });

    // ---- releases
    admin.get("/api/releases", async () => store.listReleases().map(relView));

    admin.post("/api/releases", async (req, reply) => {
      const fields: Record<string, string> = {};
      let buf: Buffer | undefined;
      for await (const part of req.parts()) {
        if (part.type === "file") buf = await part.toBuffer();
        else fields[part.fieldname] = String(part.value ?? "").trim();
      }
      if (!buf) return reply.code(400).send({ error: "Attach the firmware as the multipart field \"file\"." });
      const rel = await publishRelease(buf, fields);
      return reply.code(201).send(relView(rel));
    });

    admin.delete("/api/releases/:id", async (req, reply) => {
      const rel = store.getRelease(Number((req.params as { id: string }).id));
      if (!rel) return reply.code(404).send({ error: "No such release" });
      const n = store.nodesTargeting(rel.id);
      if (n) return reply.code(409).send({ error: `${n} node${n > 1 ? "s are" : " is"} assigned this release. Unassign ${n > 1 ? "them" : "it"} first.` });
      store.deleteRelease(rel.id);
      await rm(path.join(fwDir, rel.filename), { force: true });
      return reply.code(204).send();
    });

    admin.get("/api/releases/:id/firmware.bin", async (req, reply) => {
      const rel = store.getRelease(Number((req.params as { id: string }).id));
      const file = rel ? path.join(fwDir, rel.filename) : "";
      if (!rel || !existsSync(file)) return reply.code(404).send({ error: "No such release" });
      return reply.header("Content-Disposition", `attachment; filename="firmware-${rel.version}.bin"`).type("application/octet-stream").send(createReadStream(file));
    });

    // ---- nodes
    admin.get("/api/nodes", async () => {
      const rels = releaseMap();
      return store.listNodes().map((n) => nodeView(n, rels));
    });

    admin.get("/api/nodes/:id", async (req, reply) => {
      const id = (req.params as { id: string }).id;
      const n = store.getNode(id);
      if (!n) return reply.code(404).send({ error: "No such node" });
      return { node: nodeView(n, releaseMap()), log: store.nodeLog(id, 150), events: store.nodeEvents(id, 50) };
    });

    admin.post("/api/nodes/:id", async (req, reply) => {
      const id = (req.params as { id: string }).id;
      if (!store.getNode(id)) return reply.code(404).send({ error: "No such node" });
      const label = String(((req.body ?? {}) as { label?: unknown }).label ?? "").trim().slice(0, 40);
      store.updateNode(id, { label });
      return nodeView(store.getNode(id)!, releaseMap());
    });

    admin.delete("/api/nodes/:id", async (req, reply) => {
      const id = (req.params as { id: string }).id;
      if (!store.getNode(id)) return reply.code(404).send({ error: "No such node" });
      store.deleteNode(id); // a kiosk that is still alive registers itself again on its next contact
      return reply.code(204).send();
    });

    /** Assign (or clear, with releaseId null) the release for an explicit list of nodes. There is no "all nodes" shortcut by design. */
    admin.post("/api/nodes/target", async (req, reply) => {
      const b = (req.body ?? {}) as { releaseId?: unknown; deviceIds?: unknown };
      const ids = Array.isArray(b.deviceIds) ? [...new Set(b.deviceIds.filter((x): x is string => typeof x === "string" && DEVICE_ID_RE.test(x)))] : [];
      if (!ids.length || ids.length > 200) return reply.code(400).send({ error: "Select between 1 and 200 nodes." });
      const rel = b.releaseId === null ? null : store.getRelease(Number(b.releaseId));
      if (rel === undefined) return reply.code(404).send({ error: "No such release" });
      const results: { deviceId: string; ok: boolean; error?: string }[] = [];
      for (const id of ids) {
        const n = store.getNode(id);
        if (!n) { results.push({ deviceId: id, ok: false, error: "unknown node" }); continue; }
        if (rel && n.fw_version && cmpVer(rel.version, n.fw_version) <= 0) {
          results.push({ deviceId: id, ok: false, error: `runs ${n.fw_version}; kiosks refuse to downgrade or reinstall` });
          continue;
        }
        if (rel && rel.min_version && n.fw_version && cmpVer(n.fw_version, rel.min_version) < 0) {
          results.push({ deviceId: id, ok: false, error: `needs ${rel.min_version} or newer first (release min_version)` });
          continue;
        }
        store.setTarget(id, rel ? rel.id : null);
        store.logNode(id, rel ? "assigned" : "unassigned", rel ? `release ${rel.version}` : "update cancelled");
        results.push({ deviceId: id, ok: true });
      }
      return { results };
    });

    admin.get("/api/events", async (req) => {
      const q = req.query as { limit?: string; kind?: string };
      const limit = Math.min(Math.max(Number(q.limit ?? 100) || 100, 1), 500);
      const kind = q.kind === "manifest" || q.kind === "firmware" ? (q.kind as EventKind) : undefined;
      return store.listEvents(limit, kind);
    });

    admin.get("/api/activity", async (req) => {
      const limit = Math.min(Math.max(Number((req.query as { limit?: string }).limit ?? 100) || 100, 1), 300);
      const labels = new Map(store.listNodes().map((n) => [n.device_id, `${n.label || (n.machine_id ? "Mesin " + n.machine_id : n.device_id)}`]));
      return store.recentNodeLog(limit).map((l) => ({ ...l, name: labels.get(l.device_id) ?? l.device_id }));
    });

    registerStudio(admin, { config, workspace, jobs, publishRelease });
  });

  return app;
}
