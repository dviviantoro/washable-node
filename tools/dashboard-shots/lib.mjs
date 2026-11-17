import { createHash } from "node:crypto";
import { existsSync, readdirSync } from "node:fs";
import os from "node:os";
import path from "node:path";

/** Deterministic filler (xorshift32), so the same version always gives the same bytes and the same SHA-256 in every pass. */
function filler(n, seed) {
  const out = Buffer.alloc(n);
  let x = (seed >>> 0) || 1;
  for (let i = 0; i < n; i++) {
    x ^= x << 13; x >>>= 0; x ^= x >>> 17; x ^= x << 5; x >>>= 0;
    out[i] = x & 0xff;
  }
  return out;
}

/** A structurally valid ESP32 app image carrying the OTA marker (what the server's upload checks look for). */
export function fakeImage(version, size, { marker = true } = {}) {
  const header = Buffer.alloc(24);
  header[0] = 0xe9;
  header[1] = 1;
  header.writeUInt16LE(0, 12);
  header[23] = 1;
  const m = marker ? `washable-ota/${version}` : "no-marker-here";
  const body = Buffer.concat([header, Buffer.from(m + "\0"), filler(size - 32 - 24 - m.length - 1, createHash("sha256").update(m).digest().readUInt32LE(0))]);
  return Buffer.concat([body, createHash("sha256").update(body).digest()]);
}

/** CHROME_PATH, else the newest Chromium that Playwright installed under ~/.cache/ms-playwright (or the macOS cache). */
export function findChrome() {
  if (process.env.CHROME_PATH) return process.env.CHROME_PATH;
  for (const base of [path.join(os.homedir(), ".cache", "ms-playwright"), path.join(os.homedir(), "Library", "Caches", "ms-playwright")]) {
    if (!existsSync(base)) continue;
    const dirs = readdirSync(base).filter((d) => /^chromium-\d+$/.test(d)).sort().reverse();
    for (const d of dirs) {
      for (const rel of ["chrome-linux-arm64/chrome", "chrome-linux/chrome", "chrome-mac/Chromium.app/Contents/MacOS/Chromium", "chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing"]) {
        const p = path.join(base, d, rel);
        if (existsSync(p)) return p;
      }
    }
  }
  return undefined;
}
