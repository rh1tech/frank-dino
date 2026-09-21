# ROMs

## ZX Spectrum 128K

| File | Bytes | MD5 | What |
|------|-------|-----|------|
| `128-0.rom`      | 16384 | `b4d2692115a9f2924df92a3cbfb358fb` | ROM 0 — the 128K editor and menu |
| `128-1.rom`      | 16384 | `6e09e5d3c4aef166601669feaaadc01c` | ROM 1 — 48K BASIC |
| `48.rom`         | 16384 | `4c42a2f075212361c3117015b107ff68` | The 48K machine's single ROM |
| `plus2.rom`      | 16384 | `b3db95931cc844efaeb82db9c171b9f3` | ROM 0 of the Amstrad +2 |
| `pentagon-0.rom` | 16384 | `a249565f03b98d004ee7f019570069cd` | ROM 0 of the Pentagon 128 |
| `trdos.rom`      | 16384 | `b4c9634312b796063015450daef13dfa` | TR-DOS 5.04T, the Beta Disk's own ROM |

These are the stock Sinclair 128K ROMs, and those first two MD5s are the
canonical ones every emulator checks against. Every machine here except
the 48K uses `128-1.rom` as its ROM 1: the 48K BASIC half is shared, and
it is the half `LD-BYTES` lives in, which is what the tape trap patches.

All of them come from [pico-speccy](https://github.com/rh1tech/pico-speccy),
which is worth a note on how it stores them: each family is one flat base
array plus run-list overlays for the variants, to save flash. The Sinclair
128K pair is in its source tree as `roms/pentagon/src/sinclair_128k_0.bin`
and `roms/128k/src/sinclair_128k_1.bin` — note where the first one lives,
because ROM 0 of that family is itself a 101-byte overlay over the
Pentagon base and only the source tree keeps it flattened. The Pentagon
and TR-DOS images are the bases themselves, unpacked from
`src/roms/pentagon/pentagon_base.c` and `src/roms/trdos/trdos_base.c`, so
only one revision of each is here.

**Copyright.** The ZX Spectrum ROMs are copyright Amstrad plc, who have
given permission for them to be redistributed with emulators. TR-DOS is
copyright Technology Research Ltd. None of them is GPL and none is
covered by this repository's licence; they are included on the same terms
every other Spectrum emulator includes them.

## Why they are welded into the binary

`src/spectrum/spectrum.c` pulls them in with `.incbin`, the same way
frank-xt8086 embeds GLaBIOS. The card is read now, and they still are: a
machine that cannot start without a file on a card is a machine that
cannot be tested by a script, and one that cannot start *at all* without
one is a board that looks dead when the card is missing.

They cost SRAM rather than flash, because this is a `copy_to_ram` build
— ninety-six kilobytes of it. That is the price of the bus loop never
waiting on XIP.
