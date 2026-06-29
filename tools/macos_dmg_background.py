#!/usr/bin/env python3
"""Compose the KeeperFX .dmg window background.

Lays the repo's banner (docs/assets/readme-banner.png) across the top as a header
strip and fills the rest with a near-black dungeon tone, so the Finder window can
sit large app + Applications icons on the dark area below with a drag arrow between.

Dependency-free on purpose (matches macos.mk's "built-in tools only" packaging):
  * scaling/decoding the banner is delegated to the built-in `sips` (-> a 32bpp BMP),
  * everything else is pure-Python stdlib (struct/zlib) — no PIL/ImageMagick.

Usage:
  macos_dmg_background.py --banner PATH --out PATH.png --width W --height H \
      [--bg RRGGBB] [--arrow X1 X2 Y]
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import zlib


def scale_banner_to_bmp(banner, width, tmpdir):
    """Use the built-in sips to resample the banner to `width` and emit a BMP."""
    bmp = os.path.join(tmpdir, "banner.bmp")
    subprocess.run(
        ["sips", "--resampleWidth", str(width), "-s", "format", "bmp", banner, "--out", bmp],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    return bmp


def read_bmp(path):
    """Minimal BMP reader -> (w, h, rows[y][x] = (r, g, b, a)). Handles 24/32bpp,
    top-down (negative height) and bottom-up storage."""
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    top_down = h < 0
    h = abs(h)
    stride = ((w * bpp + 31) // 32) * 4
    bytespp = bpp // 8
    rows = []
    for y in range(h):
        ry = y if top_down else h - 1 - y
        base = off + ry * stride
        row = []
        for x in range(w):
            o = base + x * bytespp
            b, g, r = d[o], d[o + 1], d[o + 2]
            a = d[o + 3] if bpp == 32 else 255
            row.append((r, g, b, a))
        rows.append(row)
    return w, h, rows


def write_png(path, w, h, px):
    """Write an 8-bit RGB PNG from px[y][x] = (r, g, b[, a])."""
    raw = bytearray()
    for row in px:
        raw.append(0)  # filter type 0 (None)
        for c in row:
            raw += bytes(c[:3])
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def draw_arrow(canvas, w, h, x1, x2, y, color=(214, 158, 54)):
    """A bold horizontal shaft from x1 with a triangular head whose tip is at x2.
    Anti-aliased via supersampling so the diagonal head edges are smooth/sharp rather
    than jagged. All sizes scale with the canvas width so 1x and 2x renders match."""
    shaft_half = w * 0.013     # shaft half-thickness (float)
    head_len = w * 0.044       # length of the triangular head
    head_half = w * 0.030      # half-height of the head's base
    shaft_end = x2 - head_len
    cy = float(y)
    SS = 4                      # 4x4 supersampling per pixel

    def covered(px, py):
        """Fraction (0..1) of pixel (px,py) inside the arrow shape."""
        hits = 0
        for sy in range(SS):
            fy = py + (sy + 0.5) / SS
            dy = abs(fy - cy)
            for sx in range(SS):
                fx = px + (sx + 0.5) / SS
                if x1 <= fx < shaft_end:
                    if dy <= shaft_half:
                        hits += 1
                elif shaft_end <= fx <= x2:
                    if dy <= head_half * (x2 - fx) / head_len:
                        hits += 1
        return hits / (SS * SS)

    y0 = max(0, int(cy - head_half) - 1)
    y1 = min(h, int(cy + head_half) + 2)
    x0 = max(0, x1 - 1)
    xe = min(w, x2 + 1)
    for py in range(y0, y1):
        for px in range(x0, xe):
            c = covered(px, py)
            if c <= 0:
                continue
            br, bg, bb, _ = canvas[py][px]
            canvas[py][px] = (int(color[0] * c + br * (1 - c)),
                              int(color[1] * c + bg * (1 - c)),
                              int(color[2] * c + bb * (1 - c)), 255)


def _rrect_cov(px, py, cx, cy, hw, hh, radius):
    """Anti-aliased coverage (0..1) of pixel (px,py) inside a rounded rect."""
    import math
    dx = abs(px + 0.5 - cx) - (hw - radius)
    dy = abs(py + 0.5 - cy) - (hh - radius)
    qx, qy = max(dx, 0.0), max(dy, 0.0)
    dist = math.sqrt(qx * qx + qy * qy) + min(max(dx, dy), 0.0) - radius
    return min(1.0, max(0.0, 0.5 - dist))


def _blend(canvas, px, py, color, cov):
    if cov <= 0:
        return
    br, bg, bb, _ = canvas[py][px]
    canvas[py][px] = (int(color[0] * cov + br * (1 - cov)),
                      int(color[1] * cov + bg * (1 - cov)),
                      int(color[2] * cov + bb * (1 - cov)), 255)


def draw_label_chip(canvas, w, h, cx, cy, cw, ch, radius, color=(232, 228, 220), alpha=0.92):
    """A snug, anti-aliased rounded "pill" behind an icon label so Finder's label text
    stays readable on the dark background, with a soft drop shadow for depth."""
    hw, hh = cw / 2.0, ch / 2.0
    pad = int(radius) + 4
    # soft drop shadow: a few offset, expanding, low-alpha dark passes (cheap blur)
    for k, sa in ((2, 0.16), (3, 0.12), (4, 0.08)):
        for py in range(max(0, int(cy - hh) - pad), min(h, int(cy + hh) + pad + k)):
            for px in range(max(0, int(cx - hw) - pad), min(w, int(cx + hw) + pad)):
                cov = _rrect_cov(px, py - k, cx, cy, hw + k * 0.5, hh + k * 0.5, radius) * sa
                _blend(canvas, px, py, (0, 0, 0), cov)
    # the pill itself
    for py in range(max(0, int(cy - hh) - 2), min(h, int(cy + hh) + 2)):
        for px in range(max(0, int(cx - hw) - 2), min(w, int(cx + hw) + 2)):
            _blend(canvas, px, py, color, _rrect_cov(px, py, cx, cy, hw, hh, radius) * alpha)


# ---------------------------------------------------------------------------
# KeeperFX DMG background layout (1x). Coordinates are in image pixels, measured
# against a Finder window of 560x460pt (icon size 128, status bar visible). The 2x
# Retina layer is just every value doubled. Tweak these to move things.
# ---------------------------------------------------------------------------
LAYOUT = {
    "width": 572,            # canvas a touch wider than the 560pt window so it always covers
    "height": 440,
    "bg": "0F0B07",          # near-black dungeon fill below the banner
    "arrow": (226, 334, 246),        # (x1, x2, y)  — y is also the icon centre
    "labels": [(150, 329), (410, 329)],  # chip centres (under each icon's Finder label)
}


def render(banner, scale):
    """Render the whole background at the given scale -> (W, H, canvas)."""
    L = LAYOUT
    W, H = round(L["width"] * scale), round(L["height"] * scale)
    bg = tuple(int(L["bg"][i:i + 2], 16) for i in (0, 2, 4)) + (255,)
    canvas = [[bg for _ in range(W)] for _ in range(H)]

    with tempfile.TemporaryDirectory() as tmp:
        bw, bh, banner_px = read_bmp(scale_banner_to_bmp(banner, W, tmp))
    for y in range(min(bh, H)):
        for x in range(min(bw, W)):
            r, g, b, _ = banner_px[y][x]
            canvas[y][x] = (r, g, b, 255)

    ch = round(W * 0.041)
    for cx, cy in L["labels"]:
        draw_label_chip(canvas, W, H, round(cx * scale), round(cy * scale),
                        cw=round(W * 0.25), ch=ch, radius=ch // 2)
    x1, x2, y = L["arrow"]
    draw_arrow(canvas, W, H, round(x1 * scale), round(x2 * scale), round(y * scale))
    return W, H, canvas


def build_tiff(banner, out_tiff):
    """Produce the Retina multi-rep .tiff (1x + 2x) Finder uses as the window bg."""
    with tempfile.TemporaryDirectory() as tmp:
        p1, p2 = os.path.join(tmp, "1x.png"), os.path.join(tmp, "2x.png")
        for path, sc in ((p1, 1), (p2, 2)):
            W, H, canvas = render(banner, sc)
            write_png(path, W, H, canvas)
        q = {"stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL}
        subprocess.run(["sips", "-s", "dpiHeight", "144", "-s", "dpiWidth", "144", p2], check=True, **q)
        subprocess.run(["tiffutil", "-cathidpicheck", p1, p2, "-out", out_tiff], check=True, **q)
    print(f"wrote {out_tiff} (Retina 1x+2x)")


def main(argv):
    ap = argparse.ArgumentParser(description="Generate the KeeperFX .dmg window background.")
    ap.add_argument("--banner", default="docs/assets/readme-banner.png")
    ap.add_argument("--out", default="res/dmg-background.tiff",
                    help=".tiff -> Retina multi-rep (default); any other extension -> single 1x PNG")
    a = ap.parse_args(argv)

    if a.out.lower().endswith(".tiff"):
        build_tiff(a.banner, a.out)
    else:
        W, H, canvas = render(a.banner, 1)
        write_png(a.out, W, H, canvas)
        print(f"wrote {a.out} ({W}x{H})")


if __name__ == "__main__":
    main(sys.argv[1:])
