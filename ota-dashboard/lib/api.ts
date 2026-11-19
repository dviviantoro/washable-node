import { DemoHttpError, demoApi, demoDownload, demoUpload, isDemoToken } from "./demo";

export { DEMO_TOKEN, isDemoToken, resetDemo } from "./demo";

export const API_URL = (process.env.NEXT_PUBLIC_API_URL ?? "").replace(/\/$/, "");

export interface Release {
  id: number;
  version: string;
  sha256: string;
  size: number;
  min_version: string;
  notes: string;
  filename: string;
  created_at: string;
  /** How many nodes are currently assigned this release. */
  nodes: number;
}
export interface VersionStat {
  version: string;
  sources: number;
  checks: number;
  last_seen: string;
}
export interface DailyStat {
  day: string;
  checks: number;
  downloads: number;
}
export interface Status {
  serverVersion: string;
  manifestUrl: string;
  releaseCount: number;
  limits: { maxBytes: number; minBytes: number };
  nodes: { total: number; online: number; attention: number; offline: number; pendingUpdates: number };
  versions24h: VersionStat[];
  versions7d: VersionStat[];
  daily: DailyStat[];
}
export interface DeviceEvent {
  id: number;
  ts: string;
  kind: "manifest" | "firmware";
  ip: string;
  device_version: string;
  served_version: string;
  status: number;
  bytes: number;
  complete: number;
  device_id: string;
}

export interface NodeHealth {
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
  resetReason: string;
  screen: string;
  activity: string;
  otaState: string;
}
export interface NodeInfo {
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
  level: "ok" | "warn" | "bad" | "offline";
  issues: string[];
  health: NodeHealth | null;
  target: { releaseId: number; version: string; setAt: string } | null;
  update: "none" | "pending" | "done";
}
export interface NodeLog {
  id: number;
  ts: string;
  device_id: string;
  kind: string;
  detail: string;
  name?: string;
}
export interface NodeDetail {
  node: NodeInfo;
  log: NodeLog[];
  events: DeviceEvent[];
}

export interface FileEntry {
  path: string;
  size: number;
  modified: boolean;
}
export type JobStatus = "running" | "ok" | "failed" | "canceled";
export interface JobInfo {
  id: string;
  kind: "firmware" | "preview";
  status: JobStatus;
  startedAt: string;
  finishedAt: string;
  exitCode: number | null;
  result: Record<string, unknown> | null;
  error: string;
}
export interface StudioStatus {
  toolchain: { pio: boolean; emsdk: boolean };
  workspaceSeeded: boolean;
  busy: JobInfo | null;
  lastFirmwareJob: JobInfo | null;
  lastPreviewJob: JobInfo | null;
  preview: { builtAt: string; size: number; version: string } | null;
}

export class ApiError extends Error {
  constructor(
    public status: number,
    message: string,
  ) {
    super(message);
  }
}

const TOKEN_KEY = "washable-ota-token";
export function loadToken(): string | null {
  try {
    return window.localStorage.getItem(TOKEN_KEY);
  } catch {
    return null;
  }
}
export function saveToken(token: string | null): void {
  try {
    if (token) window.localStorage.setItem(TOKEN_KEY, token);
    else window.localStorage.removeItem(TOKEN_KEY);
  } catch {
    /* private mode: the session simply won't survive a reload */
  }
}

async function errorFrom(res: Response): Promise<ApiError> {
  let msg = res.statusText || `HTTP ${res.status}`;
  try {
    const j = (await res.json()) as { error?: string };
    if (j.error) msg = j.error;
  } catch {
    /* not JSON */
  }
  return new ApiError(res.status, msg);
}

export async function api<T>(path: string, token: string | null, init: RequestInit = {}): Promise<T> {
  if (isDemoToken(token)) {
    try {
      return (await demoApi(path, init)) as T;
    } catch (e) {
      throw e instanceof DemoHttpError ? new ApiError(e.status, e.message) : e;
    }
  }
  const headers = new Headers(init.headers);
  if (token) headers.set("Authorization", `Bearer ${token}`);
  let res: Response;
  try {
    res = await fetch(API_URL + path, { ...init, headers, cache: "no-store" });
  } catch {
    throw new ApiError(0, "Cannot reach the OTA server. Is the Cloudflare tunnel up?");
  }
  if (!res.ok) throw await errorFrom(res);
  if (res.status === 204) return undefined as T;
  return (await res.json()) as T;
}

export function uploadRelease(
  token: string,
  file: File,
  fields: { version: string; min_version: string; notes: string },
  onProgress: (pct: number) => void,
): Promise<Release> {
  if (isDemoToken(token)) return demoUpload(file, fields, onProgress).catch((e) => { throw e instanceof DemoHttpError ? new ApiError(e.status, e.message) : e; });
  return new Promise((resolve, reject) => {
    const form = new FormData();
    // text fields first, file last: the server reads parts in order
    form.append("version", fields.version);
    form.append("min_version", fields.min_version);
    form.append("notes", fields.notes);
    form.append("file", file, "firmware.bin");
    const xhr = new XMLHttpRequest();
    xhr.open("POST", API_URL + "/api/releases");
    xhr.setRequestHeader("Authorization", `Bearer ${token}`);
    xhr.upload.onprogress = (e) => e.lengthComputable && onProgress(Math.round((e.loaded / e.total) * 100));
    xhr.onerror = () => reject(new ApiError(0, "Cannot reach the OTA server. Is the Cloudflare tunnel up?"));
    xhr.onload = () => {
      let body: { error?: string } & Partial<Release> = {};
      try {
        body = JSON.parse(xhr.responseText);
      } catch {
        /* ignore */
      }
      if (xhr.status >= 200 && xhr.status < 300) resolve(body as Release);
      else reject(new ApiError(xhr.status, body.error ?? `Upload failed (HTTP ${xhr.status})`));
    };
    xhr.send(form);
  });
}

/** Download an authenticated file (the API needs the bearer token, so a plain link will not do). */
export async function downloadFile(token: string, path: string, filename: string): Promise<void> {
  let blob: Blob;
  if (isDemoToken(token)) {
    blob = await demoDownload(path);
  } else {
    const res = await fetch(API_URL + path, { headers: { Authorization: `Bearer ${token}` }, cache: "no-store" });
    if (!res.ok) throw await errorFrom(res);
    blob = await res.blob();
  }
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 5000);
}
