import { createHash, randomBytes } from "node:crypto";

/** Build a fake-but-structurally-valid ESP32 app image: header, one segment containing the OTA marker, SHA-256 appended. */
export function fakeImage(version: string, opts: { size?: number; chip?: number; magic?: number; hashOk?: boolean; marker?: boolean } = {}): Buffer {
  const size = opts.size ?? 200_000;
  const header = Buffer.alloc(24);
  header[0] = opts.magic ?? 0xe9;
  header[1] = 1; // segment count
  header.writeUInt16LE(opts.chip ?? 0, 12);
  header[23] = 1; // hash appended
  const marker = opts.marker === false ? "no-marker-here" : `washable-ota/${version}`;
  const body = Buffer.concat([header, Buffer.from(marker + "\0"), randomBytes(Math.max(0, size - 32 - 24 - marker.length - 1))]);
  const digest = createHash("sha256").update(body).digest();
  if (opts.hashOk === false) digest[0] = digest[0]! ^ 0xff;
  return Buffer.concat([body, digest]);
}

export function multipart(fields: Record<string, string>, file?: { name: string; data: Buffer }) {
  const boundary = "----test" + randomBytes(8).toString("hex");
  const parts: Buffer[] = [];
  for (const [k, v] of Object.entries(fields)) {
    parts.push(Buffer.from(`--${boundary}\r\nContent-Disposition: form-data; name="${k}"\r\n\r\n${v}\r\n`));
  }
  if (file) {
    parts.push(Buffer.from(`--${boundary}\r\nContent-Disposition: form-data; name="file"; filename="${file.name}"\r\nContent-Type: application/octet-stream\r\n\r\n`));
    parts.push(file.data, Buffer.from("\r\n"));
  }
  parts.push(Buffer.from(`--${boundary}--\r\n`));
  return { payload: Buffer.concat(parts), headers: { "content-type": `multipart/form-data; boundary=${boundary}` } };
}
