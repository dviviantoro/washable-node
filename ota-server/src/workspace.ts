import { createHash } from "node:crypto";
import { cp, lstat, mkdir, readFile, readdir, rename, rm, writeFile } from "node:fs/promises";
import { existsSync } from "node:fs";
import { spawn } from "node:child_process";
import path from "node:path";

/**
 * The code editor never touches the git checkout. It works on a private copy under <data>/workspace that is seeded
 * from the repo; <data>/workspace-base keeps the pristine copy of the editable files so the editor can show a diff
 * and export a patch. Only the source files below can be read or written through the API; build scripts
 * (platformio.ini, library.json, extra_scripts) are deliberately not editable because they run code during a build.
 */
const EDIT_ROOTS = ["esp32-kiosk/src", "esp32-kiosk/include", "shared/relay_proto"];
const SEED_ROOTS = ["esp32-kiosk", "shared", "tools/sim"];
const SKIP_DIRS = new Set([".pio", ".git", "node_modules", "fonts"]); // fonts/*.c are generated LVGL font tables, not meant to be edited
const EDIT_EXT = new Set([".c", ".cpp", ".h", ".hpp"]);
export const MAX_FILE_BYTES = 512 * 1024;

export class WorkspaceError extends Error {
  constructor(message: string, readonly statusCode = 400) {
    super(message);
  }
}

export interface FileEntry {
  path: string;
  size: number;
  modified: boolean;
}

export class Workspace {
  readonly root: string;
  readonly base: string;

  constructor(
    private readonly repoDir: string,
    dataDir: string,
  ) {
    this.root = path.join(dataDir, "workspace");
    this.base = path.join(dataDir, "workspace-base");
  }

  get seeded(): boolean {
    return existsSync(path.join(this.root, "esp32-kiosk", "platformio.ini"));
  }

  get projectDir(): string {
    return path.join(this.root, "esp32-kiosk");
  }

  /** Copy esp32-kiosk/, shared/ and tools/sim/ from the repo (once). */
  async ensure(): Promise<void> {
    if (this.seeded) return;
    if (!existsSync(path.join(this.repoDir, "esp32-kiosk", "platformio.ini"))) {
      throw new WorkspaceError(`REPO_DIR (${this.repoDir}) does not contain esp32-kiosk/platformio.ini`, 500);
    }
    await this.seed();
  }

  /** Throw away all edits and copy the repo again. */
  async reset(): Promise<void> {
    await rm(this.root, { recursive: true, force: true });
    await rm(this.base, { recursive: true, force: true });
    await this.seed();
  }

  private async seed(): Promise<void> {
    const filter = (src: string) => !SKIP_DIRS.has(path.basename(src)) || path.basename(src) === "fonts";
    for (const r of SEED_ROOTS) await cp(path.join(this.repoDir, r), path.join(this.root, r), { recursive: true, filter, dereference: false });
    for (const r of EDIT_ROOTS) {
      for (const f of await this.walk(path.join(this.root, r))) {
        const rel = path.relative(this.root, f);
        await mkdir(path.dirname(path.join(this.base, rel)), { recursive: true });
        await cp(f, path.join(this.base, rel));
      }
    }
  }

  private async walk(dir: string): Promise<string[]> {
    const out: string[] = [];
    let ents;
    try {
      ents = await readdir(dir, { withFileTypes: true });
    } catch {
      return out;
    }
    for (const e of ents) {
      if (e.isSymbolicLink()) continue;
      const p = path.join(dir, e.name);
      if (e.isDirectory()) {
        if (!SKIP_DIRS.has(e.name)) out.push(...(await this.walk(p)));
      } else if (e.isFile() && EDIT_EXT.has(path.extname(e.name).toLowerCase())) out.push(p);
    }
    return out;
  }

  /** Validate a client-supplied relative path: inside an editable root, a source extension, no traversal, no symlinks. */
  private resolve(rel: unknown): { rel: string; abs: string } {
    if (typeof rel !== "string" || !rel || rel.includes("\0") || rel.includes("\\")) throw new WorkspaceError("Bad path");
    const norm = path.posix.normalize(rel);
    if (norm.startsWith("/") || norm.startsWith("..") || norm !== rel) throw new WorkspaceError("Bad path");
    if (!EDIT_ROOTS.some((r) => norm.startsWith(r + "/"))) throw new WorkspaceError("That file is not editable here", 403);
    if (norm.split("/").some((seg) => SKIP_DIRS.has(seg))) throw new WorkspaceError("That file is not editable here", 403);
    if (!EDIT_EXT.has(path.posix.extname(norm).toLowerCase())) throw new WorkspaceError("Only .c .cpp .h .hpp files can be edited", 403);
    return { rel: norm, abs: path.join(this.root, ...norm.split("/")) };
  }

  async list(): Promise<FileEntry[]> {
    await this.ensure();
    const out: FileEntry[] = [];
    for (const r of EDIT_ROOTS) {
      for (const f of await this.walk(path.join(this.root, r))) {
        const rel = path.relative(this.root, f).split(path.sep).join("/");
        const cur = await readFile(f);
        const baseFile = path.join(this.base, ...rel.split("/"));
        const modified = !existsSync(baseFile) || !cur.equals(await readFile(baseFile));
        out.push({ path: rel, size: cur.length, modified });
      }
    }
    return out.sort((a, b) => a.path.localeCompare(b.path));
  }

  async read(rel: unknown): Promise<{ path: string; content: string; base: string | null; modified: boolean }> {
    await this.ensure();
    const { rel: p, abs } = this.resolve(rel);
    const st = await lstat(abs).catch(() => null);
    if (!st || !st.isFile()) throw new WorkspaceError("No such file", 404);
    if (st.size > MAX_FILE_BYTES) throw new WorkspaceError("File is too large to edit in the browser", 413);
    const content = await readFile(abs, "utf8");
    const baseAbs = path.join(this.base, ...p.split("/"));
    const base = existsSync(baseAbs) ? await readFile(baseAbs, "utf8") : null;
    return { path: p, content, base, modified: base !== content };
  }

  async write(rel: unknown, content: unknown): Promise<{ size: number; sha1: string }> {
    await this.ensure();
    if (typeof content !== "string") throw new WorkspaceError("content must be a string");
    const buf = Buffer.from(content, "utf8");
    if (buf.length > MAX_FILE_BYTES) throw new WorkspaceError("File is too large", 413);
    if (buf.includes(0)) throw new WorkspaceError("Binary content is not allowed");
    const { abs } = this.resolve(rel);
    const st = await lstat(abs).catch(() => null);
    if (st && !st.isFile()) throw new WorkspaceError("Not a regular file", 403);
    await mkdir(path.dirname(abs), { recursive: true });
    const tmp = `${abs}.part`;
    await writeFile(tmp, buf);
    await rename(tmp, abs);
    return { size: buf.length, sha1: createHash("sha1").update(buf).digest("hex") };
  }

  /** Restore one file from the pristine copy. */
  async revert(rel: unknown): Promise<void> {
    await this.ensure();
    const { rel: p, abs } = this.resolve(rel);
    const baseAbs = path.join(this.base, ...p.split("/"));
    if (existsSync(baseAbs)) await cp(baseAbs, abs);
    else await rm(abs, { force: true });
  }

  /** Unified diff (git apply-able from the repo root) of everything changed since the workspace was seeded. */
  async patch(): Promise<string> {
    await this.ensure();
    const parent = path.dirname(this.root);
    const rootName = path.basename(this.root);
    const baseName = path.basename(this.base);
    let out = "";
    for (const r of EDIT_ROOTS) {
      out += await new Promise<string>((resolve) => {
        const chunks: Buffer[] = [];
        const p = spawn("git", ["diff", "--no-index", "--no-color", "--src-prefix=a/", "--dst-prefix=b/", `${baseName}/${r}`, `${rootName}/${r}`], { cwd: parent });
        p.stdout.on("data", (c: Buffer) => chunks.push(c));
        p.on("error", () => resolve(""));
        p.on("close", () => resolve(Buffer.concat(chunks).toString("utf8")));
      });
    }
    return out.split(`a/${baseName}/`).join("a/").split(`b/${rootName}/`).join("b/");
  }
}
