#!/usr/bin/env bash
#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# keys.sh — type at the Spectrum over the console.
#
# The board has no keyboard socket yet, so the console is the keyboard:
# every byte sent down it is held on the Spectrum's key matrix for a few
# frames. Which means the machine can be driven from a script over the
# same cable that flashes it, and "does it still boot to the menu and run
# a program" becomes a thing a test can answer with a screenshot.
#
#   ./keys.sh 'PRINT 1'      type it
#   ./keys.sh --enter        one ENTER
#   ./keys.sh --down --enter navigate the 128K menu
#   ./keys.sh --raw $'\r'    send bytes as given
#
# Keys are paced apart on purpose: the ROM scans the keyboard once per
# frame and debounces across frames, so bytes sent back to back arrive as
# one keypress or none.
#
set -euo pipefail
cd "$(dirname "$0")"

PORT="${CONSOLE_PORT:-}"
BAUD="${CONSOLE_BAUD:-115200}"
GAP="${KEY_GAP:-0.12}"

# pyserial rather than stty: on macOS the termios settings do not survive
# the next open, so `stty -f port 115200; cat port` reads at whatever the
# port had before. That silently produces the right number of bytes as
# pure garbage, which reads like a wiring fault rather than a host one.
exec python3 - "${PORT:-}" "${BAUD}" "${GAP}" "$@" <<'PY'
import sys, time, serial

port, baud, gap, *args = sys.argv[1:]
gap = float(gap)

# Find the board, not the probe.
#
# With a Debug Probe attached there are two /dev/cu.usbmodem* and picking
# the wrong one is silent: keystrokes go to the probe's UART, which on
# this board is connected to nothing, so nothing happens and nothing
# complains. They are trivial to tell apart by USB descriptor -- the
# probe says "Debug Probe (CMSIS-DAP)" and the board says "Pico".
if not port:
    from serial.tools import list_ports
    cands = [p for p in list_ports.comports()
             if p.vid == 0x2E8A and 'Debug Probe' not in (p.description or '')]
    if not cands:
        sys.exit("ERROR: no board CDC port found. Is USB plugged in, and S1 set to "
                 "device mode? (CONSOLE_PORT=/dev/... to override.)")
    port = cands[0].device

s = serial.Serial(port, int(baud), timeout=0.2)

NAMED = {
    '--enter': '\r', '--space': ' ', '--del': '\x7f',
    # The 128K menu moves on the cursor keys, which a Spectrum spells as
    # CAPS SHIFT plus 5/6/7/8. The firmware knows that; these are just the
    # escape sequences a terminal would send, so both paths agree.
    '--up': '\x1b[A', '--down': '\x1b[B', '--right': '\x1b[C', '--left': '\x1b[D',
}

out = []
raw = False
for a in args:
    if a == '--raw':      raw = True; continue
    if a in NAMED:        out.append(NAMED[a]); continue
    out.append(a if raw else a)

for chunk in out:
    for ch in chunk:
        s.write(ch.encode('latin-1'))
        s.flush()
        time.sleep(gap)

# Anything the board says back, so a caller can see a banner or a warning.
time.sleep(0.2)
tail = s.read(4096)
if tail:
    sys.stdout.write(tail.decode('latin-1'))
s.close()
PY
