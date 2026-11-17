#!/usr/bin/env python3
"""Writes <out>/README.md: a visual index of every screenshot (so the whole set can be browsed on GitHub).
usage: gallery.py <out-dir>
"""
import json, pathlib, sys

out = pathlib.Path(sys.argv[1])
meta = json.loads((pathlib.Path(__file__).parent / "pages.json").read_text())
SETUPS = [("desktop-light", "Desktop, light"), ("desktop-dark", "Desktop, dark"), ("mobile-light", "Phone, light"), ("mobile-dark", "Phone, dark")]

lines = [
    "# OTA dashboard screenshots",
    "",
    "Every page of the dashboard in four setups (desktop and phone, light and dark theme), and every kiosk page rendered through the dashboard's **Kiosk preview**.",
    "All data is made up (demo fleet on a throwaway server). Regenerate with `tools/dashboard-shots/run.sh` after a UI change.",
    "",
    "## Dashboard pages",
    "",
]
for p in meta["pages"]:
    cells = []
    for key, label in SETUPS:
        f = out / key / f"{p['id']}.png"
        if f.exists():
            cells.append((label, f"{key}/{p['id']}.png"))
    if not cells:
        continue
    lines += [f"### {p['id'].split('-', 1)[0]}. {p['title']}", ""]
    lines += ["| " + " | ".join(c[0] for c in cells) + " |", "|" + "---|" * len(cells)]
    lines += ["| " + " | ".join(f'<a href="{c[1]}"><img src="{c[1]}" width="{300 if "mobile" in c[1] else 420}"></a>' for c in cells) + " |", ""]

kiosk = out / "kiosk"
if kiosk.is_dir():
    lines += ["## Kiosk pages (through the Kiosk preview)", "", "Native 320x480 renders of the real kiosk UI code, in both languages and both kiosk themes. Click a sheet for the full size.", ""]
    for sub in sorted(p for p in kiosk.iterdir() if p.is_dir()):
        lang, theme = sub.name.split("-")
        sheet = kiosk / f"sheet-{sub.name}.png"
        title = f"{'Bahasa Indonesia' if lang == 'id' else 'English'}, {theme} theme"
        lines += [f"### {title}", ""]
        if sheet.exists():
            lines += [f'<a href="kiosk/{sheet.name}"><img src="kiosk/{sheet.name}" width="900"></a>', ""]
        lines += [f"Individual pages: `kiosk/{sub.name}/NN-name.png` ({len(list(sub.glob('*.png')))} files).", ""]

(out / "README.md").write_text("\n".join(lines) + "\n")
print("wrote", out / "README.md")
