#!/usr/bin/env bash
#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# capture.sh — grab the board's video through the HDMI capture stick.
#
# This board has no VGA connector: the six-bit ladder on GP30-GP37 feeds
# an MS9288A (U16) and the MS9288A drives J9, so HDMI is the only output
# there is. A MACROSILICON MS21xx stick on the far end enumerates as
# "USB Video" and gives 640x480 at 60 fps in UYVY — which is exactly the
# mode the firmware emits, so a character cell survives the round trip
# and an 80x30 report is legible in a still.
#
# That is what makes "did stage 0 pass" a question a script can answer.
#
# Usage:
#   ./capture.sh                     one frame to out/screen.png
#   ./capture.sh shot out/foo.png    one frame, named
#   ./capture.sh clip 10 out/b.mp4   ten seconds of video
#   ./capture.sh view                live preview
#   ./capture.sh modes               what the stick will give you
#
set -euo pipefail
cd "$(dirname "$0")"

DEVICE="${CAPTURE_DEVICE:-USB Video}"
SIZE="${CAPTURE_SIZE:-640x480}"
FPS="${CAPTURE_FPS:-60}"

# The stick keeps sending the last frame it saw for a moment after the
# source changes, and its first frames after opening are often the tail of
# the previous mode. Discarding a few makes a shot taken right after a
# reset show what is on the screen now.
WARMUP="${CAPTURE_WARMUP:-12}"

common=(-hide_banner -loglevel error -f avfoundation -framerate "${FPS}" -video_size "${SIZE}" -i "${DEVICE}")

mkdir -p out

case "${1:-shot}" in
    shot)
        OUT="${2:-out/screen.png}"
        ffmpeg "${common[@]}" -vf "select=gte(n\,${WARMUP})" -frames:v 1 -update 1 -y "${OUT}"
        echo "${OUT}"
        ;;
    clip)
        SECS="${2:-5}"
        OUT="${3:-out/capture.mp4}"
        ffmpeg "${common[@]}" -t "${SECS}" -c:v libx264 -pix_fmt yuv420p -y "${OUT}"
        echo "${OUT}"
        ;;
    view)
        exec ffplay -hide_banner -loglevel error \
            -f avfoundation -framerate "${FPS}" -video_size "${SIZE}" -i "${DEVICE}"
        ;;
    modes)
        # The stick reports its whole mode list when asked for one it does
        # not have, which is the only way to enumerate it through ffmpeg.
        ffmpeg -hide_banner -f avfoundation -video_size 99999x99999 -i "${DEVICE}" 2>&1 \
            | sed -n 's/^.*Supported modes:/Supported modes:/p;s/^\[avfoundation[^]]*\] *\( *[0-9]\+x[0-9]\+@.*\)$/\1/p'
        ;;
    *)
        echo "usage: $0 [shot [file] | clip [secs] [file] | view | modes]" >&2
        exit 2
        ;;
esac
