#!/usr/bin/env python3
"""
Build the binary fixtures the host tests read.

Generated rather than committed so the shape of each one is legible:
what a test asserts about a disk should be readable next to the code
that laid the disk out.
"""
import os, struct, zlib, sys

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")
os.makedirs(HERE, exist_ok=True)

TRACKS, SIDES, SPT, SECSIZE = 40, 1, 9, 512
# +3 sector numbering starts at 0xC1, as a PCW/+3 formatted disk does.
SECTOR_IDS = [0xC1 + i for i in range(SPT)]

def track(t, h):
    """One Track-Info block: header, sector table, then the sector data."""
    hdr = bytearray(256)
    hdr[0:12] = b"Track-Info\r\n"
    hdr[0x10] = t          # track
    hdr[0x11] = h          # side
    hdr[0x14] = 2          # sector size N=2 -> 512
    hdr[0x15] = SPT
    hdr[0x16] = 0x4E       # gap3
    hdr[0x17] = 0xE5       # filler
    for i, r in enumerate(SECTOR_IDS):
        o = 0x18 + i * 8
        hdr[o+0], hdr[o+1], hdr[o+2], hdr[o+3] = t, h, r, 2
        hdr[o+4], hdr[o+5] = 0, 0          # st1, st2
        hdr[o+6], hdr[o+7] = SECSIZE & 0xFF, SECSIZE >> 8
    # Every byte of a sector is (track*16 + index), so a test can say
    # exactly what it expects to read back.
    data = b"".join(bytes([(t * 16 + i) & 0xFF]) * SECSIZE for i in range(SPT))
    return bytes(hdr) + data

def dsk(extended, unformatted=()):
    n = TRACKS * SIDES
    hdr = bytearray(256)
    sig = b"EXTENDED CPC DSK File\r\nDisk-Info\r\n" if extended \
          else b"MV - CPCEMU Disk-File\r\nDisk-Info\r\n"
    hdr[0:len(sig)] = sig
    hdr[0x30], hdr[0x31] = TRACKS, SIDES
    tsize = 256 + SPT * SECSIZE
    body = b""
    if extended:
        for i in range(n):
            hdr[0x34 + i] = 0 if i in unformatted else tsize // 256
        for i in range(n):
            if i not in unformatted:
                body += track(i // SIDES, i % SIDES)
    else:
        hdr[0x32], hdr[0x33] = tsize & 0xFF, tsize >> 8
        for i in range(n):
            body += track(i // SIDES, i % SIDES)
    return bytes(hdr) + body

def write(name, data):
    with open(os.path.join(HERE, name), "wb") as f:
        f.write(data)
    print("  %-16s %7d bytes" % (name, len(data)))

print("fixtures:")
write("std.dsk", dsk(False))
# Track 3 left unformatted: the reason the extended format exists, and
# the case that breaks any code computing track offsets by multiplication.
write("ext.dsk", dsk(True, unformatted={3}))

# ---------------------------------------------------------------------
# Copy protection, one track: C1 a plain sector, C2 a WEAK one recorded
# as three copies (0x11, 0x22, 0x33), C3 a short one -- N=2 but only 256
# bytes recorded, the rest reads as the filler -- and R=2 all 0xE5, the
# sector Speedlock reads twice.
# ---------------------------------------------------------------------
def weak_dsk():
    secs = [(0xC1, b"\x55" * 512), (0xC2, b"\x11" * 512 + b"\x22" * 512 + b"\x33" * 512),
            (0xC3, b"\x44" * 256), (0x02, b"\xE5" * 512)]
    hdr = bytearray(256)
    hdr[0:12] = b"Track-Info\r\n"
    hdr[0x14], hdr[0x15], hdr[0x16], hdr[0x17] = 2, len(secs), 0x4E, 0xE5
    for i, (r, data) in enumerate(secs):
        o = 0x18 + i * 8
        hdr[o+0], hdr[o+1], hdr[o+2], hdr[o+3] = 0, 0, r, 2
        hdr[o+6], hdr[o+7] = len(data) & 0xFF, len(data) >> 8
    body = bytes(hdr) + b"".join(d for _, d in secs)
    body += b"\0" * ((-len(body)) % 256)
    disk = bytearray(256)
    sig = b"EXTENDED CPC DSK File\r\nDisk-Info\r\n"
    disk[0:len(sig)] = sig
    disk[0x30], disk[0x31] = 1, 1
    disk[0x34] = len(body) // 256
    return bytes(disk) + body

write("weak.dsk", weak_dsk())

# ---------------------------------------------------------------------
# A .zip holding three members: stored, deflated, and one in a format
# the firmware does not read (which must be left out of the listing).
# ---------------------------------------------------------------------
def zipfile_(members):
    """members: list of (name, data, deflate?)"""
    out, central = b"", b""
    for name, data, deflate in members:
        nb = name.encode()
        crc = zlib.crc32(data) & 0xFFFFFFFF
        if deflate:
            co = zlib.compressobj(9, zlib.DEFLATED, -15)
            comp = co.compress(data) + co.flush()
            method = 8
        else:
            comp, method = data, 0
        off = len(out)
        lh = struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, method, 0, 0,
                         crc, len(comp), len(data), len(nb), 0)
        out += lh + nb + comp
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0,
                               method, 0, 0, crc, len(comp), len(data),
                               len(nb), 0, 0, 0, 0, 0, off) + nb
    cd_off = len(out)
    out += central
    out += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(members),
                       len(members), len(central), cd_off, 0)
    return out

# 180 KB of compressible data, so deflate is genuinely exercised rather
# than falling back to stored.
big = bytes((i * 7 + (i >> 5)) & 0xFF for i in range(180 * 1024))
small = bytes(range(256)) * 12               # 3072 bytes, stored
write("games.zip", zipfile_([
    ("small.tap", small, False),
    ("big.z80",   big,   True),
    ("readme.txt", b"not a thing the firmware loads\n", False),
]))
with open(os.path.join(HERE, "small.tap.expected"), "wb") as f: f.write(small)
with open(os.path.join(HERE, "big.z80.expected"), "wb") as f: f.write(big)

# ---------------------------------------------------------------------
# A Timex .dck: one bank header byte per 8 KB chunk, then the chunks.
#   0 = not present, 1 = RAM (writable), 2 = ROM (read-only)
# ---------------------------------------------------------------------
# The type byte is a bit field: bit 0 = RAM (writable), bit 1 = the
# chunk's 8 KB is carried in the file. So 1 is RAM the file does NOT
# carry -- it occupies no space, and counting it in the stride would
# desynchronise every chunk after it.
hdr = bytearray(9)
hdr[0] = 0                     # bank id 0 = the dock
#         0     1     2        3          4           5..7
#         ROM   ROM   absent   RAM+data   RAM, empty  absent
kinds = [  2,    2,    0,       3,         1,          0, 0, 0 ]
for i, k in enumerate(kinds):
    hdr[1 + i] = k
body = b""
for i, k in enumerate(kinds):
    if k & 2:                  # only chunks whose data is in the file
        body += bytes([0xC0 + i]) * 8192
write("cart.dck", bytes(hdr) + body)

# ---------------------------------------------------------------------
# A card image for the DivMMC. Only a handful of blocks are ever read,
# so it stays small; block N is filled with a value derived from N so a
# test can say which block it got.
# ---------------------------------------------------------------------
BLOCKS = 64
card = bytearray()
for b in range(BLOCKS):
    card += bytes([(b * 3 + 1) & 0xFF]) * 512
    card[b * 512 + 0] = 0xAA           # a marker at each end of the block
    card[b * 512 + 511] = b & 0xFF
write("card.mmc", bytes(card))

# ---------------------------------------------------------------------
# Tape images. The same two blocks in all three formats, so a test can
# assert that .tap, .tzx and .pzx arrive at identical contents.
# ---------------------------------------------------------------------
def tap_block(flag, payload):
    body = bytes([flag]) + payload
    chk = 0
    for b in body: chk ^= b
    body += bytes([chk])
    return struct.pack("<H", len(body)) + body

# A standard header block (19 bytes of body) and a data block.
header = bytes([0]) + b"testtape  " + struct.pack("<HHH", 256, 32768, 32768)
data   = bytes((i * 11) & 0xFF for i in range(256))
TAP = tap_block(0x00, header) + tap_block(0xFF, data)
write("tape.tap", TAP)

# .tzx: the signature, then each block as a type-0x10 standard block.
tzx = b"ZXTape!\x1a\x01\x14"
for flag, payload in ((0x00, header), (0xFF, data)):
    body = bytes([flag]) + payload
    chk = 0
    for b in body: chk ^= b
    body += bytes([chk])
    tzx += bytes([0x10]) + struct.pack("<HH", 1000, len(body)) + body
write("tape.tzx", tzx)

# .pzx: PZXT header, a PULS chunk that carries no data, then one DATA
# chunk per block. A DATA chunk is: count (bit 31 = initial level, low
# 31 = BIT count), a 16-bit tail, two pulse counts p0/p1, then p0+p1
# 16-bit pulse words, then the bytes themselves.
def pzx_chunk(tag, body):
    return tag + struct.pack("<I", len(body)) + body

pzx = pzx_chunk(b"PZXT", bytes([1, 0]) + b"host-test\0")
pzx += pzx_chunk(b"PULS", struct.pack("<HH", 0x8000 | 3223, 500))
for flag, payload in ((0x00, header), (0xFF, data)):
    body = bytes([flag]) + payload
    chk = 0
    for b in body: chk ^= b
    body += bytes([chk])
    p0, p1 = 2, 2
    ch = struct.pack("<I", 0x80000000 | (len(body) * 8))   # bits, level high
    ch += struct.pack("<H", 8)                             # tail
    ch += bytes([p0, p1])
    ch += struct.pack("<HH", 855, 855)                     # p0 pulses
    ch += struct.pack("<HH", 1710, 1710)                   # p1 pulses
    ch += body
    pzx += pzx_chunk(b"DATA", ch)
# A DATA chunk whose bit count is not a whole number of bytes must be
# dropped, not rounded: rounding hands LD-BYTES a checksum over a byte
# that was never on the tape.
ragged = struct.pack("<I", 0x80000000 | 37) + struct.pack("<H", 8) + bytes([0, 0]) + b"\xAA" * 5
pzx += pzx_chunk(b"DATA", ragged)
pzx += pzx_chunk(b"STOP", struct.pack("<H", 0))
write("tape.pzx", pzx)

# ---------------------------------------------------------------------
# A TR-DOS .trd: 40 tracks, 2 sides, 16 sectors of 256 bytes. Every
# sector is filled with a byte derived from its logical position, so a
# read that lands on the wrong side or track shows up as wrong data --
# the side bit is inverted in the hardware, and that is exactly the kind
# of thing that makes every other track wrong.
# ---------------------------------------------------------------------
TRD_TRACKS, TRD_SIDES, TRD_SPT, TRD_SEC = 40, 2, 16, 256
trd = bytearray()
for t in range(TRD_TRACKS):
    for side in range(TRD_SIDES):
        for s in range(1, TRD_SPT + 1):
            trd += bytes([(t * 32 + side * 16 + s) & 0xFF]) * TRD_SEC
write("disk.trd", bytes(trd))
