# ota-dashboard

Admin UI for [`ota-server`](../ota-server/README.md): upload firmware, publish or pause a rollout, see which versions the kiosks run and
what they requested. Next.js 15 (static page, everything runs in the browser), deploys to Vercel as is.

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

1. Bump and build: `python3 tools/release.py 1.2.0` (or `pio run -d esp32-kiosk`); the file is `dist/firmware.bin`.
2. Drop it on the page. The browser checks it first (size, ESP32 header, checksum, embedded OTA client and version); the server checks again.
3. **Upload and publish**. Kiosks pick it up on their next check and install during the night window.
