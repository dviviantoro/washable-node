# Plan: bringing the kiosk in line with the Nirmala IoT developer guide (v1.0, 19 Nov 2026)

Source: `nirmala-iot-dev-guide.md` from the full-stack team (the PDF is the same document; its four diagrams were not reviewed).
This plan compares the guide with what this repo does today (kiosk firmware `esp32-kiosk/`, relay controller `stm32-relay/`, OTA server and dashboard `ota-server/` + `ota-dashboard/`) and says what to do and in which order.

## 0. Update 2026-11-20: the real OpenAPI spec (supersedes parts of the guide)

The full-stack team published `https://washable-dashboard.vercel.app/api/docs/openapi.json` (OpenAPI 3.0.3, "Washable Kiosk / IoT API", version 2026-11-20, server `https://backend-nirmalah.web.id`, prefix `/machine/v1`). It is newer than the guide and **differs from it in ways that change our work**. Where they disagree, treat the spec as current until the team says otherwise.

| Topic | Guide (v1.0) | OpenAPI spec (2026-11-20) | Consequence for us |
|---|---|---|---|
| Identity | `X-Machine-Id` (one UUID per machine) | `X-Outlet-Id` + `X-Set-Code` + `X-Role` (`washer`/`dryer`); `X-Machine-Id` only as "legacy fallback" | The kiosk must hold an outlet id and a set code, not one machine UUID. |
| Machines per kiosk | one | **1 kiosk = 1 set = 1 washer + 1 dryer**, each with its **own `device_secret`**; heartbeat and polling run **per role** ("two loops") | Our kiosk drives both. Firmware needs two identities, two secrets, two heartbeats and two command loops on one ESP32 (twice the HTTPS traffic and state). Biggest new item. |
| Transport | WSS main + long-poll fallback | **Long-poll only** (`GET /commands?wait=20&after=c_NNN`), plus HTTP heartbeat; no WebSocket anywhere | Decision 3 is settled in favour of long-poll: no WebSocket library, saves flash and one TLS session. Heap risk drops to polling + one HTTPS call. |
| Heartbeat | 20-30 s | ~20 s, and the **response carries `poll_interval_ms`, `heartbeat_interval_s`, `commands_pending`, `config_changed`, `firmware_update`** | Intervals come from the server; do not hard-code them. |
| Config | `GET /config` | Same, ETag = `"<config_version>"`, 304 supported; programs have `relay_profile`, `temp_c`, `active`; machine has `kind`, `role`, `set_code`, `capacity_kg` | Matches phase 3. Our UI must handle a washer program list and a dryer program list. |
| Payment | create, poll, cancel | Same, plus `list_price` vs `amount`, statuses `pending/paid/expired/cancelled/...`, `command_id` set once paid, errors `MACHINE_BUSY`, `MACHINE_RESERVED`, `PROGRAM_NOT_AVAILABLE`, `GATEWAY_UNAVAILABLE` | Each error needs its own screen text. Door check is **not** in the API: it stays purely local. |
| Drop-off orders | not in the guide | `POST /pin/start` (6-digit PIN + order code, errors `PIN_INVALID/PIN_EXPIRED/PIN_LOCKED`) | **New kiosk feature**: a PIN entry screen. Not in our UI today (our mock "member/OTP" screens are a different thing). |
| ACK | started/rejected/done | `started`, `rejected`, `done` (started -> done allowed), idempotent; response `refund`: `none/points_refunded/payment_refund_pending`; 404/409/410 errors | Matches. A `rejected` ACK triggers refunds, so a wrong reject (for example, door open) costs money. |
| Refund | open question | "automatic QRIS refund if the gateway supports it, otherwise manual" | Decision 6 is partly answered; the screen wording must still not promise a refund. |
| Events | long list incl. `cycle_*`, `door_*`, `fault*`, `boot` | enum: `cycle_started, cycle_done, cycle_aborted, door_opened, door_closed, fault, fault_cleared, power_restored, button_pressed, config_applied, secret_rotated, boot`; batch max 50, idempotent per `event_id` (UUID) | **No `cycle_resumed`**. Our resume-after-power-cut needs `power_restored` + a `cycle_started` with `data.resumed=true`, or the team adds a type. Each event needs a UUID `event_id` generated on the device. |
| Commands | START/STOP/LOCK/UNLOCK/REBOOT/ROTATE_SECRET/... | only `START` is shown (payload `program_id, minutes, source`); the type field is a free string | Ask which others exist before building handlers for them. |
| Firmware | `GET /firmware/latest?current=` | `GET /firmware/latest` -> **"currently always `update=false`"**; heartbeat has a `firmware_update` flag | Nothing to integrate yet. Our own OTA server stays the working path; do not retire it (changes decision 7 and phase 5). |
| Nonce / clock | not specified | nonce 16-32 lowercase hex, new for every request **including retries**, unique for 5 min; timestamp +-60 s; on `TIMESTAMP_SKEW` use `server_time` from the error body | Needs a hardware RNG nonce. Sync the clock from the error body, then retry once. |
| Rate limits | table in the guide | none in the spec | The guide's table (see §3.2) is unconfirmed; ask. |

New problems found in the spec:

1. **Two secrets per kiosk.** `device_secret` is hex bytes (`bytes.fromhex`); our `apiKey[64]` holds one secret and does not even fit a 64-hex secret (65 bytes). We need two 64-hex fields, entered once, never shown again.
2. **The spec has no health/offline contract.** Nothing says when a washer or a dryer counts as offline, and the two roles can disagree (the washer heartbeat works, the dryer does not). Ask the team how the dashboard treats a half-online kiosk.
3. **Clock is critical.** A +-60 s window and a nonce that must change on every retry mean the kiosk must have valid time before its first call. NTP failure should show a clear error, not silent 401s.
4. **Two long-polls hold two TLS connections** (20 s waits, one per role), plus any payment/config request. Measure heap on a real board; consider polling one role at a time.
5. **No fixture for the test vectors in the new header set.** The message format is unchanged (`METHOD\nPATH_WITH_QUERY\nts\nnonce\nsha256hex(body)`), so the guide's two vectors still apply to the signer. Add one with the identity headers once the team supplies it.
6. **The docs host and the API host differ** (`washable-dashboard.vercel.app` vs `backend-nirmalah.web.id`). I could not test a live call: signing needs a real `device_secret`, and we have none. Ask for a staging outlet, set code and two secrets.

## 1. Where we really are

The kiosk today is a **polished UI prototype with solid machine-safety logic, but no backend client at all**.

| Area | Today | Guide expects |
|---|---|---|
| Payment | Fixed (fake) QRIS payload; "paid" is the button "Demo: anggap sudah dibayar" | Backend creates a QRIS per order (`POST /payments`), kiosk shows `qr_string`, server pushes `START` after the webhook |
| Start after payment | Manual button "Mulai sekarang" | Automatic on `START` (PRD US-2) |
| Prices, programs, minutes | Hard-coded in `main.cpp` | From `GET /config` (incl. `relay_profile`) |
| Wash/dry sequence | Placeholder phase tables (`WASH_PHASES`/`DRY_PHASES`) | Real tables, selected by `relay_profile` |
| Identity / auth | `machineId[8]` ("03"), `apiKey[64]`, no signing. OTA heartbeat sends a spoofable device id | `machine_id` (UUID), 256-bit device secret, HMAC-SHA256 on every request |
| Transport | None to a Machine API. OTA: HTTPS poll of `manifest.json` every 6 h, heartbeat every 5 min to our OTA server | WSS main channel, HTTP long-poll fallback, heartbeat 20-30 s |
| Door check | Text only ("pastikan pintu tertutup"); no sensor input | Door sensor checked **before** QRIS is created; `DOOR_OPEN` ACK; `door_opened/closed` events |
| Events | Cycles are not logged | `cycle_started/done/aborted`, `fault`, `boot` ... persisted in flash, sent in order with `seq` |
| States | idle/running + OTA/health screens | `idle`, `awaiting_payment`, `running`, `error`, `locked`; `LOCK/UNLOCK/STOP/REBOOT/ROTATE_SECRET` |
| Member / OTP / coupons | Mock screens in the firmware (fixed code 123456) | **Not in v1 kiosk** (guide §11, Open Question 7) |
| Screen | ST7796, **320x480 portrait** | Guide assumes **768x450** (Open Question 12, listed as an assumption) |
| OTA | `manifest.json`, HTTPS + CA bundle, SHA-256, install window 01:00-05:00, idle gate, rollback guard, per-node targeting; **unsigned** | `GET /firmware/latest?current=`, idle only, SHA-256 before switching slot, rollback; signing / staged rollout / hosting are "TBD" |

Already aligned (keep): relay failsafe (STM32 drops relays after 3 s), relay `ttl` keep-alive, cycle resume after power loss, task watchdog, NTP time, OTA only when idle, SHA-256 + rollback, "Bayar QRIS" disabled while the relay controller does not answer, `DEBUG_KEYS` compiled out.

## 2. What is wrong or risky (our side)

1. **Flash is almost full.** The last build uses **89.6 % of the app partition** (1,761,569 of 1,966,080 bytes; board has 4 MB flash, `min_spiffs` gives two 1.9 MB app slots). A WebSocket client, HMAC, JSON handling and a persistent event queue will not fit without trimming. Cheapest saving: compile the mock member/OTP/coupon screens out (the guide says they are not v1 anyway). Measure before adding any library.
2. **Config fields are too small for the new identity.** `machineId[8]` cannot hold a UUID (36), `ownerId[32]` cannot hold an `outlet_id` UUID (36), and `apiKey[64]` cannot hold the device secret: 64 hex characters need 65 bytes with the terminator. The settings page and the PC web form need new fields and validation (secret entered once, never shown again).
3. **Power-cut behaviour contradicts the guide's proposal.** We already **resume** a cycle after a power cut (remaining time stored in NVS). The guide proposes **cancel + `cycle_aborted`** (Open Question 8) and its draft screens assume resume. A customer who paid and loses power should not lose the cycle: keep resume, add a `cycle_resumed` event, and ask the backend team to accept it.
4. **"Last 20 command ids in RAM" is not enough.** After a reboot an un-ACKed `START` can be delivered again and run twice. Persist the executed command ids (NVS) with the cycle state.
5. **Two fleet systems will overlap.** Our kiosks send a 5-minute heartbeat to our OTA server; the guide has a 20 s heartbeat to the Machine API. Decide who owns fleet health, OTA targeting and device identity (see §4, decision 7) before the kiosk learns a second protocol.
6. **OTA identity is weak.** Device id headers can be spoofed; the guide's per-machine secret fixes this, so OTA should reuse it. Firmware is still unsigned (already listed in `docs/production-readiness.md`); the guide also lists signing as TBD, so nobody owns it yet.
7. **TLS trust differs.** Guide: pin the root CA (not the leaf). Ours: the full Mozilla CA bundle. Pinning one root needs a second root as backup (rotation), otherwise a CA change bricks the fleet.
8. **Memory risk with two TLS sessions.** WSS stays open while payments/config use separate HTTPS requests, and OTA adds a third. Our `ota.cpp` already tracks the largest free heap block; check it on a real board with WSS + one HTTPS call at the same time before committing to the WebSocket design.
9. **Door sensor and interlocks do not exist yet** (hardware item in `production-readiness.md`). The guide's flow depends on it, so this is a hardware task, not only firmware.
10. **Admin PIN default `123456`, secrets unencrypted in NVS.** The device secret becomes the key to all money flows, so flash encryption / secure boot (irreversible eFuses) must be decided before field units.

## 3. What is wrong or inconsistent in the guide itself

(Their own Open Questions already cover items 1-6; the rest I found.)

1. **Offline threshold flaps.** Open Question 5 picks "offline after 30 s" while the WebSocket `hb` is sent every 30 s (assumption). Machines would flap offline. Use a threshold of at least 2-3 heartbeat intervals (e.g. heartbeat 20 s, offline at 60 s), or derive offline from the WebSocket connection state.
2. **Rate-limit table does not fit the flow.** `payments` is limited to 10/min but the kiosk polls `GET /payments/{id}` every 2 s (30/min). Whether that is a separate bucket is unconfirmed. Also `commands` is 4/min while continuous long-poll with `wait=20` already uses 3/min.
3. **Screen size.** The guide's FE assumes 768x450 landscape; our hardware is 320x480 portrait. If the FE team designs for the wrong panel, all UI work is wasted.
4. **Refund wording.** Kiosk screen text must not promise automatic refunds (QRIS refund support in the gateways is unconfirmed, Open Question 4); also a customer can open the door **after** the pre-check while paying, so `DOOR_OPEN` rejections will happen and need an owner workflow.
5. **Client_ref in RAM only.** If the kiosk reboots during QR display, the pending payment is orphaned (reserved for up to `payment_timeout_s` = 300 s). The server should expire it, and the kiosk should call `GET /payments` or cancel on boot.
6. **Two transports in phase 1.** WSS plus long-poll fallback plus HTTP heartbeat mode is three paths to build and test on a small MCU. Option: ship long-poll + HTTP heartbeat first (simpler, no persistent TLS session), add WSS later. Needs the team's decision (TPRD says WS first; PRD still says polling).
7. **Test vectors are correct.** Both vectors (body hash and HMAC) were recomputed and match, so they can be used as-is in a host unit test.

## 4. Decisions needed from the team (before coding)

1. Screen/hardware: keep ST7796 320x480 portrait, or a 768x450 panel? (changes the FE work and the firmware UI)
2. Brand rename scope: do `washable.v1`, the `WB-` reference prefix, domain and kiosk logo change with "Nirmala"? Freeze these strings before firmware ships.
3. ~~Transport for phase 1~~ settled by the spec: long-poll + HTTP heartbeat, no WebSocket (confirm with the team, since the guide still says WSS).
3b. Two roles per kiosk: confirm that one kiosk really signs for both a washer and a dryer with separate secrets, and which command types exist beyond `START`.
4. Power cut mid-cycle: resume (current behaviour, recommended) or abort + compensate?
5. Offline threshold and heartbeat interval (see §3.1).
6. Kiosk-paid failure: who refunds and what does the screen say? Gateway choice (DOKU / iPaymu / Xendit) blocks the real QRIS format and expiry.
7. OTA ownership: either fold per-machine targeting into the Machine API (`GET /firmware/latest` returning the machine's assigned version, our dashboard stays an internal build/upload tool) or keep our OTA server as a separate service the backend points to. Recommended: first option, one identity and one heartbeat.
8. Firmware signing (ECDSA) and flash encryption / secure boot: decide per fleet.
9. Member/OTP/coupon/extend-time screens: out of v1 (guide) or in?

## 5. Work plan

Sizes are rough relative sizes (S = days, M = about a week, L = several weeks), not estimates.

**Phase 0: decisions and cleanup (S).** Settle §4 items 1-5 and 9. Free flash: put the mock member screens behind a compile flag; remove "Mulai sekarang" and the demo buttons from release builds (`-DKIOSK_DEMO=0`); record the flash budget in CI.

**Phase 1: Machine API client foundations (M).**
- Settings (per the 2026-11-20 spec): `outlet_id`, `set_code`, **two** 64-hex device secrets (washer, dryer), API base URL; fix field sizes; secrets shown once. Signing takes a role and picks its secret.
- Signing module (SHA-256 body hash, HMAC, nonce, timestamp) with a **host unit test using the two guide vectors** (`tests/`).
- Clock: NTP at boot plus `X-Server-Time` sync, one retry on `TIMESTAMP_SKEW`.
- Persistent event queue (ring buffer in flash, monotonically increasing `seq`, delete only after 200) and persistent executed-command ids.
- Backoff 1-30 s with jitter, shared by every call.

**Phase 2: a Machine API simulator (M).** Extend the Node/Fastify stack here with a mock of `/config`, `/payments`, `/commands`, `/commands/{id}/ack`, `/events`, `/heartbeat`, with HMAC verification, fake QRIS, a "mark as paid" button and fault injection (expired QR, door open, 5xx, clock skew). Lets firmware and the FE team test before the SaaS backend exists, and doubles as the acceptance harness for §12 of the guide.

**Phase 3: kiosk flows (L).**
- Programs, prices and minutes from `/config` (ETag, reload only on Splash); `relay_profile` selects the phase table; replace placeholder tables with real sequences.
- Door sensor input (STM32 pin + new relay-protocol event) and the "close the door" screen before `POST /payments`.
- Payment: `client_ref` UUID, `qr_string` shown (quiet zone, EC level M, no logo), countdown from `expires_at`, 2 s poll (4 s on 429), cancel, expired/failed screens with a short `WB-xxxxxx` reference.
- Command handling: `START` validation (dedupe, expiry, idle, door), ACK, auto-start, `STOP/LOCK/UNLOCK/REFRESH_CONFIG/REBOOT/ROTATE_SECRET` (atomic flash write before ACK).
- States and screens: `awaiting_payment`, `error`, `locked`, offline screen (>5 min), disabled screen.
- Events: `boot`, `power_restored`, `cycle_*`, `door_*`, `fault*`, `config_applied`.

**Phase 4: transport and offline behaviour (M-L).** Long-poll + HTTP heartbeat for both roles (the spec has no WebSocket); test heap with the real TLS sessions; rate-limit compliance; Wi-Fi unplug tests at the three points listed in the guide; resume-after-power-cut event.

**Phase 5: OTA and fleet alignment (M).** The Machine API's `firmware/latest` always answers `update=false` today, so keep our OTA server as the working path and revisit when the team implements it (decision 7). Then: reuse the device secret to sign OTA requests; add ECDSA signature verification; force an admin PIN change on first use; decide flash encryption; retire the duplicate 5-minute heartbeat.

**Phase 6: acceptance (M).** Run guide §12 on real hardware (including the 72 h soak, power-pull and controller-unplug tests from `production-readiness.md`) and the 300-machine polling load test against the simulator.

## 6. Dependencies outside our control

Payment gateway choice (real QRIS format, expiry, refund), final API domain and root CA (the spec lists `backend-nirmalah.web.id`), a staging outlet with set code and two device secrets for testing, door sensor and interlock hardware, real wash/dry sequences from the machine builder, and the PRD/TPRD v0.2 documents (referenced by the guide but not in this repo). The OpenAPI file is now available online (§0) and is the newest source.
