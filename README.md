# frank-dino

Firmware for the **dino_z80** board. An RP2350B plays the whole machine
(clock, memory, I/O and video) around a real Zilog Z84C00 sitting in a
socket next to it.

The Z80 is not emulated. It fetches opcodes and drives ~MREQ, ~RD and
~WR, and everything on the other side of those pins is software on the
RP2350B.

Written by Mikhail Matveev, <xtreme@rh1.tech>, https://rh1.tech.
Copyright (c) 2026.

It started as a port of [frank-xt8086](https://github.com/rh1tech/frank-xt8086),
which does the same job for a real Intel 8086. The toolchain, the bench
scripts, the video timing and the overall structure come from there. The
bus engine does not, because the two CPUs have none of the important
signals in common.

![ZX Spectrum 128K, booted on a real Z84C00](docs/screenshots/spectrum128.png)

## What it is

A **ZX Spectrum** and about thirty of its relatives, with sound, storage
and a settings menu. The Z84C00 in the socket runs every instruction; the
RP2350B is everything else the CPU can see.

| | |
|---|---|
| Machines | 16K, 48K, 128K, +2, +2A, +3; Pentagon 128, 512 and 1024SL; Scorpion ZS-256 and ZS-1024 (ProfROM), ScorpEvo; Nemo KAY-256, KAY-1024 (two ROMs), ZXM-Phoenix; Profi 1024 with DS80 and CP/M; ATM-Turbo 1, 2, 2+ (two BIOSes) and 3 (all 4 MB); ZX Evolution BaseConf (EVO Reset Service, the Magic NMI menu, EVO ProfROM, NEO-DOS, 4 MB); Timex TC2048 and TC2068; Didaktik Gama 89; Leningrad-1 and -2; Byte; ALF TV Game. Spanish ROMs for the 48K, 128K and +2 |
| CPU | A real Z84C00 at each machine's own clock (3.5, 3.5469 or 3.528 MHz), paced by the wall clock; 5.3 and 7 MHz on request |
| Timing | Every port and memory handler knows its T-state: ULA contention and the floating bus on the Sinclair machines, a picture built the way the ULA fetched it (multicolour, border effects), Gigascreen with an Auto mode |
| RAM | 128 KB in SRAM, plus up to 1 MB in the 8 MB PSRAM (4 MB on the ATM-Turbo 3 and the Evo) |
| Video | 640x480 over HDMI; ULA+, Gigascreen, the Timex modes, Pentagon 16 colours, Profi 512x240, the ATM's EGA, hires and 80x25 text, the Evo's one-page text mode; six palettes, four scanline strengths and a CRT filter |
| Sound | Beeper, two AYs (TurboSound), TurboSound FM, SAA1099 and the CMS pair, OPL3, YM2413, two SN76489s, Covox, SounDrive, SpecDrum and the Byte's 8253, mixed to stereo PWM |
| Tape | `.tap`, `.tzx`, `.pzx`, `.csw` and `.wav`, played in real time or loaded instantly through a ROM trap; a real cassette player on J8; a worn-tape model; a tape browser; SAVE recorded back to `.tap` |
| Snapshots | `.sna` and `.z80` in, `.z80` out, ten quick-save slots, POKEs, and a page of the CPU's registers, read from the real Z80 by NMI |
| Disks | Beta Disk with five TR-DOS versions: `.trd`, `.scl`, `.fdi`, `.udi`, `.td0` in drive A and three more `.trd` drives read straight from the card; the +3's uPD765 with weak sectors and Speedlock; DivMMC with esxDOS; a Z-Controller; IDE behind NEMO, DivIDE, ATM or SMUC |
| Input | USB keyboard, the serial console, USB gamepads (generic, DualShock 4, DualSense, XInput) with every button mappable, USB mice as a Kempston mouse or the Profi's serial mouse; Kempston, Sinclair, Cursor and Fuller joysticks |
| Other | Mr Gluk's clock with its NVRAM on the card, Interface 2 and ALF cartridges, custom ROMs from `/roms` on the card, settings profiles, `.gz` and `.zip` |

`docs/spectrum.md` covers every machine, every setting and every file
format in detail, including what is deliberately left out.

## Using the board

### Connecting it

| Connector | What goes there |
|---|---|
| J9 | HDMI to a monitor or TV. The picture is 640x480 at 60 Hz; the sound can go over HDMI too |
| J2 | 3.5 mm audio jack, for headphones or an amplifier. S2 routes sound to the HDMI audio and to the on-board speaker |
| J3 | microSD card, FAT32. Games, disk images, ROMs and the settings file live here |
| J5 (USB-C) | Power, and the serial console when S1 points at it |
| J6 (2 x USB-A) | Keyboard, mouse, gamepad or USB stick, through the hub, when S1 points at the hub (USB keyboard build) |
| J8 | A cassette player's earphone output, for loading real tapes |
| J4 | The Raspberry Pi Debug Probe, for flashing and debugging |

There are two builds of the firmware, and S1 must match the one flashed:

- **USB keyboard build** (`USB_KEYBOARD=ON`, the release `.uf2`): S1 points
  the RP2350's USB at the hub, so the keyboard, mouse, pads and USB stick
  on J6 all work. There is no serial console.
- **Console build** (the default for development): S1 points at the USB-C
  socket, and a terminal on the computer is the keyboard. This is the
  build the bench scripts drive.

### First start

Copy your games and disk images onto the microSD card, put it in J3,
connect HDMI and power. The board starts the machine named in
`/frank-dino.cfg` on the card, or a 128K the first time. The ROMs are
built into the firmware, so nothing else has to be on the card.

### Keys

| Key (USB keyboard) | Key (console) | What it does |
|---|---|---|
| F11 | Tab | Opens the file browser; in the browser or menu, goes back to the machine |
| F12 | `\` | Opens the settings menu |
| Pause | | Pauses and resumes the machine |
| Shift | | CAPS SHIFT |
| Ctrl | | SYMBOL SHIFT |
| Cursor keys | Cursor keys | CAPS SHIFT + 5/6/7/8, or the joystick when "Cursor keys as joy" is on |
| Alt | | Fire, when the cursor keys are the joystick |
| Gamepad Start | | Opens the file browser |

Every other key goes to the Spectrum's keyboard matrix. Punctuation is
mapped the way it is printed on a PC keyboard, so typing `"` presses
SYMBOL SHIFT + P.

### The file browser

The browser lists the card (and a USB stick, if one is plugged in).
Up and Down move, Enter opens a folder or loads a file, Backspace goes up
a level, a letter or digit jumps to the next name starting with it, and
F3 searches.

Enter does whatever suits the file:

- `.tap`, `.tzx`, `.pzx`, `.csw`, `.wav`: mounts the tape and, with
  auto-start on, resets the machine and types `LOAD ""`.
- `.trd`, `.scl`, `.fdi`, `.udi`, `.td0`: turns the Beta Disk on, puts the
  disk in drive A and boots it. "Next .trd into" in Storage sends the next
  one to drive B, C or D instead.
- `.dsk`: the +3's drive.
- `.sna`, `.z80`: restores the snapshot.
- `.hdf`, `.mmc`, `.img`: a hard disk (with an IDE interface chosen) or the
  DivMMC card; on the ZX Evolution, its SD card.
- `.rom`, `.dck`, `.scr`: an Interface 2 or ALF cartridge, a Timex dock
  cartridge, a screen.
- `.zip`, `.gz`: opened, and the file inside is loaded.

### The settings menu

F12 (or `\`) opens it. Up and Down move, Enter or Right goes into a
submenu or opens a list of choices, Enter picks, Left goes back, Esc
closes. Changes take effect at once. **Save settings to the card** writes
them to `/frank-dino.cfg`; anything not saved is gone at the next power-on.

- **Machine.** Which machine to be (it restarts), Issue 2 keyboard, the
  Z80's speed, ULA contention, Spanish ROMs, Reset, and which ROM is in
  use. On the ZX Evolution, also the Magic button (NMI) and its raster.
- **Snapshots.** Save a snapshot, ten quick-save slots, and POKE.
- **Audio.** Where the sound goes (jack, HDMI, or the speaker loud),
  master, beeper and AY volume, AY stereo mode, and each extra sound
  device on or off. A test tone checks the outputs.
- **Video.** Beam-accurate picture, palette, scanlines, CRT filter, ULA+,
  Gigascreen and the Timex video modes.
- **Input.** Joystick type and Kempston port, cursor keys as the joystick,
  every gamepad button's mapping (saved and loaded as named pad maps),
  Kempston mouse and its speed, the Gluk clock.
- **Storage.** Tapes (auto-start, real-time play, play/stop/rewind,
  tape wear, a real cassette on J8, saving what the machine SAVEs), the
  Beta Disk (TR-DOS version, drives A to D, eject, save), the +3 drive,
  cartridges, DivMMC/esxDOS, the Z-Controller, the IDE interface, a
  screenshot to the card, and re-reading the card.
- **System.** Settings profiles (save, load, delete), the PSRAM and its
  test, and diagnostics: the Z80's clock, frame timing, headroom, the USB
  keyboard, and a bus recorder.
- **Emulator info.** What is running and how.
- **Restore defaults.**

### Gamepads

A USB gamepad is a joystick by default: the D-pad or left stick moves, A
and B fire. Every button can be mapped to a fire button or to any
Spectrum key under Input > Pad buttons. Start always opens the file
browser, and in the menus A and B choose and Select goes back.

## How it works

Five things here are worth a sentence each, because they are not what an
emulator does.

**Snapshots.** You cannot write a Z84C00's registers from outside it. A
`.sna` or `.z80` is restored by *generating a program that loads them*,
mapping it over the ROM window and letting the CPU run it: the CPU loads
itself and jumps into the snapshot, and the real ROM comes back on the
first fetch after the jump. See `src/spectrum/launch.c`.

**The PSRAM is required.** U9 is soldered down and the firmware relies on
it: disk and tape images, the menus' framebuffer, machines above 128 KB
and the inflate window all live there. Without it, `main()` stops with a
message on the console instead of starting in some reduced form.

**A megabyte of guest RAM on a chip with half of one.** The bus engine
reads guest memory on every cycle, so whatever is *mapped* has to be in
SRAM; a PSRAM miss would stretch a clock phase. A Profi's 1 MB is mostly
not mapped, though. Banks 0-7 stay in SRAM and the rest live in PSRAM,
with one 16 KB staging page in SRAM for whichever high bank is current.
See `spec_bank_ptr()`.

**Sixty-four colours from four levels.** The video is a two-resistor
ladder: two bits per channel. ULA+ asks for sixty-four arbitrary colours.
That works because every Spectrum pixel is doubled into a 2x2 block on
screen, which is exactly one Bayer dither cell, so each colour is
dithered inside its own pixel. Gigascreen uses the same block to show two
screens at once. See `src/drivers/vga/vga.c`.

**Tape and disk paging.** The tape trap needs IX, DE and A from inside
`LD-BYTES`, so it patches a stub into the ROM that pushes them out
through a port. The Beta Disk has no paging port at all: TR-DOS appears
when the CPU fetches an opcode from 0x3Dxx and disappears on the first
fetch above 0x3FFF, and the bus engine's opcode-fetch hook sees exactly
what the interface's address comparator saw. See `src/spectrum/tape.c`
and `src/spectrum/beta.c`.

## Stage 0: proving the bus first

Stage 0 still exists, and is where this started.

![Stage 0 passing on the bench](docs/screenshots/stage0.png)

Build it with `SELFTEST=ON`; the default build boots the Spectrum and
runs only the two non-destructive checks on the way past.

This was **stage 0**: a test firmware. There is no ROM, no emulated
machine, and nothing to boot. Its whole job is to answer one question
before a ZX Spectrum emulator is built on top of it — *does the Z80 work,
and does this board's idea of a bus match the Z80's?*

It runs ten tests on the physical CPU and reports the result over HDMI
and over the console:

| Test | Proves |
|------|--------|
| CPU present | Something is driving ~MREQ, ~RD, ~WR and ~M1 at rest |
| Bus wiring | With the CPU in reset, all 24 bus lines follow a pull and none is shorted to another |
| Reset and first fetch | ~RESET works and the CPU comes up fetching from 0000h |
| Address bus A0-A15 | Thirty targets reached by `JP`, walking ones and zeros |
| Data bus D0-D7 | Twenty values round-tripped out as an operand and back as written data |
| I/O without IORQ | `OUT` and `IN` are recognised from ~MREQ alone |
| Execution | The CPU sums 0..255 and gets 7F80h — flags, branches, 16-bit stores |
| NMI | GP39 vectors the CPU to 0066h |
| INT in IM 1 | GP38 produces an acknowledge cycle and vectors to 0038h |
| Speed | How fast the bus engine can clock the CPU and still be right |

Then it leaves the Z80 running a counting loop and puts the count on the
screen, so a still photograph distinguishes a finished report from a dead
machine.

On the first board built, all ten pass and the Z80 clocks cleanly at
**4.4-4.5 MHz** — comfortably above the 3.5 MHz a ZX Spectrum needs,
which is the question stage 0 existed to answer.

It earned its keep before that, too. The first CPU fitted to this board
failed seven of the ten, and the report named the fault without a scope:
D2-D6 held low past the 200R resistors, and A1 stuck at the CPU's own pin.
A replacement passed everything. The two halves of that diagnosis come
from different tests on purpose — the wiring test runs with the CPU in
reset and can only see faults that persist when its outputs are
high-impedance, and an output driver that fails *while driving* is
invisible to it, which is why the address lines are also checked against
the program counter while the CPU runs.

## Quick start

```sh
make hooks                      # once, per clone
./build.sh                      # build
./swd_flash.sh                  # flash through the Debug Probe on J4
./capture.sh shot               # photograph the screen
./keys.sh --tab                 # open the file browser
./keys.sh --down --enter        # pick something and run it
./keys.sh 'beep 1,0' --enter    # or type at the machine and listen
```

`keys.sh` is the point of the last three: with no keyboard plugged in the
console *is* the keyboard, so the whole machine can be driven from a
script over the same cable that flashes it. "Does it still boot, load a
game and run it" is a question a test can answer with a screenshot.

The browser and menu keys are under [Using the board](#using-the-board).
With a real keyboard (a `USB_KEYBOARD=ON` build) the browser is on **F11**
and the settings on **F12** instead of Tab and backslash, because every
key the Spectrum has belongs to its matrix.

## What the board gives the Z80

| Part | This firmware |
|------|---------------|
| Clock | Generated in software on core 1, edge by edge |
| RAM | Four 16 KB windows; the Spectrum aims them at its eight banks |
| ROM | A window whose write pointer goes to a bit bucket |
| I/O | Two function pointers, re-pointed by whatever machine is running |
| Video | 640x480 at 60 Hz — 80x30 text, or the ULA's screen |
| Sound | Two PWM DACs, left on GP46 and right on GP44, fed 44100 stereo samples a second by the frame loop |
| Storage | microSD on SPI1, and 8 MB of PSRAM for what will not fit in SRAM |
| Console | USB CDC on the USB-C socket. There is no UART; a `USB_KEYBOARD` build has no console at all |

## How the bus engine works

The interesting constraint is that **~WAIT is strapped to VCC**, so the
CPU cannot be stalled. On a CPU with its own oscillator that would mean
every bus cycle had to be answered inside a hard deadline.

It is not, because the clock is ours. Core 1 runs a loop that drives GP28
high, waits, answers whatever the Z80 has asked for, drives it low, waits,
and answers again — two samples per T-state. The Z84C00 is fully static,
so a phase that runs long is just a long phase, and there is no clock
speed at which this becomes *incorrect*. What sets the speed is how long
the answer takes, which is what the speed test measures and what makes the
number it reports the one that matters for stage 1: a ZX Spectrum needs
3.5 MHz.

Two consequences are worth knowing before reading `src/core/z80_bus.c`:

- **The service routine is edge-triggered on the strobes.** The first
  version was level-driven, which is correct for a read — answering twice
  with the same byte is answering once — and wrong for everything else.
  ~WR is low across two samples, so every `OUT` dispatched four times.
- **~IORQ is not connected.** An I/O cycle is inferred from ~RD or ~WR
  active while ~MREQ is idle, and an interrupt acknowledge from ~M1 active
  with ~MREQ never following. Both inferences are tested on the real CPU,
  which is the only place they can be.

`docs/pinout.md` has the rest of the hardware, including what else is not
connected and why the clock is the one signal that goes through a buffer.

## Layout

```
src/
  core/            the pin map, the bus engine, the guest's memory
  test/            the stage-0 suite and the Z80 programs it runs
  spectrum/        the machine: paging, ULA ports, AY, tape, disks, settings
  drivers/vga/     640x480, PIO and DMA out of the R2R ladder
  drivers/audio/   the PWM DAC and its ring buffer
  drivers/sdcard/  SPI1 to J3, and FatFs on top of it
  drivers/psram/   the 8 MB on XIP CS1
  drivers/usbkbd/  TinyUSB as a HID host
  ui/              the report, the file browser and the settings page
  app/             main.c, the linker script, the top-level CMakeLists
boards/            the SDK board header
roms/              the ROM images, and where they came from
docs/              the pin map and the machine
tools/             the attribution checker the hooks and CI share
```

`boards/` sits outside `src/` because it is configuration the build reads,
not code the firmware compiles: `PICO_BOARD_HEADER_DIRS` points the SDK at
it and the SDK expects it at the repository root.

The Z80's test programs are hand-assembled byte arrays in
`src/test/z80_progs.h`, with the listing in the comment beside each one.
That is deliberate: these are the bytes that prove the data bus works, and
an assembler between the listing and the binary is one more thing that can
be the reason a test fails.

## Building

```sh
./build.sh
```

`src/app/CMakeLists.txt` is the top of the build — `cmake -S src/app -B build`
if you would rather drive it directly.

| Variable | Default | What it does |
|----------|---------|--------------|
| `CPU_SPEED` | `504` | System clock, MHz |
| `SELFTEST` | `OFF` | Build the destructive stage-0 suite instead of the machine |
| `USB_KEYBOARD` | `OFF` | TinyUSB as a HID host rather than a CDC console |
| `CLEAN` | `0` | Delete the build tree first |

`CPU_SPEED` sets the ceiling on how fast the Z80 can be clocked, and
nothing else — it cannot make this firmware *wrong*, only slow. 378 MHz
needs only the 1.30 V regulator setting and was the default through stage
0. The Spectrum wants more: the frame loop also renders two AY chips and
pushes 882 samples, and at 378 MHz the clock tuner ran out of headroom
and the machine ran twelve per cent slow. 504 MHz and 1.60 V is what it
takes, and is proven on this board.

## The bench toolset

Three things plug into the board, and there is a script for each.

### SWD — flashing and debugging

A Raspberry Pi Debug Probe on **J4** (pin 1 SWDIO, pin 2 GND, pin 3
SWCLK).

```sh
./swd_flash.sh                  # program, verify, reset, run
./swd_flash.sh --reset-only     # just restart what is already there
./debug.sh                      # build, flash, break at main, run under GDB
make swd-check                  # is the debug port reachable, and how reliably
```

**The reset matters.** `swd_flash.sh` does `program ... verify`, then
`reset halt`, then `resume` — not OpenOCD's own `program ... reset`. That
default does not reliably stop core 1 on this chip, and this image is
`copy_to_ram`: core 0 comes out of reset and starts copying the new image
into RAM while core 1 is still executing the old one out of the same RAM.

It presents as a firmware that hangs in a different place on every build,
with no fault flags set, core 0 healthy, and core 1's program counter
sitting in the bootrom. That is an extremely convincing impression of a
hardware fault. If this firmware ever starts behaving that way again,
check how it was reset before suspecting anything else.

`./flash.sh` is the USB-BOOTSEL fallback: hold S3, press S4, and the board
appears as a drive.

The image is `copy_to_ram`, which confuses software breakpoints set before
the copy runs — `monitor reset halt; load; break main` in that order, as
`debug.sh` does it.

**If OpenOCD says `Error connecting DP: cannot read IDR`,** check the
cable orientation before anything else. J4 has GND in the middle, so a
cable plugged in backwards keeps its ground and swaps the other two —
which looks exactly like an unpowered board. A Debug Probe's own
connectors use the opposite order from J4's (the Pico convention is
SWCLK, GND, SWDIO), so a straight-through cable needs turning round.

### USB — the console

stdio is **USB CDC** on the board's USB-C socket (J5), with S1 set to
device mode. There is no UART on this board: GP44 is the right audio
channel and GP45 the tape input, and nothing brings a UART out.

```sh
./console.sh                    # picocom or screen on the board's CDC port
```

`console.sh` and `keys.sh` find the board by its USB descriptor rather
than taking the first `/dev/cu.usbmodem*`, because a Debug Probe
enumerates a CDC port of its own — and on this board that port is
connected to nothing, so picking it is silent.

A `USB_KEYBOARD=ON` build has no console at all: S1 points the USB
controller at the hub, the firmware is a host, and the screen is the only
report there is. Flash it over SWD.

### HDMI capture — seeing the screen from a script

This board has no VGA connector: the ladder feeds an MS9288A (U16) which
drives J9, so HDMI is the only output. A MACROSILICON MS21xx capture
stick on the far end enumerates as `USB Video` and gives **640x480 at
60 fps** in UYVY — the same mode the firmware emits, so a character cell
survives the round trip and an 80x30 report is legible in a still.

```sh
./capture.sh                    # one frame to out/screen.png
./capture.sh clip 10 out/b.mp4  # ten seconds
./capture.sh view               # live preview
./capture.sh modes              # what the stick will give you
```

This makes "did stage 0 pass" a question a script can answer.

## What is not here

The 7 and 14 MHz turbo modes of the ATM and the Evo: a real Z84C00 on
this bus does not reach them, so the turbo bits are accepted and ignored.
Machines that need a second CPU or a faster one (Spectrum Next, SAM
Coupé, TS-Conf, Scorpion GMX, MB-02+, General Sound) are out of scope.

## Toolchain

- Pico SDK 2.2.0. `sdk_env.sh` finds it — an exported `PICO_SDK_PATH` wins
  only if it actually contains an SDK, because a stale export onto an
  unmounted volume is a confusing way to fail.
- Arm GNU Toolchain 14.x (`arm-none-eabi-gcc`)
- CMake 3.13+
- OpenOCD with `cmsis-dap` and `target/rp2350.cfg`
- `picotool` for the USB path, `ffmpeg` for capture, `picocom` or `screen`
  for the console

## Attribution

This repository's history names the people responsible for it and nobody
else. A co-author trailer naming a tool is not a courtesy — it puts that
tool in GitHub's contributor list, where it stays until the history is
rewritten.

`tools/check-attribution.sh` is the one implementation of that rule, and
the hooks and CI both call it.

```sh
make hooks               # once per clone; also runs the self-test
make check-attribution   # scan what this branch adds over the remote
```

## Author

frank-dino, the dino_z80 firmware, is written by **Mikhail Matveev**,
<xtreme@rh1.tech>, https://rh1.tech. Copyright (c) 2026.

## Credits

Much of the machine above the bus is ported from other people's work, all
of it GPL-3.0 or compatible. Each ported file says so at its top.

- [pico-speccy](https://github.com/drewpo28/pico-speccy) by drew.po28: the
  machine models (Pentagon 512/1024, Scorpion, ProfROM, KAY, Phoenix,
  ATM-Turbo, the ZX Evolution's BaseConf and its AVR, Byte, ALF, Profi
  DS80), the FM and PSG cores, the tape timings, the TD0 decoder, the
  Kempston mouse, SMUC, the tape wear model, the gamepad decoders, the
  palettes and CRT filter, and the menus.
- [ESPectrum](https://github.com/EremusOne/ESPectrum) by Víctor Iborra
  (Eremus) and David Crespo (dcrespo3d), which pico-speccy grew from; and
  before it [ZX-ESPectrum](https://github.com/rampa069/ZX-ESPectrum) by
  Ramón Martinez and Jorge Fuertes, and Pete Todd's
  [paseVGA](https://github.com/retrogubbins/paseVGA).
- [SpeccyP](https://github.com/billgilbert7000/SpeccyP) by billgilbert7000:
  the keyboard's extra keys.
- [MAME](https://github.com/mamedev/mame): fm.cpp, ymf262.cpp and
  ym2413.cpp by Jarek Burczynski and Tatsuyuki Satoh under the FM cores,
  and the Scorpion and ATM hardware.
- [UnrealSpeccy](https://github.com/mkoloberdin/unrealspeccy) by SMT,
  Alone Coder and Deathsoft: the ATM memory manager and the Teledisk loader.
- [Fuse](https://fuse-emulator.sourceforge.net/): the contention patterns
  and the +3's Speedlock trick.
- [ZXMAK2](https://github.com/zxmak/ZXMAK2): the 24LC16 model behind
  SMUC's NVRAM.
- [ZX Evolution](https://github.com/tslabs/zx-evo) by NedoPC: the
  BaseConf FPGA sources the Evo is modelled on, and its ROM.
- [usb64](https://github.com/Ryzee119/usb64) by Ryan Wendland: the XInput
  class driver (MIT).
- [FatFs](http://elm-chan.org/fsw/ff/) by ChaN,
  [pico_fatfs_test](https://github.com/elehobica/pico_fatfs_test) by
  Elehobica (the SD card driver), and [miniz](https://github.com/richgel999/miniz)
  by Rich Geldreich.
- [Pico SDK](https://github.com/raspberrypi/pico-sdk) and
  [TinyUSB](https://github.com/hathach/tinyusb).
- Sinclair Research: the menus' font is the 48K ROM's own.

The ROM images belong to their owners; `roms/README.md` says where each
came from.

## Licence

GPL-3.0-or-later. Copyright (c) 2026 Mikhail Matveev.

The 8x16 font is carried over from frank-xt8086.
