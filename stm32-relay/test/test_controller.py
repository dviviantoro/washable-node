#!/usr/bin/env python3
"""Host-side test of the STM32 protocol core: python3 stm32-relay/test/test_controller.py"""
import json, pathlib, subprocess, sys

here = pathlib.Path(__file__).resolve().parent
root = here.parents[1]                     # repo root (stm32-relay/test -> stm32-relay -> repo)
binpath = here / "host_test_bin"
subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-I", str(root / "shared" / "relay_proto"),
                str(here / "host_main.cpp"), "-o", str(binpath)], check=True)

def crc16(b, crc=0xFFFF):
    for x in b:
        crc ^= x << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc
def frame(j): return "%s*%04X" % (j, crc16(j.encode()))
def parse(line):
    assert line.startswith("TX "), line
    f = line[3:].rstrip("\n")
    body, crc = f.rsplit("*", 1)
    assert int(crc, 16) == crc16(body.encode()), "reply CRC wrong: " + f
    return json.loads(body)

def run(steps):
    """steps: list of (ms, text|None, 'tick'|'line'). returns list of output lines"""
    inp = ""
    for ms, text in steps:
        inp += ("TICK %d\n" % ms) if text is None else ("T%d %s\n" % (ms, text))
    out = subprocess.run([str(binpath)], input=inp, capture_output=True, text=True, timeout=10).stdout.splitlines(True)
    return out
def tx(out): return [parse(l) for l in out if l.startswith("TX ")]
def rel(out): return [tuple(map(int, l.split()[1:])) for l in out if l.startswith("RELAY")]

fails = 0
def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ((" -> " + str(extra)) if (not cond and extra != "") else ""))
    if not cond: fails += 1

HELLO = frame('{"v":1,"id":1,"t":"cmd","op":"hello","fw":"1.1.0","machine":"03"}')
# known-good frames produced by an independent implementation (python) and listed in docs/relay-protocol.md
check("doc frame hello has CRC F787", HELLO.endswith("*F787"))
check("doc frame hb has CRC 9ADA", frame('{"v":1,"t":"hb","seq":42}').endswith("*9ADA"))
check("doc frame relay has CRC EA17", frame('{"v":1,"id":12,"t":"cmd","op":"relay","on":[1,2],"off":[3,4],"ttl":10}').endswith("*EA17"))

# 1. boot: all relays off (8 RELAY lines), nothing transmitted
out = run([(0, None)])
check("boot: 8 relays driven off, silent", len(rel(out)) == 8 and all(o == 0 for _, o in rel(out)) and not tx(out))

# 2. hello
out = run([(100, HELLO)]); r = tx(out)
check("hello -> ack with fw/relays", len(r) == 1 and r[0]["t"] == "ack" and r[0]["id"] == 1 and r[0]["fw"] == "0.1.0" and r[0]["relays"] == 8, r)

# 3. relay on with ttl, ttl expiry, evt
cmd = frame('{"v":1,"id":12,"t":"cmd","op":"relay","on":[1,2],"off":[3,4],"ttl":10}')
hbs = [(ms, frame('{"v":1,"t":"hb","seq":%d}' % (ms // 1000))) for ms in range(2000, 11000, 1000)]   # ESP32 sends 1 per second
out = run([(100, HELLO), (1000, cmd)] + hbs + [(10900, frame('{"v":1,"t":"hb","seq":10}')), (11100, None)])
t = tx(out)
acks = [x for x in t if x.get("id") == 12]
check("relay cmd ack mask=3", acks and acks[0]["ok"] == 1 and acks[0]["mask"] == 3, t)
on_calls = [(c, o) for c, o in rel(out)[8:] if o == 1]
check("relays 1 and 2 switched on", (1, 1) in on_calls and (2, 1) in on_calls, rel(out))
evts = [x for x in t if x.get("t") == "evt" and x.get("e") == "relay"]
check("ttl expiry switches both off + evt why=ttl", len(evts) == 2 and all(e["why"] == "ttl" for e in evts), t)
check("hb pong carries mask", [x for x in t if x.get("t") == "hb"][0]["mask"] == 3)
check("no failsafe while heartbeats keep coming", not [x for x in t if x.get("e") == "failsafe"], t)

# 4. heartbeat loss -> failsafe; evt after the link returns
out = run([(100, HELLO), (1000, frame('{"v":1,"id":2,"t":"cmd","op":"relay","on":[5],"ttl":100}')),
           (2000, frame('{"v":1,"t":"hb","seq":1}')), (5500, None), (8000, frame('{"v":1,"t":"hb","seq":2}'))])
t = tx(out)
check("no heartbeat for 3 s -> relay 5 switched off", (5, 0) in rel(out)[9:], rel(out))
fs = [x for x in t if x.get("e") == "failsafe"]
check("failsafe event sent after the link returns", len(fs) == 1 and fs[0]["mask"] == 0, t)

# 5. errors
def errs(payload):
    t = tx(run([(100, HELLO), (200, frame(payload))]))
    return t[-1]
check("channel 9 -> nack bad_ch", errs('{"v":1,"id":5,"t":"cmd","op":"relay","on":[9],"ttl":5}').get("err") == "bad_ch")
check("missing ttl -> nack ttl", errs('{"v":1,"id":6,"t":"cmd","op":"relay","on":[1]}').get("err") == "ttl")
check("ttl too long -> nack ttl", errs('{"v":1,"id":7,"t":"cmd","op":"relay","on":[1],"ttl":99999}').get("err") == "ttl")
check("same channel in on+off -> nack conflict", errs('{"v":1,"id":8,"t":"cmd","op":"relay","on":[1],"off":[1],"ttl":5}').get("err") == "conflict")
check("unknown op -> nack op", errs('{"v":1,"id":9,"t":"cmd","op":"fly"}').get("err") == "op")
check("relay without args -> nack args", errs('{"v":1,"id":10,"t":"cmd","op":"relay"}').get("err") == "args")
r = errs('{"v":1,"id":11,"t":"cmd","op":"relay","on":[1],"ttl":5}')
check("nack echoes id on errors", errs('{"v":1,"id":8,"t":"cmd","op":"relay","on":[1],"off":[1],"ttl":5}').get("id") == 8)

# 6. alloff
out = run([(100, HELLO), (200, frame('{"v":1,"id":3,"t":"cmd","op":"relay","on":[1,2,3],"ttl":50}')), (300, frame('{"v":1,"id":4,"t":"cmd","op":"alloff"}'))])
t = tx(out)
check("alloff -> mask 0 and relays off", t[-1]["mask"] == 0 and (1, 0) in rel(out)[8:], t)

# 7. silence / garbage handling
out = run([(100, "LED: ready"), (150, "ets Jul 29 2019 12:21:46"), (200, '{"v":1,"t":"hb","seq":1}*0000')])
check("silent before the first valid frame (log lines and bad CRC)", not tx(out), tx(out))
out = run([(100, HELLO), (200, '{"v":1,"t":"hb","seq":1}*0000')])
check("bad CRC after link -> nack crc", tx(out)[-1].get("err") == "crc")
out = run([(100, HELLO), (300, "garbage {{{ not a frame")])
check("non-frame text ignored", len(tx(out)) == 1)

# 8. does not speak for a long-gone ESP32 (flash/USB time)
out = run([(100, HELLO), (200, frame('{"v":1,"id":2,"t":"cmd","op":"relay","on":[1],"ttl":30}')), (40000, None)])
t = tx(out)
check("no spontaneous events once the ESP32 has been silent > 10 s", not [x for x in t if x.get("t") == "evt"], t)
check("...but the relay is still switched off (failsafe)", (1, 0) in rel(out)[9:], rel(out))

binpath.unlink()
print("\n%s" % ("ALL PASSED" if not fails else "%d FAILED" % fails))
sys.exit(1 if fails else 0)
