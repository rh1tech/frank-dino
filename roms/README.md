# ROMs

## ZX Spectrum 128K

| File | Bytes | MD5 | What |
|------|-------|-----|------|
| `128-0.rom` | 16384 | `b4d2692115a9f2924df92a3cbfb358fb` | ROM 0 — the 128K editor and menu |
| `128-1.rom` | 16384 | `6e09e5d3c4aef166601669feaaadc01c` | ROM 1 — 48K BASIC |

These are the stock Sinclair 128K ROMs, and those two MD5s are the
canonical ones every emulator checks against.

Taken from [pico-speccy](https://github.com/rh1tech/pico-speccy), which
carries them as `roms/pentagon/src/sinclair_128k_0.bin` and
`roms/128k/src/sinclair_128k_1.bin`. Note where the first one lives:
pico-speccy stores ROM 0 of this family as a 101-byte overlay over the
Pentagon base ROM to save flash, and only the *source* tree keeps the
flattened binary. Copying the overlay would have meant reimplementing
`RomOverlay.h` here to read it.

**Copyright.** The ZX Spectrum ROMs are copyright Amstrad plc, who have
given permission for them to be redistributed with emulators. They are
not GPL and are not covered by this repository's licence; they are
included on the same terms every other Spectrum emulator includes them.

## Why they are welded into the binary

`src/core/state_roms.c` pulls them in with `.incbin`, the same way
frank-xt8086 embeds GLaBIOS. The board has a microSD slot and nothing
reads it yet; a machine that cannot start without a file on a card is a
machine that cannot be tested by a script.
