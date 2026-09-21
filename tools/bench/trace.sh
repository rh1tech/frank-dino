#!/bin/sh
# Arm the bus trace on a running board, let it fill, and read it back.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd); cd "$ROOT"
ELF=${ELF:-build-kbd/frank-dino.elf}
sym() { arm-none-eabi-nm --radix=d "$ELF" | grep -E " $1\$" | cut -d' ' -f1; }
ON=$(sym z80_trace_on); POS=$(sym z80_trace_pos); RING=$(sym z80_trace)
openocd -f interface/cmsis-dap.cfg -c "adapter speed 4000" -f target/rp2350.cfg \
    -c "init" -c "mwb $ON 1" -c "sleep 60" -c "mwb $ON 0" \
    -c "mdw $POS 1" -c "dump_image out/trace.bin $RING 1024" -c "exit" 2>&1 \
    | grep -E "^0x|dumped"
