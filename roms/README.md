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
| `plus3-0.rom`    | 16384 | `9833b8b73384dd5fa3678377ff00a2bb` | +3 ROM 0 — the editor |
| `plus3-1.rom`    | 16384 | `0f711ceb5ab801b4701989982e0f334c` | +3 ROM 1 — syntax |
| `plus3-2.rom`    | 16384 | `3b6dd659d5e4ec97f0e2f7878152c987` | +3 ROM 2 — +3DOS |
| `plus3-3.rom`    | 16384 | `a148bcc575e51389e84fdf5d555c3196` | +3 ROM 3 — 48K BASIC |
| `scorpion-0.rom` | 16384 | `b9fda5b6a747ff037365b0e2d8c4379a` | Scorpion — BASIC 128 |
| `scorpion-1.rom` | 16384 | `fbca50eff4937afde90dd9e55387d9e6` | Scorpion — BASIC 48 |
| `scorpion-2.rom` | 16384 | `7cb480da76a596ef265c58038820a705` | Scorpion — the service monitor |
| `scorpion-3.rom` | 16384 | `40dccd1b7f80bb211bdc299df0234084` | Scorpion — TR-DOS |
| `profi-0.rom`    | 16384 | `c1b33ec8c3cd436df5a1aa7c2679bb8a` | Profi — bank 0 |
| `profi-1.rom`    | 16384 | `f036cde8c1ef83ee3cee62c4146b49d3` | Profi — bank 1 |
| `profi-2.rom`    | 16384 | `a249565f03b98d004ee7f019570069cd` | Profi — bank 2 |
| `profi-3.rom`    | 16384 | `8e376009d0b6712f63fa6da672791574` | Profi — bank 3 |
| `timex-home.rom` | 16384 | `575d203c6e15e679fba0b73f854ec7a2` | Timex TC2068 — the home ROM |
| `timex-exrom.rom`|  8192 | `55d462fccc6c536037404ef4ced08bec` | Timex TC2068 — the EX-ROM |
| `esxdos.rom`     |  8192 | `fa50b0258e52b8d72bd83cc2fb6e1013` | esxDOS 0.8.9, in DivMMC's EEPROM |

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
copyright Technology Research Ltd. esxDOS is copyright Papaya Dezign and
is redistributed with emulators on the same understanding. None of them is GPL and none is
covered by this repository's licence; they are included on the same terms
every other Spectrum emulator includes them.

The +3's four are the canonical v4.0 set, and they are four because the
machine pages one of four into the bottom window: #7FFD bit 4 is the low
bit of the ROM number and #1FFD bit 2 is the high one. They come from
pico-speccy's `src/roms/plus3/src/`, where they are kept unpacked.


## The rest of the machines

From pico-speccy's `src/speccy/roms/*/src/` (the raw images), or flattened
from its run-list overlays where only an overlay exists. Their copyright is
their makers'; they are carried here as pico-speccy carries them.

| Machine | File | Bytes | MD5 | What |
|---|---|---|---|---|
| 16K / 48K Spanish | `es48.rom` | 16384 | `60e1bd8b4d59b55e5b2175126239517f` | 48K ROM with pico-speccy's 48k_es overlay applied |
| 128K Spanish | `es128-0.rom` | 16384 | `c1231a70b8129311216acb7479b031d5` | ROM 0, Investronica's editor (pico-speccy rom128Kspanish.h) |
| 128K Spanish | `es128-1.rom` | 16384 | `4f341936594cbcab75cfa62ce96e3682` | ROM 1, the translated 48 BASIC (128k_es overlay) |
| +2 Spanish | `plus2es-0.rom` | 16384 | `7b92332e5dc791ab75d1a7e97dbf02fb` | ROM 0 (RomPlus2spanish.h) |
| +2 Spanish | `plus2es-1.rom` | 16384 | `a0218949af7ff6fe7acc8d52ae92734a` | ROM 1 (128k_plus2es overlay) |
| TC2048 | `tc2048.rom` | 16384 | `9dd7ecf784a6c04265c073c236f5fadb` | Timex TC2048 |
| Gama 89 | `gama89.rom` | 16384 | `28287c397defff765b39bd0660da6d01` | Didaktik Gama 89 |
| Byte | `byte48.rom` | 16384 | `295773ea298373ff80230d7f48c751a6` | Byte 48K |
| TR-DOS | `trdos-503.rom` | 16384 | `0da70a5d2a0e733398e005b96b7e4ba6` | TR-DOS 5.03 |
| TR-DOS | `trdos-504tm.rom` | 16384 | `f61f9fc0ca97498ff812884b5abbeb03` | TR-DOS 5.04TM |
| TR-DOS | `trdos-505d.rom` | 16384 | `4283ab5829a14bee31fd436989f2f1b2` | TR-DOS 5.05D |
| TR-DOS | `trdos-611e.rom` | 16384 | `116bf9177c846e0dc756b059fcd6a8fe` | TR-DOS 6.11e |
| ZS-1024 | `profrom.rom` | 262144 | `1d0cfcf57739e9c265713f8960018be0` | ProfROM 4.44s, four planes of four ROMs |
| ScorpEvo | `scorpevo.rom` | 262144 | `095ddc547179f55f52127a131c70300b` | ProfROM 4.44 as the Evo runs it |
| KAY-256 | `kay256.rom` | 65536 | `a115a10de8fae68c4b09009f936d76c6` | Nemo KAY-256 |
| KAY-1024 | `kay1024.rom` | 65536 | `b73346ac326ed486c5df4e1ac1f3d012` | Nemo KAY-1024, 2000 |
| KAY-1024 (2010) | `kay1024las.rom` | 65536 | `f84eaa6b31100cbe3f7fc8509369547d` | Nemo KAY-1024, 2002 LAS |
| Phoenix | `phoenix.rom` | 65536 | `bf74a90ac25c5b0e9b7056b7b02e7971` | ZXM-Phoenix BIOS 5.04T |
| ATM-Turbo 1 | `atm1.rom` | 65536 | `c5de7f62d86fa808b8c8150c216f3a82` | BIOS 1.04rs |
| ATM-Turbo 2 | `atm2-106.rom` | 65536 | `29979687d68d0107001edbf369d435de` | BIOS 1.06.02 |
| ATM-Turbo 2+ | `atm2-107.rom` | 65536 | `28ce89a88089417db4d3057de942a1bb` | BIOS 1.07.13 |
| ATM-Turbo 2+ | `atm2-xbios.rom` | 131072 | `323688eecf221d2c6808ad498a88cb22` | xBIOS 1.37 |
| ATM-Turbo 3 | `atm3.rom` | 262144 | `e4a297ab905cd77be3dc486245067aaa` | MSD888's test ROM and xBIOS 1.37 |
| ATM | `atm-font.rom` | 2048 | `bd4e0f6f9177f38a71f081882ba3c454` | The text mode's character generator (SGEN) |
| ALF | `alf.rom` | 32768 | `0f896b0811e6db1cdd72f68760331fbc` | The ALF TV Game system ROM |

## Where they live, which is not where they used to

They are **not** in the SRAM any more. `src/spectrum/spectrum.c` still
pulls them in with `.incbin`, but into `.rodata` -- which in an XIP build
is flash -- and `spec_init()` copies the running machine's set into a
five-slot window in SRAM.

That matters because the bus engine reads the bottom window on every
opcode fetch and an XIP miss there stretches a clock phase. Only what is
*mapped* has to be fast, and only one machine's ROMs are ever mapped. So
the window is eighty kilobytes whatever happens, and adding a machine
costs sixteen kilobytes of flash and nothing at all of the 512 KB. Ten
images in the old arrangement would have been a hundred and sixty.

## Why they are welded into the binary

`src/spectrum/spectrum.c` pulls them in with `.incbin`, the same way
frank-xt8086 embeds GLaBIOS. The card is read now, and they still are: a
machine that cannot start without a file on a card is a machine that
cannot be tested by a script, and one that cannot start *at all* without
one is a board that looks dead when the card is missing.

They cost SRAM rather than flash, because this is a `copy_to_ram` build
— ninety-six kilobytes of it. That is the price of the bus loop never
waiting on XIP.
