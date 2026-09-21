#!/bin/sh
#
# The host test suite.
#
# Every test links the REAL firmware module it exercises -- the stubs in
# tests/stub are only for the parts of the machine a host has no answer
# for (the SDK, FatFs, the peripherals a given test is not about). What
# each test asserts is therefore the shipping code's behaviour.
#
#   tests/run.sh          run everything
#   tests/run.sh paging   run one
#
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
OUT=${OUT:-/tmp/frank-tests}
mkdir -p "$OUT" tests/out
B=tests/build.sh

python3 tests/mkfixtures.py > "$OUT/fixtures.log"

# Each line: name | extra -D flags | sources
run_one() {
    name=$1; defs=$2; shift 2
    $B $defs -c tests/stub/stubs.c -o "$OUT/stubs_$name.o"
    $B $defs "tests/test_$name.c" "$@" "$OUT/stubs_$name.o" -o "$OUT/$name" 2>&1 \
        | grep -v "reducing alignment" || true
    "$OUT/$name"
}

only=${1:-}
fail=0
try() { [ -z "$only" ] || [ "$only" = "$1" ] || return 0; run_one "$@" || fail=1; }

try paging  "-DREAL_SPECTRUM -DREAL_VGA" \
    src/spectrum/spectrum.c src/core/z80_bus.c src/spectrum/saa.c
try fdc     "-DREAL_FDC" \
    src/spectrum/fdc.c src/spectrum/storage.c tests/stub/ff_host.c
try beta    "-DREAL_BETA" \
    src/spectrum/beta.c src/spectrum/storage.c tests/stub/ff_host.c
try media   "-DREAL_DCK" \
    src/spectrum/zip.c src/spectrum/dck.c src/spectrum/storage.c \
    tests/stub/ff_host.c src/drivers/miniz/miniz.c
try divmmc  "-DREAL_DIVMMC" \
    src/spectrum/divmmc.c src/spectrum/storage.c tests/stub/ff_host.c
try tape    "-DREAL_TAPE" \
    src/spectrum/tape.c src/spectrum/storage.c src/core/z80_bus.c tests/stub/ff_host.c
try saa     "" src/spectrum/saa.c
try video   "-DREAL_VGA"
try ui      "" src/ui/ui_gfx.c src/ui/ui_font.c

[ $fail -eq 0 ] && echo "all host tests passed" || echo "SOME TESTS FAILED"
exit $fail
