#!/usr/bin/env python3
"""Generate the placeholder UWP package art in app/Assets (Python stdlib only: zlib PNG writer, no PIL).

Calm dark plate + a simple mark (a rounded screen outline with a soft dot). Replace with real art later; keep the file
names, the manifest references the unscaled names and MRT picks the qualified variants.

  python3 tools/make_assets.py          # writes app/Assets/*.png
"""
import math
import struct
import zlib
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / 'app' / 'Assets'
BG = (11, 11, 15)
FG = (232, 232, 237)
ACCENT = (120, 160, 255)
SCALES = (100, 125, 150, 200, 400)
TILES = {  # name: (w, h) at scale-100
    'StoreLogo': (50, 50),
    'Square44x44Logo': (44, 44),
    'Square150x150Logo': (150, 150),
    'Wide310x150Logo': (310, 150),
    'SmallTile': (71, 71),
    'LargeTile': (310, 310),
    'SplashScreen': (620, 300),
}
TARGET_SIZES = (16, 24, 32, 48, 256)  # Square44x44Logo.targetsize-*


def sd_round_rect(px, py, cx, cy, hw, hh, r):
    qx, qy = abs(px - cx) - hw + r, abs(py - cy) - hh + r
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r


def cover(d, aa):
    """signed distance -> coverage 0..1 with a 1-pixel smooth edge"""
    return min(max(0.5 - d / aa, 0.0), 1.0)


def render(w, h, plate=True):
    s = min(w, h)
    # the mark spans ~42% of the short side (less on tiny icons would vanish, so more there)
    m = s * (0.62 if s <= 48 else 0.42)
    cx, cy = w / 2, h / 2
    hw, hh = m / 2, m * 0.33
    r = m * 0.09
    stroke = max(m * 0.07, 1.2)
    dot = m * 0.09
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            px, py = x + 0.5, y + 0.5
            d = sd_round_rect(px, py, cx, cy, hw, hh, r)
            ring = cover(abs(d) - stroke / 2, 1.0)
            dd = math.hypot(px - cx, py - cy) - dot
            fill = cover(dd, 1.0)
            if plate:
                base = BG
                a_out = 255
            else:
                base = (0, 0, 0)
                a_out = None
            col = list(base)
            a = 0.0
            for c, k in ((FG, ring), (ACCENT, fill)):
                if k > 0:
                    col = [col[i] * (1 - k) + c[i] * k for i in range(3)]
                    a = a + k * (1 - a)
            if a_out is None:
                alpha = int(round(a * 255))
                if a > 0:
                    # un-premultiply against black
                    col = [min(255, v / a) for v in col]
                row += bytes([int(col[0]), int(col[1]), int(col[2]), alpha])
            else:
                row += bytes([int(col[0]), int(col[1]), int(col[2]), 255])
        rows.append(bytes(row))
    return png(w, h, b''.join(rows))


def png(w, h, raw):
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    n = 0
    for name, (w, h) in TILES.items():
        for sc in SCALES:
            sw, sh = round(w * sc / 100), round(h * sc / 100)
            (OUT / f'{name}.scale-{sc}.png').write_bytes(render(sw, sh))
            n += 1
    for t in TARGET_SIZES:
        (OUT / f'Square44x44Logo.targetsize-{t}.png').write_bytes(render(t, t))
        (OUT / f'Square44x44Logo.targetsize-{t}_altform-unplated.png').write_bytes(render(t, t, plate=False))
        n += 2
    print(f'wrote {n} PNGs to {OUT}')


if __name__ == '__main__':
    main()
