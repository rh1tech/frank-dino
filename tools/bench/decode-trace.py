#!/usr/bin/env python3
"""Decode the 256-entry bus trace ring."""
import struct, sys
K = {0:"---",1:"fetch",2:"read",3:"write",4:"IN",5:"OUT",6:"intack"}
pos = int(sys.argv[1], 0)
raw = open("out/trace.bin","rb").read()
n = len(raw)//4
ev = [struct.unpack_from("<HBB", raw, i*4) for i in range(n)]
base = 0 if pos < n else pos - n
ev = [ev[(base+i) % n] for i in range(min(pos, n))]
import collections
print("kinds:", dict(collections.Counter(K.get(k,k) for _,_,k in ev)))
lo = [a for a,_,k in ev if k == 1]
if lo: print("opfetch range: %04X..%04X" % (min(lo), max(lo)))
for i,(a,v,k) in enumerate(ev[-int(sys.argv[2] if len(sys.argv)>2 else 48):]):
    print("  %04X %02X %s" % (a, v, K.get(k,k)))
