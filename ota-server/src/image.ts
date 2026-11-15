import { createHash } from "node:crypto";
import { cmpVer, isSemver } from "./version.js";

export class ImageError extends Error {}

export interface ImageInfo {
  sha256: string;
  size: number;
  /** Version baked into the binary (ota.cpp sets the User-Agent "washable-ota/<FW_VERSION>"). */
  detectedVersion: string;
}

const ESP_IMAGE_MAGIC = 0xe9;
const CHIP_ID_ESP32 = 0x0000;
const MARKER = /washable-ota\/(\d+\.\d+\.\d+)/g;

/**
 * Validates an uploaded firmware.bin before it can ever reach a kiosk.
 * Every check here prevents a way of bricking or looping a deployed device:
 *  - size window: the slot is 0x1E0000 bytes, ota.cpp refuses manifests with size < 100000
 *  - ESP32 app image header (magic 0xE9, chip id 0) and the appended SHA-256 (catches truncated uploads)
 *  - the "washable-ota/<ver>" marker: proves the binary contains the OTA client, and that the version
 *    we publish equals the version the device will report. A mismatch makes a kiosk reinstall the same
 *    image every night, because it compares manifest.version with its own FW_VERSION.
 */
export function inspectFirmware(buf: Buffer, limits: { min: number; max: number }): ImageInfo {
  if (buf.length < limits.min) {
    throw new ImageError(`File is ${buf.length} bytes; a kiosk firmware is at least ${limits.min} bytes.`);
  }
  if (buf.length > limits.max) {
    throw new ImageError(`File is ${buf.length} bytes; the OTA slot only holds ${limits.max} bytes.`);
  }
  if (buf[0] !== ESP_IMAGE_MAGIC) {
    throw new ImageError("Not an ESP32 app image (first byte is not 0xE9). Upload .pio/build/esp32-3248s035/firmware.bin, not the .elf or a merged flash image.");
  }
  const segments = buf[1] ?? 0;
  if (segments < 1 || segments > 16) throw new ImageError("Corrupt ESP32 image header (bad segment count).");
  const chipId = buf.readUInt16LE(12);
  if (chipId !== CHIP_ID_ESP32) {
    throw new ImageError(`Image targets chip id 0x${chipId.toString(16)}, but the kiosk is a classic ESP32 (0x0).`);
  }
  if (buf[23] === 1) {
    const body = buf.subarray(0, buf.length - 32);
    const tail = buf.subarray(buf.length - 32);
    if (!createHash("sha256").update(body).digest().equals(tail)) {
      throw new ImageError("The image's own SHA-256 does not match its content: the file is truncated or corrupted.");
    }
  }

  const found = new Set<string>();
  for (const m of buf.toString("latin1").matchAll(MARKER)) found.add(m[1]!);
  if (found.size === 0) {
    throw new ImageError('No "washable-ota/<version>" marker found. This build has no OTA client (or was not built from esp32-kiosk), so kiosks could never be updated again after installing it.');
  }
  if (found.size > 1) {
    throw new ImageError(`Several firmware versions are embedded (${[...found].join(", ")}); do a clean build.`);
  }
  const detectedVersion = [...found][0]!;
  if (!isSemver(detectedVersion)) throw new ImageError("Embedded version is not x.y.z.");

  return { sha256: createHash("sha256").update(buf).digest("hex"), size: buf.length, detectedVersion };
}

export function checkMinVersion(version: string, minVersion: string): void {
  if (!isSemver(minVersion)) throw new ImageError("min_version must look like 1.2.0.");
  if (cmpVer(minVersion, version) > 0) throw new ImageError("min_version cannot be newer than the release itself.");
}
