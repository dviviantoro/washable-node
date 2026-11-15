# ota-server

Self-hosted OTA backend for the kiosk. It replaces "GitHub Releases" as the place the ESP32 downloads firmware from, and gives the
[dashboard](../ota-dashboard/README.md) an admin API. **No firmware change is needed**: the kiosk already polls a manifest URL
over HTTPS (`esp32-kiosk/src/ota.cpp`), this server speaks exactly that protocol. Kiosks running **firmware 1.3.0 or newer** also
identify themselves (device ID + machine ID) and send a heartbeat every 5 minutes; that is what makes **per-node updates** and the **fleet health view** possible.
The dashboard also contains a code editor, a PlatformIO build and an interactive kiosk preview (see "Code, build and preview" below).

Node 22.13+ (uses the built-in `node:sqlite`), Fastify 5, SQLite, files on disk. No native modules.

## What the kiosk sees

| Request | Answer |
|---|---|
| `GET /ota/manifest.json` | `{"version","sha256","size"[,"min_version"]}` of the release **assigned to that kiosk** (by its `X-Device-Id`) |
| `GET /ota/firmware.bin` | the image assigned to that kiosk, with an explicit `Content-Length` (the kiosk requires `size` to match the manifest) |
| `POST /ota/heartbeat` | JSON health report from the kiosk (firmware, uptime, heap, Wi-Fi RSSI, reset reason, relay-controller link, running cycles, screen, OTA state) |

Kiosks add `X-Device-Id` (factory MAC, 12 hex digits), `X-Machine-Id` and `X-FW-Version` headers, but only when their OTA URL ends in `/ota/manifest.json`, so nothing is leaked to GitHub.
**Updates are never broadcast.** A kiosk is offered a release only if an admin assigned it to that device; every other kiosk, and old firmware that sends no device ID, gets the inert manifest below.
`firmware.bin` must sit in the same folder as `manifest.json` (the kiosk derives its URL from the manifest URL), which is why both live under `/ota/`.
Set the kiosk's **OTA URL** to `https://<tunnel-host>/ota/manifest.json`. It never changes between releases.

When nothing is assigned to a kiosk the manifest says `0.0.0`; kiosks read that as "up to date" instead of showing an error.

A node appears in the dashboard the first time it contacts the server (the device ID is its identity; the machine ID is only a label, two kiosks may share it). A node is **offline** after 12 minutes without a heartbeat. Heartbeats are rate limited per device, clamped and stripped before they are stored, and at most 1000 nodes are accepted.

## Admin API (JWT, used by the dashboard)

| | |
|---|---|
| `POST /api/login` `{password}` | returns `{token}` (12 h). Rate limited to 8/min per IP |
| `GET /api/status` | node counts (total, online, needing attention, offline, pending updates), manifest URL, 14-day activity |
| `GET /api/nodes`, `GET /api/nodes/:id` | the fleet with health, issues, assigned release; one node with its timeline |
| `POST /api/nodes/:id` `{label}`, `DELETE /api/nodes/:id` | rename / forget a node |
| `POST /api/nodes/target` `{releaseId\|null, deviceIds[]}` | assign (or clear) a release for **an explicit list** of nodes. Refuses downgrades and `min_version` violations per node |
| `GET /api/releases`, `POST /api/releases` | list; upload (multipart: `version?`, `min_version?`, `notes?`, **`file`** last) |
| `DELETE /api/releases/:id` | delete a stored release (refused while a node is assigned it) |
| `GET /api/releases/:id/firmware.bin` | download a stored image |
| `GET /api/activity`, `GET /api/events` | fleet timeline (registered, reboot, crash, firmware change, assigned, download...) and raw requests |
| `GET/PUT /api/workspace/file(s)`, `POST /api/workspace/revert\|reset`, `GET /api/workspace/patch` | the code editor's working copy (see below) |
| `GET /api/studio`, `POST /api/build`, `GET /api/jobs[/:id]`, `POST /api/jobs/:id/cancel\|release` | PlatformIO build, live log, "add this build to releases" |
| `POST /api/preview/build`, `GET /api/preview/sim.js` | compile / fetch the WebAssembly kiosk preview |
| `GET /healthz` | liveness, no auth |

## Upload checks (why a bad file cannot reach a kiosk)

An upload is rejected unless it is: between 100,000 bytes (kiosk sanity limit) and 1,966,080 bytes (the OTA slot in `min_spiffs.csv`);
an ESP32 app image (magic `0xE9`, chip id 0); intact (the image's own appended SHA-256 matches, so a truncated upload is caught);
and it contains the string `washable-ota/<version>` that `ota.cpp` bakes in as its HTTP User-Agent.
That last check proves two things: the binary still contains the OTA client (otherwise the kiosk could never be updated again),
and the version you publish equals the version the kiosk will report (a mismatch makes a kiosk reinstall the same image every night).
The version is therefore read **from the binary**; a typed version that disagrees is refused.

## Setup

```bash
cd ota-server
npm install
cp .env.example .env        # set ADMIN_PASSWORD, JWT_SECRET (openssl rand -hex 32), ALLOWED_ORIGINS
npm run build
npm test                    # image validation, HTTP flow, node targeting/health, workspace sandbox
npm start                   # listens on http://127.0.0.1:8787 (PORT in .env)
```

Run it as a service (user unit; Node path is pinned to the Node 22 binary):

```bash
mkdir -p ~/.config/systemd/user && cp deploy/washable-ota.service ~/.config/systemd/user/
systemctl --user daemon-reload && systemctl --user enable --now washable-ota
sudo loginctl enable-linger $USER      # keep it running after logout / across reboots
journalctl --user -u washable-ota -f
```

### Cloudflare tunnel

Point the tunnel at **`http://localhost:<PORT>`** (plain `http://`, not `https://`: the server itself does not speak TLS, the tunnel does). Quick test: `cloudflared tunnel --url http://localhost:8787`.
For a named tunnel, add an ingress rule `service: http://localhost:<PORT>` for your hostname; for a token (dashboard-managed) tunnel set the public hostname's service to the same URL.
The server binds to loopback only, so the tunnel is the only way in.

Check what a kiosk will get (the size and content-length must match):

```bash
curl -s https://<tunnel-host>/ota/manifest.json
curl -sI https://<tunnel-host>/ota/firmware.bin | grep -i content-length
```

### Environment

| Variable | Meaning |
|---|---|
| `ADMIN_PASSWORD` | dashboard login (min 8 chars) |
| `JWT_SECRET` | signs session tokens (min 32 chars) |
| `ALLOWED_ORIGINS` | browser origins allowed to call `/api/*`: your Vercel URL, comma separated; `https://*.vercel.app` wildcards work. The kiosk endpoints do not need CORS |
| `PUBLIC_URL` | optional; the manifest URL shown in the dashboard (default: taken from the request Host) |
| `HOST`, `PORT` | default `127.0.0.1:8787` |
| `REPO_DIR` | git checkout the editor's working copy is seeded from (default: the parent of `ota-server/`) |
| `PIO_BIN`, `EMSDK_DIR` | PlatformIO CLI and emsdk checkout (default: `<DATA_DIR>/tools/pio-venv/bin/pio` and `<DATA_DIR>/tools/emsdk`) |
| `DATA_DIR` | default `./data` (`ota.db` + `firmware/`). Back this folder up |
| `EVENT_RETENTION_DAYS` | kiosk activity log retention, default 90 |

## Things to know

- **Kiosks never downgrade** (`ota.cpp` installs only a version greater than its own). To roll back a bad release, rebuild the old code with a *higher* version number and publish that. Publishing an older release only helps kiosks that are still below it.
- Kiosks install only when idle, at night (01:00-05:00 WIB), so a new release reaches them gradually, not instantly.
- Nodes are identified by the MAC the firmware reports. The server cannot authenticate a kiosk (there is no per-device secret yet), so a hostile client that knows a device ID could pose as that node or register fake ones (capped at 1000). It cannot read anything but the firmware that was assigned to that ID, and the firmware is not secret. Add a per-device token before exposing this beyond a trusted fleet.
- Old firmware (< 1.3.0) cannot be targeted: it sends no device ID. Flash 1.3.0 once over USB; after that every update is managed here.
- Firmware is **not signed**. SHA-256 protects against corruption, not against someone who controls this server or its admin password. Keep `.env` private; add ECDSA signing before large-scale field use (already listed as an open item in `esp32-kiosk/README.md`).
- Publishing a different release while a kiosk is mid-download makes that download fail the size check; the kiosk retries on its next check.

## Code, build and preview

The dashboard's **Code & build** tab edits a **private copy** of the firmware on this machine, never your git checkout.

- `<DATA_DIR>/workspace` is seeded once from `REPO_DIR` (`esp32-kiosk/`, `shared/`, `tools/sim/`); `<DATA_DIR>/workspace-base` keeps the pristine editable files so the editor can show a diff, revert a file, and export `GET /api/workspace/patch` (apply it in the repo with `git apply`). *Reset all* re-copies the repo.
- Only `.c .cpp .h .hpp` files under `esp32-kiosk/src`, `esp32-kiosk/include` and `shared/relay_proto` are readable/writable through the API (no `..`, no symlinks, 512 KB max). `platformio.ini` and library scripts are not editable because PlatformIO runs them during a build.
- **Build** runs `pio run -j 2` in the copy (one heavy job at a time, 4 MB log cap, cancelable). The first build downloads the ESP32 toolchain (about 1 GB). A successful build is validated like an upload and can be turned into a release with one click; set `FW_VERSION` in `src/version.h` first (it must be higher than what the kiosks run).
- **Kiosk preview**: `scripts/preview-build.sh` compiles the real `main.cpp` + LVGL with Emscripten (`tools/sim/web_main.cpp` is the harness, the same stubs as `tools/sim/screenshots.sh`) to a single `sim.js`. The dashboard runs it on a canvas and feeds mouse input in as touch. Payments and the relay controller are emulated.

Install the toolchains once (they live under `<DATA_DIR>/tools`, which is git-ignored):

```bash
mkdir -p data/tools && cd data/tools
python3 -m venv pio-venv && ./pio-venv/bin/pip install platformio
git clone --depth 1 https://github.com/emscripten-core/emsdk.git && ./emsdk/emsdk install latest && ./emsdk/emsdk activate latest
```

**Security:** whoever has the admin password can make this machine compile and serve arbitrary firmware, which every assigned kiosk will then install (unsigned). Use a long random `ADMIN_PASSWORD`, keep it out of chat and logs, and treat the dashboard login like SSH access to the fleet.
