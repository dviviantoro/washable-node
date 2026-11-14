#!/usr/bin/env bash
# Render every kiosk screen on the PC (real UI code + LVGL, no hardware) into docs/img/screens/.
# Needs: a C/C++ compiler, python3, and the PlatformIO library cache (run `pio run` once in esp32-kiosk/).
set -euo pipefail
cd "$(dirname "$0")/../.."
LVGL=$(ls -d esp32-kiosk/.pio/libdeps/*/lvgl | head -1)
OUT=$(mktemp -d)
SIM=tools/sim
FLAGS="-O1 -w -DLV_CONF_SKIP=1 -DLV_COLOR_DEPTH=16 -DLV_USE_QRCODE=1 -DLV_COLOR_16_SWAP=1 -DLV_MEM_CUSTOM=1 -DLV_INV_BUF_SIZE=128 \
 -DLV_FONT_MONTSERRAT_16=1 -DLV_FONT_MONTSERRAT_20=1 -DLV_FONT_MONTSERRAT_28=1 -DLV_FONT_MONTSERRAT_48=1 \
 -I $SIM/stubs -I $LVGL -I $LVGL/src -I shared/relay_proto -I esp32-kiosk/src"
mkdir -p "$OUT/obj"
for f in $(find "$LVGL/src" -name '*.c') esp32-kiosk/src/fonts/*.c; do
  o="$OUT/obj/$(echo "$f" | tr '/' '_').o"; [ -f "$o" ] || cc $FLAGS -c "$f" -o "$o" &
  while [ "$(jobs -r | wc -l)" -ge 8 ]; do sleep 0.05; done
done; wait
c++ -std=c++17 $FLAGS "$SIM/sim_main.cpp" "$OUT"/obj/*.o -o "$OUT/sim"
mkdir -p "$OUT/raw"; "$OUT/sim" "$OUT/raw"
python3 -I "$SIM/rgb2png.py" "$OUT/raw" docs/img/screens
rm -rf "$OUT"
