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
import { ImageError, checkMinVersion, inspectFirmware } from "./image.js";
import { isSemver } from "./version.js";

declare module "fastify" {
  interface FastifyInstance {
    authenticate: (req: FastifyRequest) => Promise<void>;
  }
}

const SERVER_VERSION = "1.0.0";

/**
 * Served while no release is active (or the rollout is paused). ota.cpp compares versions and sees
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

export async function buildApp(config: Config, store: Store): Promise<FastifyInstance> {
  const fwDir = path.join(config.dataDir, "firmware");
  mkdirSync(fwDir, { recursive: true });

  const app = Fastify({ logger: process.env.NODE_ENV === "test" ? false : { level: "info" }, trustProxy: true });

  await app.register(cors, {
    origin: (origin, cb) => {
      if (!origin || originAllowed(origin, config.allowedOrigins)) cb(null, true);
      else cb(null, false);
    },
    methods: ["GET", "HEAD", "POST", "DELETE", "OPTIONS"],
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

  // ------------------------------------------------------------------ device endpoints (public, read-only)
  // The kiosk's "OTA URL" is https://<tunnel-host>/ota/manifest.json ; firmware.bin must live in the same folder.

  app.get("/ota/manifest.json", async (req, reply) => {
    const rel = store.getActive();
    if (req.method === "GET") {
      store.logEvent({ kind: "manifest", ip: clientIp(req), device_version: deviceVersion(req), served_version: rel?.version ?? "none", status: 200, bytes: 0, complete: 1 });
    }
    return reply
      .header("Cache-Control", "no-store")
      .type("application/json; charset=utf-8")
      .send(JSON.stringify(manifestFor(rel), null, 2) + "\n");
  });

  app.get("/ota/firmware.bin", async (req, reply) => {
    const rel = store.getActive();
    const file = rel ? path.join(fwDir, rel.filename) : "";
    const log = (status: number, bytes: number, complete: boolean) => {
      if (req.method !== "GET") return;
      store.logEvent({ kind: "firmware", ip: clientIp(req), device_version: deviceVersion(req), served_version: rel?.version ?? "none", status, bytes, complete: complete ? 1 : 0 });
    };
    if (!rel || !existsSync(file)) {
      log(404, 0, false);
      return reply.code(404).header("Cache-Control", "no-store").send({ error: "No firmware is published" });
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

  await app.register(async (admin) => {
    admin.addHook("preHandler", app.authenticate);

    const publicBase = (req: FastifyRequest) => config.publicUrl || `${req.protocol}://${req.headers.host ?? "localhost"}`;
    const view = (r: Release, activeId?: number) => ({ ...r, active: r.id === activeId });

    admin.get("/api/status", async (req) => {
      const active = store.getActive();
      const week = new Date(Date.now() - 7 * 864e5).toISOString();
      const day = new Date(Date.now() - 864e5).toISOString();
      return {
        serverVersion: SERVER_VERSION,
        manifestUrl: `${publicBase(req)}/ota/manifest.json`,
        active: active ? view(active, active.id) : null,
        paused: !active,
        releaseCount: store.listReleases().length,
        limits: { maxBytes: config.maxFirmwareBytes, minBytes: config.minFirmwareBytes },
        versions24h: store.versionStats(day),
        versions7d: store.versionStats(week),
        daily: store.dailyChecks(new Date(Date.now() - 14 * 864e5).toISOString()),
      };
    });

    admin.get("/api/releases", async () => {
      const active = store.getActive();
      return store.listReleases().map((r) => view(r, active?.id));
    });

    admin.post("/api/releases", async (req, reply) => {
      const fields: Record<string, string> = {};
      let buf: Buffer | undefined;
      for await (const part of req.parts()) {
        if (part.type === "file") buf = await part.toBuffer();
        else fields[part.fieldname] = String(part.value ?? "").trim();
      }
      if (!buf) return reply.code(400).send({ error: "Attach the firmware as the multipart field \"file\"." });

      const info = inspectFirmware(buf, { min: config.minFirmwareBytes, max: config.maxFirmwareBytes });
      const version = fields.version || info.detectedVersion;
      if (!isSemver(version)) throw new ImageError("Version must look like 1.2.0 (no leading v).");
      if (version !== info.detectedVersion) {
        throw new ImageError(`Version mismatch: you typed ${version} but this binary reports ${info.detectedVersion}. Kiosks would reinstall it forever. Rebuild with FW_VERSION ${version} (tools/release.py ${version}).`);
      }
      const minVersion = fields.min_version ?? "";
      if (minVersion) checkMinVersion(version, minVersion);
      const notes = (fields.notes ?? "").slice(0, 500);
      if (store.getReleaseByVersion(version)) return reply.code(409).send({ error: `Version ${version} already exists. Bump FW_VERSION and rebuild.` });

      const filename = `${version}-${info.sha256.slice(0, 8)}.bin`;
      const tmp = path.join(fwDir, `.${filename}.part`);
      await writeFile(tmp, buf);
      await rename(tmp, path.join(fwDir, filename));
      const rel = store.insertRelease({ version, sha256: info.sha256, size: info.size, min_version: minVersion, notes, filename });
      if (fields.activate === "true" || fields.activate === "1") store.setActive(rel.id);
      return reply.code(201).send(view(rel, store.getActive()?.id));
    });

    admin.post("/api/releases/:id/activate", async (req, reply) => {
      const rel = store.getRelease(Number((req.params as { id: string }).id));
      if (!rel) return reply.code(404).send({ error: "No such release" });
      store.setActive(rel.id);
      return view(rel, rel.id);
    });

    // Pause the rollout: kiosks get the inert manifest and keep running what they have.
    admin.post("/api/releases/deactivate", async () => {
      store.setActive(null);
      return { paused: true };
    });

    admin.delete("/api/releases/:id", async (req, reply) => {
      const rel = store.getRelease(Number((req.params as { id: string }).id));
      if (!rel) return reply.code(404).send({ error: "No such release" });
      if (store.getActive()?.id === rel.id) return reply.code(409).send({ error: "This release is active. Activate another one or pause the rollout first." });
      store.deleteRelease(rel.id);
      await rm(path.join(fwDir, rel.filename), { force: true });
      return reply.code(204).send();
    });

    admin.get("/api/events", async (req) => {
      const q = req.query as { limit?: string; kind?: string };
      const limit = Math.min(Math.max(Number(q.limit ?? 100) || 100, 1), 500);
      const kind = q.kind === "manifest" || q.kind === "firmware" ? (q.kind as EventKind) : undefined;
      return store.listEvents(limit, kind);
    });
  });

  return app;
}
