#!/usr/bin/env bash
#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# console.sh — open the firmware's console.
#
# stdio is USB CDC on the board's own USB-C socket (J5), with S1 set to
# device mode. There is no UART on this board, so a Debug Probe's UART
# port has nothing to talk to; the probe is for SWD on J4 only.
#
# A USB_KEYBOARD build has no console at all: the USB controller is a
# host then, and the screen is the only report.
#
# Usage: ./console.sh [port] [baud]
#
# With no arguments it finds the board by USB descriptor -- Raspberry Pi's
# vendor ID, and not the Debug Probe, which enumerates a CDC port of its
# own that is easy to pick by mistake and silent when you do.
#
set -euo pipefail

BAUD="${2:-115200}"
PORT="${1:-${CONSOLE_PORT:-}}"

if [[ -z "${PORT}" ]]; then
    PORT="$(python3 - <<'PY' 2>/dev/null || true
import sys
from serial.tools import list_ports
c = [p.device for p in list_ports.comports()
     if p.vid == 0x2E8A and 'Debug Probe' not in (p.description or '')]
print(c[0] if len(c) == 1 else '')
sys.exit(0 if len(c) == 1 else 1)
PY
)"
fi

if [[ -z "${PORT}" ]]; then
    echo "ERROR: could not pick the board's CDC port. Is USB-C (J5) plugged in and" >&2
    echo "       S1 set to device mode? Name the port to override; candidates:" >&2
    for p in /dev/cu.usbmodem*; do [[ -e "$p" ]] && echo "  $p" >&2; done
    exit 1
fi

echo "Console: ${PORT} @ ${BAUD} (ctrl-a k to quit screen, ctrl-] for picocom)"

if command -v picocom >/dev/null 2>&1; then
    exec picocom -b "${BAUD}" "${PORT}"
elif command -v screen >/dev/null 2>&1; then
    exec screen "${PORT}" "${BAUD}"
else
    echo "Neither picocom nor screen is installed; falling back to cat." >&2
    stty -f "${PORT}" "${BAUD}" cs8 -cstopb -parenb
    exec cat "${PORT}"
fi
