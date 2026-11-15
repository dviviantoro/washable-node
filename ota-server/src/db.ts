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
    `);
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

  /** The release currently announced in manifest.json, or undefined (rollout paused). */
  getActive(): Release | undefined {
    const row = this.db.prepare("SELECT value FROM settings WHERE key = 'active_release_id'").get() as { value: string } | undefined;
    return row ? this.getRelease(Number(row.value)) : undefined;
  }

  setActive(id: number | null): void {
    if (id === null) this.db.prepare("DELETE FROM settings WHERE key = 'active_release_id'").run();
    else this.db.prepare("INSERT INTO settings (key, value) VALUES ('active_release_id', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value").run(String(id));
  }

  logEvent(e: Omit<DeviceEvent, "id" | "ts"> & { ts?: string }): void {
    this.db
      .prepare("INSERT INTO events (ts, kind, ip, device_version, served_version, status, bytes, complete) VALUES (?,?,?,?,?,?,?,?)")
      .run(e.ts ?? new Date().toISOString(), e.kind, e.ip, e.device_version, e.served_version, e.status, e.bytes, e.complete);
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
    return Number(this.db.prepare("DELETE FROM events WHERE ts < ?").run(olderThanIso).changes);
  }

  close(): void {
    this.db.close();
  }
}
