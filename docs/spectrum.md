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

## The machines

| Family | Machines | RAM | Notes |
|---|---|---|---|
| Sinclair 48K | 16K, 48K (Sinclair or Spanish ROM) | 16-48 KB | One ROM, no paging port; contended, floating bus |
| Clones of the 48K | Timex TC2048, Didaktik Gama 89, Leningrad-1 and -2, Byte | 48 KB | Their own ROMs; the TC2048 has the SCLD's video, the Byte an 8253 for sound, the Leningrads a short ~INT |
| Sinclair 128K | 128K, +2 (Sinclair or Spanish ROMs) | 128 KB | #7FFD; contended, floating bus |
| Amstrad | +2A, +3 | 128 KB | #1FFD and the all-RAM modes; the +3 has the uPD765 |
| Pentagon | 128, 512, 1024SL | up to 1 MB | 71680 T-states, no contention, TR-DOS native; the 1024SL's #EFF7, cache page and 16 colours |
| Scorpion | ZS-256, ZS-1024 (ProfROM 4.44), ScorpEvo, Nemo KAY-256 / KAY-1024 / KAY-1024 (2010), ZXM-Phoenix | up to 1 MB | #1FFD in each one's own shape; ProfROM's four planes |
| Profi | Profi 1024 | 1 MB | #DFFD; DS80 512x240 and CP/M mode |
| ATM-Turbo | 1, 2, 2+ (BIOS 1.07.13 or xBIOS 1.37), 3 | 1 MB | The memory manager in every window; EGA, hires and text modes; the 3 has all 4 MB |
| Timex | TC2068 | 48 KB | The SCLD, the dock and the EX-ROM |
| ALF | ALF TV Game | 128 KB | Banked cartridges |
| ZX Evolution | BaseConf | 4 MB | The 2+'s manager with the Evo's ports; EVO Reset Service, its Magic NMI menu in the one-page text mode, EVO ProfROM, NEO-DOS; four rasters |

Every machine runs at its own clock and frame length — a 128's ULA runs
311 lines to the 48's 312 and the line is a T-state longer; a Pentagon's
is longer again — and is paced against the wall clock, so it plays in
tune whatever it is doing.

Turbo bits (the Pentagon 1024SL's #EFF7, the KAY's #1FFD, the ATM's #77)
are latched and ignored: the clock is a real Z84C00's and this board does
not reach 7 MHz.

Not here, because they need more than one Z80 or a faster one: the
Spectrum Next, SAM Coupe, TS-Conf, Scorpion GMX, MB-02+ and General
Sound.

### The ZX Evolution

BaseConf, the Evo's standard FPGA configuration, is an ATM-Turbo 2+
memory manager with ports of its own (`src/spectrum/atm.c`, after
pico-speccy's EvoBase.cpp and the base_trdemu FPGA sources). It starts in
EVO Reset Service, whose menu leads to NEO-DOS, BASIC 48 and 128, and —
with **G** — to EVO ProfROM after the next reset; ERS keeps its settings
in the Gluk clock's cells #E7-#EF, so they are saved to `/cmos.nvr`.

- **Magic button (NMI)**, in Settings > Machine: ERS's Magic Service, in
  the one-page 80x25 text mode. The NMI is taken with the next frame, as
  on the board, and RAM page #FF is in window 0 from the fetch at #0066
  until two M1s after OUT (#BE).
- **Evo raster**: Pentagon (71680 T-states), 60 Hz (58688), 48K (69888)
  or 128K (70908), the last two with contention — by address, #4000 and
  on the 128K raster #C000 with #7FFD D0, whatever the manager has there.
- The font RAM: #BF D2 lets every memory write land in the character
  generator. ERS reloads it at every reset with a KOI-8 font, so Magic's
  frame characters come out as Cyrillic letters, as they do with that font.
- The AVR answers its version window (#F0-#FF) with this firmware's name
  and build date; the PS/2 scancode log is there but always empty, so the
  Evo-only programs that read keys from it (avrconf) see none.
- The NEMO IDE moves whole words through #10 alone, as Evo ProfROM wants.
- 7 and 14 MHz (#77 D3, #EFF7 D4) are latched and ignored, and the drive
  emulator's page-#FE trap is modelled but ERS's virtual drives have not
  been tried.

### Timing

**Contention.** The 48K, 128K, +2, +2A and +3 lose time to their ULA as
the real ones did: a contended access moves the frame's T-state on with
no clock edges, which is exactly what the ULA's stopped clock looks like
to a static CPU. The Fuse patterns, at the Fuse T-states.

**The floating bus.** An unclaimed `IN` on a 48K or 128K returns the
byte the ULA is fetching at that T-state, in its order — bitmap,
attribute, bitmap, attribute, then four idle — and 0xFF in the border.

**The picture.** With *Beam-accurate picture* on, core 1 builds each
frame as the ULA fetched it, so multicolour and border effects come out
as they did on a television.

## Getting to the browser

| | Console build | USB keyboard build |
|---|---|---|
| Open the file browser | **Tab** | **F11** |
| Leave it | Tab | F11, Tab or Escape |

It has to be F11 on a real keyboard (F12 opens the settings directly) because every key a Spectrum has
belongs to the matrix, and Tab is EDIT. And it has to
be *something*, because a USB_KEYBOARD build has no console to press Tab
at: S1 has given those pins to the host controller, so if the keyboard
cannot reach the browser then nothing can.

Inside the browser: the arrows move, Enter opens, Backspace goes up, any
letter or digit jumps to the next entry beginning with it, and backslash
opens the settings page.

## The keyboard

A USB keyboard is mapped the way pico-speccy and SpeccyP map it: by key
position, not by character. Shift is CAPS SHIFT and Ctrl is SYMBOL
SHIFT, so Shift+2 is CAPS LOCK and not `@`, exactly as on the real
machine.

| PC key | Spectrum |
|---|---|
| Letters, digits, Enter, Space | the same key |
| Shift / Ctrl | CAPS SHIFT / SYMBOL SHIFT |
| Arrows | CAPS SHIFT + 5 6 7 8, always — and the joystick too, if *Cursor keys as joystick* is on |
| Backspace, Delete | DELETE (CAPS + 0) |
| Esc | BREAK (CAPS + SPACE) |
| Caps Lock | CAPS LOCK (CAPS + 2) |
| Tab | EDIT (CAPS + 1) |
| `; ' , . / - =` | SYMBOL SHIFT + O P N M V J L |
| `[ ] \` `` ` `` | SYMBOL SHIFT + 8 9 D A — `(` `)` `\` STOP |
| Keypad | its digits and Enter; `/ * - +` as SYMBOL SHIFT + V B J K |
| Alt, keypad `.` | joystick fire, when *Cursor keys as joystick* is on |

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
| **Machine** | Any of the machines above. Restarts the machine. |
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

Everything is mixed in stereo to two 44100 Hz PWM DACs — left on GP46,
right on GP44 — sampled 882 times per frame by the frame loop. Both feed
the 3.5 mm jack directly, the HDMI audio and the speaker through S2; the
speaker amplifier sums them back to mono.

| Setting | What it does |
|---|---|
| **Master / Beeper / AY volume** | 0-10 each. The beeper sits at about a quarter of full scale, which is the compromise every emulator makes: on real hardware it is much louder than the AY, and reproducing that puts every 128K tune under its own sound effects. |
| **AY stereo** | Where the AY's three channels sit. **ABC** (the default) puts A left, B in the middle and C right, as the Pentagon and most clones with stereo did; **ACB** puts C in the middle and B right; **Mono** is all three in both, as a Sinclair 128 was. A channel panned hard to one side is twice as loud there as it is in mono, so a side's peak is the same in every mode. TurboSound's second chip is panned the same way. |
| **Output level** | Full scale for headphones and the speaker, or a third of it for HDMI, whose line inputs distort on a full-scale signal. Nothing on the board can tell which is listening, so this has to be told. |
| **AY-3-8912 on a 48K** | Sinclair's 48K had none; half the clones and most add-on boards put one on the same two ports, and a fair amount of 48K music expects to find it. |
| **TurboSound** | A second AY behind the same pair of ports, selected by writing 0xFF or 0xFE to 0xFFFD. On by default: software that does not know about it never writes those values, and the 128K ROM only ever names registers 0-13. |
| **Covox** | An 8-bit DAC on port 0xFB. |
| **SounDrive** | Four of them: 0x0F and 0x1F on the left, 0x4F and 0x5F on the right. |
| **SAA1099** | A Philips SAA1099 on the 0xFF family, as the Profi has. It is a stereo chip in its own right and pans its own six channels. |

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
| **Cursor keys as joystick** | The four arrows drive the selected joystick as well as being CAPS SHIFT and a digit; Alt and the keypad's `.` are fire. |

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
| **Next .trd into** | Which drive the browser's next `.trd` goes into. A is the whole image in PSRAM and boots; B, C and D are mounted without a reset and read from the card. |
| **Drive B / C / D** | What is in each; Enter ejects. |
| **Speedlock trick** | +3 only. Fuse's heuristic: a 512-byte sector read again and again (R = N = 2) comes back with bytes changed from the second read on, the way Speedlock's weak sector does on a real disk. |

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

#### Drives B, C and D

Drive A is the whole image in PSRAM. The other three are `.trd` files
left on the card and read a cylinder (both sides, 8 KB) at a time: when
TR-DOS asks for a sector on a cylinder that is not in the drive's cache,
the controller holds the command BUSY until core 0 has read it in — a
real drive is slower than that. Writes land in the cache and go back to
the card when the head moves on, or a second and a half after the last
one. Select one in TR-DOS with `*"B"`.

#### The +3's protection

An extended `.dsk` can record a sector more than once: a stored length of
two or three times its size is that many copies of a sector that reads
differently each time. Each read here returns the next copy; a write puts
the data into all of them. A sector stored shorter than its size reads
the track's filler byte past what was stored.

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

Four users that never run together share one 3968 KB arena
(`src/spectrum/arena.h`): the extra RAM pages, the tape capture, the +3's
disk image and the ALF cartridge. The ATM-Turbo 3 takes all of it as RAM
— 4 MB with the 128 KB in SRAM — and, while it runs, there is no +3
disk, no cartridge and no tape capture.

Nothing the bus engine touches while the clock is running may go there. A
PSRAM read is a cache miss away from a microsecond, and a microsecond is
three T-states at 3.5 MHz.
