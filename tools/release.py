#!/usr/bin/env python3
"""Build a release: tools/release.py 1.2.0 [--min-version 1.1.0] [--out dist]

Writes src/version.h, builds with PlatformIO and produces dist/firmware.bin + dist/manifest.json
(version, sha256, size, min_version). Upload both files as assets of the GitHub release v1.2.0
(the kiosks read .../releases/latest/download/manifest.json).
Bench test with a laptop: python3 -m http.server -d dist  (build with -DOTA_ALLOW_HTTP=1)
Runs from the repo root; the kiosk firmware lives in esp32-kiosk/.
"""
import argparse, hashlib, json, pathlib, re, shutil, subprocess, sys

repo = pathlib.Path(__file__).resolve().parent.parent
root = repo / "esp32-kiosk"                  # the PlatformIO project that is released over the air
ap = argparse.ArgumentParser()
ap.add_argument("version", help="semantic version, e.g. 1.2.0 (no leading v)")
ap.add_argument("--min-version", default="", help="oldest version allowed to update directly")
ap.add_argument("--out", default="dist")
a = ap.parse_args()
if not re.fullmatch(r"\d+\.\d+\.\d+", a.version):
    sys.exit("version must look like 1.2.0")

(root / "src" / "version.h").write_text(
    '#pragma once\n// Bumped by tools/release.py / the release workflow for every published firmware.\n'
    f'#define FW_VERSION "{a.version}"\n')
subprocess.run(["pio", "run"], cwd=root, check=True)
src = root / ".pio" / "build" / "esp32-3248s035" / "firmware.bin"
out = repo / a.out
out.mkdir(exist_ok=True)
shutil.copy(src, out / "firmware.bin")
data = (out / "firmware.bin").read_bytes()
manifest = {"version": a.version, "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}
if a.min_version:
    manifest["min_version"] = a.min_version
(out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(json.dumps(manifest, indent=2))
