#!/usr/bin/env python3
"""
Is anything arriving on the tape input?

Switches real_player on, samples the bit buffer a few times, and says
how much the input is moving. A ZX header tone is 807 Hz, so a frame of
it should show roughly 32 transitions per millisecond of buffer -- a
flat reading means the comparator is idle, not that the sampler is
broken.
"""
import re, subprocess, sys, time

ELF = "build-kbd/frank-dino.elf"

def syms():
    out = subprocess.run(["arm-none-eabi-nm", "-l", "--radix=d", ELF],
                         capture_output=True, text=True).stdout
    S, T = {}, {}
    for l in out.splitlines():
        p = l.split()
        if len(p) >= 3:
            S.setdefault(p[2], int(p[0]))
            if len(p) > 3 and "tapein.c" in p[3]: T[p[2]] = int(p[0])
    return S, T

S, T = syms()
RP = S["settings"] + 12          # settings.real_player
cmds = [f"mwb {RP} 1", "sleep 600"]
for i in range(4):
    cmds += [f"dump_image out/tb{i}.bin {T['buf_a']} 384",
             f"dump_image out/tb{i}b.bin {T['buf_b']} 384",
             f"mdw {T['frame_bits']} 1", "sleep 400"]
args = ["openocd", "-f", "interface/cmsis-dap.cfg", "-c", "adapter speed 4000",
        "-f", "target/rp2350.cfg", "-c", "init"] + \
       [x for c in cmds for x in ("-c", c)] + ["-c", "exit"]
out = subprocess.run(args, capture_output=True, text=True, timeout=180)
txt = out.stdout + out.stderr

print("frame_bits:", re.findall(r"^0x[0-9a-f]+:\s+([0-9a-f]+)", txt, re.M))
for i in range(4):
    for suf in ("", "b"):
        try: d = open(f"out/tb{i}{suf}.bin", "rb").read()
        except OSError: continue
        bits = [(b >> k) & 1 for b in d for k in range(8)]
        tr = sum(1 for a, b in zip(bits, bits[1:]) if a != b)
        print(f"  buf_{'a' if suf=='' else 'b'} #{i}: ones {sum(bits):5d}/{len(bits)}  "
              f"transitions {tr:5d}   {'MOVING' if tr > 20 else 'flat'}")
