# frank-dino

Firmware for the **dino_z80** board: an RP2350B acting as the entire
machine — clock, memory, I/O, video — for a real Zilog Z84C00 sitting next
to it in a socket.

The Z80 is not emulated. It fetches, it drives ~MREQ and ~RD and ~WR, and
everything on the other side of those pins is software on the RP2350B.

Ported from [frank-xt8086](https://github.com/rh1tech/frank-xt8086), which
does the same thing for a real 8086. The toolchain, the bench scripts, the
video timing and the structure are that firmware's; the bus engine is not,
because the two CPUs share none of the signals that matter.

![ZX Spectrum 128K, booted on a real Z84C00](docs/screenshots/spectrum128.png)

## What it is now

A **ZX Spectrum**, in four flavours, with sound, storage and a settings
page — all of it executed by a Z84C00 in a socket, with the RP2350B
being everything else the CPU can see.

| | |
|---|---|
| Machines | 48K, 128K, +2, Pentagon 128 — chosen at runtime, saved to the card |
| CPU | A real Z84C00 at **3.5469 MHz**, tuned against the wall clock; 5.3 and 7 MHz on request |
| RAM | 128 KB, eight banks, port 0x7FFD |
| ROM | The stock Sinclair pair, Amstrad's +2 ROM 0, the Pentagon's, and TR-DOS 5.04T |
| Video | The ULA's 256x192 and border, doubled into 640x480 over HDMI; optional scanlines |
| Sound | Beeper, AY-3-8912, TurboSound (two of them), Covox and SounDrive, mixed to a 44.1 kHz PWM DAC on GP46 |
| Input | The full matrix over USB or the console; Kempston, Sinclair 1 and 2, Cursor and Fuller joysticks |
| Storage | microSD: a file browser, `.tap`, `.tzx`, `.sna`, `.z80`, `.trd`, `.scl` and `.scr` |
| Disks | A Beta Disk interface and a WD1793, with TR-DOS in ROM |
| Memory | 8 MB of PSRAM on XIP CS1, holding the disk and tape images |

Three of those are worth a sentence each, because they are not what an
emulator does.

**Snapshots.** You cannot write a Z84C00's registers from outside it, so
a `.sna` or a `.z80` is restored by *generating a program that loads
them* and mapping it over the ROM window: the CPU loads itself and jumps
into the snapshot, and the real ROM comes back on the first fetch after
the jump. See `src/spectrum/launch.c`.

**Tape.** The same problem, once more: a `.tap` is served by trapping the
ROM's `LD-BYTES`, and the trap needs IX, DE and A — which it gets by
patching a stub into the ROM that pushes each of them out through a port
this firmware is listening on. See `src/spectrum/tape.c`.

**Disks.** The Beta Disk's ROM paging has no port at all: TR-DOS replaces
the BASIC ROM when the CPU fetches an opcode from 0x3Dxx and disappears
on the first fetch at 0x4000 or above. The bus engine's opcode-fetch hook
sees exactly what the interface's address comparator saw. See
`src/spectrum/beta.c`.

`docs/spectrum.md` has the whole of the machine: what each setting does,
which parts of each file format are supported, and what is deliberately
out of scope.

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
./swd_flash.sh                  # flash through the Debug Probe on J5
./capture.sh shot               # photograph the screen
./keys.sh --tab                 # open the file browser
./keys.sh --down --enter        # pick something and run it
./keys.sh 'beep 1,0' --enter    # or type at the machine and listen
```

`keys.sh` is the point of the last three: with no keyboard plugged in the
console *is* the keyboard, so the whole machine can be driven from a
script over the same cable that flashes it. "Does it still boot, load a
game and run it" is a question a test can answer with a screenshot.

Inside the browser: the arrows move, Enter opens, Backspace goes up, any
letter or digit jumps to the next entry beginning with it, backslash
opens the settings page, and Tab goes back to the machine.

## What the board gives the Z80

| Part | This firmware |
|------|---------------|
| Clock | Generated in software on core 1, edge by edge |
| RAM | Four 16 KB windows; the Spectrum aims them at its eight banks |
| ROM | A window whose write pointer goes to a bit bucket |
| I/O | Two function pointers, re-pointed by whatever machine is running |
| Video | 640x480 at 60 Hz — 80x30 text, or the ULA's screen |
| Sound | A PWM DAC on GP46, fed 44100 samples a second by the frame loop |
| Storage | microSD on SPI1, and 8 MB of PSRAM for what will not fit in SRAM |
| Console | UART0 on J1, and USB CDC, both at 115200 |

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

A Raspberry Pi Debug Probe on **J5** (pin 1 SWDIO, pin 2 GND, pin 3
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
cable orientation before anything else. J1 and J5 are both three-pin
JST-SH headers a few centimetres apart with GND in the middle, so a cable
plugged in backwards keeps its ground and swaps the other two — which
looks exactly like an unpowered board. A Debug Probe's own connectors use
the opposite order from these (the Pico convention is SWCLK, GND, SWDIO),
so a straight-through cable needs turning round.

### UART — the console

stdio is **UART0 on J1** at 115200 8N1: pin 1 TX, pin 2 GND, pin 3 RX.

```sh
./console.sh                    # picocom or screen on the probe's CDC port
```

The Debug Probe carries a UART alongside CMSIS-DAP and enumerates it as a
CDC port, so one cable does both jobs — probe UART TX to J1 **pin 3**,
probe UART RX to J1 **pin 1**, grounds common. Both ends call their own
pin 1 "TX", so this one needs crossing.

Note that GP45 is shared with the tape input. A tape lead and a console
cable cannot both be attached.

### HDMI capture — seeing the screen from a script

This board has no VGA connector: the ladder feeds an MS9288A (U13) which
drives J10, so HDMI is the only output. A MACROSILICON MS21xx capture
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

## What comes next

The machine runs, loads and plays. What it has not got:

- **Custom tape loaders.** Speedlock, Alkatraz and every turbo loader
  never call the ROM's `LD-BYTES`, so the trap never sees them. Reaching
  them needs the tape replayed as a real signal on the ULA's EAR bit with
  the original timings, which is a different feature and a larger one.
- **Writing disks back to the card.** A `.trd` can be written to, but the
  changes live in the PSRAM copy and go when the power does.
- **Contention and the floating bus.** Port 0xFF returns 0xFF here.
  Reproducing the real thing needs the video and the CPU to share a
  clock, which on this board they do not.
- **The tape input on GP45.** The hardware is there; the pin is the
  console's RX, and they cannot both have it.

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

## Licence

GPL-3.0-or-later.

The 8x16 font is carried over from frank-xt8086.
