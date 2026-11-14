# Production readiness review

Review of the ESP32 kiosk and the STM32 relay controller, what was fixed, and what is still open before putting machines in the field.
Status: **fixed** = in the code and built/tested here; **open** = needs a decision, hardware or an external service.

## Safety model (relays and money)

| Layer | Rule | Where | Status |
|---|---|---|---|
| STM32 | All relays off at power-up and after every reset (pins are driven before anything else) | `stm32-relay/src/main.cpp` | fixed (already) |
| STM32 | Every `on` needs a `ttl` (max 2 h) and switches itself off | `ctrl_core.h` | fixed (already), host-tested |
| STM32 | No valid frame for 3 s -> all relays off (failsafe) | `ctrl_core.h` | fixed (already), host-tested |
| STM32 | Independent watchdog (2 s): a hung MCU resets and relays drop | `main.cpp` | fixed (already) |
| STM32 | Reports why it restarted (`rst`: power / pin / soft / iwdg / wwdg) in the hello reply | `ctrl_core.h`, `relay_link.cpp` | **fixed now**, logged by the kiosk |
| ESP32 | A cycle keeps the relays alive itself: re-asserts the wanted channels every 5 s with a 20 s `ttl` | `main.cpp` (`jobsTick`) | **fixed now** |
| ESP32 | Cycle time only counts while the controller confirms the relays are on (link lost / controller reset = cycle pauses, resumes when the link is back) | `main.cpp` | **fixed now** |
| ESP32 | "Bayar QRIS" is disabled ("Mesin belum siap") while the controller does not answer, so the kiosk never takes money for a machine that cannot start (demo mode is exempt) | `main.cpp` | **fixed now** |
| ESP32 | Task watchdog (20 s) on the main loop: a hung UI restarts the kiosk; the STM32 failsafe then drops the relays within 3 s | `main.cpp` | **fixed now** |
| ESP32 | A running cycle survives a power cut or restart: remaining time is stored in NVS once a minute and resumed at boot (the customer gets at most 60 s extra) | `main.cpp` (`jobSave/jobsRestore`) | **fixed now** |
| ESP32 | No OTA install while a cycle runs (even when forced); relays off before an update | `main.cpp` | fixed (already) |

## Security and robustness

| Topic | Finding | Status |
|---|---|---|
| Serial debug keys | `#x` lines on UART0 could jump screens, toggle relays or bypass the OTA gate; any noise or device on that UART could trigger them | **fixed**: compiled out (`DEBUG_KEYS=0`); enable only locally with `-DDEBUG_KEYS=1` |
| Settings | NVS plus checksummed flash mirror, verified before each OTA | fixed (already) |
| OTA | HTTPS with CA bundle, SHA-256, size check, install window and gate, rollback guard | fixed (already) |
| OTA signing | Firmware is **not signed**: a compromised GitHub account could push firmware to every kiosk | **open** (add ECDSA P-256 signature check; keep the private key outside GitHub) |
| Web form | HTTP Basic over plain HTTP, lockout after 5 tries, only while an admin page is open | acceptable on a trusted LAN; **open**: HTTPS if the network is shared |
| Admin PIN | Default `123456` (the settings page warns) | **open**: force a change on first use |
| Flash protection | Secrets (API key, Wi-Fi password) are stored unencrypted in NVS | **open**: ESP32 flash encryption + secure boot v2 for machines in public places (irreversible eFuses: decide per fleet) |

## Open items that need your decision or external parts

1. **Dynamic QRIS and payment confirmation.** The QR is a fixed sample payload and "paid" is a demo button. Production needs a payment API: create a QRIS per order, confirm by webhook or polling, idempotent order ids, refund/timeout handling, and a log of every cycle. Until then the system must not be used to take real money.
2. **Wash/dry phase tables.** `WASH_PHASES` / `DRY_PHASES` in `main.cpp` are a placeholder (main motor only: CH2 for wash, CH6 for dry). Replace them with the real sequence (inlet, motor, drain, soap, heater, fan) once agreed; the engine, keep-alive, pause and resume already handle any table. Add machine interlocks (door switch, water level, over-temperature) as inputs; the STM32 has free pins for them.
3. **Hardware protection.** Opto-isolated relay board, fuses per load, a hardware emergency stop that cuts the loads independent of software, flyback/snubber on motor and valve coils, pull-down resistors (10 k) on the STM32 relay pins so relays stay off while the MCU resets, a TVS and polyfuse on the 5 V rail, and a conformal-coated enclosure. A mains-rated enclosure and electrical certification are your responsibility.
4. **Real time and logging.** Cycles are not logged. Add a persistent cycle log (start, program, price, result) and report it to the backend; NTP time is already available for timestamps.
5. **Fleet monitoring.** Report heartbeat, firmware versions, controller resets (`rst`) and free heap to the server so failures are noticed remotely.
6. **Field soak test.** Run 72 h with demo mode off and a dummy load, pull the power during a cycle, unplug the controller during a cycle, and confirm: relays off within 3 s, cycle resumes with the right time, no stuck "paused" state.

## How the cycle/relay logic behaves now

```
cycle starts -> wanted relay mask from the phase table (by % of the cycle)
  every 100 ms: if the controller does not confirm the wanted channels -> the cycle clock stands still
  every 5 s   : "relay on=[..] off=[the rest] ttl=20"  (1 s while the controller disagrees)
  every 60 s  : remaining time saved to NVS
  cycle ends  : "alloff" at once, green LED flash, "selesai" until the clothes are collected
power loss    : STM32 failsafe (<= 3 s) drops the relays; at boot the ESP32 resumes the saved cycle and re-asserts the relays
```

## Added in the second review round

- **Health check** (both directions, `docs/relay-protocol.md`): ESP32 -> STM32 ping, STM32 -> ESP32 self-test, controller counters. Visible on the kiosk (Settings > Kesehatan STM32); host-tested and verified on the board with the STM32 simulator. Not done yet: automatic periodic health checks reported to a server, and real MCU voltage/temperature (`vdd`/`temp` are 0 until the STM32 firmware reads VREFINT/TEMPSENSOR).
- **Layout linter** in the PC renderer (text on cards, overflow, truncation); 5 overlaps fixed (member question, phone, code, QRIS hint, coupon notes).
- **Scrolling lists** (Info perangkat, Settings, Wi-Fi) are now hit-testable, so a swipe that starts on a card scrolls (it only worked when the finger started on a button).
- **UI frame rate** about 2x (see the ESP32 README).
