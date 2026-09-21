#!/bin/sh
#
# Boot the board into one machine and photograph the result.
#
# Machine selection lives in settings.machine, which main() reads again
# AFTER the clock tuner -- see the note above the second
# spec_set_machine() call. So the board is reset, that one byte is
# written over SWD while the tuner is still converging, and the machine
# that finally starts is the one asked for. No keypress, no card edit.
#
#   tools/bench/boot-machine.sh <0-7> <name>
#
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
ELF=${ELF:-build-kbd/frank-dino.elf}
OUT=${OUT:-out/machines}
mkdir -p "$OUT"

sym() { arm-none-eabi-nm --radix=d "$ELF" | grep -E " $1\$" | cut -d' ' -f1; }
SETTINGS=$(sym settings)            # .machine is the first field
MACHINE=$(sym spec_machine)
P7FFD=$(sym spec_port_7ffd)
SCRBANK=$(sym spec_screen_bank)
RAM=$(sym spec_ram)

N=$1; NAME=$2
openocd -f interface/cmsis-dap.cfg -c "adapter speed 4000" -f target/rp2350.cfg \
    -c "init" -c "reset run" \
    -c "sleep 1800" \
    -c "mwb $SETTINGS $N" -c "sleep 400" \
    -c "mwb $SETTINGS $N" -c "sleep 400" \
    -c "mwb $SETTINGS $N" \
    -c "sleep ${SETTLE:-11000}" \
    -c "mdb $MACHINE 1" -c "mdb $P7FFD 1" -c "mdb $SCRBANK 1" \
    -c "dump_image $OUT/$NAME.ram $RAM $((8 * 16384))" \
    -c "exit" 2>&1 | grep -E "^0x|dumped|Error" || true
