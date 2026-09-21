#!/usr/bin/env python3
"""Which RAM bank is a machine actually drawing in? Render all eight."""
import subprocess, sys, os
ram = open(sys.argv[1], "rb").read()
name = sys.argv[2]
for b in range(8):
    scr = ram[b * 16384 : b * 16384 + 6912]
    ink = sum(1 for x in scr[:6144] if x)
    attrs = len(set(scr[6144:]))
    mark = " <-- looks like a screen" if 40 < ink < 4000 and attrs > 1 else ""
    print("  bank %d: %5d set, %2d attrs%s" % (b, ink, attrs, mark))
    if ink:
        open("/tmp/b%d.bin" % b, "wb").write(scr)
        subprocess.run([sys.executable, "tools/bench/render-screen.py",
                        "/tmp/b%d.bin" % b, "out/machines/%s-bank%d.png" % (name, b)],
                       stdout=subprocess.DEVNULL)
