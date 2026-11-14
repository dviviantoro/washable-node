#!/usr/bin/env bash
# Host tests (no hardware needed): run from anywhere.
#   1. STM32 protocol core:  stm32-relay/test/test_controller.py
#   2. ESP32 link code <-> STM32 core, in memory, with fault injection:  tests/integration/link_test.cpp
set -euo pipefail
cd "$(dirname "$0")/.."
python3 stm32-relay/test/test_controller.py | tail -3
bin="$(mktemp -t link_test.XXXXXX)"
trap 'rm -f "$bin"' EXIT
c++ -std=c++14 -Wall -Wextra \
    -I tests/integration -I shared/relay_proto -I stm32-relay/src -I esp32-kiosk/src \
    tests/integration/link_test.cpp esp32-kiosk/src/relay_link.cpp -include vector -o "$bin"
"$bin" | tail -4
