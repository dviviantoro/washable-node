# washable - STM32 relay controller

Firmware for a **WeAct Studio STM32G030F6P6** core board (Cortex-M0+, 32 KB flash, 8 KB RAM) that switches 8 relays for the washable kiosk:
channels 1-4 wash machine, 5-8 dryer. It talks JSON lines to the kiosk ESP32 over USART2 and enforces the safety rules itself.
Part of the [washable monorepo](../README.md); protocol: [../docs/relay-protocol.md](../docs/relay-protocol.md).

## Wiring

| STM32 | ESP32-3248S035 |
|---|---|
| PA2 (USART2 TX) | GPIO3 (RX0) |
| PA3 (USART2 RX) | GPIO1 (TX0) |
| GND | GND |
| PA0, PA1, PA4, PA5, PA6, PA7, PB0, PB1 | relay channels 1-8 (relay board inputs) |

115200 8N1, 3.3 V. **Check the relay pins against your board's pinout before connecting loads**; they are one array (`RELAY_PIN`) in `src/main.cpp`.
Relays are active-high by default (`RELAY_ACTIVE_HIGH`). Use an opto-isolated relay board, fuses, and a hardware emergency stop that does not depend on software.
ESP32 GPIO1/3 are also its USB console: disconnect PA2 (or use a 1 k series resistor) while you upload firmware to the ESP32.

## Build and flash

```bash
pio run                 # build (24 KB flash, 1.5 KB RAM)
pio run -t upload       # flash with an ST-Link over SWD (see upload_protocol in platformio.ini for other probes)
```

`boards/weact_g030f6p6.json` defines the board for PlatformIO (the framework already has the generic G030F6P variant, where `Serial` is USART2 on PA2/PA3).

## Safety rules (implemented in `src/ctrl_core.h`)

1. All relays off at power-up and after every reset; independent watchdog enabled.
2. No valid frame for 3 s -> all relays off.
3. Every "on" needs a time limit (`ttl`, 1..7200 s) and switches off by itself.
4. Silent until it has heard the ESP32 (so flashing the ESP32 over the shared USB lines is not disturbed).

## Tests

`python3 test/test_controller.py` (from this folder, or `./tests/run_host_tests.sh` from the repo root) compiles `src/ctrl_core.h` for the host and checks the handshake, safety timer, heartbeat loss, error replies and the silence rules.
`../tools/stm32_sim.py` does the opposite job: it pretends to be this controller so the ESP32 can be tested without the board.

## Layout

```
platformio.ini      env for the WeAct G030F6P6 (STM32duino)
boards/             PlatformIO board definition
src/ctrl_core.h     protocol + safety logic (hardware independent, host-tested)
src/main.cpp        USART2, relay pins, watchdog, main loop
test/               host test driver + test script
../shared/relay_proto/relay_proto.h   framing / CRC / parsing shared with the ESP32 firmware
```
