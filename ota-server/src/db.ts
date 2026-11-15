import { DatabaseSync } from "node:sqlite";
import { mkdirSync } from "node:fs";
import path from "node:path";

export interface Release {
  id: number;
  version: string;
  sha256: string;
  size: number;
  min_version: string;
  notes: string;
  filename: string;
  created_at: string;
}

export type EventKind = "manifest" | "firmware";

export interface NodeRow {
  device_id: string;
  machine_id: string;
  label: string;
  fw_version: string;
  first_seen: string;
  last_seen: string;
  last_beat: string;
  last_ip: string;
  health: string; // JSON of the last heartbeat (already sanitised)
  boots: number;
  target_release_id: number | null;
  target_set_at: string;
}

export interface NodeLogRow {
  id: number;
  ts: string;
  device_id: string;
  kind: string;
  detail: string;
}

export interface DeviceEvent {
  id: number;
  ts: string;
  kind: EventKind;
  ip: string;
  device_version: string;
  served_version: string;
  status: number;
  bytes: number;
  complete: number;
  device_id: string;
}

export class Store {
  readonly db: DatabaseSync;

  constructor(file: string) {
    if (file !== ":memory:") mkdirSync(path.dirname(file), { recursive: true });
    this.db = new DatabaseSync(file);
    this.db.exec(`
      PRAGMA journal_mode = WAL;
      PRAGMA foreign_keys = ON;
      CREATE TABLE IF NOT EXISTS releases (
        id          INTEGER PRIMARY KEY AUTOINCREMENT,
        version     TEXT NOT NULL UNIQUE,
        sha256      TEXT NOT NULL,
        size        INTEGER NOT NULL,
        min_version TEXT NOT NULL DEFAULT '',
        notes       TEXT NOT NULL DEFAULT '',
        filename    TEXT NOT NULL,
        created_at  TEXT NOT NULL
      );
      CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
      CREATE TABLE IF NOT EXISTS events (
        id             INTEGER PRIMARY KEY AUTOINCREMENT,
        ts             TEXT NOT NULL,
        kind           TEXT NOT NULL,
        ip             TEXT NOT NULL DEFAULT '',
        device_version TEXT NOT NULL DEFAULT '',
        served_version TEXT NOT NULL DEFAULT '',
        status         INTEGER NOT NULL DEFAULT 0,
        bytes          INTEGER NOT NULL DEFAULT 0,
        complete       INTEGER NOT NULL DEFAULT 0
      );
      CREATE INDEX IF NOT EXISTS events_ts ON events(ts);
      CREATE TABLE IF NOT EXISTS nodes (
        device_id         TEXT PRIMARY KEY,
        machine_id        TEXT NOT NULL DEFAULT '',
        label             TEXT NOT NULL DEFAULT '',
        fw_version        TEXT NOT NULL DEFAULT '',
        first_seen        TEXT NOT NULL,
        last_seen         TEXT NOT NULL,
        last_beat         TEXT NOT NULL DEFAULT '',
        last_ip           TEXT NOT NULL DEFAULT '',
        health            TEXT NOT NULL DEFAULT '{}',
        boots             INTEGER NOT NULL DEFAULT 0,
        target_release_id INTEGER REFERENCES releases(id),
        target_set_at     TEXT NOT NULL DEFAULT ''
      );
      CREATE TABLE IF NOT EXISTS node_log (
        id        INTEGER PRIMARY KEY AUTOINCREMENT,
        ts        TEXT NOT NULL,
        device_id TEXT NOT NULL,
        kind      TEXT NOT NULL,
        detail    TEXT NOT NULL DEFAULT ''
      );
      CREATE INDEX IF NOT EXISTS node_log_dev ON node_log(device_id, id);
    `);
    const cols = this.db.prepare("PRAGMA table_info(events)").all() as unknown as { name: string }[];
    if (!cols.some((c) => c.name === "device_id")) this.db.exec("ALTER TABLE events ADD COLUMN device_id TEXT NOT NULL DEFAULT ''");
  }

  listReleases(): Release[] {
    return this.db.prepare("SELECT * FROM releases ORDER BY id DESC").all() as unknown as Release[];
  }

  getRelease(id: number): Release | undefined {
    return this.db.prepare("SELECT * FROM releases WHERE id = ?").get(id) as unknown as Release | undefined;
  }

  getReleaseByVersion(version: string): Release | undefined {
    return this.db.prepare("SELECT * FROM releases WHERE version = ?").get(version) as unknown as Release | undefined;
  }

  insertRelease(r: Omit<Release, "id" | "created_at">): Release {
    const created = new Date().toISOString();
    const info = this.db
      .prepare("INSERT INTO releases (version, sha256, size, min_version, notes, filename, created_at) VALUES (?,?,?,?,?,?,?)")
      .run(r.version, r.sha256, r.size, r.min_version, r.notes, r.filename, created);
    return this.getRelease(Number(info.lastInsertRowid))!;
  }

  deleteRelease(id: number): void {
    this.db.prepare("DELETE FROM releases WHERE id = ?").run(id);
  }


  // ------------------------------------------------------------------ nodes (kiosks that identify themselves)
  listNodes(): NodeRow[] {
    return this.db.prepare("SELECT * FROM nodes ORDER BY machine_id, device_id").all() as unknown as NodeRow[];
  }

  getNode(id: string): NodeRow | undefined {
    return this.db.prepare("SELECT * FROM nodes WHERE device_id = ?").get(id) as unknown as NodeRow | undefined;
  }

  nodeCount(): number {
    return Number((this.db.prepare("SELECT COUNT(*) AS n FROM nodes").get() as { n: number }).n);
  }

  insertNode(id: string, machine: string, fw: string, ip: string): NodeRow {
    const now = new Date().toISOString();
    this.db.prepare("INSERT INTO nodes (device_id, machine_id, fw_version, first_seen, last_seen, last_ip) VALUES (?,?,?,?,?,?)").run(id, machine, fw, now, now, ip);
    return this.getNode(id)!;
  }

  /** Apply a partial update to a node row. */
  updateNode(id: string, patch: Partial<Omit<NodeRow, "device_id">>): void {
    const keys = Object.keys(patch) as (keyof typeof patch)[];
    if (!keys.length) return;
    const sets = keys.map((k) => `${String(k)} = ?`).join(", ");
    this.db.prepare(`UPDATE nodes SET ${sets} WHERE device_id = ?`).run(...(keys.map((k) => patch[k]) as (string | number | null)[]), id);
  }

  deleteNode(id: string): void {
    this.db.prepare("DELETE FROM nodes WHERE device_id = ?").run(id);
    this.db.prepare("DELETE FROM node_log WHERE device_id = ?").run(id);
  }

  setTarget(id: string, releaseId: number | null): void {
    this.db.prepare("UPDATE nodes SET target_release_id = ?, target_set_at = ? WHERE device_id = ?").run(releaseId, releaseId === null ? "" : new Date().toISOString(), id);
  }

  nodesTargeting(releaseId: number): number {
    return Number((this.db.prepare("SELECT COUNT(*) AS n FROM nodes WHERE target_release_id = ?").get(releaseId) as { n: number }).n);
  }

  logNode(deviceId: string, kind: string, detail = ""): void {
    this.db.prepare("INSERT INTO node_log (ts, device_id, kind, detail) VALUES (?,?,?,?)").run(new Date().toISOString(), deviceId, kind, detail);
  }

  nodeLog(deviceId: string, limit: number): NodeLogRow[] {
    return this.db.prepare("SELECT * FROM node_log WHERE device_id = ? ORDER BY id DESC LIMIT ?").all(deviceId, limit) as unknown as NodeLogRow[];
  }

  recentNodeLog(limit: number): NodeLogRow[] {
    return this.db.prepare("SELECT * FROM node_log ORDER BY id DESC LIMIT ?").all(limit) as unknown as NodeLogRow[];
  }

  nodeEvents(deviceId: string, limit: number): DeviceEvent[] {
    return this.db.prepare("SELECT * FROM events WHERE device_id = ? ORDER BY id DESC LIMIT ?").all(deviceId, limit) as unknown as DeviceEvent[];
  }

  logEvent(e: Omit<DeviceEvent, "id" | "ts" | "device_id"> & { ts?: string; device_id?: string }): void {
    this.db
      .prepare("INSERT INTO events (ts, kind, ip, device_version, served_version, status, bytes, complete, device_id) VALUES (?,?,?,?,?,?,?,?,?)")
      .run(e.ts ?? new Date().toISOString(), e.kind, e.ip, e.device_version, e.served_version, e.status, e.bytes, e.complete, e.device_id ?? "");
  }

  listEvents(limit: number, kind?: EventKind): DeviceEvent[] {
    const sql = kind ? "SELECT * FROM events WHERE kind = ? ORDER BY id DESC LIMIT ?" : "SELECT * FROM events ORDER BY id DESC LIMIT ?";
    const stmt = this.db.prepare(sql);
    return (kind ? stmt.all(kind, limit) : stmt.all(limit)) as unknown as DeviceEvent[];
  }

  versionStats(sinceIso: string): { version: string; sources: number; checks: number; last_seen: string }[] {
    return this.db
      .prepare(
        `SELECT device_version AS version, COUNT(DISTINCT ip) AS sources, COUNT(*) AS checks, MAX(ts) AS last_seen
         FROM events WHERE kind = 'manifest' AND ts >= ? GROUP BY device_version ORDER BY last_seen DESC`,
      )
      .all(sinceIso) as unknown as { version: string; sources: number; checks: number; last_seen: string }[];
  }

  dailyChecks(sinceIso: string): { day: string; checks: number; downloads: number }[] {
    return this.db
      .prepare(
        `SELECT substr(ts, 1, 10) AS day,
                SUM(CASE WHEN kind = 'manifest' THEN 1 ELSE 0 END) AS checks,
                SUM(CASE WHEN kind = 'firmware' AND complete = 1 THEN 1 ELSE 0 END) AS downloads
         FROM events WHERE ts >= ? GROUP BY day ORDER BY day`,
      )
      .all(sinceIso) as unknown as { day: string; checks: number; downloads: number }[];
  }

  pruneEvents(olderThanIso: string): number {
    this.db.prepare("DELETE FROM node_log WHERE ts < ?").run(olderThanIso);
    return Number(this.db.prepare("DELETE FROM events WHERE ts < ?").run(olderThanIso).changes);
  }

  close(): void {
    this.db.close();
  }
}
