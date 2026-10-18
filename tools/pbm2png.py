#!/usr/bin/env python3
"""Convert screenshots to PNG: P4 PBM (the Mac framebuffer verbatim) or P6
PPM (the whole panel, in colour).

usage: pbm2png.py a.pbm [b.ppm ...]   -> a.png, b.png, ...
"""

import struct
import sys
import zlib


def read_netpbm(path):
    data = open(path, "rb").read()
    fields, pos = [], 0
    wanted = 3
    while len(fields) < wanted:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
        if fields[0] == b"P6":
            wanted = 4  # also a maxval
    pos += 1
    if fields[0] not in (b"P4", b"P6"):
        sys.exit(f"{path}: not a P4 PBM or P6 PPM")
    return fields[0], int(fields[1]), int(fields[2]), data[pos:]


def write_png(path, w, h, depth, colour, rows):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    raw = b"".join(b"\x00" + r for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, colour, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


for p in sys.argv[1:]:
    kind, w, h, body = read_netpbm(p)
    out = p.rsplit(".", 1)[0] + ".png"
    if kind == b"P4":
        # 1-bit greyscale. PBM's 1 = black is PNG's 0 = black, so invert.
        row = (w + 7) // 8
        rows = [bytes(255 - b for b in body[y * row:(y + 1) * row]) for y in range(h)]
        write_png(out, w, h, 1, 0, rows)
    else:
        rows = [body[y * w * 3:(y + 1) * w * 3] for y in range(h)]
        write_png(out, w, h, 8, 2, rows)
    print(out)
