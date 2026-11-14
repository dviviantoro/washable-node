# Relay link protocol (ESP32 <-> STM32)

The kiosk (ESP32) drives the relay controller (STM32G030F6P6, 8 relays: channels 1-4 wash, 5-8 dry) over a serial link
with one JSON object per line. The ESP32 decides *what* to switch; the STM32 enforces the safety rules.

## Wiring

| ESP32-3248S035 | STM32G030F6P6 (WeAct) |
|---|---|
| GPIO1 (TX0) | PA3 (USART2 RX) |
| GPIO3 (RX0) | PA2 (USART2 TX) |
| GND | GND |

115200 8N1, 3.3 V logic on both sides. GPIO1/GPIO3 are also the USB console of the ESP32, so:

- The ESP32's normal log text goes down the same wire. The controller ignores every line that is not a `{...}*CRC` frame.
- ESP32 serial debug keys are typed as lines starting with `#` (e.g. `#h` + Enter) so they cannot be confused with frames.
- **Flashing the ESP32 over USB while the STM32 is connected:** the controller stays silent until it has received a valid frame, but its TX (PA2) is still wired to the CH340 path.
  Disconnect PA2 (or put a 1 k series resistor in the STM32 TX line) while uploading firmware to the ESP32 to be safe. The ESP32 reset makes the controller fail safe (all relays off) after 3 s.

## Framing

```
{json}*CCCC\n
```

`CCCC` = CRC-16/CCITT-FALSE (polynomial 0x1021, initial value 0xFFFF, no reflection, no final XOR) of the json bytes, 4 upper-case hex digits.
Maximum line length 255 bytes. A line that is not a valid frame is ignored (the controller answers `nack` with `err:"crc"` only if it has heard the ESP32 recently).
Every message has `"v":1`. Keys are matched by name, so unknown extra keys are ignored (forward compatible).

## ESP32 -> STM32

| Message | Meaning |
|---|---|
| `{"v":1,"t":"hb","seq":42}` | Heartbeat, every 1 s. The controller answers with its own `hb`. |
| `{"v":1,"id":1,"t":"cmd","op":"hello","fw":"1.1.0","machine":"03"}` | Handshake; repeated every 2 s until acknowledged. |
| `{"v":1,"id":12,"t":"cmd","op":"relay","on":[1,2],"off":[3,4],"ttl":10}` | Switch channels. `on` needs `ttl` (1..7200 s): the relay switches itself off after that time unless refreshed with a new command. `on` and `off` may not share a channel. Commands set states, they do not toggle, so a retry is harmless. |
| `{"v":1,"id":13,"t":"cmd","op":"alloff"}` | All relays off. |
| `{"v":1,"id":14,"t":"cmd","op":"status"}` | Ask for the current state. |

Commands carry an `id`; the ESP32 waits 300 ms for the reply and retries up to 3 times.

## STM32 -> ESP32

| Message | Meaning |
|---|---|
| `{"v":1,"t":"hb","seq":42,"mask":3,"up":120}` | Reply to every heartbeat: relay bitmask (bit0 = channel 1) and controller uptime in seconds. If `up` goes backwards the controller rebooted. |
| `{"v":1,"t":"ack","id":1,"ok":1,"mask":0,"up":5,"fw":"0.1.0","relays":8}` | Command done (`fw` and `relays` only in the `hello` reply). |
| `{"v":1,"t":"nack","id":13,"err":"bad_ch"}` | Command refused. `err`: `crc`, `bad_ch`, `ttl`, `conflict`, `args`, `op`, `id`, `type`. |
| `{"v":1,"t":"evt","e":"relay","ch":1,"on":0,"why":"ttl","mask":0}` | A relay changed by itself (safety timer). |
| `{"v":1,"t":"evt","e":"failsafe","why":"hb_timeout","mask":0}` | The heartbeat was lost, all relays were switched off (sent when the ESP32 is heard again). |

## Example frames (checksums verified by `stm32-relay/test/test_controller.py`)

```
{"v":1,"id":1,"t":"cmd","op":"hello","fw":"1.1.0","machine":"03"}*F787
{"v":1,"id":12,"t":"cmd","op":"relay","on":[1,2],"off":[3,4],"ttl":10}*EA17
{"v":1,"t":"hb","seq":42}*9ADA
{"v":1,"t":"ack","id":1,"ok":1,"fw":"0.1.0","relays":8,"wash":[1,2,3,4],"dry":[5,6,7,8]}*AB6B   (documentation sample of a richer ack)
{"v":1,"t":"nack","id":13,"err":"bad_ch"}*356C
```

## Controller safety rules

1. All relays are off at power-up and after any reset (independent watchdog included).
2. No valid frame for **3 s** -> all relays off (`failsafe`).
3. Every `on` has a mandatory time limit (`ttl`).
4. The controller only transmits as a reply, or within 10 s after it last heard the ESP32 (so it stays quiet while the ESP32 is flashed).
5. Reserved for later: interlocks (e.g. inlet and drain never together, heater only with fan), per-group maximum on-times, input events (door, water level, emergency stop). The relay roles and phase sequences are decided by the ESP32 and will be added later.

## ESP32 behaviour

- `RelayLink` (`esp32-kiosk/src/relay_link.cpp`) sends the heartbeat and the hello handshake, retries commands and tracks `up` (a valid frame within 3 s), `helloOk`, the controller firmware, the relay mask and the controller uptime.
- Settings -> **Tes relay** switches each of the 8 channels by hand (20 s safety timer, refreshed while the page is open; everything goes off when you leave the page). The Info card shows the controller status.

## Testing

- `python3 stm32-relay/test/test_controller.py` - STM32 protocol core on the host (handshake, ttl, failsafe, error cases, silence rules).
- `tests/run_host_tests.sh` - runs the controller test above and `tests/integration/link_test.cpp`: the real ESP32 link code against the real controller core, with lost frames, a silent link, a frozen ESP32 and a controller reboot.
- `tools/stm32_sim.py` - pretends to be the STM32 on a serial port (e.g. over the ESP32 USB cable) to test the ESP32 firmware before the real board is wired.
- Build the controller firmware: `cd stm32-relay && pio run` (upload with an ST-Link: `pio run -t upload`).
