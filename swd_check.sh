#!/usr/bin/env bash
#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# swd_check.sh — is the debug port reachable, and how reliably.
#
# It measures rather than reports pass or fail, because on this board the
# honest answer was neither. The same command said "no response" for most
# of an afternoon and "DPIDR 0x4c013477" minutes later, and both were
# true: the SWD pair is a marginal connection, and what it does drifts.
#
# One window looked cleanly speed-dependent --
#
#   5000 kHz   0 / 5     2000 kHz   4 / 5     1000 kHz   8 / 8
#
# -- and half an hour later every speed passed 3 / 3, which means the
# speed gradient was real but was not the whole story. Handling and
# temperature move it too.
#
# So: several attempts per speed, and the numbers printed rather than
# summarised. A working board is 5/5 everywhere and stays that way. An
# open fails identically everywhere. Anything in between, including
# anything that changes between runs, is a joint -- and a low clock
# hides it rather than fixing it.
#
set -uo pipefail
cd "$(dirname "$0")"

TRIES="${TRIES:-5}"
SPEEDS="${SPEEDS:-5000 2000 1000 500 200}"

probe_seen=1
best=""

attempt() {             # attempt <kHz> -> 0 if DPIDR came back
    local out
    out=$(timeout 12 openocd -f interface/cmsis-dap.cfg \
            -c "adapter speed $1" -f target/rp2350.cfg -c "init; exit" 2>&1)
    grep -q "CMSIS-DAP: Interface ready" <<<"$out" && probe_seen=0
    grep -q "DPIDR" <<<"$out"
}

echo "Debug port, ${TRIES} attempts per speed:"
for spd in $SPEEDS; do
    ok=0
    for _ in $(seq 1 "$TRIES"); do attempt "$spd" && ok=$((ok + 1)); done
    printf '  %5s kHz   %d / %d\n' "$spd" "$ok" "$TRIES"
    # The fastest speed that was perfect, not merely the fastest that ever
    # worked: a flash that drops halfway through leaves a half-erased chip.
    [[ $ok -eq $TRIES && -z $best ]] && best="$spd"
done
echo

if [[ $probe_seen -ne 0 ]]; then
    echo "No CMSIS-DAP adapter responded at all. Is the Debug Probe plugged"
    echo "into this machine? Nothing above has been tested."
    exit 1
fi

if [[ -n $best ]]; then
    echo "Reliable at ${best} kHz and below."
    if [[ $best -lt 5000 ]]; then
        cat <<TXT

That is slower than an RP2350 should need. A sound SWD pair runs at
5 MHz; needing to back off means the connection is marginal -- a
high-resistance joint or too much capacitance on SWDIO/SWCLK -- and it
will get worse with temperature and handling. Worth reflowing U5 pins
33 and 34, and J5, while it is still only inconvenient.

The scripts here default to ADAPTER_SPEED=1000 for that reason.
TXT
    fi
    exit 0
fi

cat <<'TXT'
No response at any speed. What that rules out:

  - Firmware.     The DPIDR read is answered by the Debug Port from the
                  always-on domain, before any AP, clock or user code.
                  Confirm by trying it in BOOTSEL, where none exists.
  - OTP fusing.   Check with: picotool otp get CRIT1
                  DEBUG_DISABLE / SECURE_DEBUG_DISABLE must read 0.
  - The protocol. Plain and multidrop are different wire sequences:
                  openocd -c "set SWD_MULTIDROP 1" ...

Failing identically at every speed is an open rather than a marginal
joint -- a marginal one fails fast and passes slow, which is the whole
reason this script sweeps.

On dino_z80 each SWD net has exactly two nodes and no vias, all on F.Cu:

    J5 pin 1  <--- 5 segments, top layer --->  U5 pin 34  (SWDIO)
    J5 pin 3  <--- 3 segments, top layer --->  U5 pin 33  (SWCLK)

Measure, powered, with nothing in J5 -- the RP2350 has a fixed pull-up on
SWDIO and a fixed pull-down on SWCLK:

    J5 pin 1 should sit near 3.3 V
    J5 pin 3 should sit near 0 V

Both the same, or both floating, means J5 is not reaching the chip.
Then, power off, check continuity J5.1-U5.34 and J5.3-U5.33; both ends
are on the top side and need no rework to probe.

Until it is fixed, flashing does not need SWD:

    picotool reboot -u -f && ./flash.sh
TXT
exit 1
