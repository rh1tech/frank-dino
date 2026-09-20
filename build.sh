#!/usr/bin/env bash
#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# build.sh — configure and build.
#
# Every knob is an environment variable with a default, so the common case
# is `./build.sh` and the uncommon case does not need a second script:
#
#   CPU_SPEED=504 ./build.sh          # the fast clock, 1.60 V
#   CLEAN=1 ./build.sh                # from scratch
#
set -euo pipefail
cd "$(dirname "$0")"
source ./sdk_env.sh

# A one-line nudge, not a gate. The commit hooks only exist for a clone
# that has run `make hooks`, and the usual way to find that out is to have
# a push rejected later. This disappears as soon as they are installed.
if [ "$(git config core.hooksPath 2>/dev/null || true)" != ".githooks" ]; then
    echo "note: git hooks are not installed in this clone -- run 'make hooks'"
fi

BUILD_DIR="${BUILD_DIR:-build}"
BOARD="${BOARD:-frank_dino}"
CPU_SPEED="${CPU_SPEED:-378}"
CLEAN="${CLEAN:-0}"

[[ "${CLEAN}" == "1" ]] && rm -rf "${BUILD_DIR}"

echo "=== frank-dino ==="
echo "  board ${BOARD}   CPU ${CPU_SPEED} MHz"
echo

cmake -S src/app -B "${BUILD_DIR}" \
      -DPICO_BOARD="${BOARD}" \
      -DCPU_SPEED="${CPU_SPEED}"

cmake --build "${BUILD_DIR}" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo
echo "Built: ${BUILD_DIR}/frank-dino.uf2"
