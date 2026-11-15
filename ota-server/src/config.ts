import { existsSync } from "node:fs";
import path from "node:path";

export interface Config {
  host: string;
  port: number;
  dataDir: string;
  adminPassword: string;
  jwtSecret: string;
  /** Exact origins, or "https://*.vercel.app" style wildcards, allowed to call the admin API from a browser. */
  allowedOrigins: string[];
  /** Public https URL of the tunnel (only used to print the manifest URL; falls back to the request Host). */
  publicUrl: string;
  /** Hard limits: the ESP32 OTA slot is 0x1E0000 bytes and ota.cpp rejects manifests with size < 100000. */
  maxFirmwareBytes: number;
  minFirmwareBytes: number;
  eventRetentionDays: number;
  tokenTtl: string;
  /** Git checkout the code editor's working copy is seeded from (contains esp32-kiosk/, shared/, tools/sim/). */
  repoDir: string;
  /** PlatformIO CLI used by the firmware build. */
  pioBin: string;
  /** emsdk checkout used to compile the interactive kiosk preview to WebAssembly. */
  emsdkDir: string;
}

export const OTA_SLOT_BYTES = 0x1e0000; // min_spiffs.csv: two 1,966,080 byte app slots
export const MIN_FIRMWARE_BYTES = 100_000; // ota.cpp: size < 100000 -> "manifest tidak valid"

export function loadConfig(env: NodeJS.ProcessEnv = process.env): Config {
  const need = (k: string): string => {
    const v = env[k];
    if (!v) throw new Error(`Missing required environment variable ${k} (see .env.example)`);
    return v;
  };
  const jwtSecret = need("JWT_SECRET");
  if (jwtSecret.length < 32) throw new Error("JWT_SECRET must be at least 32 characters");
  const adminPassword = need("ADMIN_PASSWORD");
  if (adminPassword.length < 8) throw new Error("ADMIN_PASSWORD must be at least 8 characters");
  const dataDir = path.resolve(env.DATA_DIR ?? "./data");
  return {
    host: env.HOST ?? "127.0.0.1",
    port: Number(env.PORT ?? 8787),
    dataDir,
    adminPassword,
    jwtSecret,
    allowedOrigins: (env.ALLOWED_ORIGINS ?? "")
      .split(",")
      .map((s) => s.trim().replace(/\/$/, ""))
      .filter(Boolean),
    publicUrl: (env.PUBLIC_URL ?? "").replace(/\/$/, ""),
    maxFirmwareBytes: OTA_SLOT_BYTES,
    minFirmwareBytes: MIN_FIRMWARE_BYTES,
    eventRetentionDays: Number(env.EVENT_RETENTION_DAYS ?? 90),
    tokenTtl: env.TOKEN_TTL ?? "12h",
    repoDir: path.resolve(env.REPO_DIR ?? path.join(process.cwd(), "..")),
    pioBin: env.PIO_BIN ?? path.join(dataDir, "tools", "pio-venv", "bin", "pio"),
    emsdkDir: env.EMSDK_DIR ?? path.join(dataDir, "tools", "emsdk"),
  };
}

/** Load ./.env (if present) into process.env without overriding real environment variables. */
export function loadDotEnv(file = path.resolve(process.cwd(), ".env")): void {
  if (existsSync(file)) process.loadEnvFile(file);
}
