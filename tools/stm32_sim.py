#!/usr/bin/env python3
"""Pretends to be the STM32 relay controller on a serial port (to test the ESP32 before the real board is wired).

  python3 tools/stm32_sim.py /dev/cu.usbserial-XXXX [--silent-after 20] [--corrupt 5] [--logs]

The ESP32's UART0 (GPIO1/3) is also the USB console, so the simulator can use the USB cable. Needs pyserial
(it is in the PlatformIO python: ~/.platformio/penv/bin/python tools/stm32_sim.py ...).
  --silent-after S   stop answering after S seconds (the ESP32 should report "link down")
  --corrupt N        damage every Nth reply (the ESP32 must ignore it and carry on)
  --logs             also print the ESP32's normal log lines
Protocol: docs/relay-protocol.md
"""
import argparse, json, re, sys, time
import serial

def crc16(b, crc=0xFFFF):
    for x in b:
        crc ^= x << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc

ap = argparse.ArgumentParser()
ap.add_argument("port"); ap.add_argument("--baud", type=int, default=115200)
ap.add_argument("--silent-after", type=float, default=0); ap.add_argument("--corrupt", type=int, default=0)
ap.add_argument("--logs", action="store_true")
a = ap.parse_args()

ser = serial.Serial(a.port, a.baud, timeout=0.05)
t0 = time.time(); mask = 0; off_at = {}; last_rx = 0.0; sent = 0; buf = b""; fs_pending = False

def now(): return time.time() - t0
def send(obj):
    global sent
    body = json.dumps(obj, separators=(",", ":"))
    crc = crc16(body.encode())
    sent += 1
    if a.corrupt and sent % a.corrupt == 0: crc ^= 0x00FF
    ser.write(("%s*%04X\n" % (body, crc)).encode())
def fmt_mask(m): return "".join("1" if m & (1 << i) else "0" for i in range(8))[::-1]

print("simulating the STM32 on %s - Ctrl-C to stop" % a.port)
while True:
    try:
        buf += ser.read(256)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.strip().decode(errors="replace")
            if a.silent_after and now() > a.silent_after:
                continue
            if not line.startswith("{"):
                if a.logs and line: print("  esp:", line)
                continue
            m = re.match(r"^(\{.*\})\*([0-9A-Fa-f]{4})$", line)
            if not m or crc16(m.group(1).encode()) != int(m.group(2), 16):
                print("  bad frame from ESP32:", line[:80]); continue
            msg = json.loads(m.group(1)); last_rx = now()
            if fs_pending:
                fs_pending = False
                send({"v": 1, "t": "evt", "e": "failsafe", "why": "hb_timeout", "mask": mask})
            up = int(now())
            if msg.get("t") == "hb":
                send({"v": 1, "t": "hb", "seq": msg.get("seq", 0), "mask": mask, "up": up})
            elif msg.get("t") == "cmd":
                i, op = msg.get("id", 0), msg.get("op")
                if op == "hello":
                    print("hello from ESP32:", msg); send({"v": 1, "t": "ack", "id": i, "ok": 1, "mask": mask, "up": up, "fw": "sim-0.1", "relays": 8})
                elif op in ("status",): send({"v": 1, "t": "ack", "id": i, "ok": 1, "mask": mask, "up": up})
                elif op == "alloff": mask = 0; print("relays:", fmt_mask(mask)); send({"v": 1, "t": "ack", "id": i, "ok": 1, "mask": mask, "up": up})
                elif op == "relay":
                    on, offl, ttl = msg.get("on", []), msg.get("off", []), msg.get("ttl", 0)
                    if any(not 1 <= c <= 8 for c in on + offl): send({"v": 1, "t": "nack", "id": i, "err": "bad_ch"})
                    elif on and not 1 <= ttl <= 7200: send({"v": 1, "t": "nack", "id": i, "err": "ttl"})
                    else:
                        for c in offl: mask &= ~(1 << (c - 1)); off_at.pop(c, None)
                        for c in on: mask |= 1 << (c - 1); off_at[c] = now() + ttl
                        print("relays:", fmt_mask(mask), "(ch8..ch1)"); send({"v": 1, "t": "ack", "id": i, "ok": 1, "mask": mask, "up": up})
                else: send({"v": 1, "t": "nack", "id": i, "err": "op"})
        for c, t in list(off_at.items()):                      # safety timers
            if now() >= t:
                mask &= ~(1 << (c - 1)); del off_at[c]; print("relay %d off (ttl)" % c)
                send({"v": 1, "t": "evt", "e": "relay", "ch": c, "on": 0, "why": "ttl", "mask": mask})
        if mask and last_rx and now() - last_rx > 3:           # heartbeat lost -> fail safe
            mask = 0; off_at.clear(); fs_pending = True; print("FAILSAFE: no heartbeat, all relays off")
    except KeyboardInterrupt:
        break
