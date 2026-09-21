#!/usr/bin/env python3
"""
Boot the board into one machine, and optionally capture the bus trace.

  tools/bench/boot.py <machine 0-7> <name> [--trace] [--settle MS]

settings.machine has to be written AFTER settings_load() has read the
card and BEFORE main() reads it for the last time. Both edges move with
how long the boot takes, so rather than guess the window this writes
across the whole of it: a write landing before the card read is simply
overwritten, and one after the last read does nothing.
"""
import re, subprocess, sys

ELF = "build-kbd/frank-dino.elf"

def syms():
    out = subprocess.run(["arm-none-eabi-nm", "--radix=d", ELF],
                         capture_output=True, text=True).stdout
    d = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3: d.setdefault(p[2], int(p[0]))
    return d

S = syms()

def ocd(cmds, timeout=240):
    args = ["openocd", "-f", "interface/cmsis-dap.cfg", "-c", "adapter speed 4000",
            "-f", "target/rp2350.cfg", "-c", "init"]
    for c in cmds: args += ["-c", c]
    args += ["-c", "exit"]
    r = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    return r.stdout + r.stderr

def boot(n, name, trace=False, settle=16000):
    cmds = ["reset run", "sleep 1200"]
    for _ in range(28):                     # ~7 s of writes, 250 ms apart
        cmds += [f"mwb {S['settings']} {n}", "sleep 250"]
    cmds += [f"sleep {settle}", f"mdb {S['spec_machine']} 1"]
    if trace:
        cmds += [f"mwb {S['z80_trace_on']} 1", "sleep 300",
                 f"mdb {S['z80_trace_on']} 1", f"mdw {S['z80_trace_pos']} 1",
                 f"dump_image out/tx.bin {S['z80_trace']} 65536"]
    cmds += [f"dump_image out/machines/{name}.ram {S['spec_ram']} {8*16384}"]
    out = ocd(cmds)
    v = re.findall(r"^0x[0-9a-f]+:\s+([0-9a-f]+)", out, re.M)
    print(f"  spec_machine = {int(v[0],16)}")
    if trace:
        print(f"  trace_on after = {int(v[1],16)}  (0 = the freeze fired)")
        print(f"  trace_pos = {int(v[2],16)}")
    return int(v[0], 16)

if __name__ == "__main__":
    n, name = int(sys.argv[1]), sys.argv[2]
    tr = "--trace" in sys.argv
    st = int(sys.argv[sys.argv.index("--settle")+1]) if "--settle" in sys.argv else 16000
    got = boot(n, name, tr, st)
    sys.exit(0 if got == n else 1)
