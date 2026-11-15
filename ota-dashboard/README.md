# ota-dashboard

Admin UI for [`ota-server`](../ota-server/README.md). Next.js 15 (static page, everything runs in the browser), deploys to Vercel as is.

| Tab | What it does |
|---|---|
| Overview | fleet counts (online, need attention, updates pending), problems at a glance, the kiosk OTA URL |
| Nodes | every kiosk that reported in: status, firmware, activity, Wi-Fi, uptime, last seen; click one for health details and its timeline; tick nodes and **send a release to only those** (there is no "all kiosks" button) |
| Releases | stored firmware images, download/delete, manual `.bin` upload |
| Code & build | Monaco editor on a private copy of the firmware source, diff against the repo, **PlatformIO build** with a live log, one click to add the result to Releases |
| Kiosk preview | the real kiosk UI compiled to WebAssembly from your edited code, clickable in the browser (screens, language, theme, fast-forward a cycle) |
| Activity | fleet timeline (reboots, crashes, firmware changes, downloads) and raw requests |

The browser talks **directly** to the backend (through the Cloudflare tunnel), so firmware uploads never pass through a Vercel function
(Vercel limits request bodies to 4.5 MB). Login is the backend's admin password; the session token is kept in `localStorage`.

## Vercel

Import the repo, set **Root Directory** to `ota-dashboard`, framework Next.js (auto-detected), and add one environment variable:

| Name | Value |
|---|---|
| `NEXT_PUBLIC_API_URL` | public https URL of the Cloudflare tunnel, no trailing slash, e.g. `https://ota.example.com` |

Then add the Vercel URL to `ALLOWED_ORIGINS` in `ota-server/.env` and restart the backend (`systemctl --user restart washable-ota`).
`NEXT_PUBLIC_*` variables are inlined at build time: redeploy after changing it.

## Local development

```bash
cd ota-dashboard
npm install
NEXT_PUBLIC_API_URL=http://localhost:8787 npm run dev      # http://localhost:3000, allowed by the backend's default ALLOWED_ORIGINS
```

## Uploading a release

Either build it in the **Code & build** tab (set `FW_VERSION` in `version.h`, **Build firmware**, **Add to releases**), or upload a file in **Releases**:
`python3 tools/release.py 1.3.0` (or `pio run -d esp32-kiosk`) writes `dist/firmware.bin`; the browser checks it (size, ESP32 header, checksum, embedded OTA client and version) and the server checks again.
Then open **Nodes**, tick the kiosks that should get it, pick the release and **Send to selected**. Each of them installs it on its own during the night window (01:00-05:00 WIB) when idle, and the node row turns to "installed" when its heartbeat reports the new version.

Nodes need firmware **1.3.0 or newer** (device ID + heartbeat). Flash it once over USB; older kiosks are not listed and cannot be targeted.
