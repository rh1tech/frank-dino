#!/bin/sh
#
# Boot every machine in turn and render what came up.
#
# The only end-to-end check there is for a machine: its own ROM has to
# get far enough to draw its own start-up screen. Everything below that
# -- the ROM window, the paging, the interrupt chain, the renderer --
# has to be right for the screen to be right.
#
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
OUT=${OUT:-out/machines}
mkdir -p "$OUT"

set -- "0 48k" "1 128k" "2 plus2" "3 plus3" "4 pentagon" \
       "5 scorpion" "6 profi" "7 timex"
for e in "$@"; do
    n=${e%% *}; name=${e#* }
    printf "\n=== %s (machine %s) ===\n" "$name" "$n"
    tools/bench/boot-machine.sh "$n" "$name"
    python3 tools/bench/render-screen.py "$OUT/$name.bank5" "$OUT/$name.png"
    python3 tools/bench/render-screen.py "$OUT/$name.bank7" "$OUT/$name-shadow.png"
done
echo
echo "screens in $OUT/"
