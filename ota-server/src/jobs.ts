import { spawn, type ChildProcess } from "node:child_process";
import { randomBytes } from "node:crypto";
import { appendFileSync, closeSync, existsSync, mkdirSync, openSync, readFileSync, readSync, readdirSync, rmSync, statSync, writeFileSync, fstatSync } from "node:fs";
import path from "node:path";

export type JobKind = "firmware" | "preview";
export type JobStatus = "running" | "ok" | "failed" | "canceled";

export interface JobInfo {
  id: string;
  kind: JobKind;
  status: JobStatus;
  startedAt: string;
  finishedAt: string;
  exitCode: number | null;
  /** Filled by the job's `after` hook (e.g. firmware version, size, sha256). */
  result: Record<string, unknown> | null;
  error: string;
}

export class JobBusyError extends Error {
  readonly statusCode = 409;
}

export interface JobSpec {
  kind: JobKind;
  cmd: string;
  args: string[];
  cwd: string;
  env?: NodeJS.ProcessEnv;
  /** Runs after a zero exit code; may return result data or throw to mark the job failed. */
  after?: (dir: string) => Promise<Record<string, unknown> | void> | Record<string, unknown> | void;
}

/** The command line as it is shown at the top of a job's log: the log is displayed in the dashboard, so no absolute server paths. */
function shownCommand(spec: JobSpec): string {
  const short = (a: string): string => {
    if (!path.isAbsolute(a)) return a;
    const rel = path.relative(spec.cwd, a);
    return rel === "" ? "." : rel.startsWith("..") ? `…/${path.basename(a)}` : rel;
  };
  return [path.basename(spec.cmd), ...spec.args.map(short)].join(" ");
}

const MAX_LOG_BYTES = 4 * 1024 * 1024;
const KEEP = 12;

/** One heavy job at a time (a PlatformIO build needs most of this machine's RAM). Logs live on disk so they survive restarts. */
export class JobManager {
  private current: { info: JobInfo; child: ChildProcess | null } | null = null;
  private readonly infos = new Map<string, JobInfo>();

  constructor(private readonly dir: string) {
    mkdirSync(dir, { recursive: true });
    for (const name of readdirSync(dir)) {
      const meta = path.join(dir, name, "meta.json");
      if (!existsSync(meta)) continue;
      try {
        const info = JSON.parse(readFileSync(meta, "utf8")) as JobInfo;
        if (info.status === "running") {
          info.status = "failed";
          info.error = "Server restarted while this job was running";
          info.finishedAt = new Date().toISOString();
          writeFileSync(meta, JSON.stringify(info));
        }
        this.infos.set(info.id, info);
      } catch {
        /* ignore a damaged record */
      }
    }
  }

  jobDir(id: string): string {
    return path.join(this.dir, id);
  }

  private save(info: JobInfo): void {
    writeFileSync(path.join(this.jobDir(info.id), "meta.json"), JSON.stringify(info));
  }

  list(kind?: JobKind): JobInfo[] {
    return [...this.infos.values()].filter((j) => !kind || j.kind === kind).sort((a, b) => b.startedAt.localeCompare(a.startedAt));
  }

  get(id: string): JobInfo | undefined {
    return this.infos.get(id);
  }

  get busy(): JobInfo | null {
    return this.current?.info ?? null;
  }

  start(spec: JobSpec): JobInfo {
    if (this.current) throw new JobBusyError(`A ${this.current.info.kind} job is already running. Wait for it to finish or cancel it.`);
    const id = `${Date.now().toString(36)}${randomBytes(3).toString("hex")}`;
    const dir = this.jobDir(id);
    mkdirSync(dir, { recursive: true });
    const logFile = path.join(dir, "log.txt");
    writeFileSync(logFile, `$ ${shownCommand(spec)}\n`);
    const info: JobInfo = { id, kind: spec.kind, status: "running", startedAt: new Date().toISOString(), finishedAt: "", exitCode: null, result: null, error: "" };
    this.infos.set(id, info);
    this.save(info);

    let logged = 0;
    const append = (chunk: Buffer | string) => {
      const b = typeof chunk === "string" ? Buffer.from(chunk) : chunk;
      if (logged >= MAX_LOG_BYTES) return;
      const room = MAX_LOG_BYTES - logged;
      appendFileSync(logFile, b.length > room ? Buffer.concat([b.subarray(0, room), Buffer.from("\n[log truncated]\n")]) : b);
      logged += b.length;
    };

    const finish = async (status: JobStatus, exitCode: number | null, error = "") => {
      if (info.status !== "running") return;
      info.exitCode = exitCode;
      info.error = error;
      if (status === "ok" && spec.after) {
        try {
          info.result = (await spec.after(dir)) ?? null;
        } catch (e) {
          status = "failed";
          info.error = e instanceof Error ? e.message : String(e);
          append(`\n[after-build step failed] ${info.error}\n`);
        }
      }
      info.status = status;
      info.finishedAt = new Date().toISOString();
      this.save(info);
      this.current = null;
      this.prune();
    };

    let child: ChildProcess;
    try {
      child = spawn(spec.cmd, spec.args, { cwd: spec.cwd, env: { ...process.env, ...spec.env }, stdio: ["ignore", "pipe", "pipe"], detached: true });
    } catch (e) {
      info.status = "failed";
      info.error = e instanceof Error ? e.message : String(e);
      info.finishedAt = new Date().toISOString();
      this.save(info);
      return info;
    }
    this.current = { info, child };
    child.stdout?.on("data", append);
    child.stderr?.on("data", append);
    child.on("error", (e) => {
      append(`\n[could not start] ${e.message}\n`);
      void finish("failed", null, `Could not start ${spec.cmd}: ${e.message}`);
    });
    child.on("close", (code, signal) => {
      if (info.status !== "running") return;
      if (signal) void finish("canceled", code, `Stopped (${signal})`);
      else void finish(code === 0 ? "ok" : "failed", code, code === 0 ? "" : `Exited with code ${code}`);
    });
    return info;
  }

  cancel(id: string): boolean {
    if (!this.current || this.current.info.id !== id || !this.current.child?.pid) return false;
    try {
      process.kill(-this.current.child.pid, "SIGTERM"); // whole process group (pio -> scons -> compilers)
    } catch {
      /* already gone */
    }
    return true;
  }

  /** Bytes of the log starting at `from` (a byte offset) and the offset to ask for next. */
  readLog(id: string, from: number): { text: string; next: number } | null {
    const file = path.join(this.jobDir(id), "log.txt");
    if (!this.infos.has(id) || !existsSync(file)) return null;
    const fd = openSync(file, "r");
    try {
      const size = fstatSync(fd).size;
      const start = Math.min(Math.max(0, from), size);
      const buf = Buffer.alloc(Math.min(size - start, 256 * 1024));
      readSync(fd, buf, 0, buf.length, start);
      return { text: buf.toString("utf8"), next: start + buf.length };
    } finally {
      closeSync(fd);
    }
  }

  private prune(): void {
    const old = this.list().slice(KEEP);
    for (const j of old) {
      if (j.status === "running") continue;
      rmSync(this.jobDir(j.id), { recursive: true, force: true });
      this.infos.delete(j.id);
    }
  }

  fileSize(p: string): number {
    try {
      return statSync(p).size;
    } catch {
      return 0;
    }
  }
}
