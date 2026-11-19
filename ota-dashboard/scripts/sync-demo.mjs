// Refreshes the files the dashboard's demo mode serves from public/demo/:
//   sim.js          the compiled kiosk preview (WebAssembly in one file), from the server's last "Build preview" (ota-server/data/preview/current)
//   src/<path>      the firmware sources the demo's Code tab opens (same relative paths as the real workspace)
// Run it after changing the kiosk firmware or rebuilding the preview, and commit the result (Vercel builds from this folder only).
//   node scripts/sync-demo.mjs [path/to/sim.js]
import { copyFileSync, existsSync, mkdirSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "../..");
const out = path.resolve(here, "../public/demo");
const FILES = [
  "esp32-kiosk/src/i18n.cpp", "esp32-kiosk/src/i18n.h", "esp32-kiosk/src/main.cpp", "esp32-kiosk/src/ota.cpp", "esp32-kiosk/src/ota.h",
  "esp32-kiosk/src/relay_link.cpp", "esp32-kiosk/src/relay_link.h", "esp32-kiosk/src/version.h", "shared/relay_proto/relay_proto.h",
];
const sim = process.argv[2] ?? path.join(root, "ota-server/data/preview/current/sim.js");
if (!existsSync(sim)) throw new Error(`no preview build at ${sim}: use "Build preview" in the dashboard first, or pass the path`);
mkdirSync(out, { recursive: true });
copyFileSync(sim, path.join(out, "sim.js"));
for (const f of FILES) {
  mkdirSync(path.dirname(path.join(out, "src", f)), { recursive: true });
  copyFileSync(path.join(root, f), path.join(out, "src", f));
}
console.log(`demo files refreshed in ${out}`);
