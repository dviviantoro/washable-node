export interface Check {
  ok: boolean;
  label: string;
}
export interface Preview {
  name: string;
  size: number;
  sha256: string;
  version: string | null;
  checks: Check[];
  valid: boolean;
}

async function sha256Hex(buf: ArrayBuffer): Promise<string> {
  if (!globalThis.crypto?.subtle) return "";
  const d = await crypto.subtle.digest("SHA-256", buf);
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

/** Mirrors the server's checks so mistakes show up before the upload. The server re-validates everything. */
export async function previewFirmware(file: File, limits: { minBytes: number; maxBytes: number }): Promise<Preview> {
  const buf = await file.arrayBuffer();
  const u8 = new Uint8Array(buf);
  const checks: Check[] = [];

  checks.push({ ok: file.size >= limits.minBytes && file.size <= limits.maxBytes, label: `Size within the OTA slot (${limits.minBytes}–${limits.maxBytes} bytes)` });
  checks.push({ ok: u8[0] === 0xe9, label: "ESP32 app image header (0xE9)" });
  checks.push({ ok: u8.length > 13 && u8[12] === 0 && u8[13] === 0, label: "Built for a classic ESP32" });

  if (u8[23] === 1 && u8.length > 32) {
    const body = buf.slice(0, u8.length - 32);
    const digest = [...u8.slice(u8.length - 32)].map((b) => b.toString(16).padStart(2, "0")).join("");
    const actual = await sha256Hex(body);
    checks.push({ ok: !actual || actual === digest, label: "Image checksum intact (not truncated)" });
  }

  const text = new TextDecoder("latin1").decode(u8);
  const versions = new Set([...text.matchAll(/washable-ota\/(\d+\.\d+\.\d+)/g)].map((m) => m[1]!));
  const version = versions.size === 1 ? [...versions][0]! : null;
  checks.push({ ok: versions.size === 1, label: version ? `Contains the OTA client, firmware version ${version}` : versions.size ? "Several firmware versions embedded (clean build needed)" : "OTA client marker not found" });

  return { name: file.name, size: file.size, sha256: await sha256Hex(buf), version, checks, valid: checks.every((c) => c.ok) };
}
