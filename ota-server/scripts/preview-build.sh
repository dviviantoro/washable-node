#!/usr/bin/env bash
# Compile the kiosk UI (workspace copy of esp32-kiosk + LVGL) to WebAssembly for the dashboard's interactive preview.
# usage: preview-build.sh <workspace> <out-dir> <cache-dir> <repo tools/sim dir> <pio> <emsdk dir>
# Result: <out-dir>/sim.js (ES module with the .wasm embedded). LVGL objects are cached, so later runs only rebuild the kiosk code.
set -euo pipefail
WS=$1; OUT=$2; CACHE=$3; SIMSRC=$4; PIO=$5; EMSDK=$6
JOBS=${PREVIEW_JOBS:-3}

# shellcheck disable=SC1091
source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
command -v emcc >/dev/null || { echo "emcc not found in $EMSDK"; exit 1; }

echo "== harness: refreshing tools/sim from the repo"
mkdir -p "$WS/tools/sim"
cp -r "$SIMSRC"/. "$WS/tools/sim/"

PROJ="$WS/esp32-kiosk"
LVGL_SPEC=$(grep -o 'lvgl/lvgl@[^ ]*' "$PROJ/platformio.ini" | head -1)
LVGL=$(ls -d "$PROJ"/.pio/libdeps/*/lvgl 2>/dev/null | head -1 || true)
if [ -z "$LVGL" ]; then
  echo "== fetching ${LVGL_SPEC:-lvgl/lvgl} with PlatformIO"
  "$PIO" pkg install -d "$PROJ" -e esp32-3248s035 -l "${LVGL_SPEC:-lvgl/lvgl@~8.3.11}"
  LVGL=$(ls -d "$PROJ"/.pio/libdeps/*/lvgl | head -1)
fi
LVGL_VER=$(grep -o '"version": *"[^"]*"' "$LVGL/library.json" | head -1 | grep -o '[0-9][^"]*')
OBJ="$CACHE/lvgl-$LVGL_VER"
mkdir -p "$OBJ" "$OUT"
echo "== LVGL $LVGL_VER, object cache $OBJ"

FLAGS="-O2 -w -DLV_CONF_SKIP=1 -DLV_COLOR_DEPTH=16 -DLV_USE_QRCODE=1 -DLV_COLOR_16_SWAP=1 -DLV_MEM_CUSTOM=1 -DLV_INV_BUF_SIZE=128 \
 -DLV_FONT_MONTSERRAT_16=1 -DLV_FONT_MONTSERRAT_20=1 -DLV_FONT_MONTSERRAT_28=1 -DLV_FONT_MONTSERRAT_48=1 -DDEBUG_KEYS=0 \
 -I $WS/tools/sim/stubs -I $LVGL -I $LVGL/src -I $WS/shared/relay_proto -I $WS/esp32-kiosk/src"

compile_c() {   # compile one C file into the cache (skipped when cached)
  local f=$1 o
  o="$OBJ/$(echo "$f" | sed "s#^$LVGL/##; s#^$PROJ/##" | tr '/' '_').o"
  [ -f "$o" ] || { emcc $FLAGS -c "$f" -o "$o.tmp" && mv "$o.tmp" "$o"; }
}
export -f compile_c; export FLAGS OBJ LVGL PROJ

echo "== compiling LVGL and fonts (cached after the first run)"
{ find "$LVGL/src" -name '*.c'; ls "$PROJ"/src/fonts/*.c; } | xargs -P "$JOBS" -I{} bash -c 'compile_c {}'

echo "== compiling the kiosk UI"
em++ -std=c++17 $FLAGS -c "$WS/tools/sim/web_main.cpp" -o "$OUT/web_main.o"

echo "== linking"
em++ -O2 "$OUT/web_main.o" "$OBJ"/*.o -o "$OUT/sim.js" \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sSINGLE_FILE=1 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864 -sSTACK_SIZE=1048576 --no-entry \
  -sEXPORTED_FUNCTIONS=_sim_init,_sim_frame,_sim_pixels,_sim_touch,_sim_screen,_sim_relays,_sim_scene_count,_sim_scene_name,_sim_fw_version,_sim_skip,_sim_scene \
  -sEXPORTED_RUNTIME_METHODS=UTF8ToString,HEAPU8,HEAPU32
rm -f "$OUT/web_main.o"
ls -l "$OUT/sim.js"
echo "== done"
