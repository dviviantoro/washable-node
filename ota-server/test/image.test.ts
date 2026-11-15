import test from "node:test";
import assert from "node:assert/strict";
import { ImageError, inspectFirmware } from "../src/image.js";
import { fakeImage } from "./helpers.js";

const limits = { min: 100_000, max: 1_966_080 };

test("accepts a valid image and reads the embedded version", () => {
  const info = inspectFirmware(fakeImage("1.2.3"), limits);
  assert.equal(info.detectedVersion, "1.2.3");
  assert.equal(info.sha256.length, 64);
});

test("rejects images that could brick or loop a kiosk", () => {
  assert.throws(() => inspectFirmware(fakeImage("1.2.3", { size: 50_000 }), limits), ImageError);
  assert.throws(() => inspectFirmware(fakeImage("1.2.3", { size: 2_000_000 }), limits), /OTA slot/);
  assert.throws(() => inspectFirmware(fakeImage("1.2.3", { magic: 0x7f }), limits), /0xE9/);
  assert.throws(() => inspectFirmware(fakeImage("1.2.3", { chip: 5 }), limits), /chip id/);
  assert.throws(() => inspectFirmware(fakeImage("1.2.3", { hashOk: false }), limits), /truncated or corrupted/);
  assert.throws(() => inspectFirmware(fakeImage("1.2.3", { marker: false }), limits), /No "washable-ota/);
});
