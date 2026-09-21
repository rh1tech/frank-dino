#!/usr/bin/env bash
#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# swd_flash.sh — flash over SWD with a Raspberry Pi Debug Probe.
#
# The preferred path once a probe is attached. SWD does not care what the
# target is doing, so a firmware that has faulted into lockup still takes
# a new image without anyone touching the board.
#
# Wiring: J5 on the board, pin 1 = SWDIO, pin 2 = GND, pin 3 = SWCLK.
#
# J1 next to it is the UART and has the same three-pin footprint. A probe
# on the wrong one gives "Error connecting DP: cannot read IDR" and a
# silent console at the same time, which looks exactly like a dead board.
#
# Usage: ./swd_flash.sh [image.elf] [--reset-only]
#
# With no image it flashes ${BUILD_DIR}/frank-dino.elf, which is build/
# unless BUILD_DIR says otherwise -- the same variable build.sh takes, so
# `BUILD_DIR=build-kbd ./build.sh && BUILD_DIR=build-kbd ./swd_flash.sh`
# builds and flashes the same tree. It used to ignore BUILD_DIR and always
# take build/, which flashes a perfectly good image of the wrong
# configuration and reports success: the symptom is a feature that was
# just built and tested not being there at all.
set -euo pipefail
cd "$(dirname "$0")"

BUILD_DIR="${BUILD_DIR:-build}"
ELF="${1:-${BUILD_DIR}/frank-dino.elf}"

# SWD clock: 1 MHz, not the 5 MHz an RP2350 will normally take.
#
# Measured on this board: 5000 kHz connects 0 times in 5, 2000 kHz 4 times
# in 5, and everything at or below 1000 kHz 8 times in 8. That is a
# marginal connection on the SWD pair rather than a healthy one -- a good
# joint does not care about the difference -- so the low clock is a
# workaround and the joint is still worth reflowing. Until then this is
# the difference between a debugger and a coin toss.
#
# Override with ADAPTER_SPEED=<kHz> if the hardware is ever fixed.
ADAPTER_SPEED="${ADAPTER_SPEED:-1000}"

OPENOCD_ARGS=(-f interface/cmsis-dap.cfg -c "adapter speed ${ADAPTER_SPEED}" -f target/rp2350.cfg)

# `reset halt` then `resume`, for the same reason the flash path does it:
# `reset run` alone does not reliably bring both cores out together.
if [[ "${1:-}" == "--reset-only" || "${2:-}" == "--reset-only" ]]; then
    exec openocd "${OPENOCD_ARGS[@]}" -c "init" -c "reset halt" -c "resume" -c "exit"
fi

if [[ ! -f "${ELF}" ]]; then
    echo "ERROR: ${ELF} not found. Run ./build.sh first." >&2
    exit 1
fi

# Note the reset: `reset halt` then `resume`, not `program ... reset`.
#
# OpenOCD's own post-program reset does not reliably stop core 1 on this
# chip, and this image is copy_to_ram. So core 0 came out of reset and
# began copying the new image into RAM while core 1 was still executing
# the old one out of the same RAM. What that looks like from the bench is
# a firmware that hangs at a different place on every build, with no
# fault flags set and core 1's program counter somewhere in the bootrom —
# which is a very convincing impression of a hardware fault, and cost
# most of an afternoon before the PC gave it away.
#
# `reset halt` stops both cores at the bootrom entry; `resume` starts
# them together, from the top.
echo "Flashing over SWD: ${ELF}"
openocd "${OPENOCD_ARGS[@]}" \
        -c "init" \
        -c "program ${ELF} verify" \
        -c "reset halt" \
        -c "resume" \
        -c "exit"
