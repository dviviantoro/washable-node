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
ap.add_argument("--key", default="", help="send this debug key to a DEBUG_KEYS build after 8 s, e.g. H (health page)")
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
st = {"on": False, "n": 0, "sent": 0, "ok": 0, "next": 0.0, "last": 0.0, "at": [], "rtt": []}   # STM32 -> ESP32 self-test state
rx_ok = 0; tx_n = 0
def fmt_mask(m): return "".join("1" if m & (1 << i) else "0" for i in range(8))[::-1]

print("simulating the STM32 on %s - Ctrl-C to stop" % a.port)
key_sent = not a.key
while True:
    try:
        buf += ser.read(256)
        if not key_sent and now() > 8: ser.write(("#%s\n" % a.key).encode()); key_sent = True
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
            rx_ok += 1
            if msg.get("t") == "ping":                                       # health check ESP32 -> STM32
                send({"v": 1, "t": "pong", "n": msg.get("n", 0)})
            elif msg.get("t") == "pong" and st["on"]:                         # answer to our self-test ping
                n = msg.get("n", 0)
                if 1 <= n <= st["sent"] and st["at"][n - 1] is not None:
                    st["ok"] += 1; st["rtt"].append((now() - st["at"][n - 1]) * 1000); st["at"][n - 1] = None
            elif msg.get("t") == "hb":
                send({"v": 1, "t": "hb", "seq": msg.get("seq", 0), "mask": mask, "up": up})
            elif msg.get("t") == "cmd":
                i, op = msg.get("id", 0), msg.get("op")
                if op == "hello":
                    print("hello from ESP32:", msg); send({"v": 1, "t": "ack", "id": i, "ok": 1, "mask": mask, "up": up, "fw": "sim-0.1", "relays": 8})
                elif op == "health": send({"v": 1, "t": "ack", "id": i, "ok": 1, "mask": mask, "up": up, "fw": "sim-0.1", "rst": "power", "rx": rx_ok, "tx": sent, "bad": 0, "vdd": 3300, "temp": 31})
                elif op == "selftest":
                    n = msg.get("n", 10)
                    if not 1 <= n <= 20: send({"v": 1, "t": "nack", "id": i, "err": "args"})
                    else:
                        st.update(on=True, n=n, sent=0, ok=0, next=now() + 0.1, last=now(), at=[None] * n, rtt=[])
                        send({"v": 1, "t": "ack", "id": i, "ok": 1, "mask": mask, "up": up})
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
        if st["on"]:                                           # self-test: ping the ESP32 n times, then report
            if st["sent"] < st["n"] and now() >= st["next"]:
                st["at"][st["sent"]] = now(); st["sent"] += 1; st["last"] = now(); st["next"] = now() + 0.1
                send({"v": 1, "t": "ping", "n": st["sent"]})
            elif st["sent"] >= st["n"] and (st["ok"] >= st["n"] or now() - st["last"] > 0.8):
                avg = int(sum(st["rtt"]) / len(st["rtt"])) if st["rtt"] else 0
                print("self-test: %d/%d answered by the ESP32" % (st["ok"], st["n"]))
                send({"v": 1, "t": "evt", "e": "selftest", "n": st["n"], "ok": st["ok"], "rtt": avg, "max": int(max(st["rtt"])) if st["rtt"] else 0})
                st["on"] = False
        for c, t in list(off_at.items()):                      # safety timers
            if now() >= t:
                mask &= ~(1 << (c - 1)); del off_at[c]; print("relay %d off (ttl)" % c)
                send({"v": 1, "t": "evt", "e": "relay", "ch": c, "on": 0, "why": "ttl", "mask": mask})
        if mask and last_rx and now() - last_rx > 3:           # heartbeat lost -> fail safe
            mask = 0; off_at.clear(); fs_pending = True; print("FAILSAFE: no heartbeat, all relays off")
    except KeyboardInterrupt:
        break
