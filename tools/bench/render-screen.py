#!/usr/bin/env python3
"""Turn a 6912-byte Spectrum screen dump into a PNG."""
import struct, sys, zlib

PAL = [(0,0,0),(0,0,205),(205,0,0),(205,0,205),(0,205,0),(0,205,205),(205,205,0),(205,205,205),
       (0,0,0),(0,0,255),(255,0,0),(255,0,255),(0,255,0),(0,255,255),(255,255,0),(255,255,255)]

def render(d):
    rows = []
    for y in range(192):
        third, line, sub = y >> 6, (y >> 3) & 7, y & 7
        off = (third << 11) | (sub << 8) | (line << 5)
        row = bytearray()
        for cx in range(32):
            b, at = d[off + cx], d[6144 + (y >> 3) * 32 + cx]
            ink = PAL[(at & 7) | ((at & 0x40) >> 3)]
            pap = PAL[((at >> 3) & 7) | ((at & 0x40) >> 3)]
            for bit in range(7, -1, -1):
                row += bytes(ink if (b >> bit) & 1 else pap)
        rows.append(bytes(row))
    return b"".join(b"\x00" + r for r in rows)

def png(raw, w, h):
    def ch(t, d): c = t + d; return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c))
    return (b"\x89PNG\r\n\x1a\n"
            + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + ch(b"IDAT", zlib.compress(raw, 9)) + ch(b"IEND", b""))

d = open(sys.argv[1], "rb").read()
open(sys.argv[2], "wb").write(png(render(d), 256, 192))
ink = sum(1 for b in d[:6144] if b)
print("  %-28s %5d/6144 bitmap bytes set, %2d distinct attrs"
      % (sys.argv[2].split("/")[-1], ink, len(set(d[6144:]))))
