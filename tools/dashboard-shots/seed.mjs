// Fills a freshly started ota-server with a believable demo fleet for the screenshots.
// usage: node seed.mjs <api-url> <admin-password> <data-dir>      (the data dir of that server: the nodes are written to its ota.db)
// Everything here is made up: device IDs, labels, documentation-range IPs, firmware images (valid in structure, random in content).
import { DatabaseSync } from "node:sqlite";
import path from "node:path";
import { fakeImage } from "./lib.mjs";

const [api, password, dataDir] = process.argv.slice(2);
if (!api || !password || !dataDir) {
  console.error("usage: node seed.mjs <api-url> <admin-password> <data-dir>");
  process.exit(2);
}

const login = await fetch(`${api}/api/login`, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ password }) });
if (!login.ok) throw new Error(`login failed: HTTP ${login.status}`);
const { token } = await login.json();

const releases = [
  { version: "1.1.0", size: 1_620_000, min: "", notes: "First fleet release: night install window and rollback guard" },
  { version: "1.2.0", size: 1_704_000, min: "1.1.0", notes: "Faster idle screen, member sign-in fixes" },
];
for (const r of releases) {
  const form = new FormData();
  form.append("version", r.version);
  form.append("min_version", r.min);
  form.append("notes", r.notes);
  form.append("file", new Blob([fakeImage(r.version, r.size)]), "firmware.bin");
  const res = await fetch(`${api}/api/releases`, { method: "POST", headers: { authorization: `Bearer ${token}` }, body: form });
  if (!res.ok) throw new Error(`upload ${r.version}: HTTP ${res.status} ${await res.text()}`);
}
const rels = await (await fetch(`${api}/api/releases`, { headers: { authorization: `Bearer ${token}` } })).json();
const relId = Object.fromEntries(rels.map((r) => [r.version, r.id]));
// make the history look lived-in: 1.1.0 uploaded weeks ago, 1.2.0 a few days ago (1.3.0 is built later, so it stays "just now")
{
  const bdb = new DatabaseSync(path.join(dataDir, "ota.db"));
  bdb.exec("PRAGMA busy_timeout = 5000");
  const upd = bdb.prepare("UPDATE releases SET created_at = ? WHERE version = ?");
  upd.run(new Date(Date.now() - 38 * 86_400_000).toISOString(), "1.1.0");
  upd.run(new Date(Date.now() - 7 * 86_400_000).toISOString(), "1.2.0");
  bdb.close();
}

// ------------------------------------------------------------------ nodes (written straight into the database: timestamps in the past)
const db = new DatabaseSync(path.join(dataDir, "ota.db"));
db.exec("PRAGMA busy_timeout = 5000");
const MIN = 60_000;
const ago = (min) => new Date(Date.now() - min * MIN).toISOString();

// esp_reset_reason: 1 power-on, 5 interrupt watchdog, 6 task watchdog
const nodes = [
  { id: "a1b2c3d4e5f6", machine: "03", label: "Warung Budi", fw: "1.2.0", ip: "203.0.113.24", first: 40 * 1440, beat: 1, boots: 0,
    h: { up: 500_000, heap: 148_000, maxblk: 98_000, rssi: -52, rst: 1, relay: true, jobs: 1, scr: 5, ota: 1, latest: "1.2.0" } },
  { id: "3c71bf5a9e02", machine: "04", label: "Laundry Melati", fw: "1.2.0", ip: "198.51.100.77", first: 33 * 1440, beat: 2, boots: 1,
    h: { up: 208_000, heap: 152_000, maxblk: 104_000, rssi: -61, rst: 1, relay: true, jobs: 0, scr: 0, ota: 1, latest: "1.2.0" } },
  { id: "0a0b0c0d0e01", machine: "07", label: "Kost Mawar", fw: "1.1.0", ip: "203.0.113.130", first: 21 * 1440, beat: 3, boots: 3,
    h: { up: 3_000, heap: 61_000, maxblk: 31_000, rssi: -88, rst: 6, relay: false, relayfw: "", relayrst: "", jobs: 0, scr: 0, ota: 6, latest: "1.2.0" } },
  { id: "ffeeddccbbaa", machine: "12", label: "Ruko Sentosa", fw: "1.2.0", ip: "198.51.100.9", first: 28 * 1440, beat: 4, boots: 0, target: "1.2.0", targetAgo: 1440 + 120,
    h: { up: 82_000, heap: 139_000, maxblk: 96_000, rssi: -57, rst: 1, relay: true, jobs: 3, scr: 5, ota: 1, latest: "1.2.0" } },
  { id: "7e5d4c3b2a19", machine: "15", label: "Kampus Barat", fw: "1.1.0", ip: "203.0.113.201", first: 12 * 1440, beat: 2, boots: 1,
    h: { up: 720, heap: 143_000, maxblk: 99_000, rssi: -70, rst: 5, relay: true, jobs: 0, scr: 0, ota: 1, latest: "1.2.0" } },
  { id: "90ab12cd34ef", machine: "21", label: "Apartemen Hijau", fw: "1.2.0", ip: "198.51.100.140", first: 18 * 1440, beat: 185, boots: 0,
    h: { up: 412_000, heap: 150_000, maxblk: 101_000, rssi: -64, rst: 1, relay: true, jobs: 0, scr: 0, ota: 1, latest: "1.2.0" } },
  { id: "5a5a5a5a5a5a", machine: "02", label: "", fw: "1.2.0", ip: "203.0.113.55", first: 9 * 1440, beat: 1, boots: 0,
    h: { up: 95_000, heap: 147_000, maxblk: 97_000, rssi: -66, rst: 1, relay: true, jobs: 2, scr: 5, ota: 1, latest: "1.2.0" } },
  { id: "123456abcdef", machine: "09", label: "Pasar Baru", fw: "1.1.0", ip: "198.51.100.211", first: 25 * 1440, beat: 2 * 1440 + 30, boots: 2, target: "1.2.0", targetAgo: 2 * 1440 + 45,
    h: { up: 270_000, heap: 141_000, maxblk: 95_000, rssi: -73, rst: 1, relay: true, jobs: 0, scr: 0, ota: 1, latest: "1.1.0" } },
];

const insNode = db.prepare(
  `INSERT INTO nodes (device_id, machine_id, label, fw_version, first_seen, last_seen, last_beat, last_ip, health, boots, target_release_id, target_set_at)
   VALUES (?,?,?,?,?,?,?,?,?,?,?,?)`,
);
const insLog = db.prepare("INSERT INTO node_log (ts, device_id, kind, detail) VALUES (?,?,?,?)");
const insEv = db.prepare("INSERT INTO events (ts, kind, ip, device_version, served_version, status, bytes, complete, device_id) VALUES (?,?,?,?,?,?,?,?,?)");

for (const n of nodes) {
  const health = { fw: n.fw, relayfw: "0.2.0", relayrst: "power", ...n.h };
  insNode.run(n.id, n.machine, n.label, n.fw, ago(n.first), ago(n.beat), ago(n.beat), n.ip, JSON.stringify(health), n.boots,
    n.target ? relId[n.target] : null, n.target ? ago(n.targetAgo) : "");
  insLog.run(ago(n.first), n.id, "registered", `machine ${n.machine}, firmware 1.0.0`);
}

const log = (id, minAgo, kind, detail) => insLog.run(ago(minAgo), id, kind, detail);
// Warung Budi: updated to 1.2.0 six nights ago, busy today
log("a1b2c3d4e5f6", 30 * 1440, "firmware", "1.0.0 → 1.1.0");
log("a1b2c3d4e5f6", 6 * 1440 + 560, "assigned", "release 1.2.0");
log("a1b2c3d4e5f6", 5 * 1440 + 1290, "download", "1.2.0: 1704000 of 1704000 bytes");
log("a1b2c3d4e5f6", 5 * 1440 + 1285, "firmware", "1.1.0 → 1.2.0");
log("a1b2c3d4e5f6", 3 * 1440, "online", "back after 47 min offline");
log("a1b2c3d4e5f6", 170, "activity", "wash cycle running");
log("a1b2c3d4e5f6", 125, "activity", "machines idle");
log("a1b2c3d4e5f6", 38, "activity", "wash cycle running");
// Laundry Melati
log("3c71bf5a9e02", 20 * 1440, "firmware", "1.0.0 → 1.1.0");
log("3c71bf5a9e02", 5 * 1440 + 1290, "download", "1.2.0: 1704000 of 1704000 bytes");
log("3c71bf5a9e02", 5 * 1440 + 1286, "firmware", "1.1.0 → 1.2.0");
log("3c71bf5a9e02", 2 * 1440 + 400, "reboot", "restarted (software restart)");
log("3c71bf5a9e02", 95, "activity", "dry cycle running");
log("3c71bf5a9e02", 60, "activity", "machines idle");
// Kost Mawar: struggling
log("0a0b0c0d0e01", 3 * 1440, "reboot", "restarted (power-on)");
log("0a0b0c0d0e01", 2 * 1440 + 90, "download-failed", "1.2.0: 640000 of 1704000 bytes");
log("0a0b0c0d0e01", 200, "crash", "restarted (task watchdog)");
log("0a0b0c0d0e01", 120, "crash", "restarted (task watchdog)");
log("0a0b0c0d0e01", 52, "relay", "controller link lost");
log("0a0b0c0d0e01", 50, "crash", "restarted (task watchdog)");
// Ruko Sentosa: last night's update
log("ffeeddccbbaa", 1440 + 120, "assigned", "release 1.2.0");
log("ffeeddccbbaa", 1440 - 35, "download", "1.2.0: 1704000 of 1704000 bytes");
log("ffeeddccbbaa", 1440 - 40, "firmware", "1.1.0 → 1.2.0");
log("ffeeddccbbaa", 240, "activity", "wash and dry cycles running");
// Kampus Barat
log("7e5d4c3b2a19", 4 * 1440, "reboot", "restarted (software restart)");
log("7e5d4c3b2a19", 12, "crash", "restarted (interrupt watchdog)");
// Apartemen Hijau went quiet
log("90ab12cd34ef", 5 * 1440 + 1290, "firmware", "1.1.0 → 1.2.0");
log("90ab12cd34ef", 190, "activity", "machines idle");
// Mesin 02
log("5a5a5a5a5a5a", 3 * 1440, "online", "back after 22 min offline");
log("5a5a5a5a5a5a", 24, "activity", "dry cycle running");
// Pasar Baru: assigned, never came back
log("123456abcdef", 2 * 1440 + 45, "assigned", "release 1.2.0");
log("123456abcdef", 2 * 1440 + 30, "activity", "machines idle");

// ------------------------------------------------------------------ raw requests: an update check every ~6 h per node for three days
// `upd` = minutes ago when the node got 1.2.0 (nodes that run 1.2.0 but have no `upd` have had it since before the window)
const upd = { a1b2c3d4e5f6: 7485, "3c71bf5a9e02": 7486, ffeeddccbbaa: 1400, "90ab12cd34ef": 7485, "5a5a5a5a5a5a": 99_999 };
const jitter = (i, n) => ((i * 37 + n * 11) % 25) - 12;
nodes.forEach((n, ni) => {
  for (let k = 0; k < 12; k++) {
    const m = k * 360 + 15 + ni * 7 + jitter(k, ni);
    if (m < n.beat) continue;
    const upgraded = upd[n.id] !== undefined && m < upd[n.id];
    const running = n.fw === "1.2.0" ? (upgraded ? "1.2.0" : "1.1.0") : n.fw;
    const assigned = n.target && m < n.targetAgo && !upgraded;
    insEv.run(ago(m), "manifest", n.ip, running, assigned ? n.target : "none", 200, 0, 1, n.id);
  }
});
insEv.run(ago(1440 - 36), "firmware", "198.51.100.9", "1.1.0", "1.2.0", 200, 1_704_000, 1, "ffeeddccbbaa");
insEv.run(ago(2 * 1440 + 90), "firmware", "203.0.113.130", "1.1.0", "1.2.0", 200, 640_000, 0, "0a0b0c0d0e01");
insEv.run(ago(5 * 1440 + 1290), "firmware", "203.0.113.24", "1.1.0", "1.2.0", 200, 1_704_000, 1, "a1b2c3d4e5f6");
insEv.run(ago(5 * 1440 + 1289), "firmware", "198.51.100.77", "1.1.0", "1.2.0", 200, 1_704_000, 1, "3c71bf5a9e02");
db.close();
console.log(`seeded ${nodes.length} nodes, ${releases.length} releases`);
