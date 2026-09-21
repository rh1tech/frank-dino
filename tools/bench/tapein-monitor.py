#!/usr/bin/env python3
"""
Watch the tape input and say what, if anything, is on it.

Run this WHILE the tape is playing. It reports whether the pin is being
driven at all, how much of the time it is high, and the dominant pulse
width -- which is what identifies a signal: a ZX header tone is 807 Hz,
so half a cycle is about 620 us.
"""
import re, subprocess, collections

ELF = "build-kbd/frank-dino.elf"
US_PER_BIT = 1000000 / 125000

out = subprocess.run(["arm-none-eabi-nm", "-l", "--radix=d", ELF],
                     capture_output=True, text=True).stdout
S, T = {}, {}
for l in out.splitlines():
    p = l.split()
    if len(p) >= 3:
        S.setdefault(p[2], int(p[0]))
        if len(p) > 3 and "tapein.c" in p[3]: T[p[2]] = int(p[0])

N = 8
cmds = [f"mwb {S['settings'] + 12} 1", "sleep 700", f"mdb {S['tapein_last_probe']} 1"]
for i in range(N):
    cmds += [f"dump_image out/tm{i}.bin {T['buf_a']} 384", "sleep 120",
             f"dump_image out/tm{i}b.bin {T['buf_b']} 384", "sleep 120"]
args = ["openocd", "-f", "interface/cmsis-dap.cfg", "-c", "adapter speed 4000",
        "-f", "target/rp2350.cfg", "-c", "init"] + \
       [x for c in cmds for x in ("-c", c)] + ["-c", "exit"]
r = subprocess.run(args, capture_output=True, text=True, timeout=240)
v = re.findall(r"^0x[0-9a-f]+:\s+([0-9a-f]+)", r.stdout + r.stderr, re.M)

probe = int(v[0], 16) if v else -1
print({0: "pin held LOW", 2: "pin FLOATING - nothing connected",
       3: "pin held HIGH (idle comparator)", 1: "pin odd"}.get(probe, "probe ?"))

widths, total, ones, trans = collections.Counter(), 0, 0, 0
for i in range(N):
    for suf in ("", "b"):
        try: d = open(f"out/tm{i}{suf}.bin", "rb").read()
        except OSError: continue
        bits = [(b >> k) & 1 for b in d for k in range(8)]
        total += len(bits); ones += sum(bits)
        run, cur = 0, bits[0]
        for x in bits:
            if x == cur: run += 1
            else:
                trans += 1
                if 2 <= run <= 600: widths[run] += 1
                cur, run = x, 1

print(f"sampled {total} bits ({total*US_PER_BIT/1000:.0f} ms)  "
      f"high {100*ones/max(total,1):.1f}%  transitions {trans}")
if not widths:
    print("no pulses at all -- the input is not moving")
else:
    print("commonest pulse widths:")
    for w, n in widths.most_common(6):
        print(f"   {w*US_PER_BIT:7.0f} us  x{n:4d}   ->  {500000/(w*US_PER_BIT):7.0f} Hz")
    w = widths.most_common(1)[0][0] * US_PER_BIT
    f = 500000 / w
    if 700 < f < 950:  print(f"  that is a ZX HEADER TONE ({f:.0f} Hz)")
    elif 1800 < f < 2600: print(f"  that is ZX DATA ({f:.0f} Hz)")
