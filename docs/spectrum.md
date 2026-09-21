# The machine

What the RP2350B pretends to be, around a Z84C00 that is not pretending
anything. This is the reference for the four machines, every setting, and
what each file format does and does not get you.

The rules — which bits of which port do what, how the screen is laid out,
how many T-states a frame is — are the ones [pico-speccy][ps] and
[ESPectrum][esp] established. The code is not theirs: theirs is built
around ticking an emulated Z80 through twenty different machines, and
this is built around answering a real one. Where a sequence *is* theirs
— the PSRAM bring-up, the `.z80` page tables — the file says so.

[ps]: https://github.com/xrip/pico-speccy
[esp]: https://github.com/EremusOne/ZX-ESPectrum-IDF

## The four machines

| | ROM 0 | ROM 1 | T-states | Notes |
|---|---|---|---|---|
| **48K** | Sinclair 48K BASIC | — | 69888 | One ROM, no paging port |
| **128K** | Sinclair 128 editor | 48K BASIC | 70908 | Port 0x7FFD, AY-3-8912 |
| **+2** | Amstrad's | 48K BASIC | 70908 | The 128K with a different menu |
| **Pentagon 128** | Pentagon's | 48K BASIC | 71680 | No contention; TR-DOS is native |

The T-state counts are not decoration. A 128's ULA runs 311 lines to the
48's 312 and the line is a T-state longer; a Pentagon's is longer again.
Getting one wrong does not break anything visibly — it makes the machine
run at the wrong speed and every piece of music play flat.

Switching machines restarts the machine. The choice is saved with
everything else, so the board comes up as whatever it was last set to.

### What none of them have

**Contention.** A real ULA steals cycles from the CPU when it is drawing
and the CPU touches contended memory. Here the CPU has its own clock and
the video is a DMA channel; they share nothing to be contended over.
Programs whose timing depends on it — a handful of demos, the tighter
multicolour effects — will run, and run wrong.

**The floating bus.** An unclaimed `IN` returns 0xFF. On real hardware it
returns whatever the ULA was fetching at that instant, which a few
loading effects depend on and which needs the same shared clock.

## Settings

Backslash in the file browser opens the settings page. Arrows move, left
and right change a value, Enter acts on a row, S saves. Everything lives
in `/frank-dino.cfg` on the card as `key value` text — so it survives a
firmware update that adds a field, an unknown key is skipped rather than
throwing the file away, and a setting that makes the machine unbootable
can be fixed with a text editor.

### Machine

| Setting | What it does |
|---|---|
| **Machine** | 48K, 128K, +2 or Pentagon 128. Restarts the machine. |
| **Issue 2 keyboard** | Bit 6 of port 0xFE reads back the last write: bit 4 on an issue 3, bits 3 *and* 4 on an issue 2. A handful of 48K games test this to tell the boards apart, and one written for an issue 2 reads the key rows as permanently pressed on an issue 3. |
| **Z80 speed** | 3.5, 5.3 or 7 MHz. Not a trick: the clock is generated here edge by edge and the part is a Z84C00-**10**. |

Turbo keeps the frame at a fiftieth of a second and gives it
proportionally more T-states, so a game that paces itself off the
interrupt runs at the right speed and thinks faster, while one that
counts instructions runs at double speed. Music is unaffected — the AY
keeps its own 1.7734 MHz whatever the CPU is doing. The page reports the
frame rate it measured afterwards, because asking for 7 MHz does not make
it so: past some speed the RP2350 cannot generate the edges and answer
the cycles, the pace bottoms out at zero, and the machine runs *slow*.
That failure is invisible from the inside, which is why the number is
printed.

### Audio

Everything is mixed to one 44100 Hz PWM DAC on GP46, sampled 882 times
per frame by the frame loop.

| Setting | What it does |
|---|---|
| **Master / Beeper / AY volume** | 0-10 each. The beeper sits at about a quarter of full scale, which is the compromise every emulator makes: on real hardware it is much louder than the AY, and reproducing that puts every 128K tune under its own sound effects. |
| **AY-3-8912 on a 48K** | Sinclair's 48K had none; half the clones and most add-on boards put one on the same two ports, and a fair amount of 48K music expects to find it. |
| **TurboSound** | A second AY behind the same pair of ports, selected by writing 0xFF or 0xFE to 0xFFFD. On by default: software that does not know about it never writes those values, and the 128K ROM only ever names registers 0-13. |
| **Covox** | An 8-bit DAC on port 0xFB. |
| **SounDrive** | Four of them, on 0x0F, 0x1F, 0x4F and 0x5F. |

Covox and SounDrive are off by default, because a machine that never had
one must not turn stray writes to those ports into noise.

### Video

**Scanlines** dims every second output line. It is free: the picture is
already doubled, so the effect is a second palette one step darker chosen
on the odd line of each pair. With two bits per channel there is exactly
one step to give, which is why it is a toggle and not a strength.

### Input

| Setting | What it does |
|---|---|
| **Joystick** | None, Kempston, Sinclair 1, Sinclair 2, Cursor or Fuller. |
| **Kempston port** | 0x1F, or 0xDF for the interfaces that used it. |
| **Cursor keys as joystick** | The four arrows drive the selected joystick instead of being CAPS SHIFT and a digit. |

Nothing about a Spectrum joystick is standard, because the machine has no
joystick port: every interface is somebody's add-on and they agree about
nothing. Kempston has its own port, active high. The two Sinclairs *are*
keyboard rows — a game reading one is reading the keyboard — and the
Cursor interface picked a third set of keys. Fuller used yet another port
with the bits in a different order and inverted. So the state is held
once and each interface is a way of presenting it; the keyboard-wired
ones are folded into the matrix at read time rather than pressed into it,
so a real key and a direction cannot cancel each other out.

### Storage

| Setting | What it does |
|---|---|
| **Auto-start tapes** | After mounting a `.tap`, cold-start the machine and type `LOAD ""` into it. Also types a disk's start-up command. |
| **Eject tape / disk** | Enter on the row. |
| **Beta Disk (TR-DOS)** | The interface and its ROM. Turned on automatically by mounting a disk. |

The Beta Disk is off by default because it is not free: its ROM replaces
the BASIC ROM whenever the CPU fetches from 0x3Dxx, which means every
opcode fetch has to be inspected for it.

### Diagnostics

With a physical CPU there is no debugger that can see inside the machine,
and "the screen is wrong" is otherwise the whole of the evidence. These
exist because that was not enough.

| Row | What it tells you |
|---|---|
| **PSRAM** | How much the probe found, and a read-write test over 4 MB. |
| **Z80** | The address of the last opcode fetch, the pace, and the running fetch count. A tight loop repeats one address; a stopped CPU does not move between two visits. |
| **last FDC command** | The command, the port it came in on, the track, sector and side, and the counts of reads, writes and not-founds. |
| **Record the bus** | Two stores per bus cycle into a 256-entry ring. Turn it on, leave the menu, come back and **Show the bus trace**. |

Between them they found the two bugs in this firmware that looked most
like failing hardware: the index pulse, below, and the pace.

### The pace, and the floor under it

Pace is how long the bus engine waits after a clock edge before it
samples the Z80's pins. Below some value it samples them too early and
reads the previous cycle's address — a firmware limit, not a silicon
one, and stage 0's speed ramp has always measured it.

The clock tuner adjusts that padding until a frame takes 20 ms, and it
had no floor. Every time the frame loop got heavier — two AY chips, then
per-source volumes, then an opcode-fetch hook — it paid for the extra
work by taking padding away from the CPU's clock, and eventually it
bought a frame rate of exactly 50 Hz with an engine that was reading the
wrong address. The arithmetic cannot tell "the machine is slow" from
"the machine is wrong". What you see is a Spectrum that boots, passes
both self-tests, keeps perfect time and puts noise on the screen.

There is a floor now, and it is *measured*: at boot, before the machine
starts, the CPU runs a program whose answer is known at falling paces
until the answer stops being right, and the floor goes four steps above
that. It has to be measured rather than assumed because the boundary
moves — with the system clock, with the size of the service routine,
with what the frame loop does around it, and with how warm the board is.
It was seen at 4, 6, 8 and 10 in a single evening.

The tuner stops at the floor, so a machine that cannot hold 50 Hz
reports a low frame rate instead of quietly corrupting itself. The Z80
row shows the pace it settled on; on this board it is around 18, with
the floor near 14.

## File formats

The browser reads the card, sorts directories first and then by name, and
opens whatever is selected.

### `.sna` — snapshots, 48K and 128K

Twenty-seven bytes of registers and then memory. The 48K form has no
program counter at all: the snapshot was taken inside an interrupt, so PC
is on the stack. The 128K form keeps it in a footer along with the paging
latch.

Restoring it is the interesting part. You cannot write a Z84C00's
registers from outside it, so the firmware *generates a program that
loads them* — every value an immediate operand, AF and AF' popped off a
stack pointed at the page itself — maps that program over the ROM window,
and lets the CPU load itself. The last instruction is a jump to the
snapshot's PC, and the real ROM comes back on the first opcode fetch
after it, which is the moment nothing of the trampoline is needed any
more. `src/spectrum/launch.c`.

### `.z80` — snapshots, v1 to v3

The format everything is archived in, and the one with the most history
showing: the version is inferred from a program counter of zero, the
extension block's length distinguishes v2 from v3, and byte 34 names a
machine differently depending on which of the two it is. Memory is ED ED
run-length compressed except where a block calls itself 0xFFFF long.

Machines this firmware has not got are mapped to the nearest one it has
and the status line says so. A snapshot is RAM and registers: a Scorpion
dump runs on a 128K, slightly wrongly, which beats refusing it.

### `.tap` and `.tzx` — tapes

A `.tap` is the bytes a tape carried with the timing thrown away, so it
cannot be replayed as a signal — it has to be handed to whatever asked
for it. What asks is `LD-BYTES` at 0x0556.

On real hardware that trap needs an extra step. An emulator reads IX, DE
and A out of its CPU struct; there is no such struct here. So the CPU is
made to hand them over: the ROM window is replaced by a copy with a short
routine patched in at 0x0556, and that routine pushes each register out
through a port this firmware is listening on, waits for the verdict with
an `IN`, and returns the carry flag the ROM expects.

A `.tzx` is a `.tap` with the timing put back, and then some: data blocks
that carry their own pilot and bit timings, but also pure tones, raw
pulse trains, pauses, loops, jumps, selection menus and several kinds of
text. Two record types — 0x10 and 0x11 — have a payload that is exactly a
`.tap` block, so a `.tzx` is converted to one on the way in and
everything else is stepped over. A tape whose loader really is a pulse
train converts to nothing and is refused by name.

**Custom loaders do not work.** Speedlock, Alkatraz, most commercial
protection and every turbo loader never call 0x0556 and never see any of
this. They need the tape replayed as a real signal on the ULA's EAR bit
with the original timings.

### `.trd` and `.scl` — disks

A `.trd` is a raw dump of a TR-DOS disk: two sides, forty or eighty
tracks, sixteen sectors of 256 bytes, and nothing else. A `.scl` is the
same disk with the empty space left out — a signature, a count, one
fourteen-byte directory entry per file and then the files' sectors end to
end — and is converted to a `.trd` on the way in, which means writing the
catalogue and the volume descriptor that a `.scl` leaves out.

Mounting either turns the Beta Disk on and restarts the machine with the
TR-DOS ROM in the bottom window, the way a Beta Disk 128 comes up. A disk
with a `boot` file starts itself. One without — which most single-file
`.scl` images are — gets `RUN "NAME"` typed into it, built from the first
catalogue entry.

The controller is modelled by result rather than by timing: the sectors
are in memory and go over as fast as the Z80 asks, because the Z80 is
real and there is nothing to synchronise it to. Protection that depends
on flux, weak sectors or non-standard formats will not work.

Writes go to the image in PSRAM and are not written back to the card.

#### The index pulse

Worth recording, because it looked exactly like failing memory.

Bit 1 of a type I status is the index hole, and TR-DOS reads it twice
over: to decide whether there is a disk in the drive at all — it gives up
with "No disk" if the pulse never comes — and to time operations out by
counting revolutions. So it has to be a *clock*: 300 rpm, a 4 ms pulse
every 200 ms. The first version fired every eighth status read, which
satisfies the first use and is nonsense as the second.

And it is only bit 1 **after a type I command**. After a read or a write
that same bit is DRQ. A phantom pulse offered to a transfer loop polling
for DRQ is an extra byte read out of a finished sector and dropped into
the middle of whatever was loading — which showed up as a boot program
that drew its menu correctly about half the time and drew noise the rest.

### `.scr` — a screen

Six and three-quarter kilobytes straight into the display bank. Not a
program, so nothing is launched: the picture is written and the machine
goes on running whatever it was. The cheapest possible end-to-end test
that the card, the paths and the ULA renderer all agree.

## Where the memory goes

The RP2350B has 512 KB of SRAM, and by the time the guest's 128 KB, five
ROMs, a patched ROM page, two video buffers and the code itself are in it
there is not much left. So anything large that is *not* on the Z80's
critical path lives in the 8 MB of PSRAM on XIP CS1 instead: the tape
image and the disk image, both read with the CPU stopped inside a bus
cycle.

Nothing the bus engine touches while the clock is running may go there. A
PSRAM read is a cache miss away from a microsecond, and a microsecond is
three T-states at 3.5 MHz.
