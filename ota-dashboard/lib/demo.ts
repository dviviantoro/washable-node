// Demo mode: the whole API in memory, with made-up data, so the dashboard can be explored without a server (and on the public Vercel site).
// Nothing here talks to a network except loading the bundled /demo files. Changes live until the page is reloaded or the demo is left.
// Everything is invented: device IDs, shop names, documentation-range IPs, firmware images.
import type { DailyStat, DeviceEvent, FileEntry, JobInfo, NodeDetail, NodeHealth, NodeInfo, NodeLog, Release, Status, StudioStatus, VersionStat } from "./api";

export const DEMO_TOKEN = "demo";
export const isDemoToken = (t: string | null | undefined): boolean => t === DEMO_TOKEN;

/** Thrown for "HTTP errors"; api() turns it into an ApiError. */
export class DemoHttpError extends Error {
  constructor(public status: number, message: string) {
    super(message);
  }
}

const MIN = 60_000;
const DAY = 86_400_000;
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const vparts = (v: string) => v.split(".").map((x) => parseInt(x, 10) || 0);
const vcmp = (a: string, b: string) => { const x = vparts(a), y = vparts(b); for (let i = 0; i < 3; i++) if ((x[i] ?? 0) !== (y[i] ?? 0)) return (x[i] ?? 0) - (y[i] ?? 0); return 0; };
const hex = (n: number, seed: number) => { let x = (seed >>> 0) || 1, s = ""; while (s.length < n) { x ^= x << 13; x >>>= 0; x ^= x >>> 17; x ^= x << 5; x >>>= 0; s += x.toString(16).padStart(8, "0"); } return s.slice(0, n); };

// ---------------------------------------------------------------- the made-up fleet (same story as the screenshots in docs/img/dashboard)
type LogRow = [minAgo: number, kind: string, detail: string];
interface Spec {
  id: string; machine: string; label: string; fw: string; ip: string; first: number; beat: number; boots: number;
  up: number; heap: number; maxblk: number; rssi: number; rst: number; relay: boolean; jobs: number; activity: string; ota: "ok" | "error";
  level: NodeInfo["level"]; issues: string[]; target?: { version: string; agoMin: number; update: "pending" | "done" }; log: LogRow[];
}
const SPECS: Spec[] = [
  { id: "a1b2c3d4e5f6", machine: "03", label: "Warung Budi", fw: "1.2.0", ip: "203.0.113.24", first: 40 * 1440, beat: 1, boots: 0, up: 500_000, heap: 148_000, maxblk: 98_000, rssi: -52, rst: 1, relay: true, jobs: 1, activity: "wash cycle running", ota: "ok", level: "ok", issues: [],
    log: [[30 * 1440, "firmware", "1.0.0 → 1.1.0"], [6 * 1440 + 560, "assigned", "release 1.2.0"], [5 * 1440 + 1290, "download", "1.2.0: 1704000 of 1704000 bytes"], [5 * 1440 + 1285, "firmware", "1.1.0 → 1.2.0"], [3 * 1440, "online", "back after 47 min offline"], [170, "activity", "wash cycle running"], [125, "activity", "machines idle"], [38, "activity", "wash cycle running"]] },
  { id: "3c71bf5a9e02", machine: "04", label: "Laundry Melati", fw: "1.2.0", ip: "198.51.100.77", first: 33 * 1440, beat: 2, boots: 1, up: 208_000, heap: 152_000, maxblk: 104_000, rssi: -61, rst: 1, relay: true, jobs: 0, activity: "machines idle", ota: "ok", level: "ok", issues: [],
    log: [[20 * 1440, "firmware", "1.0.0 → 1.1.0"], [5 * 1440 + 1290, "download", "1.2.0: 1704000 of 1704000 bytes"], [5 * 1440 + 1286, "firmware", "1.1.0 → 1.2.0"], [2 * 1440 + 400, "reboot", "restarted (software restart)"], [95, "activity", "dry cycle running"], [60, "activity", "machines idle"]] },
  { id: "0a0b0c0d0e01", machine: "07", label: "Kost Mawar", fw: "1.1.0", ip: "203.0.113.130", first: 21 * 1440, beat: 3, boots: 3, up: 3_000, heap: 61_000, maxblk: 31_000, rssi: -88, rst: 6, relay: false, jobs: 0, activity: "machines idle", ota: "error", level: "bad",
    issues: ["relay controller not responding", "weak Wi-Fi (-88 dBm)", "low memory (largest free block 30 KB)", "restarted after task watchdog", "last OTA check or install failed"],
    log: [[3 * 1440, "reboot", "restarted (power-on)"], [2 * 1440 + 90, "download-failed", "1.2.0: 640000 of 1704000 bytes"], [200, "crash", "restarted (task watchdog)"], [120, "crash", "restarted (task watchdog)"], [52, "relay", "controller link lost"], [50, "crash", "restarted (task watchdog)"]] },
  { id: "ffeeddccbbaa", machine: "12", label: "Ruko Sentosa", fw: "1.2.0", ip: "198.51.100.9", first: 28 * 1440, beat: 4, boots: 0, up: 82_000, heap: 139_000, maxblk: 96_000, rssi: -57, rst: 1, relay: true, jobs: 3, activity: "wash and dry cycles running", ota: "ok", level: "ok", issues: [],
    target: { version: "1.2.0", agoMin: 1440 + 120, update: "done" },
    log: [[1440 + 120, "assigned", "release 1.2.0"], [1440 - 35, "download", "1.2.0: 1704000 of 1704000 bytes"], [1440 - 40, "firmware", "1.1.0 → 1.2.0"], [240, "activity", "wash and dry cycles running"]] },
  { id: "7e5d4c3b2a19", machine: "15", label: "Kampus Barat", fw: "1.1.0", ip: "203.0.113.201", first: 12 * 1440, beat: 2, boots: 1, up: 720, heap: 143_000, maxblk: 99_000, rssi: -70, rst: 5, relay: true, jobs: 0, activity: "machines idle", ota: "ok", level: "warn", issues: ["restarted after interrupt watchdog"],
    log: [[4 * 1440, "reboot", "restarted (software restart)"], [12, "crash", "restarted (interrupt watchdog)"]] },
  { id: "90ab12cd34ef", machine: "21", label: "Apartemen Hijau", fw: "1.2.0", ip: "198.51.100.140", first: 18 * 1440, beat: 185, boots: 0, up: 412_000, heap: 150_000, maxblk: 101_000, rssi: -64, rst: 1, relay: true, jobs: 0, activity: "machines idle", ota: "ok", level: "offline", issues: ["no heartbeat for more than 12 minutes"],
    log: [[5 * 1440 + 1290, "firmware", "1.1.0 → 1.2.0"], [190, "activity", "machines idle"]] },
  { id: "5a5a5a5a5a5a", machine: "02", label: "", fw: "1.2.0", ip: "203.0.113.55", first: 9 * 1440, beat: 1, boots: 0, up: 95_000, heap: 147_000, maxblk: 97_000, rssi: -66, rst: 1, relay: true, jobs: 2, activity: "dry cycle running", ota: "ok", level: "ok", issues: [],
    log: [[3 * 1440, "online", "back after 22 min offline"], [24, "activity", "dry cycle running"]] },
  { id: "123456abcdef", machine: "09", label: "Pasar Baru", fw: "1.1.0", ip: "198.51.100.211", first: 25 * 1440, beat: 2 * 1440 + 30, boots: 2, up: 270_000, heap: 141_000, maxblk: 95_000, rssi: -73, rst: 1, relay: true, jobs: 0, activity: "machines idle", ota: "ok", level: "offline", issues: ["no heartbeat for more than 12 minutes"],
    target: { version: "1.2.0", agoMin: 2 * 1440 + 45, update: "pending" },
    log: [[2 * 1440 + 45, "assigned", "release 1.2.0"], [2 * 1440 + 30, "activity", "machines idle"]] },
];
const RESET = ["", "power-on", "software restart", "", "", "interrupt watchdog", "task watchdog"];

// ---------------------------------------------------------------- the state
interface DemoJob { info: JobInfo; lines: [number, string][]; durMs: number; startedMs: number; canceled: boolean; resultFn: () => Record<string, unknown> | null }
interface State {
  releases: Release[]; nodes: NodeInfo[]; logs: NodeLog[]; events: DeviceEvent[];
  files: Map<string, { content: string; base: string }>; jobs: Map<string, DemoJob>; lastFirmware: string | null; lastPreview: string | null;
  preview: { builtAt: string; size: number; version: string }; nextId: number; startedMs: number;
}
let S: State | null = null;
let loading: Promise<State> | null = null;
const FILE_PATHS = ["esp32-kiosk/src/i18n.cpp", "esp32-kiosk/src/i18n.h", "esp32-kiosk/src/main.cpp", "esp32-kiosk/src/ota.cpp", "esp32-kiosk/src/ota.h", "esp32-kiosk/src/relay_link.cpp", "esp32-kiosk/src/relay_link.h", "esp32-kiosk/src/version.h", "shared/relay_proto/relay_proto.h"];
const VERSION_H = "esp32-kiosk/src/version.h";

export function resetDemo(): void { S = null; loading = null; }

function load(): Promise<State> {
  if (S) return Promise.resolve(S);
  loading ??= (async () => {
    const now = Date.now();
    const ago = (m: number) => new Date(now - m * MIN).toISOString();
    let id = 1;
    const mk = (version: string, size: number, createdAgo: number, notes: string, min = ""): Release => ({ id: id++, version, sha256: hex(64, size + vparts(version)[1]!), size, min_version: min, notes, filename: `firmware-${version}.bin`, created_at: ago(createdAgo), nodes: 0 });
    const releases = [
      mk("1.1.0", 1_620_000, 38 * 1440, "First fleet release: night install window and rollback guard"),
      mk("1.2.0", 1_704_000, 7 * 1440, "Faster idle screen, member sign-in fixes", "1.1.0"),
    ];
    const relId = (v: string) => releases.find((r) => r.version === v)!.id;

    const nodes: NodeInfo[] = [];
    const logs: NodeLog[] = [];
    const events: DeviceEvent[] = [];
    for (const [ni, sp] of SPECS.entries()) {
      const online = sp.level !== "offline";
      const health: NodeHealth = {
        fw: sp.fw, up: sp.up, heap: sp.heap, maxblk: sp.maxblk, rssi: sp.rssi, rst: sp.rst, relay: sp.relay, relayfw: sp.relay ? "0.2.0" : "", relayrst: sp.relay ? "power" : "",
        jobs: sp.jobs, scr: sp.jobs ? 5 : 0, ota: sp.ota === "ok" ? 1 : 6, latest: sp.id === "123456abcdef" ? "1.1.0" : "1.2.0",
        resetReason: RESET[sp.rst] ?? "", screen: sp.jobs ? "running" : "idle", activity: sp.activity, otaState: sp.ota,
      };
      nodes.push({
        deviceId: sp.id, machineId: sp.machine, label: sp.label, fwVersion: sp.fw, firstSeen: ago(sp.first), lastSeen: ago(sp.beat), lastBeat: ago(sp.beat), lastIp: sp.ip, boots: sp.boots,
        online, level: sp.level, issues: sp.issues, health, update: sp.target?.update ?? "none",
        target: sp.target ? { releaseId: relId(sp.target.version), version: sp.target.version, setAt: ago(sp.target.agoMin) } : null,
      });
      const name = sp.label || `Mesin ${sp.machine}`;
      const rows: LogRow[] = [[sp.first, "registered", `machine ${sp.machine}, firmware 1.0.0`], ...sp.log];
      for (const [m, kind, detail] of rows) logs.push({ id: id++, ts: ago(m), device_id: sp.id, kind, detail, name });
      // an update check about every 6 hours for three days
      for (let k = 0; k < 12; k++) {
        const m = k * 360 + 15 + ni * 7 + (((k * 37 + ni * 11) % 25) - 12);
        if (m < sp.beat) continue;
        const upgradedAt = sp.log.find((l) => l[1] === "firmware" && l[2].endsWith("→ 1.2.0"))?.[0];
        const running = sp.fw === "1.2.0" ? (upgradedAt !== undefined && m > upgradedAt ? "1.1.0" : "1.2.0") : sp.fw;
        events.push({ id: id++, ts: ago(m), kind: "manifest", ip: sp.ip, device_version: running, served_version: sp.target && m < sp.target.agoMin ? sp.target.version : "none", status: 200, bytes: 0, complete: 1, device_id: sp.id });
      }
    }
    for (const [dev, ip, m, from, bytes, done] of [["ffeeddccbbaa", "198.51.100.9", 1440 - 36, "1.1.0", 1_704_000, 1], ["0a0b0c0d0e01", "203.0.113.130", 2 * 1440 + 90, "1.1.0", 640_000, 0], ["a1b2c3d4e5f6", "203.0.113.24", 5 * 1440 + 1290, "1.1.0", 1_704_000, 1], ["3c71bf5a9e02", "198.51.100.77", 5 * 1440 + 1289, "1.1.0", 1_704_000, 1]] as const)
      events.push({ id: id++, ts: ago(m), kind: "firmware", ip, device_version: from, served_version: "1.2.0", status: 200, bytes, complete: done, device_id: dev });
    events.sort((a, b) => b.ts.localeCompare(a.ts));
    logs.sort((a, b) => b.ts.localeCompare(a.ts));

    // the editor's working copy: the real sources, with the version bumped (so the demo shows "1 file changed" and a diff)
    const files = new Map<string, { content: string; base: string }>();
    await Promise.all(FILE_PATHS.map(async (p) => {
      let text = "";
      try {
        const res = await fetch(`/demo/src/${p}`);
        text = res.ok ? await res.text() : `// (demo copy of ${p} is missing)\n`;
      } catch {
        text = `// (demo copy of ${p} could not be loaded)\n`;
      }
      files.set(p, { content: text, base: text });
    }));
    const vh = files.get(VERSION_H)!;
    vh.content = vh.base.replace(/(#define\s+FW_VERSION\s+")[^"]*(")/, "$11.3.0$2");

    S = { releases, nodes, logs, events, files, jobs: new Map(), lastFirmware: null, lastPreview: null, preview: { builtAt: ago(2 * 1440 + 400), size: 887_325, version: "1.1.0" }, nextId: id, startedMs: now };
    return S;
  })();
  return loading;
}

// ---------------------------------------------------------------- derived views
const nameOf = (n: NodeInfo) => n.label || (n.machineId ? `Mesin ${n.machineId}` : n.deviceId);
const withCounts = (s: State): Release[] => s.releases.map((r) => ({ ...r, nodes: s.nodes.filter((n) => n.target?.releaseId === r.id).length })).sort((a, b) => vcmp(b.version, a.version));

function status(s: State): Status {
  const online = s.nodes.filter((n) => n.online);
  const group = (hours: number): VersionStat[] => {
    const cut = Date.now() - hours * 3_600_000;
    const by = new Map<string, VersionStat>();
    for (const e of s.events) {
      if (e.kind !== "manifest" || new Date(e.ts).getTime() < cut) continue;
      const v = by.get(e.device_version) ?? { version: e.device_version, sources: 0, checks: 0, last_seen: e.ts };
      v.checks++;
      if (e.ts > v.last_seen) v.last_seen = e.ts;
      by.set(e.device_version, v);
    }
    for (const v of by.values()) v.sources = new Set(s.events.filter((e) => e.device_version === v.version).map((e) => e.device_id)).size;
    return [...by.values()].sort((a, b) => vcmp(b.version, a.version));
  };
  const daily: DailyStat[] = Array.from({ length: 7 }, (_, i) => {
    const d = new Date(Date.now() - (6 - i) * DAY);
    return { day: d.toISOString().slice(0, 10), checks: 24 + ((i * 7) % 9), downloads: i === 1 ? 2 : i === 4 ? 1 : 0 };
  });
  return {
    serverVersion: "2.0.0", manifestUrl: "https://ota.example.com/ota/manifest.json", releaseCount: s.releases.length, limits: { maxBytes: 4_194_304, minBytes: 100_000 },
    nodes: { total: s.nodes.length, online: online.length, attention: online.filter((n) => n.level === "warn" || n.level === "bad").length, offline: s.nodes.length - online.length, pendingUpdates: s.nodes.filter((n) => n.update === "pending").length },
    versions24h: group(24), versions7d: group(24 * 7), daily,
  };
}

// ---------------------------------------------------------------- simulated builds (nothing is compiled)
const FW_LINES = [
  "# DEMO: this build is simulated, nothing is compiled. A real server runs `pio run` here.",
  "Processing esp32-3248s035 (platform: espressif32; board: esp32dev; framework: arduino)",
  "Verbose mode can be enabled via `-v, --verbose` option",
  "Library Manager: Installing lvgl/lvgl @ ~8.3.11", "Library Manager: lvgl/lvgl@8.3.11 has been installed!",
  "Library Manager: Installing bodmer/TFT_eSPI @ ^2.5.43", "Library Manager: bodmer/TFT_eSPI@2.5.43 has been installed!",
  "Compiling .pio/build/esp32-3248s035/src/i18n.cpp.o", "Compiling .pio/build/esp32-3248s035/src/main.cpp.o", "Compiling .pio/build/esp32-3248s035/src/ota.cpp.o",
  "Compiling .pio/build/esp32-3248s035/src/relay_link.cpp.o", "Compiling .pio/build/esp32-3248s035/lib/lvgl/src/core/lv_obj.c.o", "Compiling .pio/build/esp32-3248s035/lib/lvgl/src/widgets/lv_label.c.o",
  "Compiling .pio/build/esp32-3248s035/lib/lvgl/src/extra/libs/qrcode/lv_qrcode.c.o", "Archiving .pio/build/esp32-3248s035/liblvgl.a", "Linking .pio/build/esp32-3248s035/firmware.elf",
  "Retrieving maximum program size .pio/build/esp32-3248s035/firmware.elf", "RAM:   [==        ]  18.5% (used 60552 bytes from 327680 bytes)", "Flash: [=========.]  89.6% (used 1761569 bytes from 1966080 bytes)",
  "Building .pio/build/esp32-3248s035/firmware.bin", "esptool.py v4.9.0", "Creating esp32 image...", "Successfully created esp32 image.",
];
const PV_LINES = [
  "# DEMO: this build is simulated, nothing is compiled. A real server runs Emscripten here.",
  "== harness: refreshing tools/sim from the repo", "== LVGL 8.3.11, object cache ready", "== compiling the kiosk UI", "== linking", "preview built: sim.js (887325 bytes)",
];
function startJob(s: State, kind: "firmware" | "preview"): JobInfo {
  const running = [...s.jobs.values()].find((j) => j.info.status === "running");
  if (running) throw new DemoHttpError(409, `A ${running.info.kind} build is already running`);
  const id = `demo${s.nextId++}`;
  const lines = kind === "firmware" ? FW_LINES : PV_LINES;
  const durMs = kind === "firmware" ? 15_000 : 6_000;
  const info: JobInfo = { id, kind, status: "running", startedAt: new Date().toISOString(), finishedAt: "", exitCode: null, result: null, error: "" };
  const resultFn = (): Record<string, unknown> | null => {
    if (kind === "preview") return null;
    const m = /#define\s+FW_VERSION\s+"([^"]*)"/.exec(s.files.get(VERSION_H)?.content ?? "");
    if (!m) return { version: "", size: 0, sha256: "", releasable: false, problem: "FW_VERSION not found in version.h" };
    const version = m[1]!;
    const valid = /^\d+\.\d+\.\d+$/.test(version);
    return { version, size: 1_761_569, sha256: hex(64, vparts(version).reduce((a, b) => a * 131 + b, 7)), releasable: valid, problem: valid ? "" : `"${version}" is not a version like 1.4.0` };
  };
  s.jobs.set(id, { info, lines: lines.map((l, i) => [Math.round((i / lines.length) * durMs * 0.95), l]), durMs, startedMs: Date.now(), canceled: false, resultFn });
  if (kind === "firmware") s.lastFirmware = id; else s.lastPreview = id;
  return { ...info };
}
function settle(s: State, j: DemoJob): void {
  if (j.info.status !== "running") return;
  if (j.canceled) { j.info = { ...j.info, status: "canceled", finishedAt: new Date().toISOString(), exitCode: null }; return; }
  if (Date.now() - j.startedMs < j.durMs) return;
  j.info = { ...j.info, status: "ok", exitCode: 0, finishedAt: new Date(j.startedMs + j.durMs).toISOString(), result: j.resultFn() };
  if (j.info.kind === "preview") s.preview = { builtAt: j.info.finishedAt, size: 887_325, version: "1.1.0" };
}
function jobView(s: State, id: string | null): JobInfo | null {
  const j = id ? s.jobs.get(id) : undefined;
  if (!j) return null;
  settle(s, j);
  return { ...j.info };
}

// ---------------------------------------------------------------- the router
const ok = <T>(v: T): T => structuredClone(v);

export async function demoApi(path: string, init: RequestInit = {}): Promise<unknown> {
  const s = await load();
  await sleep(80);
  const method = (init.method ?? "GET").toUpperCase();
  const url = new URL(path, "http://demo.local");
  const p = url.pathname;
  const body: Record<string, unknown> = typeof init.body === "string" && init.body ? JSON.parse(init.body) : {};
  let m: RegExpExecArray | null;

  if (p === "/api/login") return { token: DEMO_TOKEN };
  if (p === "/api/status") return ok(status(s));
  if (p === "/api/releases" && method === "GET") return ok(withCounts(s));
  if ((m = /^\/api\/releases\/(\d+)$/.exec(p)) && method === "DELETE") {
    const r = s.releases.find((x) => x.id === Number(m![1]));
    if (!r) throw new DemoHttpError(404, "No such release");
    if (s.nodes.some((n) => n.target?.releaseId === r.id)) throw new DemoHttpError(409, "This release is assigned to nodes: cancel those first");
    s.releases = s.releases.filter((x) => x !== r);
    return undefined;
  }
  if (p === "/api/nodes" && method === "GET") return ok(s.nodes);
  if (p === "/api/events") return ok(s.events.slice(0, Number(url.searchParams.get("limit") ?? 200)));
  if (p === "/api/activity") return ok(s.logs.slice(0, Number(url.searchParams.get("limit") ?? 100)));
  if (p === "/api/nodes/target" && method === "POST") {
    const rel = body.releaseId == null ? null : s.releases.find((r) => r.id === Number(body.releaseId));
    if (body.releaseId != null && !rel) throw new DemoHttpError(404, "No such release");
    const results = (body.deviceIds as string[]).map((deviceId) => {
      const n = s.nodes.find((x) => x.deviceId === deviceId);
      if (!n) return { deviceId, ok: false, error: "unknown node" };
      if (!rel) { n.target = null; n.update = "none"; return { deviceId, ok: true }; }
      if (n.fwVersion && vcmp(n.fwVersion, rel.version) >= 0) return { deviceId, ok: false, error: `already runs v${n.fwVersion}` };
      n.target = { releaseId: rel.id, version: rel.version, setAt: new Date().toISOString() };
      n.update = "pending";
      s.logs.unshift({ id: s.nextId++, ts: n.target.setAt, device_id: n.deviceId, kind: "assigned", detail: `release ${rel.version}`, name: nameOf(n) });
      return { deviceId, ok: true };
    });
    return ok({ results });
  }
  if ((m = /^\/api\/nodes\/([0-9a-f]+)$/.exec(p))) {
    const n = s.nodes.find((x) => x.deviceId === m![1]);
    if (!n) throw new DemoHttpError(404, "No such node");
    if (method === "GET") {
      const detail: NodeDetail = { node: n, log: s.logs.filter((l) => l.device_id === n.deviceId).slice(0, 40), events: s.events.filter((e) => e.device_id === n.deviceId).slice(0, 30) };
      return ok(detail);
    }
    if (method === "POST") { n.label = String(body.label ?? "").slice(0, 40); return undefined; }
    if (method === "DELETE") { s.nodes = s.nodes.filter((x) => x !== n); s.logs = s.logs.filter((l) => l.device_id !== n.deviceId); s.events = s.events.filter((e) => e.device_id !== n.deviceId); return undefined; }
  }

  // ---- code editor, builds, preview
  if (p === "/api/studio") {
    const busy = [...s.jobs.values()].map((j) => jobView(s, j.info.id)!).find((j) => j.status === "running") ?? null;
    const st: StudioStatus = { toolchain: { pio: true, emsdk: true }, workspaceSeeded: true, busy, lastFirmwareJob: jobView(s, s.lastFirmware), lastPreviewJob: jobView(s, s.lastPreview), preview: s.preview };
    return ok(st);
  }
  if (p === "/api/workspace/files") {
    const files: FileEntry[] = [...s.files].map(([path, f]) => ({ path, size: new Blob([f.content]).size, modified: f.content !== f.base })).sort((a, b) => a.path.localeCompare(b.path));
    return { files };
  }
  if (p === "/api/workspace/file") {
    const file = (pth: string) => { const f = s.files.get(pth); if (!f) throw new DemoHttpError(404, "No such file"); return f; };
    if (method === "GET") { const pth = url.searchParams.get("path") ?? ""; const f = file(pth); return { path: pth, content: f.content, base: f.content === f.base ? null : f.base }; }
    if (method === "PUT") { file(String(body.path)).content = String(body.content ?? ""); return undefined; }
  }
  if (p === "/api/workspace/revert" && method === "POST") { const f = s.files.get(String(body.path)); if (f) f.content = f.base; return undefined; }
  if (p === "/api/workspace/reset" && method === "POST") { for (const f of s.files.values()) f.content = f.base; return undefined; }
  if (p === "/api/build" && method === "POST") return startJob(s, "firmware");
  if (p === "/api/preview/build" && method === "POST") return startJob(s, "preview");
  if ((m = /^\/api\/jobs\/([\w-]+)\/cancel$/.exec(p)) && method === "POST") { const j = s.jobs.get(m[1]!); if (j) j.canceled = true; return undefined; }
  if ((m = /^\/api\/jobs\/([\w-]+)\/release$/.exec(p)) && method === "POST") {
    const j = s.jobs.get(m[1]!);
    const info = j && jobView(s, j.info.id);
    const res = info?.result;
    if (!info || info.status !== "ok" || !res || res.releasable !== true) throw new DemoHttpError(409, "This build cannot be released");
    const version = String(res.version);
    if (s.releases.some((r) => r.version === version)) throw new DemoHttpError(409, `Release v${version} already exists`);
    const r: Release = { id: s.nextId++, version, sha256: String(res.sha256), size: Number(res.size), min_version: String(body.min_version ?? ""), notes: String(body.notes ?? ""), filename: `firmware-${version}.bin`, created_at: new Date().toISOString(), nodes: 0 };
    s.releases.push(r);
    return ok(r);
  }
  if ((m = /^\/api\/jobs\/([\w-]+)$/.exec(p)) && method === "GET") {
    const j = s.jobs.get(m[1]!);
    if (!j) throw new DemoHttpError(404, "No such job");
    settle(s, j);
    const elapsed = j.info.status === "running" ? Date.now() - j.startedMs : Infinity;
    const text = j.lines.filter(([t]) => t <= elapsed).map(([, l]) => l + "\n").join("") + (j.info.status === "ok" ? (j.info.kind === "firmware" ? "========================= [SUCCESS] simulated in the demo =========================\n" : "") : j.info.status === "canceled" ? "== canceled\n" : "");
    const from = Math.min(Number(url.searchParams.get("from") ?? 0), text.length);
    return { job: { ...j.info }, log: text.slice(from), next: text.length };
  }
  throw new DemoHttpError(404, `Not available in the demo: ${method} ${p}`);
}

/** Simulated upload (the form's checks already ran in the browser on the real file). */
export async function demoUpload(file: File, fields: { version: string; min_version: string; notes: string }, onProgress: (pct: number) => void): Promise<Release> {
  const s = await load();
  for (let pct = 0; pct <= 100; pct += 20) { onProgress(pct); await sleep(90); }
  if (!/^\d+\.\d+\.\d+$/.test(fields.version)) throw new DemoHttpError(422, "The image does not contain a version (washable-ota/x.y.z)");
  if (s.releases.some((r) => r.version === fields.version)) throw new DemoHttpError(409, `Release v${fields.version} already exists`);
  let sha = "";
  try { sha = [...new Uint8Array(await crypto.subtle.digest("SHA-256", await file.arrayBuffer()))].map((b) => b.toString(16).padStart(2, "0")).join(""); } catch { sha = hex(64, file.size); }
  const r: Release = { id: s.nextId++, version: fields.version, sha256: sha, size: file.size, min_version: fields.min_version, notes: fields.notes, filename: "firmware.bin", created_at: new Date().toISOString(), nodes: 0 };
  s.releases.push(r);
  return ok(r);
}

/** What a "download" gives in the demo: a short text file saying so (there is no real firmware here). */
export async function demoDownload(path: string): Promise<Blob> {
  const s = await load();
  if (path.endsWith("/workspace/patch")) {
    const diffs = [...s.files].filter(([, f]) => f.content !== f.base).map(([p, f]) => `--- a/${p}\n+++ b/${p}\n(demo: ${f.content.length - f.base.length} characters changed)\n`);
    return new Blob([diffs.join("\n") || "no changes\n"], { type: "text/plain" });
  }
  return new Blob(["This is the dashboard demo: there is no real firmware file here.\n"], { type: "text/plain" });
}
