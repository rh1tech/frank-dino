#!/bin/sh
# Compile one host test against the real firmware sources.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
OUT=${OUT:-/tmp/frank-tests}
mkdir -p "$OUT"
CFLAGS="-std=gnu2x -O1 -g -Wall -Wno-unused-function -Wno-unknown-attributes
        -include tests/stub/prelude.h -D__attribute__(x)= -DFIRMWARE_VERSION=\"host-test\"
        -Itests/stub -Isrc/spectrum -Isrc/core -Isrc/app -Isrc/ui
        -Isrc/drivers/audio -Isrc/drivers -Isrc/test
        -Isrc/drivers/fatfs -Isrc/drivers/psram -Isrc/drivers/vga
        -Isrc/drivers/miniz -Isrc/drivers/sdcard -Isrc/drivers/usbdisk"
exec cc $CFLAGS "$@"
