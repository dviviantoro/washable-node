#!/usr/bin/env python3
"""Generates docs/img/stm32-board-pinout.svg and docs/img/wiring.svg (schematic drawings, stdlib only).
Run from the repo root: python3 tools/diagrams/gen_diagrams.py"""
import pathlib
OUT = pathlib.Path(__file__).resolve().parents[2] / "docs" / "img"
OUT.mkdir(parents=True, exist_ok=True)

BG, INK, MUTE = "#0F2B50", "#EAF4FF", "#8FA6C4"
BLUE, ORANGE, TEAL, MINT, RED, GREY, PUR = "#3FA9F5", "#FF9F43", "#00B4C5", "#4ADE9A", "#FF6B6B", "#5C7090", "#B07CFF"
ST = f"font-family:Helvetica,Arial,sans-serif;fill:{INK}"

def head(w, h):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}">'
            f'<rect width="{w}" height="{h}" rx="14" fill="{BG}"/>'
            f'<style>text{{{ST};font-size:13px}} .t{{font-size:20px;font-weight:700}} .s{{font-size:11px;fill:{MUTE}}} .b{{font-weight:700}} .m{{font-family:Menlo,Consolas,monospace}}</style>')

def txt(x, y, s, cls="", anchor="start", fill=None):
    s = s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    f = f' style="fill:{fill}"' if fill else ""
    return f'<text x="{x}" y="{y}" class="{cls}" text-anchor="{anchor}"{f}>{s}</text>'

def box(x, y, w, h, fill, stroke=None, r=10):
    st = f' stroke="{stroke}" stroke-width="2"' if stroke else ""
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{fill}"{st}/>'

def line(x1, y1, x2, y2, col, w=3, dash=""):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    return f'<path d="M{x1} {y1} L{x2} {y2}" stroke="{col}" stroke-width="{w}" fill="none" stroke-linecap="round"{d}/>'

def wire(pts, col, w=3):
    return f'<path d="M{" L".join(f"{x} {y}" for x, y in pts)}" stroke="{col}" stroke-width="{w}" fill="none" stroke-linejoin="round" stroke-linecap="round"/>'

# ------------------------------------------------------------------ STM32 board pinout
def stm32():
    W, H = 760, 680
    s = [head(W, H)]
    s.append(txt(W / 2, 34, "WeAct STM32G030 Core Board - pins used by washable", "t", "middle"))
    s.append(txt(W / 2, 54, "schematic, top view (USB-less board, powered from 5V or 3V3). Check the silkscreen of your board before wiring.", "s", "middle"))
    bx, by, bw, bh = 290, 150, 180, 470
    s.append(box(bx, by, bw, bh, "#0B1A33", "#2B73B0", 14))
    # SWD header on top
    s.append(box(bx + 20, by - 38, bw - 40, 36, "#16315C", "#3F6AA8", 6))
    for i, n in enumerate(["GND", "CLK", "DIO", "3V3"]):
        x = bx + 38 + i * 35
        s.append(f'<circle cx="{x}" cy="{by - 20}" r="5" fill="#E8C468"/>')
        s.append(txt(x, by - 44, n, "s", "middle"))
    s.append(txt(bx + bw / 2, by - 62, "SWD (ST-Link): PA13 / PA14", "s", "middle", PUR))
    # chip + buttons
    s.append(box(bx + 62, by + 150, 56, 70, "#222B3F", "#566", 4))
    s.append(txt(bx + 90, by + 190, "G030", "s", "middle"))
    s.append(txt(bx + 90, by + 204, "F6P6", "s", "middle"))
    s.append(box(bx + 68, by + 250, 44, 30, "#222B3F", "#566", 6)); s.append(txt(bx + 90, by + 269, "NRST", "s", "middle"))
    s.append(f'<circle cx="{bx + 120}" cy="{by + 124}" r="6" fill="{BLUE}"/>')
    s.append(txt(bx + 90, by + 120, "LED", "s", "end")); s.append(txt(bx + 90, by + 132, "PA4", "s", "end"))
    left = [("B7", "", GREY), ("C14", "", GREY), ("C15", "", GREY), ("3V3", "3.3 V out (logic)", RED), ("GND", "GND", "#999"), ("NRST", "reset", GREY),
            ("A0", "CH1 -> relay IN1", BLUE), ("A1", "CH2 -> relay IN2", BLUE),
            ("A2", "USART2 TX -> ESP32 GPIO3 (RX)", TEAL), ("A3", "USART2 RX <- ESP32 GPIO1 (TX)", TEAL)]
    right = [("B6", "", GREY), ("A12", "CH8 -> relay IN8", ORANGE), ("A11", "CH7 -> relay IN7", ORANGE), ("A8", "CH6 -> relay IN6", ORANGE),
             ("A7", "CH5 -> relay IN5", ORANGE), ("A6", "CH4 -> relay IN4", BLUE), ("A5", "CH3 -> relay IN3", BLUE),
             ("A4", "on-board blue LED (free)", GREY), ("5V", "5 V in (shared with ESP32 VIN)", RED), ("GND", "GND (common with ESP32 + relay board)", "#999")]
    for i, (n, d, c) in enumerate(left):
        y = by + 34 + i * 44
        s.append(f'<circle cx="{bx}" cy="{y}" r="7" fill="{c}"/>')
        s.append(txt(bx + 16, y + 4, n, "b"))
        if d: s.append(txt(bx - 16, y + 4, d, "", "end", c if c != GREY else MUTE))
    for i, (n, d, c) in enumerate(right):
        y = by + 34 + i * 44
        s.append(f'<circle cx="{bx + bw}" cy="{y}" r="7" fill="{c}"/>')
        s.append(txt(bx + bw - 16, y + 4, n, "b", "end"))
        if d: s.append(txt(bx + bw + 16, y + 4, d, "", "start", c if c != GREY else MUTE))
    ly = H - 24
    for i, (c, t) in enumerate([(BLUE, "wash relay (CH1-4)"), (ORANGE, "dry relay (CH5-8)"), (TEAL, "serial link to ESP32"), (RED, "power"), (PUR, "programming")]):
        x = 40 + i * 140
        s.append(f'<circle cx="{x}" cy="{ly}" r="6" fill="{c}"/>'); s.append(txt(x + 12, ly + 4, t, "s"))
    s.append("</svg>")
    return "".join(s)

# ------------------------------------------------------------------ system wiring
def wiring():
    W, H = 1100, 640
    s = [head(W, H)]
    s.append(txt(W / 2, 34, "washable - system wiring", "t", "middle"))
    s.append(txt(W / 2, 54, "3.3 V serial between the boards, one common ground, one shared 5 V supply", "s", "middle"))
    # ESP32
    ex, ey, ew, eh = 40, 110, 260, 330
    s.append(box(ex, ey, ew, eh, "#10244A", "#3F6AA8"))
    s.append(txt(ex + ew / 2, ey + 28, "ESP32-3248S035", "b", "middle"))
    s.append(txt(ex + ew / 2, ey + 46, "kiosk (3.5\" touch, RGB LED)", "s", "middle"))
    s.append(box(ex + 20, ey + 70, ew - 40, 150, "#0B1A33", "#2B73B0", 8))
    s.append(txt(ex + ew / 2, ey + 92, "P1 connector (4P 1.25 mm)", "s", "middle"))
    p1 = [("VIN", "5 V in", RED), ("IO1 / TX0", "UART0 TX", TEAL), ("IO3 / RX0", "UART0 RX", TEAL), ("GND", "ground", "#999")]
    for i, (n, d, c) in enumerate(p1):
        y = ey + 116 + i * 28
        s.append(f'<circle cx="{ex + 260 - 20}" cy="{y}" r="7" fill="{c}"/>')
        s.append(txt(ex + 36, y + 4, n, "b")); s.append(txt(ex + 130, y + 4, d, "s"))
    s.append(txt(ex + ew / 2, ey + 250, "P1 pin order differs between board", "s", "middle"))
    s.append(txt(ex + ew / 2, ey + 264, "revisions: follow the silkscreen labels.", "s", "middle"))
    s.append(txt(ex + ew / 2, ey + 296, "Micro-USB / USB-C: power + flashing", "s", "middle"))
    s.append(txt(ex + ew / 2, ey + 312, "(same UART0 as the P1 serial pins)", "s", "middle"))
    # STM32
    sx, sy, sw, sh = 440, 110, 220, 330
    s.append(box(sx, sy, sw, sh, "#10244A", "#3F6AA8"))
    s.append(txt(sx + sw / 2, sy + 28, "WeAct STM32G030F6P6", "b", "middle"))
    s.append(txt(sx + sw / 2, sy + 46, "relay controller", "s", "middle"))
    sp = [("PA3 (RX)", TEAL), ("PA2 (TX)", TEAL), ("GND", "#999"), ("5V", RED)]
    for i, (n, c) in enumerate(sp):
        y = sy + 116 + i * 28
        s.append(f'<circle cx="{sx}" cy="{y}" r="7" fill="{c}"/>'); s.append(txt(sx + 16, y + 4, n, "b"))
    rp = [("PA0", BLUE, "CH1"), ("PA1", BLUE, "CH2"), ("PA5", BLUE, "CH3"), ("PA6", BLUE, "CH4"),
          ("PA7", ORANGE, "CH5"), ("PA8", ORANGE, "CH6"), ("PA11", ORANGE, "CH7"), ("PA12", ORANGE, "CH8")]
    for i, (n, c, ch) in enumerate(rp):
        y = sy + 70 + i * 32
        s.append(f'<circle cx="{sx + sw}" cy="{y}" r="7" fill="{c}"/>'); s.append(txt(sx + sw - 16, y + 4, n + "  " + ch, "b", "end"))
    s.append(txt(sx + sw / 2, sy + 322, "SWD: PA13 / PA14 (ST-Link)", "s", "middle", PUR))
    # relay board
    rx, ry, rw, rh = 800, 110, 260, 330
    s.append(box(rx, ry, rw, rh, "#10244A", "#3F6AA8"))
    s.append(txt(rx + rw / 2, ry + 28, "8-channel relay board", "b", "middle"))
    s.append(txt(rx + rw / 2, ry + 46, "opto-isolated, 5 V coils", "s", "middle"))
    for i in range(8):
        y = ry + 70 + i * 32
        c = BLUE if i < 4 else ORANGE
        s.append(f'<circle cx="{rx}" cy="{y}" r="7" fill="{c}"/>'); s.append(txt(rx + 16, y + 4, f"IN{i + 1}", "b"))
        s.append(txt(rx + rw - 16, y + 4, ("wash" if i < 4 else "dry") + f" CH{i + 1}", "s", "end"))
    # signal wires ESP32 <-> STM32
    ey1, ey2, ey3, ey4 = ey + 116, ey + 144, ey + 172, ey + 200
    s.append(wire([(ex + 240, ey2), (360, ey2), (360, sy + 116), (sx, sy + 116)], TEAL))          # TX0 -> PA3
    s.append(wire([(ex + 240, ey3), (380, ey3), (380, sy + 144), (sx, sy + 144)], TEAL))          # RX0 <- PA2
    s.append(wire([(ex + 240, ey4), (400, ey4), (400, sy + 172), (sx, sy + 172)], "#999"))        # GND
    s.append(wire([(ex + 240, ey1), (420, ey1), (420, sy + 200), (sx, sy + 200)], RED))           # 5V
    s.append(txt(350, ey2 - 6, "TX0 -> PA3", "s", "end", TEAL)); s.append(txt(350, ey3 + 16, "RX0 <- PA2", "s", "end", TEAL))
    # relay wires
    for i in range(8):
        y = sy + 70 + i * 32
        s.append(wire([(sx + sw, y), (rx - 0, ry + 70 + i * 32)], BLUE if i < 4 else ORANGE, 2))
    # power rail
    s.append(box(40, 500, 1020, 100, "#0B1A33", "#2B73B0"))
    s.append(txt(60, 526, "Power and safety", "b"))
    notes = ["5 V supply (>= 2 A): ESP32 VIN, STM32 5V and relay board VCC share it; all grounds connected (ESP32 GND - STM32 GND - relay GND).",
             "Mains / pump / heater loads go only through the relay contacts, behind fuses and a hardware emergency stop that does not rely on software.",
             "Relay modules are often ACTIVE-LOW: set RELAY_ACTIVE_HIGH 0 in stm32-relay/src/main.cpp if the relays click on at power-up.",
             "Unplug the TX wire (or use a 1 k series resistor) while flashing the ESP32 over USB, because GPIO1/3 are also the USB console."]
    for i, n in enumerate(notes):
        s.append(txt(60, 548 + i * 16, n, "s"))
    s.append("</svg>")
    return "".join(s)

(OUT / "stm32-board-pinout.svg").write_text(stm32())
(OUT / "wiring.svg").write_text(wiring())
print("wrote", OUT)
