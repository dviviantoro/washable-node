import type { NodeRow, Release, Store } from "./db.js";
import { cmpVer, isSemver } from "./version.js";

/** A kiosk identifies itself with its factory MAC (12 lowercase hex digits) in the X-Device-Id header. */
export const DEVICE_ID_RE = /^[0-9a-f]{12}$/;
export const MAX_NODES = 1000;
/** The firmware beats every 5 minutes; two missed beats in a row = offline. */
export const ONLINE_MS = 12 * 60 * 1000;
export const MIN_BEAT_GAP_MS = 15_000;

// esp_reset_reason_t
const RESET_REASONS: Record<number, string> = {
  0: "unknown",
  1: "power-on",
  2: "external reset",
  3: "software restart",
  4: "crash (panic)",
  5: "interrupt watchdog",
  6: "task watchdog",
  7: "watchdog",
  8: "deep sleep",
  9: "brownout",
  10: "SDIO",
};
const BAD_RESETS = new Set([4, 5, 6, 7, 9]);

// main.cpp enum Scr
const SCREENS = ["idle", "service", "cycle", "pay", "success", "running", "done", "pin", "settings", "wifi", "edit", "ota", "member", "phone", "check", "otp", "member home", "relay test", "info", "web", "health", "about"];
// ota.h enum OtaState
const OTA_STATES = ["disabled", "idle", "checking", "update available", "downloading", "verifying", "error"];

export interface Health {
  fw: string;
  up: number;
  heap: number;
  maxblk: number;
  rssi: number;
  rst: number;
  relay: boolean;
  relayfw: string;
  relayrst: string;
  jobs: number;
  scr: number;
  ota: number;
  latest: string;
}

const clampInt = (v: unknown, lo: number, hi: number, fallback = lo): number => {
  const n = typeof v === "number" ? v : Number(v);
  return Number.isFinite(n) ? Math.min(hi, Math.max(lo, Math.trunc(n))) : fallback;
};
const cleanStr = (v: unknown, max: number): string => (typeof v === "string" ? v.replace(/[^0-9A-Za-z.\-_ ]/g, "").slice(0, max) : "");

/** Never trust a device: clamp every number and strip every string before it is stored and shown in the dashboard. */
export function sanitizeHeartbeat(body: unknown): Health | null {
  if (!body || typeof body !== "object") return null;
  const b = body as Record<string, unknown>;
  const fw = cleanStr(b.fw, 15);
  if (!isSemver(fw)) return null;
  return {
    fw,
    up: clampInt(b.up, 0, 4_294_967_295),
    heap: clampInt(b.heap, 0, 8_000_000),
    maxblk: clampInt(b.maxblk, 0, 8_000_000),
    rssi: clampInt(b.rssi, -127, 0, -127),
    rst: clampInt(b.rst, 0, 20),
    relay: Number(b.relay) === 1,
    relayfw: cleanStr(b.relayfw, 15),
    relayrst: cleanStr(b.relayrst, 7),
    jobs: clampInt(b.jobs, 0, 3),
    scr: clampInt(b.scr, 0, 63),
    ota: clampInt(b.ota, 0, 15),
    latest: cleanStr(b.latest, 15),
  };
}

export const resetName = (n: number) => RESET_REASONS[n] ?? `code ${n}`;

function parseHealth(s: string): Health | null {
  try {
    const h = JSON.parse(s) as Partial<Health>;
    return h && typeof h === "object" && typeof h.fw === "string" ? (h as Health) : null;
  } catch {
    return null;
  }
}

export interface Contact {
  id: string;
  machine: string;
  fw: string;
  ip: string;
}

/** A kiosk polled the manifest or firmware: register it on first sight and keep its last-seen data fresh. */
export function recordContact(store: Store, c: Contact): NodeRow | undefined {
  let node = store.getNode(c.id);
  const machine = cleanStr(c.machine, 7);
  const fw = isSemver(c.fw) ? c.fw : "";
  if (!node) {
    if (store.nodeCount() >= MAX_NODES) return undefined;
    node = store.insertNode(c.id, machine, fw, c.ip);
    store.logNode(c.id, "registered", `machine ${machine || "?"}${fw ? `, firmware ${fw}` : ""}`);
    return node;
  }
  const patch: Partial<NodeRow> = { last_seen: new Date().toISOString(), last_ip: c.ip };
  if (machine && machine !== node.machine_id) patch.machine_id = machine;
  if (fw && fw !== node.fw_version) {
    if (node.fw_version) store.logNode(c.id, "firmware", `${node.fw_version} → ${fw}`);
    patch.fw_version = fw;
  }
  store.updateNode(c.id, patch);
  return { ...node, ...patch };
}

/** A heartbeat arrived: store health, detect reboots and a node that came back from being offline. */
export function recordHeartbeat(store: Store, node: NodeRow, h: Health, ip: string): void {
  const prev = parseHealth(node.health);
  const now = Date.now();
  const patch: Partial<NodeRow> = { health: JSON.stringify(h), last_beat: new Date(now).toISOString(), last_seen: new Date(now).toISOString(), last_ip: ip };
  if (h.fw !== node.fw_version) {
    if (node.fw_version) store.logNode(node.device_id, "firmware", `${node.fw_version} → ${h.fw}`);
    patch.fw_version = h.fw;
  }
  if (prev && h.up < prev.up) {
    patch.boots = node.boots + 1;
    store.logNode(node.device_id, BAD_RESETS.has(h.rst) ? "crash" : "reboot", `restarted (${resetName(h.rst)})`);
  } else if (!prev) {
    store.logNode(node.device_id, "boot", `up ${h.up}s, last reset: ${resetName(h.rst)}`);
  }
  const lastBeat = node.last_beat ? Date.parse(node.last_beat) : 0;
  if (lastBeat && now - lastBeat > ONLINE_MS) store.logNode(node.device_id, "online", `back after ${Math.round((now - lastBeat) / 60_000)} min offline`);
  if (prev && !prev.relay && h.relay) store.logNode(node.device_id, "relay", "controller link restored");
  if (prev && prev.relay && !h.relay) store.logNode(node.device_id, "relay", "controller link lost");
  if (prev && prev.jobs !== h.jobs) store.logNode(node.device_id, "activity", describeJobs(h.jobs));
  store.updateNode(node.device_id, patch);
}

const describeJobs = (j: number) => (j === 0 ? "machines idle" : j === 1 ? "wash cycle running" : j === 2 ? "dry cycle running" : "wash and dry cycles running");

export type HealthLevel = "ok" | "warn" | "bad" | "offline";
export type UpdateState = "none" | "pending" | "done";

export interface NodeView {
  deviceId: string;
  machineId: string;
  label: string;
  fwVersion: string;
  firstSeen: string;
  lastSeen: string;
  lastBeat: string;
  lastIp: string;
  boots: number;
  online: boolean;
  level: HealthLevel;
  issues: string[];
  health: (Health & { resetReason: string; screen: string; activity: string; otaState: string }) | null;
  target: { releaseId: number; version: string; setAt: string } | null;
  update: UpdateState;
}

export function nodeView(n: NodeRow, releases: Map<number, Release>, now = Date.now()): NodeView {
  const h = parseHealth(n.health);
  const seen = Date.parse(n.last_beat || n.last_seen);
  const online = now - seen < ONLINE_MS;
  const issues: string[] = [];
  if (!online) issues.push(n.last_beat ? "no heartbeat for more than 12 minutes" : "no heartbeat yet (firmware without telemetry?)");
  let level: HealthLevel = online ? "ok" : "offline";
  if (online && h) {
    const bump = (l: HealthLevel) => { if (level === "ok" || (level === "warn" && l === "bad")) level = l; };
    if (!h.relay) { issues.push("relay controller not responding"); bump("bad"); }
    if (h.rssi <= -85) { issues.push(`weak Wi-Fi (${h.rssi} dBm)`); bump("warn"); }
    if (h.maxblk > 0 && h.maxblk < 50_000) { issues.push(`low memory (largest free block ${Math.round(h.maxblk / 1024)} KB)`); bump("warn"); }
    if (BAD_RESETS.has(h.rst) && h.up < 3600) { issues.push(`restarted after ${resetName(h.rst)}`); bump("warn"); }
    if (h.ota === 6) { issues.push("last OTA check or install failed"); bump("warn"); }
  }
  const rel = n.target_release_id !== null ? releases.get(n.target_release_id) : undefined;
  let update: UpdateState = "none";
  if (rel) update = n.fw_version && cmpVer(rel.version, n.fw_version) <= 0 ? "done" : "pending";
  return {
    deviceId: n.device_id,
    machineId: n.machine_id,
    label: n.label,
    fwVersion: n.fw_version,
    firstSeen: n.first_seen,
    lastSeen: n.last_seen,
    lastBeat: n.last_beat,
    lastIp: n.last_ip,
    boots: n.boots,
    online,
    level,
    issues,
    health: h ? { ...h, resetReason: resetName(h.rst), screen: SCREENS[h.scr] ?? `screen ${h.scr}`, activity: describeJobs(h.jobs), otaState: OTA_STATES[h.ota] ?? String(h.ota) } : null,
    target: rel ? { releaseId: rel.id, version: rel.version, setAt: n.target_set_at } : null,
    update,
  };
}
