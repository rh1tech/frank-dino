# dino_z80 — RP2350B pin map

Read off `dino_z80.kicad_pcb`, not inherited from frank-xt8086.
`src/core/dino.h` is the machine-readable copy of this table and
`boards/frank_dino.h` is the SDK-facing one; if the three ever disagree,
the PCB wins.

## The Z80's bus — GP0-GP29

Fixed by the CPU (U3, a Z84C00 in a 40-pin socket). Nothing here is a
choice, and nothing else may use these pins.

| GPIO  | Net      | Z80 pin | Direction | Notes |
|-------|----------|---------|-----------|-------|
| 0-15  | A0-A15   | 30-40, 1-5 | in     | Direct, no series resistors |
| 16-23 | D0-D7    | 14, 15, 12, 8, 7, 9, 10, 13 | bidir | Through R9-R16, 200R each |
| 24    | ~MREQ    | 19      | in        | |
| 25    | ~RD      | 21      | in        | |
| 26    | ~WR      | 22      | in        | |
| 27    | ~M1      | 27      | in        | Through R8, 1K |
| 28    | CLK      | 6       | out       | Through U1, a 74HCT125 on 5V |
| 29    | ~RESET   | 26      | out       | R7 10K pull-**down**, and D1 through R1 |
| 38    | ~INT     | 16      | out       | R5 10K pull-up |
| 39    | ~NMI     | 17      | out       | R6 10K pull-up |

Every one of those is inside GP0-GP31 except the two interrupt lines, and
that is worth knowing rather than noticing: one 32-bit read of SIO's
`GPIO_IN` captures the whole bus at once, and the data lines are driven
and released through the 32-bit `SET`/`CLR`/`OE` registers. The bus engine
in `src/core/z80_bus.c` is built on it.

### Why the clock goes through a buffer and nothing else does

The Z80 runs on 5V (U3 pin 11 is VBUS). Its ordinary inputs are
TTL-compatible — VIH is 2.2V — so ~RESET, ~INT and ~NMI are driven
straight from 3.3V GPIOs and that is in spec.

CLK is the exception. The Z80's clock input is the one pin that wants a
CMOS level, near VCC, and 3.3V will not reliably reach it. U1 is a
74HCT125 powered from 5V: HCT input thresholds are TTL, so a 3.3V GPIO
drives it happily, and its output swings to 5V. Only channel 1 is used;
the other three have their inputs grounded and their outputs disabled.

R4 is a 10K pull-down on the GPIO side, so the clock is defined low before
the firmware claims the pin.

### ~RESET is pulled down, not up

R7 is 10K to ground, which means the Z80 is held in reset from power-on
until the firmware drives GP29 high. That is the right way round: a board
that has just been flashed, or whose firmware has faulted, leaves the CPU
stopped rather than running on a bus nobody is answering. D1 is on the
same net, so the board says which state it is in without a screen.

### What is not connected

| Z80 pin | Signal | State |
|---------|--------|-------|
| 20 | ~IORQ | not connected |
| 28 | ~RFSH | not connected |
| 18 | ~HALT | not connected |
| 23 | ~BUSACK | not connected |
| 24 | ~WAIT | strapped to VCC |
| 25 | ~BUSRQ | strapped to VCC |

Two of these shape the whole firmware.

**~IORQ absent** means an I/O cycle cannot be seen directly. It does not
need to be. The Z80 holds ~MREQ high for the whole of an I/O cycle, so
"~RD or ~WR active while ~MREQ is idle" is an I/O cycle and nothing else
is. Interrupt acknowledge is the same inference one step further: ~M1
active with ~MREQ never following it. Both are implemented in
`bus_service()` and both are tested on the real CPU — see the "I/O without
IORQ" and "INT in IM 1" tests in `src/test/selftest.c`, which exist
precisely because this is an inference rather than a wire.

**~WAIT strapped high** means the CPU cannot be stalled, so every bus
cycle must be answered within the clock it is given. On a CPU with its own
oscillator that would be a hard timing constraint. Here the clock is
ours — generated edge by edge in software on core 1 — and a Z84C00 is
fully static, so the constraint becomes "do not clock it faster than the
answer takes". That is a throughput question, and `test_speed()` measures
where the answer is.

**~HALT absent** means a halted CPU and a wedged one look identical from
this side. That is why every test program ends in a two-byte spin rather
than a HALT: a spin keeps fetching from an address the trace can show.

## Everything above GP29

| GPIO | Function | Part |
|------|----------|------|
| 30 | Blue, LSB (806R) | R64 → R74 75R, C69 → U16 pin 39 |
| 31 | Blue, MSB (402R) | R65 |
| 32 | Green, LSB (806R) | R66 → R73 75R, C70 → U16 pin 44 |
| 33 | Green, MSB (402R) | R67 |
| 34 | Red, LSB (806R) | R68 → R70 75R, C71 → U16 pin 48 |
| 35 | Red, MSB (402R) | R69 |
| 36 | HSYNC | R75 → U16 pin 34, and TP7 |
| 37 | VSYNC | R76 → U16 pin 35, and TP8 |
| 38 | ~INT | the Z80's, see above |
| 39 | ~NMI | the Z80's, see above |
| 40 | SD DAT0 / MISO | J3 microSD, SPI1 |
| 41 | SD CD/DAT3 / ~CS | J3 |
| 42 | SD CLK / SCK | J3 |
| 43 | SD CMD / MOSI | J3 |
| 44 | Audio, **right** (PWM_R) | R18/C6 → U4B |
| 45 | Tape input (TAPE_IN) | JP7: Q4/R63 or U15, from J8 |
| 46 | Audio, **left** (PWM_L) | R17/C5 → U4A |
| 47 | PSRAM ~CS | U9 ESP-PSRAM64H, XIP CS1, R39 10K pull-up |

Note the two differences from frank-xt8086's map, both of which would be
silent faults rather than build errors:

- **GP38/GP39 are ~INT and ~NMI, not I2C.** On xt8086 they are the
  DS3231's bus. This board has no RTC, and a board header that declared
  I2C on them would invite an SDK helper to drive two of the CPU's inputs.
- **GP44/GP45 are not a UART.** On xt8086 they are the console. Here
  GP44 is the right audio channel and GP45 the tape input, and there is
  no UART header at all — the console is USB only. `boards/frank_dino.h`
  declares no default UART for that reason.

## Audio

Stereo, from two PWM pins that are not neighbours: **GP46 is left and
GP44 is right**, with the tape input between them. They are slices 11
and 10 of the PWM, both on channel A, so the firmware drives them with a
DMA pair each, paced by one shared timer so the two sides stay locked.

Each pin goes through a single-pole low-pass — 1K and 10 nF, about
16 kHz (R17/C5 left, R18/C6 right) — into half of U4, an LM358 wired as
a follower on 5V. The follower outputs, AUDIO_L and AUDIO_R, go three
ways:

| Destination | How | Notes |
|---|---|---|
| J2, the 3.5 mm jack | Always: 10 µF and 402R per side (C13/R23, C14/R24) | Headphones or a line input |
| U16, the MS9288A's audio inputs | S2 switches 1 and 2, or JP2/JP3 bridged | 1 µF and 10K per side; a line input, which wants about a third of full scale |
| U6, a PAM8403, and LS1 | S2 switch 3, or JP4 bridged, enables it | R25/R26 sum the two sides to one mono speaker |

Nothing reports which of these are switched in, which is why the menu's
**Output level** setting exists: full scale for headphones and the
speaker, a third of it for HDMI.

## Tape input

J8 is a 3.5 mm jack; its tip goes through C62/C63 to two alternative
shapers, and **JP7** picks which one drives GP45:

- **Q4** (BC850), its collector pulled up by R63 — TAPE_Q.
- **U15**, a CD4069UB with one inverter biased as a linear amplifier —
  TAPE_U.

With JP7 open GP45 is connected to nothing, which is the case the tape
input's pin probe exists to report.

## Colour depth

Two bits per channel, from a two-resistor ladder per channel. Sixty-four
colours. The firmware's sixteen-colour table uses `10` rather than `01`
for a lit low-intensity channel — two thirds of full scale, not one third.
That is not a style choice: with `01` the whole palette comes off the
capture stick uniformly dim, which frank-xt8086 found the expensive way.

## Video path

There is **no VGA connector on this board**. The ladder drives U16, an
MS9288A VGA-to-HDMI converter, and the MS9288A drives J9. HDMI is the
only output there is, which makes the capture stick the only way to see
the screen and `capture.sh` the way to do it from a script.

Separate H and V sync, so there is nothing to fold onto green.

## QSPI

Flash (U8, W25Q128JVPIQ, 16 MB) on CS0, PSRAM (U9, ESP-PSRAM64H, 8 MB) on
CS1 selected by GP47. Stage 0 uses neither beyond the flash the image is
programmed into: the guest's 64 KB lives in on-chip SRAM, because the Z80
cannot address more and the read path is on every bus cycle.

## Debug and console

| Header | Pin 1 | Pin 2 | Pin 3 |
|--------|-------|-------|-------|
| J4 | SWDIO | GND | SWCLK |

That is the only debug header. **There is no UART**: the console is USB
CDC on J5, the USB-C socket, with S1 pointing U2 (a TS3USB221) at it. S1
the other way gives the RP2350's USB to U5, the hub behind J6's two USB-A
ports — which is what a `USB_KEYBOARD` build needs, and why that build
has no console at all.

A Raspberry Pi Debug Probe's own connectors are wired the other way round
from J4 — the Pico convention is SWCLK, GND, SWDIO — so a
straight-through cable swaps SWDIO and SWCLK here, keeps its ground, and
fails exactly the way an unpowered board does: `Error connecting DP:
cannot read IDR` from OpenOCD.

## A note on the address bus

A0-A15 run directly from the Z80's 5V outputs to RP2350B inputs with no
series resistors and no level shifting; only D0-D7 have the 200R of
R9-R16. A Z84C00 at 5V drives its outputs close to the rail, which is
above the RP2350's absolute maximum of IOVDD + 0.3V.

This is the board as built and it works — it is the same arrangement much
of the Murmulator family uses, and the pad clamps plus the trace impedance
keep the current small. It is written down here because it is the kind of
thing that is obvious on a schematic and invisible in firmware, and
because if a revision of this board ever adds sixteen resistors, this is
the paragraph that explains why.
