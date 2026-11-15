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
  active: boolean;
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
  active: Release | null;
  paused: boolean;
  releaseCount: number;
  limits: { maxBytes: number; minBytes: number };
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
  fields: { version: string; min_version: string; notes: string; activate: boolean },
  onProgress: (pct: number) => void,
): Promise<Release> {
  return new Promise((resolve, reject) => {
    const form = new FormData();
    // text fields first, file last: the server reads parts in order
    form.append("version", fields.version);
    form.append("min_version", fields.min_version);
    form.append("notes", fields.notes);
    form.append("activate", fields.activate ? "true" : "false");
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
