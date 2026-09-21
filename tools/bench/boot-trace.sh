#!/bin/sh
#
# Boot one machine and capture its bus trace, in a SINGLE openocd
# session.
#
# Splitting the two is how you end up diagnosing the machine that
# happened to be running rather than the one you asked for: openocd
# does not reset on connect here, so a second invocation reads whatever
# the board is still doing.
#
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd); cd "$ROOT"
ELF=${ELF:-build-kbd/frank-dino.elf}
mkdir -p out/machines
sym() { arm-none-eabi-nm --radix=d "$ELF" | grep -E " $1\$" | cut -d' ' -f1; }
SET=$(sym settings); MACH=$(sym spec_machine); RAM=$(sym spec_ram)
ON=$(sym z80_trace_on); POS=$(sym z80_trace_pos); RING=$(sym z80_trace)
N=$1; NAME=$2
openocd -f interface/cmsis-dap.cfg -c "adapter speed 4000" -f target/rp2350.cfg \
    -c "init" -c "reset run" -c "sleep 1800" \
    -c "mwb $SET $N" -c "sleep 400" -c "mwb $SET $N" -c "sleep 400" -c "mwb $SET $N" \
    -c "sleep ${SETTLE:-22000}" \
    -c "mdb $MACH 1" \
    -c "mwb $ON 1" -c "sleep 50" -c "mwb $ON 0" -c "mdw $POS 1" \
    -c "dump_image out/trace.bin $RING 1024" \
    -c "dump_image out/machines/$NAME.ram $RAM $((8 * 16384))" \
    -c "exit" 2>&1 | grep -E "^0x|dumped"
