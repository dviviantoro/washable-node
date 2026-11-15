import type { FastifyInstance } from "fastify";
import { createHash } from "node:crypto";
import { copyFile, mkdir, readFile, rename, rm, stat, writeFile } from "node:fs/promises";
import { createReadStream, existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import type { Config } from "./config.js";
import type { Release } from "./db.js";
import { inspectFirmware } from "./image.js";
import { JobManager, type JobInfo } from "./jobs.js";
import { Workspace, WorkspaceError } from "./workspace.js";

export interface StudioDeps {
  config: Config;
  workspace: Workspace;
  jobs: JobManager;
  publishRelease: (buf: Buffer, fields: { version?: string; min_version?: string; notes?: string }) => Promise<Release>;
}

const PIO_ENV = "esp32-3248s035"; // [env:...] in esp32-kiosk/platformio.ini
const here = path.dirname(fileURLToPath(import.meta.url));
const PREVIEW_SCRIPT = path.resolve(here, "..", "scripts", "preview-build.sh");

const toolchain = (config: Config) => ({
  pio: existsSync(config.pioBin),
  emsdk: existsSync(path.join(config.emsdkDir, "emsdk_env.sh")) && existsSync(path.join(config.emsdkDir, "upstream", "emscripten", "emcc")),
});

/** Editor, firmware build and kiosk-preview endpoints (all behind the admin login). */
export function registerStudio(admin: FastifyInstance, d: StudioDeps): void {
  const { config, workspace: ws, jobs } = d;
  const previewDir = path.join(config.dataDir, "preview");
  const cacheDir = path.join(config.dataDir, "preview-cache");

  // ------------------------------------------------------------------ workspace (code editor)
  admin.get("/api/workspace/files", async () => ({ files: await ws.list() }));

  admin.get("/api/workspace/file", async (req) => ws.read((req.query as { path?: string }).path));

  admin.put("/api/workspace/file", async (req) => {
    const b = (req.body ?? {}) as { path?: unknown; content?: unknown };
    return ws.write(b.path, b.content);
  });

  admin.post("/api/workspace/revert", async (req, reply) => {
    await ws.revert(((req.body ?? {}) as { path?: unknown }).path);
    return reply.code(204).send();
  });

  admin.post("/api/workspace/reset", async (_req, reply) => {
    if (jobs.busy) return reply.code(409).send({ error: "A build is running; wait for it to finish first." });
    await ws.reset();
    return reply.code(204).send();
  });

  admin.get("/api/workspace/patch", async (_req, reply) => {
    const patch = await ws.patch();
    return reply.header("Content-Disposition", 'attachment; filename="dashboard-edits.patch"').type("text/x-diff; charset=utf-8").send(patch);
  });

  // ------------------------------------------------------------------ toolchain / jobs overview
  admin.get("/api/studio", async () => {
    const tc = toolchain(config);
    const lastFw = jobs.list("firmware")[0] ?? null;
    const lastPv = jobs.list("preview")[0] ?? null;
    return {
      toolchain: tc,
      workspaceSeeded: ws.seeded,
      busy: jobs.busy,
      lastFirmwareJob: lastFw,
      lastPreviewJob: lastPv,
      preview: await previewMeta(previewDir),
    };
  });

  // ------------------------------------------------------------------ firmware build (PlatformIO)
  admin.post("/api/build", async (_req, reply) => {
    if (!toolchain(config).pio) return reply.code(412).send({ error: `PlatformIO is not installed (expected ${config.pioBin}).` });
    await ws.ensure();
    const bin = path.join(ws.projectDir, ".pio", "build", PIO_ENV, "firmware.bin");
    await rm(bin, { force: true });
    const job = jobs.start({
      kind: "firmware",
      cmd: config.pioBin,
      args: ["run", "-d", ws.projectDir, "-j", "2"],
      cwd: ws.projectDir,
      env: {
        PATH: `${path.dirname(config.pioBin)}${path.delimiter}${process.env.PATH ?? ""}`,
        CI: "true",
        TERM: "dumb",
        NO_COLOR: "1",
        PLATFORMIO_SETTING_ENABLE_TELEMETRY: "no",
      },
      after: async (dir) => {
        if (!existsSync(bin)) throw new Error("The build finished but produced no firmware.bin");
        const buf = await readFile(bin);
        await copyFile(bin, path.join(dir, "firmware.bin"));
        const sha256 = createHash("sha256").update(buf).digest("hex");
        try {
          const info = inspectFirmware(buf, { min: config.minFirmwareBytes, max: config.maxFirmwareBytes });
          return { releasable: true, version: info.detectedVersion, size: buf.length, sha256, problem: "" };
        } catch (e) {
          return { releasable: false, version: "", size: buf.length, sha256, problem: e instanceof Error ? e.message : String(e) };
        }
      },
    });
    return reply.code(202).send(job);
  });

  admin.get("/api/jobs", async (req) => {
    const kind = (req.query as { kind?: string }).kind;
    return jobs.list(kind === "firmware" || kind === "preview" ? kind : undefined);
  });

  admin.get("/api/jobs/:id", async (req, reply) => {
    const { id } = req.params as { id: string };
    const q = req.query as { from?: string };
    const info = jobs.get(id);
    const log = info ? jobs.readLog(id, Number(q.from ?? 0) || 0) : null;
    if (!info || !log) return reply.code(404).send({ error: "No such job" });
    return { job: info, log: log.text, next: log.next };
  });

  admin.post("/api/jobs/:id/cancel", async (req, reply) => {
    const { id } = req.params as { id: string };
    return jobs.cancel(id) ? { canceled: true } : reply.code(409).send({ error: "That job is not running" });
  });

  /** Turn a successful build's firmware.bin into a release (same validation as a manual upload). */
  admin.post("/api/jobs/:id/release", async (req, reply) => {
    const { id } = req.params as { id: string };
    const job = jobs.get(id);
    if (!job || job.kind !== "firmware" || job.status !== "ok") return reply.code(404).send({ error: "No successful firmware build with that id" });
    const file = path.join(jobs.jobDir(id), "firmware.bin");
    if (!existsSync(file)) return reply.code(404).send({ error: "The build artifact is gone" });
    const b = (req.body ?? {}) as { notes?: string; min_version?: string };
    const rel = await d.publishRelease(await readFile(file), { notes: b.notes, min_version: b.min_version });
    return reply.code(201).send(rel);
  });

  admin.get("/api/jobs/:id/firmware.bin", async (req, reply) => {
    const { id } = req.params as { id: string };
    const file = path.join(jobs.jobDir(id), "firmware.bin");
    if (!jobs.get(id) || !existsSync(file)) return reply.code(404).send({ error: "No artifact" });
    return reply.header("Content-Disposition", `attachment; filename="firmware-${id}.bin"`).type("application/octet-stream").send(createReadStream(file));
  });

  // ------------------------------------------------------------------ interactive kiosk preview (WebAssembly)
  admin.post("/api/preview/build", async (_req, reply) => {
    const tc = toolchain(config);
    if (!tc.emsdk) return reply.code(412).send({ error: `Emscripten is not installed (expected ${config.emsdkDir}).` });
    if (!tc.pio) return reply.code(412).send({ error: "PlatformIO is needed once to fetch the LVGL library." });
    await ws.ensure();
    await mkdir(cacheDir, { recursive: true });
    await rm(path.join(previewDir, "next"), { recursive: true, force: true });
    const job = jobs.start({
      kind: "preview",
      cmd: "bash",
      args: [PREVIEW_SCRIPT, ws.root, path.join(previewDir, "next"), cacheDir, path.join(config.repoDir, "tools", "sim"), config.pioBin, config.emsdkDir],
      cwd: ws.root,
      env: { CI: "true", TERM: "dumb", NO_COLOR: "1", PLATFORMIO_SETTING_ENABLE_TELEMETRY: "no" },
      after: async () => {
        const next = path.join(previewDir, "next");
        if (!existsSync(path.join(next, "sim.js"))) throw new Error("The preview build produced no sim.js");
        const version = /FW_VERSION\s+"([^"]+)"/.exec(await readFile(path.join(ws.projectDir, "src", "version.h"), "utf8"))?.[1] ?? "";
        const size = (await stat(path.join(next, "sim.js"))).size;
        const meta = { builtAt: new Date().toISOString(), size, version };
        await writeFile(path.join(next, "meta.json"), JSON.stringify(meta));
        await rm(path.join(previewDir, "current"), { recursive: true, force: true });
        await rename(next, path.join(previewDir, "current"));
        return meta;
      },
    });
    return reply.code(202).send(job);
  });

  admin.get("/api/preview/sim.js", async (_req, reply) => {
    const file = path.join(previewDir, "current", "sim.js");
    if (!existsSync(file)) return reply.code(404).send({ error: "The preview has not been built yet" });
    return reply.header("Cache-Control", "no-store").type("text/javascript; charset=utf-8").send(createReadStream(file));
  });
}

async function previewMeta(previewDir: string): Promise<{ builtAt: string; size: number; version: string } | null> {
  try {
    return JSON.parse(await readFile(path.join(previewDir, "current", "meta.json"), "utf8"));
  } catch {
    return null;
  }
}

export { WorkspaceError };
export type { JobInfo };
