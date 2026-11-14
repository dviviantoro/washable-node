#!/usr/bin/env bash
# usage: screenshots.sh [test]   (default: render screenshots; "test": run the headless cycle/relay checks instead)
# Render every kiosk screen on the PC (real UI code + LVGL, no hardware) into docs/img/screens/.
# Needs: a C/C++ compiler, python3, and the PlatformIO library cache (run `pio run` once in esp32-kiosk/).
set -euo pipefail
cd "$(dirname "$0")/../.."
LVGL=$(ls -d esp32-kiosk/.pio/libdeps/*/lvgl | head -1)
OUT=$(mktemp -d)
SIM=tools/sim
FLAGS="${SIM_OPT:--O1} -w -DLV_CONF_SKIP=1 -DLV_COLOR_DEPTH=16 -DLV_USE_QRCODE=1 -DLV_COLOR_16_SWAP=1 -DLV_MEM_CUSTOM=1 -DLV_INV_BUF_SIZE=128 \
 -DLV_FONT_MONTSERRAT_16=1 -DLV_FONT_MONTSERRAT_20=1 -DLV_FONT_MONTSERRAT_28=1 -DLV_FONT_MONTSERRAT_48=1 -DDEBUG_KEYS=0 \
 -I $SIM/stubs -I $LVGL -I $LVGL/src -I shared/relay_proto -I esp32-kiosk/src"
mkdir -p "$OUT/obj"
for f in $(find "$LVGL/src" -name '*.c') esp32-kiosk/src/fonts/*.c; do
  o="$OUT/obj/$(echo "$f" | tr '/' '_').o"; [ -f "$o" ] || cc $FLAGS -c "$f" -o "$o" &
  while [ "$(jobs -r | wc -l)" -ge 8 ]; do sleep 0.05; done
done; wait
c++ -std=c++17 $FLAGS "$SIM/sim_main.cpp" "$OUT"/obj/*.o -o "$OUT/sim"
if [ "${1:-}" = "build" ]; then cp "$OUT/sim" "$2"; rm -rf "$OUT"; exit 0; fi
if [ "${1:-}" = "bench" ]; then "$OUT/sim" --bench "${2:-2}"; rc=$?; rm -rf "$OUT"; exit $rc; fi
if [ "${1:-}" = "test" ]; then "$OUT/sim" --test; rc=$?; rm -rf "$OUT"; exit $rc; fi
mkdir -p "$OUT/raw" "$OUT/raw-en" "$OUT/raw-lt"; "$OUT/sim" "$OUT/raw"; SIM_LANG=1 "$OUT/sim" "$OUT/raw-en" | sed 's/^LINT /LINT[EN] /'; SIM_THEME=1 "$OUT/sim" "$OUT/raw-lt" | sed 's/^LINT /LINT[LIGHT] /'
python3 -I "$SIM/rgb2png.py" "$OUT/raw" docs/img/screens >/dev/null; python3 -I "$SIM/rgb2png.py" "$OUT/raw-en" docs/img/screens-en; mkdir -p docs/img/screens-light; python3 -I "$SIM/rgb2png.py" "$OUT/raw-lt" docs/img/screens-light >/dev/null
[ -n "${SIM_KEEP:-}" ] && cp "$OUT/sim" "$SIM_KEEP"; rm -rf "$OUT"
