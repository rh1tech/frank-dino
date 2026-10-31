#!/usr/bin/env python3
# trace.py — the last bus cycles, recorded for a moment over SWD.
import sys, time
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from dino import Board
KINDS = ['?', 'fetch', 'read', 'write', 'in', 'out', 'int']
b = Board()
a = b.syms['z80_trace_on']
b.ocd.write(a, 8, [1]); time.sleep(float(sys.argv[1]) if len(sys.argv) > 1 else 0.2); b.ocd.write(a, 8, [0])
pos = b.u32('z80_trace_pos')
raw = b.ocd.read(b.syms['z80_trace'], 32, 256)
print('events', pos)
for i in range(64):
    v = raw[(pos + i) % 256]
    k = (v >> 24) & 0xFF
    print(f'{v & 0xFFFF:04X} {(v >> 16) & 0xFF:02X} {KINDS[k] if k < 7 else k:5}', end='  ' if i % 4 != 3 else '\n')
b.close()
