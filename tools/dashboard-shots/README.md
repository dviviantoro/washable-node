# dashboard-shots

Regenerates the screenshots in [`docs/img/dashboard/`](../../docs/img/dashboard/README.md): **every page of the OTA dashboard** (desktop and phone, light and dark theme) and **every kiosk page**, rendered through the dashboard's own Kiosk preview (Bahasa and English, dark and light kiosk theme).

```bash
tools/dashboard-shots/run.sh            # about 20 minutes, 6 of them the live PlatformIO build
SHOTS_BUILD=0 tools/dashboard-shots/run.sh /tmp/shots   # skip the build, write somewhere else
```

It boots a **throwaway** `ota-server` (port 8099, its own temp data folder) and a production build of the dashboard (port 3300) on made-up demo data (`seed.mjs`: 8 kiosks in every state, 2 releases, a few days of activity), drives a real Chromium with Playwright (`capture.mjs`) and stops everything afterwards. The real server and fleet are never touched, and the only hostname in the images is `ota.example.com`.

| File | Job |
|---|---|
| `run.sh` | orchestrates: builds, starts the servers, seeds, runs the capture phases, post-processes, moves the result into place |
| `seed.mjs` | demo fleet, releases and activity (nodes are written straight into the temp database so timestamps can be in the past) |
| `capture.mjs` | `build` (live firmware build from the Code tab, shot while running and when done), `pass <setup>` (every dashboard page), `kiosk` (every kiosk page) |
| `sheets.py` | contact sheets of the kiosk pages and PNG palette optimisation (about 3x smaller) |
| `gallery.py` | writes `docs/img/dashboard/README.md`, the visual index |
| `pages.json` | what each page must show (also used as the spec for reviewing the screenshots) |

Needs: Node 22.13+, the `ota-server` toolchains (PlatformIO and emsdk, see `ota-server/README.md`), Python 3 with Pillow, and a Chromium (`CHROME_PATH`, or the one Playwright installs: `npx playwright-core install chromium`).
`KEEP=1` keeps the temp folder (logs, database), `OPTIMIZE=0` keeps the PNGs full-colour, `PIO_BIN` / `EMSDK_DIR` / `API_PORT` / `DASH_PORT` override defaults.

## Leak check

`capture.mjs` fails the run if a page shows loopback addresses, `/home/` paths, an e-mail domain, or this machine's login or host name. Add your own real names (tunnel domain, etc.) with `LEAK_WORDS="my-domain.com,my-name" tools/dashboard-shots/run.sh`.
