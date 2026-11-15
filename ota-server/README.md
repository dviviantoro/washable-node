# ota-server

Self-hosted OTA backend for the kiosk. It replaces "GitHub Releases" as the place the ESP32 downloads firmware from, and gives the
[dashboard](../ota-dashboard/README.md) an admin API. **No firmware change is needed**: the kiosk already polls a manifest URL
over HTTPS (`esp32-kiosk/src/ota.cpp`), this server speaks exactly that protocol.

Node 22.13+ (uses the built-in `node:sqlite`), Fastify 5, SQLite, files on disk. No native modules.

## What the kiosk sees

| Request | Answer |
|---|---|
| `GET /ota/manifest.json` | `{"version","sha256","size"[,"min_version"]}` of the **published** release |
| `GET /ota/firmware.bin` | the published image, with an explicit `Content-Length` (the kiosk requires `size` to match the manifest) |

`firmware.bin` must sit in the same folder as `manifest.json` (the kiosk derives its URL from the manifest URL), which is why both live under `/ota/`.
Set the kiosk's **OTA URL** to `https://<tunnel-host>/ota/manifest.json`. It never changes between releases.

When nothing is published (or the rollout is paused) the manifest says `0.0.0`; kiosks read that as "up to date" instead of showing an error.

## Admin API (JWT, used by the dashboard)

| | |
|---|---|
| `POST /api/login` `{password}` | returns `{token}` (12 h). Rate limited to 8/min per IP |
| `GET /api/status` | published release, manifest URL, kiosks seen per version, 14-day activity |
| `GET /api/releases` | all releases |
| `POST /api/releases` | multipart: `version?`, `min_version?`, `notes?`, `activate?`, **`file`** (last) |
| `POST /api/releases/:id/activate` | publish that release |
| `POST /api/releases/deactivate` | pause the rollout |
| `DELETE /api/releases/:id` | delete a stored release (not the published one) |
| `GET /api/events?limit&kind` | kiosk checks and downloads |
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
npm test                    # 9 tests (image validation + the whole HTTP flow)
npm start                   # listens on http://127.0.0.1:8787
```

Run it as a service (user unit; Node path is pinned to the Node 22 binary):

```bash
mkdir -p ~/.config/systemd/user && cp deploy/washable-ota.service ~/.config/systemd/user/
systemctl --user daemon-reload && systemctl --user enable --now washable-ota
sudo loginctl enable-linger $USER      # keep it running after logout / across reboots
journalctl --user -u washable-ota -f
```

### Cloudflare tunnel

Point the tunnel at **`http://localhost:8787`**. Quick test: `cloudflared tunnel --url http://localhost:8787`.
For a named tunnel, add an ingress rule `service: http://localhost:8787` for your hostname.
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
| `DATA_DIR` | default `./data` (`ota.db` + `firmware/`). Back this folder up |
| `EVENT_RETENTION_DAYS` | kiosk activity log retention, default 90 |

## Things to know

- **Kiosks never downgrade** (`ota.cpp` installs only a version greater than its own). To roll back a bad release, rebuild the old code with a *higher* version number and publish that. Publishing an older release only helps kiosks that are still below it.
- Kiosks install only when idle, at night (01:00-05:00 WIB), so a new release reaches them gradually, not instantly.
- The kiosks do not identify themselves, so the dashboard counts distinct public IPs ("sources"). Kiosks behind one router count once.
- Firmware is **not signed**. SHA-256 protects against corruption, not against someone who controls this server or its admin password. Keep `.env` private; add ECDSA signing before large-scale field use (already listed as an open item in `esp32-kiosk/README.md`).
- Publishing a different release while a kiosk is mid-download makes that download fail the size check; the kiosk retries on its next check.
