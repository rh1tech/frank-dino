#!/usr/bin/env python3
"""
Load a tape on the real board, end to end.

There is no way to drive the browser from here, so the tape is put in
place directly: the image into tape_image[], the mount state beside it,
and the trap stub into the live ROM window -- which is what
tape_mount() would have done. Then LOAD "" is typed on the keyboard
matrix and the machine is left to it.

What this proves that a host test cannot: that the real Z80 fetches the
patched stub out of the ROM window, and that its six OUTs and one IN
are seen as I/O at all. This board has no ~IORQ -- I/O is inferred from
~RD/~WR with ~MREQ idle -- so that inference is the part worth proving
on hardware.
"""
import re, subprocess, sys

ELF = "build-kbd/frank-dino.elf"
TAP = sys.argv[1] if len(sys.argv) > 1 else "tests/fixtures/tape.tap"

def nm():
    out = subprocess.run(["arm-none-eabi-nm", "-l", "--radix=d", ELF],
                         capture_output=True, text=True).stdout
    plain, byfile = {}, {}
    for line in out.splitlines():
        m = re.match(r"(\d+)\s+\S+\s+(\S+)(?:\s+(\S+))?", line)
        if not m: continue
        addr, name, loc = int(m.group(1)), m.group(2), m.group(3) or ""
        plain.setdefault(name, addr)
        if loc: byfile[(name, loc.split(":")[0].split("/")[-1])] = addr
    return plain, byfile

S, F = nm()
def tape_sym(n): return F[(n, "tape.c")]

KBD   = S["kbd"];        IMG  = S["tape_image"]
RPAGE = S["z80_rpage"];  RAM  = S["spec_ram"]
SET   = S["settings"];   MACH = S["spec_machine"]
MOUNT = tape_sym("mounted"); ILEN = tape_sym("image_len")
BAT   = tape_sym("block_at"); BNO = tape_sym("block_no")
LEN   = len(open(TAP, "rb").read())

def ocd(cmds, timeout=180):
    args = ["openocd", "-f", "interface/cmsis-dap.cfg", "-c", "adapter speed 4000",
            "-f", "target/rp2350.cfg", "-c", "init"]
    for c in cmds: args += ["-c", c]
    args += ["-c", "exit"]
    r = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    return r.stdout + r.stderr

# --- 1. boot a 48K; its ROM 0 is the one with LD-BYTES in it -----------
print("booting a 48K...")
out = ocd(["reset run", "sleep 1800",
           f"mwb {SET} 0", "sleep 400", f"mwb {SET} 0", "sleep 400", f"mwb {SET} 0",
           "sleep 20000",
           f"mdb {MACH} 1", f"mdw {RPAGE} 1"])
vals = re.findall(r"^0x[0-9a-f]+:\s+([0-9a-f]+)", out, re.M)
machine, rom = int(vals[0], 16), int(vals[1], 16)
print(f"  spec_machine={machine}  ROM window at 0x{rom:08x}")
assert machine == 0, "did not reach the 48K"

# --- 2. put the tape in, and the stub over LD-BYTES -------------------
# Exactly the bytes tape.c assembles: DI, six OUTs, IN, RRA, EI, RET.
STUB = [0xF3, 0xD3,0x7B, 0xF5,0xE1,0x7D,0xD3,0x7B, 0xDD,0xE5,0xE1,0x7C,0xD3,0x7B,
        0x7D,0xD3,0x7B, 0x7A,0xD3,0x7B, 0x7B,0xD3,0x7B, 0xDB,0x7B, 0x1F, 0xFB, 0xC9]
cmds = [f"load_image {TAP} {IMG} bin",
        f"mww {ILEN} {LEN}", f"mww {BAT} 0", f"mww {BNO} 0", f"mwb {MOUNT} 1"]
cmds += [f"mwb {rom + 0x0556 + i} {b}" for i, b in enumerate(STUB)]

# --- 3. type LOAD "" and ENTER ----------------------------------------
# 48K half-rows: 5 = P O I U Y, 6 = ENTER L K J H, 7 = SPACE SYMSHIFT M N B.
# A pressed key is a clear bit. LOAD is J in keyword mode; " is
# SYMBOL SHIFT + P; then ENTER.
def tap_key(rows):                       # rows: {row: mask}
    c = [f"mwb {KBD + r} {0x1F & ~m}" for r, m in rows.items()]
    c += ["sleep 200"]
    c += [f"mwb {KBD + r} 0x1F" for r in rows]
    c += ["sleep 250"]
    return c

cmds += ["sleep 500"]
cmds += tap_key({6: 0x08})               # J  -> LOAD
cmds += tap_key({7: 0x02, 5: 0x01})      # SYMSHIFT+P -> "
cmds += tap_key({7: 0x02, 5: 0x01})      # again      -> ""
cmds += tap_key({6: 0x01})               # ENTER
cmds += ["sleep 3000"]
cmds += [f"mdw {BNO} 1", f"mdb {MOUNT} 1",
         f"dump_image out/tape-after.ram {RAM} {8 * 16384}"]

print("staging the tape and typing LOAD \"\" ...")
out = ocd(cmds)
vals = re.findall(r"^0x[0-9a-f]+:\s+([0-9a-f]+)", out, re.M)
block_no = int(vals[0], 16)
print(f"  block_no after = {block_no}   (0 = the trap never fired)")

# --- 4. did the data actually arrive? ---------------------------------
ram = open("out/tape-after.ram", "rb").read()
want = bytes(((i * 11) & 0xFF) for i in range(256))
hit = ram.find(want)
if hit >= 0:
    print(f"  the 256-byte data block is in RAM at bank {hit // 16384}, "
          f"offset 0x{hit % 16384:04X}")
else:
    print("  the data block is NOT in RAM")
print("RESULT:", "tape loaded on hardware" if (block_no and hit >= 0) else "did not load")
