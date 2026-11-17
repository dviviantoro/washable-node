#!/usr/bin/env python3
"""Post-process the screenshots: contact sheets of the kiosk pages, and optional PNG palette optimisation.
usage: sheets.py <out-dir> [--optimize]
"""
import pathlib, sys
from PIL import Image, ImageDraw, ImageFont

out = pathlib.Path(sys.argv[1])


def font(size):
    for f in ("DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "Arial.ttf"):
        try:
            return ImageFont.truetype(f, size)
        except OSError:
            pass
    return ImageFont.load_default()


def sheet(folder: pathlib.Path, title: str, cols: int, scale: float, dest: pathlib.Path):
    files = sorted(folder.glob("*.png"))
    if not files:
        return
    tw, th = int(320 * scale), int(480 * scale)
    pad, cap = 14, 26
    rows = (len(files) + cols - 1) // cols
    head = 52
    W = cols * (tw + pad) + pad
    H = head + rows * (th + cap + pad) + pad
    img = Image.new("RGB", (W, H), (244, 247, 251))
    d = ImageDraw.Draw(img)
    d.text((pad, 14), title, fill=(15, 34, 54), font=font(22))
    for i, f in enumerate(files):
        r, c = divmod(i, cols)
        x, y = pad + c * (tw + pad), head + r * (th + cap + pad)
        im = Image.open(f).convert("RGB").resize((tw, th), Image.LANCZOS)
        img.paste(im, (x, y + cap))
        name = f.stem.split("-", 1)[1].replace("-", " ")
        d.text((x, y + 4), f"{int(f.stem.split('-')[0])}. {name}", fill=(60, 80, 100), font=font(13))
    dest.parent.mkdir(parents=True, exist_ok=True)
    img.save(dest, optimize=True)
    print("sheet", dest, img.size)


def masonry(setup: str, title: str, cols: int, tile_w: int, dest: pathlib.Path):
    """One overview image of all dashboard pages of a setup: tiles of equal width, each dropped into the shortest column."""
    import json
    meta = json.loads((pathlib.Path(__file__).parent / "pages.json").read_text())["pages"]
    tiles = []
    for pg in meta:
        f = out / setup / f"{pg['id']}.png"
        if f.exists():
            im = Image.open(f).convert("RGB")
            tiles.append((f"{int(pg['id'].split('-')[0])}. {pg['title']}", im.resize((tile_w, max(1, round(im.height * tile_w / im.width))), Image.LANCZOS)))
    if not tiles:
        return
    pad, cap, head = 16, 24, 56
    heights = [head] * cols
    placed = []
    for label, im in tiles:
        c = heights.index(min(heights))
        placed.append((c, heights[c], label, im))
        heights[c] += cap + im.height + pad
    W = cols * (tile_w + pad) + pad
    H = max(heights) + pad
    img = Image.new("RGB", (W, H), (230, 236, 244))
    d = ImageDraw.Draw(img)
    d.text((pad, 16), title, fill=(15, 34, 54), font=font(24))
    for c, y, label, im in placed:
        x = pad + c * (tile_w + pad)
        d.text((x, y), label[: tile_w // 7], fill=(40, 60, 85), font=font(13))
        img.paste(im, (x, y + cap))
        d.rectangle((x - 1, y + cap - 1, x + tile_w, y + cap + im.height), outline=(180, 192, 208))
    img.save(dest, optimize=True)
    print("sheet", dest, img.size)


masonry("desktop-light", "OTA dashboard - desktop, light theme", 3, 440, out / "sheet-desktop-light.png")
masonry("desktop-dark", "OTA dashboard - desktop, dark theme", 3, 440, out / "sheet-desktop-dark.png")
masonry("mobile-light", "OTA dashboard - phone, light theme", 8, 190, out / "sheet-mobile-light.png")
masonry("mobile-dark", "OTA dashboard - phone, dark theme", 8, 190, out / "sheet-mobile-dark.png")

def check_qr():
    """The kiosk firmware's demo payment QR is a real merchant's QRIS; the preview swaps in a fake one. Make sure no published image shows the real one.
    Every kiosk variant has several frames of its payment page in .qr/ (the page animates a scan line over the QR, so any single frame may not decode):
    at least one frame per variant must decode, and everything that decodes must be the fake payload. Dashboard screenshots are checked when they decode."""
    try:
        import cv2
    except ImportError:
        raise SystemExit("PRIVACY: OpenCV is needed to verify the payment QR (pip install opencv-python-headless), or pass --skip-qr-check")
    det = cv2.QRCodeDetector()

    def read(f):
        img = cv2.imread(str(f))
        for scale in (1, 2, 3):
            im = img if scale == 1 else cv2.resize(img, None, fx=scale, fy=scale, interpolation=cv2.INTER_NEAREST)
            text, _, _ = det.detectAndDecode(im)
            if text:
                return text
        return ""

    variants = sorted(p for p in (out / ".qr").glob("*") if p.is_dir())
    if not variants:
        raise SystemExit("PRIVACY: no frames of the kiosk payment page were captured, so the QR could not be verified")
    decoded = 0
    for v in variants:
        texts = [t for t in (read(f) for f in sorted(v.glob("*.png"))) if t]
        if not texts:
            raise SystemExit(f"PRIVACY: the payment QR of kiosk variant {v.name} could not be decoded in any frame, so it could not be verified")
        bad = [t for t in texts if "FAKE QR" not in t]
        if bad:
            raise SystemExit(f"PRIVACY: kiosk variant {v.name} shows a QR code that is not the fake demo one: {bad[0][:60]!r}...")
        decoded += len(texts)
    for f in sorted(out.glob("*/19-preview-payment.png")):
        text = read(f)
        if text and "FAKE QR" not in text:
            raise SystemExit(f"PRIVACY: {f} shows a QR code that is not the fake demo one: {text[:60]!r}...")
    print(f"payment QR check: {decoded} frames of {len(variants)} kiosk variants decoded, all fake")
    import shutil
    shutil.rmtree(out / ".qr", ignore_errors=True)


if "--skip-qr-check" not in sys.argv:
    check_qr()

kiosk = out / "kiosk"
if kiosk.is_dir():
    for sub in sorted(p for p in kiosk.iterdir() if p.is_dir()):
        lang, theme = sub.name.split("-")
        sheet(sub, f"Kiosk pages - {'Bahasa Indonesia' if lang == 'id' else 'English'}, {theme} theme", 8, 0.62, kiosk / f"sheet-{sub.name}.png")

if "--optimize" in sys.argv:
    before = after = 0
    for f in out.rglob("*.png"):
        b = f.stat().st_size
        im = Image.open(f).convert("RGB")
        q = im.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        tmp = f.with_suffix(".tmp.png")
        q.save(tmp, optimize=True)
        if tmp.stat().st_size < b * 0.8:
            tmp.replace(f)
        else:
            tmp.unlink()
        before += b
        after += f.stat().st_size
    print(f"optimised PNGs: {before/1e6:.1f} MB -> {after/1e6:.1f} MB")
