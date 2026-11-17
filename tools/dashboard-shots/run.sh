#!/usr/bin/env bash
# Regenerates the screenshots of every OTA dashboard page, and of every kiosk page through the dashboard's Kiosk preview.
#   tools/dashboard-shots/run.sh [out-dir]        (default: docs/img/dashboard)
#
# It boots a THROWAWAY ota-server (port 8099) and a production build of the dashboard (port 3300) on made-up demo data, so nothing of the real
# fleet is shown and the real server is not touched. Output: <out>/{desktop-light,desktop-dark,mobile-light,mobile-dark}/NN-page.png and
# <out>/kiosk/{id,en}-{dark,light}/NN-page.png (+ a contact sheet of each).
#
# Needs: node >= 22.13, the ota-server toolchains (PlatformIO and emsdk, see ota-server/README.md), and a Chromium (CHROME_PATH, or the one
# Playwright installed under ~/.cache/ms-playwright; install one with `npx playwright-core install chromium`).
# Env: SHOTS_BUILD=0 skips the live PlatformIO build of the Code tab (about 6 minutes on a cold cache); KEEP=1 keeps the temp dir;
#      OPTIMIZE=0 keeps the PNGs full-colour; PIO_BIN / EMSDK_DIR override the toolchain locations; API_PORT / DASH_PORT change the ports.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE=$ROOT/tools/dashboard-shots
FINAL=${1:-$ROOT/docs/img/dashboard}
API_PORT=${API_PORT:-8099}
DASH_PORT=${DASH_PORT:-3300}
PW=demo-password-123
TMP=$(mktemp -d)
DATA=$TMP/data
STAGE=$TMP/out
mkdir -p "$DATA" "$STAGE"

for port in "$API_PORT" "$DASH_PORT"; do
  if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then echo "port $port is already in use"; exit 1; fi
done

cleanup() {
  # `next build` rewrites ota-dashboard/tsconfig.json (formatting, and it adds the extra dist folder): put the original back
  [ -f "$TMP/tsconfig.orig" ] && cp "$TMP/tsconfig.orig" "$ROOT/ota-dashboard/tsconfig.json"
  for f in backend dash; do
    [ -f "$TMP/$f.pid" ] && kill -- "-$(cat "$TMP/$f.pid")" 2>/dev/null || true
  done
  [ "${KEEP:-0}" = 1 ] && echo "kept $TMP" || rm -rf "$TMP"
}
trap cleanup EXIT

echo "== dependencies"
[ -d "$HERE/node_modules" ] || (cd "$HERE" && npm install --no-audit --no-fund --silent)
[ -d "$ROOT/ota-server/node_modules" ] || (cd "$ROOT/ota-server" && npm install --no-audit --no-fund --silent)
[ -d "$ROOT/ota-dashboard/node_modules" ] || (cd "$ROOT/ota-dashboard" && npm install --no-audit --no-fund --silent)
(cd "$ROOT/ota-server" && npm run build --silent)

cp "$ROOT/ota-dashboard/tsconfig.json" "$TMP/tsconfig.orig"
echo "== dashboard (production build against http://localhost:$API_PORT)"
(cd "$ROOT/ota-dashboard" && NEXT_PUBLIC_API_URL="http://localhost:$API_PORT" NEXT_DIST_DIR=.next-shots npx next build >"$TMP/next-build.log" 2>&1) || { tail -30 "$TMP/next-build.log"; exit 1; }

echo "== demo server"
# reuse the real working copy (it has the PlatformIO cache, so the live build is only a compile) and the compiled preview, when they exist
for d in workspace workspace-base preview; do
  [ -d "$ROOT/ota-server/data/$d" ] && cp -r "$ROOT/ota-server/data/$d" "$DATA/$d"
done
rm -rf "$DATA/preview/next" "$DATA/preview/current.hidden"
export PIO_BIN=${PIO_BIN:-$ROOT/ota-server/data/tools/pio-venv/bin/pio}
export EMSDK_DIR=${EMSDK_DIR:-$ROOT/ota-server/data/tools/emsdk}

start_backend() {
  # exec keeps the PID: backend.pid must be the PID of node itself (setsid makes it a process-group leader, so kill -PID stops it and its children)
  ( cd "$TMP" && exec env NODE_ENV=production HOST=127.0.0.1 PORT="$API_PORT" ADMIN_PASSWORD="$PW" JWT_SECRET="demo-secret-demo-secret-demo-secret-0123456789" \
      ALLOWED_ORIGINS="http://localhost:$DASH_PORT" PUBLIC_URL="https://ota.example.com" DATA_DIR="$DATA" REPO_DIR="$ROOT" PIO_BIN="$PIO_BIN" EMSDK_DIR="$EMSDK_DIR" \
      setsid node "$ROOT/ota-server/dist/main.js" ) >>"$TMP/backend.log" 2>&1 &
  echo $! >"$TMP/backend.pid"
  for _ in $(seq 1 50); do curl -fs "http://localhost:$API_PORT/healthz" >/dev/null 2>&1 && return 0; sleep 0.3; done
  echo "backend did not start"; tail -20 "$TMP/backend.log"; exit 1
}
stop_backend() {
  kill -- "-$(cat "$TMP/backend.pid")" 2>/dev/null || true
  for _ in $(seq 1 50); do curl -fs "http://localhost:$API_PORT/healthz" >/dev/null 2>&1 || return 0; sleep 0.2; done
  echo "backend did not stop"; exit 1
}
start_backend

( cd "$ROOT/ota-dashboard" && exec env NEXT_PUBLIC_API_URL="http://localhost:$API_PORT" NEXT_DIST_DIR=.next-shots setsid node node_modules/next/dist/bin/next start -p "$DASH_PORT" ) >"$TMP/dash.log" 2>&1 &
echo $! >"$TMP/dash.pid"
for _ in $(seq 1 60); do curl -fs "http://localhost:$DASH_PORT" >/dev/null 2>&1 && break; sleep 0.5; done

echo "== demo data"
node "$HERE/seed.mjs" "http://localhost:$API_PORT" "$PW" "$DATA"

TOKEN=$(curl -fs -X POST "http://localhost:$API_PORT/api/login" -H 'content-type: application/json' -d "{\"password\":\"$PW\"}" | python3 -c 'import sys,json;print(json.load(sys.stdin)["token"])')
wait_job() {   # wait_job <id>: poll until the job leaves "running"
  local st
  for _ in $(seq 1 400); do
    st=$(curl -fs "http://localhost:$API_PORT/api/jobs/$1" -H "authorization: Bearer $TOKEN" | python3 -c 'import sys,json;print(json.load(sys.stdin)["job"]["status"])')
    [ "$st" != running ] && { echo "$st"; return; }
    sleep 3
  done
  echo timeout
}
if [ ! -f "$DATA/preview/current/sim.js" ]; then
  echo "== compiling the kiosk preview (first time only)"
  id=$(curl -fs -X POST "http://localhost:$API_PORT/api/preview/build" -H "authorization: Bearer $TOKEN" | python3 -c 'import sys,json;print(json.load(sys.stdin)["id"])')
  [ "$(wait_job "$id")" = ok ] || { echo "preview build failed (see the dashboard's Kiosk preview tab)"; exit 1; }
fi

export DASH_URL="http://localhost:$DASH_PORT" API_URL="http://localhost:$API_PORT" PASSWORD="$PW" OUT="$STAGE" DATA_DIR="$DATA"

node "$HERE/capture.mjs" prepare

if [ "${SHOTS_BUILD:-1}" != 0 ]; then
  echo "== live firmware build (Code tab)"
  node "$HERE/capture.mjs" build
fi

echo "== snapshot of the demo database"
stop_backend
cp "$DATA/ota.db" "$TMP/snapshot.db"

for setup in desktop-light desktop-dark mobile-light mobile-dark; do
  echo "== $setup"
  stop_backend
  cp "$TMP/snapshot.db" "$DATA/ota.db"; rm -f "$DATA/ota.db-wal" "$DATA/ota.db-shm"
  start_backend
  node "$HERE/capture.mjs" pass "$setup"
done

echo "== kiosk pages"
node "$HERE/capture.mjs" kiosk

echo "== contact sheets"
python3 "$HERE/sheets.py" "$STAGE" $([ "${OPTIMIZE:-1}" != 0 ] && echo --optimize)
python3 "$HERE/gallery.py" "$STAGE"

rm -rf "$STAGE/.tmp" "$STAGE/.qr"
rm -rf "$FINAL"; mkdir -p "$(dirname "$FINAL")"; mv "$STAGE" "$FINAL"
echo "done: $FINAL"
find "$FINAL" -name '*.png' | wc -l | xargs echo "PNG files:"
du -sh "$FINAL"
