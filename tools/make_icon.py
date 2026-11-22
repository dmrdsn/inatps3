"""Generates pkgfiles/ICON0.PNG (320x176) without third-party libraries."""
import math
import struct
import sys
import zlib

W, H = 320, 176

FONT = {  # 5x7 bitmap glyphs
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "3": ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
    "U": ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "L": ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "Y": ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
}


def lerp(a, b, t):
    return a + (b - a) * t


def main(out_path):
    px = [[(0, 0, 0)] * W for _ in range(H)]
    # background gradient
    for y in range(H):
        for x in range(W):
            t = (x / W) * 0.4 + (y / H) * 0.6
            px[y][x] = (int(lerp(18, 6, t)), int(lerp(36, 12, t)), int(lerp(66, 24, t)))

    def blend(x, y, col, a):
        if 0 <= x < W and 0 <= y < H and a > 0:
            r, g, b = px[y][x]
            a = min(1.0, a)
            px[y][x] = (int(lerp(r, col[0], a)), int(lerp(g, col[1], a)), int(lerp(b, col[2], a)))

    # play button: circle + triangle
    cx, cy, rad = 82, 88, 54
    accent = (61, 139, 253)
    for y in range(cy - rad - 2, cy + rad + 3):
        for x in range(cx - rad - 2, cx + rad + 3):
            d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
            blend(x, y, accent, rad + 0.5 - d)
    tri = [(cx - 16, cy - 26), (cx - 16, cy + 26), (cx + 28, cy)]

    def inside(x, y):
        (x1, y1), (x2, y2), (x3, y3) = tri
        d1 = (x - x2) * (y1 - y2) - (x1 - x2) * (y - y2)
        d2 = (x - x3) * (y2 - y3) - (x2 - x3) * (y - y3)
        d3 = (x - x1) * (y3 - y1) - (x3 - x1) * (y - y1)
        neg = d1 < 0 or d2 < 0 or d3 < 0
        pos = d1 > 0 or d2 > 0 or d3 > 0
        return not (neg and pos)

    for y in range(cy - 30, cy + 31):
        for x in range(cx - 20, cx + 32):
            cov = sum(inside(x + (i + 0.5) / 4, y + (j + 0.5) / 4) for i in range(4) for j in range(4)) / 16
            blend(x, y, (255, 255, 255), cov)

    def text(s, x0, y0, scale, col):
        x = x0
        for ch in s:
            g = FONT[ch]
            for gy, row in enumerate(g):
                for gx, bit in enumerate(row):
                    if bit == "1":
                        for yy in range(scale):
                            for xx in range(scale):
                                blend(x + gx * scale + xx, y0 + gy * scale + yy, col, 1.0)
            x += 6 * scale

    text("M3U", 158, 44, 9, (255, 255, 255))
    text("PLAYER", 160, 120, 4, (143, 170, 220))

    raw = b"".join(b"\x00" + bytes(c for p in row for c in p) for row in px)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(out_path, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "pkgfiles/ICON0.PNG")
