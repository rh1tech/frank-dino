#!/usr/bin/env python3
#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# suite.py — the on-board test suite: drives the menus and the machine
# through the console, checks over SWD and the capture that it survived,
# and saves a screenshot of every step to out/suite/ for a person to look at.
#
#   python3 tools/suite.py              every test
#   python3 tools/suite.py menu machines   just those
#
# Needs the console build (USB_KEYBOARD=OFF) and S1 on USB-C.

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dino import Board, ROOT  # noqa: E402

OUT = os.path.join(ROOT, 'out', 'suite')

# The machine radio's order, as ui_tree.c lists it. A test steps through it
# with RIGHT from the first entry.
MACHINES = [
    '48K', '128K', '+2', '+3', 'Pentagon', 'Scorpion', 'Profi', 'Timex TC2068',
    '16K', '+2A', 'TC2048', 'Gama 89', 'Leningrad-1', 'Leningrad-2', 'Byte',
    'Pentagon 512', 'Pentagon 1024SL', 'ZS-1024', 'ScorpEvo', 'KAY-256',
    'KAY-1024', 'KAY-1024 2010', 'Phoenix', 'ATM 1', 'ATM 2', 'ATM 2+',
    'ATM 2+ xBIOS', 'ATM 3', 'ALF', 'Evo',
]

# The ones whose BIOS takes its time: a memory test, or a 3 s auto-boot wait.
BOOT_S = {'ATM 2': 6, 'ATM 2+': 6, 'ATM 2+ xBIOS': 14, 'ATM 3': 14, 'Evo': 6}


class Result:
    def __init__(self):
        self.fails = []
        self.passes = 0

    def check(self, cond, what, b=None, shot=None):
        if cond:
            self.passes += 1
            print(f'  ok    {what}')
            return True
        self.fails.append(what)
        print(f'  FAIL  {what}')
        if b:
            try:
                print(f'        status {b.status()}')
                if shot:
                    b.shot(os.path.join(OUT, shot))
                    print(f'        screen out/suite/{shot}')
                for line in b.log[-8:]:
                    print(f'        log: {line}')
            except Exception as e:                       # noqa: BLE001
                print(f'        (could not collect: {e})')
        return False


# ---------------------------------------------------------------------------

def t_boot(b, r):
    a = b.alive()
    r.check(a['ok'], f'running: {a["fps"]:.1f} fps, {a["pcs"]} PCs', b, 'boot.png')
    b.shot(os.path.join(OUT, 'boot.png'))


def t_menu(b, r, rounds=40):
    """The browser in and out, and the settings in and out, over and over."""
    for i in range(rounds):
        f0 = b.u32('z80_fetch0')
        b.key('TAB', gap=0.8)                    # into the browser
        if i % 2:
            b.key('SETTINGS', gap=0.8)           # ...and on into the settings
            b.key('ESC', gap=0.6)
        b.key('TAB', gap=0.8)                    # and out
        a = b.alive(seconds=0.8, since=f0)
        if not r.check(a['ok'], f'round {i + 1}: alive after the menu '
                       f'({a["fps"]:.1f} fps, {a["pcs"]} PCs, {a["restarts"]} restarts)',
                       b, f'menu-{i + 1}.png'):
            return


def t_pause(b, r, rounds=12):
    """Long pauses at the 128K menu, whose interrupts swap ROMs: the case that crashed."""
    pr = b.addr('pause_req')
    for i in range(rounds):
        f0 = b.u32('z80_fetch0')
        b.ocd.write(pr, 8, [1]); time.sleep(1.6); b.ocd.write(pr, 8, [0]); time.sleep(0.6)
        a = b.alive(seconds=0.6, since=f0)
        if not r.check(a['ok'], f'pause {i + 1}: no restart ({a["restarts"]})', b, f'pause-{i + 1}.png'):
            return


def t_volume(b, r):
    """The audio ring neither starves nor floods while the machine runs."""
    u0, o0 = b.u32('audio_underruns'), b.u32('audio_overruns')
    time.sleep(3)
    u1, o1 = b.u32('audio_underruns'), b.u32('audio_overruns')
    r.check(u1 == u0, f'no underruns in 3 s ({u1 - u0})')
    r.check(o1 == o0, f'no overruns in 3 s ({o1 - o0})')
    r.check(b.u32('sndq_drops') == 0, 'no sound events dropped')


def t_machines(b, r):
    """Every machine: chosen through Settings > Machine, booted, alive, photographed."""
    for i, name in enumerate(MACHINES):
        b.open_settings()
        b.enter(0)                               # Machine
        b.choose(0, i)                           # the Machine radio, option i
        r.check(b.u8('settings') == i, f'{name}: selected through the menu (machine {b.u8("settings")})')
        b.close_menus()
        time.sleep(BOOT_S.get(name, 3))          # let it boot
        f0 = b.u32('z80_fetch0')
        a = b.alive(since=f0)
        safe = name.replace(' ', '_').replace('+', 'p')
        r.check(a['ok'], f'{name}: running ({a["fps"]:.1f} fps, {a["pcs"]} PCs)', b,
                f'machine-{i:02d}-{safe}.png')
        b.shot(os.path.join(OUT, f'machine-{i:02d}-{safe}.png'))
    # Back to the 128K the board boots as.
    b.open_settings(); b.enter(0)
    b.choose(0, 1)
    b.close_menus()


def t_steady(b, r):
    """
    The picture holds still: no line interrupt more than a line late, with
    the machine running and with the menu redrawing. A late one sends a
    frame out a line short, and the screen visibly jumps.
    """
    def zero():
        b.ocd.write(b.addr('vga_late_lines'), 32, [0])
        b.ocd.write(b.addr('vga_line_max_us'), 32, [0])
    b.close_menus(); zero(); time.sleep(3)
    r.check(b.u32('vga_late_lines') == 0,
            f'machine: no late lines (longest {b.u32("vga_line_max_us")} us)')
    b.open_settings(); zero()
    for k in ['DOWN'] * 10 + ['UP'] * 10:
        b.key(k, gap=0.15)
    r.check(b.u32('vga_late_lines') == 0,
            f'menu redrawing: no late lines ({b.u32("vga_late_lines")}, longest {b.u32("vga_line_max_us")} us)')
    b.close_menus()


def t_snapshot(b, r):
    """
    Quick save and quick load, through the menu, on a 48K: a pattern in
    RAM and a line on the screen survive a wipe and a reset.
    """
    from dino import pick_machine
    pick_machine(b, 0, 3)
    b.type('10 PRINT 1', gap=0.15); b.key('ENTER', gap=0.8)
    pat = bytes((i * 37 + 11) & 0xFF for i in range(256))
    b.zwrite(0x9000, pat)
    screen = b.zread(0x4000, 6912 // 4 * 4)
    b.shot(os.path.join(OUT, 'snapshot-before.png'))

    b.open_settings(); b.enter(1)                 # Snapshots
    b.select_row(2); b.key('ENTER', gap=2.0)      # Quick save, slot 1
    b.close_menus()

    b.zwrite(0x9000, bytes(256))                  # wipe the pattern...
    pick_machine(b, 0, 3)                         # ...and reset: 48K again from cold
    r.check(b.zread(0x9000, 256) != pat, 'the reset really lost the pattern')

    b.open_settings(); b.enter(1)
    b.select_row(3); b.key('ENTER', gap=3.0)      # Quick load
    b.close_menus(); time.sleep(1)
    r.check(b.zread(0x9000, 256) == pat, 'RAM pattern back after quick load', b, 'snapshot-after.png')
    got = b.zread(0x4000, 6912 // 4 * 4)
    same = sum(1 for x, y in zip(got, screen) if x == y)
    r.check(same > 6800, f'screen back ({same}/{len(screen)} bytes match)', b, 'snapshot-after.png')
    f0 = b.u32('z80_fetch0'); a = b.alive(since=f0)
    r.check(a['ok'], f'running after the load ({a["fps"]:.1f} fps, {a["pcs"]} PCs)', b, 'snapshot-after.png')
    b.shot(os.path.join(OUT, 'snapshot-after.png'))


def t_snapshot128(b, r):
    """The same on a 128K: a mark in every RAM bank, and the paging latch."""
    from dino import pick_machine
    pick_machine(b, 1, 4)
    base = b.addr('spec_ram')
    marks = {k: bytes(((k * 53 + i * 7) & 0xFF) for i in range(64)) for k in range(8)}
    for k, m in marks.items():
        if k not in (2, 5):                      # the ROM's own system variables live in 5
            b.ocd.write(base + k * 16384 + 0x3000, 8, list(m))
    p7 = b.u8('spec_port_7ffd')

    b.open_settings(); b.enter(1)
    b.select_row(2); b.key('ENTER', gap=2.0)      # Quick save, slot 1
    b.close_menus()
    for k in marks:
        if k not in (2, 5):
            b.ocd.write(base + k * 16384 + 0x3000, 8, [0] * 64)
    pick_machine(b, 1, 4)

    b.open_settings(); b.enter(1)
    b.select_row(3); b.key('ENTER', gap=3.0)      # Quick load
    b.close_menus(); time.sleep(1)
    ok = [k for k, m in marks.items() if k not in (2, 5) and
          bytes(b.ocd.read(base + k * 16384 + 0x3000, 8, 64)) == m]
    r.check(len(ok) == 6, f'every bank back after quick load ({ok})', b, 'snapshot128-after.png')
    r.check(b.u8('spec_port_7ffd') == p7, f'#7FFD back ({b.u8("spec_port_7ffd"):02X}, was {p7:02X})')
    f0 = b.u32('z80_fetch0'); a = b.alive(since=f0)
    r.check(a['ok'], f'running after the load ({a["fps"]:.1f} fps, {a["pcs"]} PCs)', b, 'snapshot128-after.png')
    b.shot(os.path.join(OUT, 'snapshot128-after.png'))


# Each sound source: the setting that fits it, and the port writes that
# start a tone on it -- ports and registers as spectrum.c decodes them.
def _ay(sel=None):
    w = [(0xFFFD, sel)] if sel is not None else []
    return w + [(0xFFFD, 7), (0xBFFD, 62), (0xFFFD, 8), (0xBFFD, 15), (0xFFFD, 0), (0xBFFD, 200)]


def _pairs(addr_port, data_port, regs):
    return [x for r, v in regs for x in ((addr_port, r), (data_port, v))]


SAA_TONE = _pairs(0x01FF, 0x00FF, [(28, 1), (20, 1), (0, 255), (8, 128), (16, 3)])
OPL_TONE = _pairs(0xC4, 0xC5, [(0x20, 1), (0x23, 1), (0x40, 16), (0x43, 0), (0x60, 240), (0x63, 240),
                               (0x80, 119), (0x83, 119), (0xC0, 48), (0xA0, 152), (0xB0, 49)])
OPN_TONE = [(0xFFFD, 0xFB)] + _pairs(0xFFFD, 0xBFFD, [
    (0xB0, 7), (0x40, 0), (0x44, 0), (0x48, 0), (0x4C, 0), (0x50, 31), (0x54, 31), (0x58, 31),
    (0x5C, 31), (0x30, 1), (0x34, 1), (0x38, 1), (0x3C, 1), (0xA4, 34), (0xA0, 105), (0x28, 240)])
SOURCES = [
    ('AY',          {},                   _ay()),
    ('TurboSound',  {'turbosound': 1},    _ay(0xFE)),
    ('SAA1099',     {'saa': 1},           SAA_TONE),
    ('CMS',         {'cms': 1},           SAA_TONE),
    ('OPL3',        {'opl3': 1},          OPL_TONE),
    ('OPLL',        {'opll': 1},          _pairs(0xC0, 0xC1, [(0x30, 16), (0x10, 172), (0x20, 24)])),
    ('SN76489',     {'sn76489': 1},       [(0xC9, 0x8E), (0xC9, 0x0F), (0xC9, 0x90)]),
    ('TSFM',        {'tsfm': 1},          OPN_TONE),
    ('Covox',       {'covox': 1},         ('toggle', 0xFB, 255)),
    ('SounDrive',   {'soundrive': 1},     ('toggle', 0x0F, 255)),
    ('SpecDrum',    {'specdrum': 1},      ('toggle', 0xDF, 255)),
    ('Beeper',      {},                   ('toggle', 0xFE, 0x10)),
]
SOUND_SETTINGS = ['turbosound', 'saa', 'cms', 'opl3', 'opll', 'sn76489', 'tsfm',
                  'covox', 'soundrive', 'specdrum']


def z80_toggle(port, hi, lo=0, loops=12000):
    """OUT (port),hi / wait / OUT (port),lo / wait, `loops` times: a square wave, ~4 s."""
    body = bytes((0x3E, hi, 0xD3, port, 0x06, 40, 0x10, 0xFE,
                  0x3E, lo, 0xD3, port, 0x06, 40, 0x10, 0xFE,
                  0x1B, 0x7A, 0xB3))
    jr = -(len(body) + 2) & 0xFF
    return bytes((0x11, loops & 0xFF, loops >> 8)) + body + bytes((0x20, jr, 0xC9))


def z80_outs(writes):
    """LD BC,port / LD A,value / OUT (C),A for each, then RET."""
    code = bytearray()
    for port, val in writes:
        code += bytes((0x01, port & 0xFF, port >> 8, 0x3E, val, 0xED, 0x79))
    return bytes(code + b'\xC9')


def swing(b, seconds=1.0):
    """How far the mix moved, left channel, over a while."""
    b.ocd.write(b.addr('sound_min'), 32, [0x7FFFFFFF, 0x7FFFFFFF])
    b.ocd.write(b.addr('sound_max'), 32, [0x80000000, 0x80000000])
    time.sleep(seconds)
    lo, hi = b.ocd.read(b.addr('sound_min'), 32, 1)[0], b.ocd.read(b.addr('sound_max'), 32, 1)[0]
    sgn = lambda v: v - (1 << 32) if v & 0x80000000 else v
    return sgn(hi) - sgn(lo)


def t_sound(b, r):
    """Every sound source, one at a time: silence first, then a tone from 128 BASIC."""
    from dino import pick_machine
    saved = {n: b.setting(n) for n in SOUND_SETTINGS}
    try:
        for name, on, basic in SOURCES:
            for n in SOUND_SETTINGS:
                b.setting(n, on.get(n, 0))
            pick_machine(b, 1, 4)                        # a cold 128K: every chip reset
            b.key('DOWN', gap=0.4); b.key('ENTER', gap=1.5)   # 128 BASIC
            code = z80_toggle(basic[1], basic[2]) if basic[0] == 'toggle' else z80_outs(basic)
            b.zwrite(0x8000, code)
            b.type('RANDOMIZE USR 32768', gap=0.15)
            time.sleep(0.8)                              # the key clicks die away
            quiet = swing(b)
            b.key('ENTER', gap=1.0)
            loud = swing(b)
            r.check(loud > 1500 and loud > 4 * max(quiet, 1),
                    f'{name}: sounds (swing {quiet} -> {loud})', b, f'sound-{name}.png')
    finally:
        for n, v in saved.items():
            b.setting(n, v)


JSW = '/ZX/TAP/j/JET SET WILLY 1 (-).TAP'


def t_tape(b, r):
    """
    A tape played in real time, not trapped: the 48K's own loader reads
    the header, the BASIC and the code header from the pulses, and the
    code block starts landing at #7FFE. 32 s is enough to see it begin.
    """
    from dino import pick_machine
    pick_machine(b, 0, 3)
    was = b.setting('tape_realtime')
    b.setting('tape_realtime', 1)
    try:
        ok, detail = b.load(JSW)
        if not r.check(ok and 'real time' in detail, f'mounted for real-time play ({detail})', b, 'tape.png'):
            return
        time.sleep(32)
        r.check(b.u32('pulse.c:blk_no') == 3, f'the tape is on the code block ({b.u32("pulse.c:blk_no")})')
        # #8200: the first word-aligned stretch of the game that is not zeros --
        # past byte 322, the #FF that a missing closing edge used to lose.
        got = b.zread(0x8200, 64)
        want = bytes.fromhex('00600061006200630064006500660067206020612062206320642065206620674060406140624063406440654066406760606061606260636064606560666067')
        r.check(got == want, 'the code block is loading, byte for byte', b, 'tape.png')
        r.check(0x5E9 <= b.status()['pc'] <= 0x5FA, f'the ROM is still reading bits (pc {b.status()["pc"]:04X})',
                b, 'tape.png')
    finally:
        b.setting('tape_realtime', was)


def t_driveb(b, r):
    """
    A second Beta drive, read from the card: TR-DOS lists drive B and
    boots its disk -- every sector of it streamed a cylinder at a time.
    """
    from dino import pick_machine
    def cyl_b():
        v = b.u16('beta.c:cyl_of')
        return v - 0x10000 if v & 0x8000 else v
    b.setting('beta_target', 0)
    b.load('/ZX/TRD/MUSIC/bestbulba1.trd')      # drive A, so the pick below is a cold reset
    pick_machine(b, 4, 3)                       # Pentagon
    b.setting('beta_target', 1)
    try:
        ok, detail = b.load('/ZX/TRD/MUSIC/bestbulba2.trd')
        r.check(ok and 'drive B' in detail, f'mounted into drive B ({detail})')
    finally:
        b.setting('beta_target', 0)
    b.key('DOWN', 4, gap=0.4)
    b.key('ENTER', gap=3)                       # TR-DOS
    b.type('*"b"', gap=0.3)
    b.key('ENTER', gap=2)
    b.type('k', gap=0.3)                        # LIST
    b.key('ENTER', gap=0.05)
    seen, t0 = [], time.time()                  # TR-DOS reads cylinder 0, then looks at 7
    while time.time() - t0 < 4:
        c = cyl_b()
        if not seen or seen[-1] != c:
            seen.append(c)
    r.check(0 in seen, f'the catalogue came from drive B, cylinder 0 (cylinders {seen})', b, 'driveb_list.png')
    b.type('n', gap=1)
    b.key('BS', 8, gap=0.3)
    b.type('r', gap=0.4)                        # RUN: the disk's boot
    b.key('ENTER', gap=0.05)
    seen, t0 = [], time.time()
    while time.time() - t0 < 12:
        c = cyl_b()
        if not seen or seen[-1] != c:
            seen.append(c)
    r.check(len(seen) > 2, f'and the boot streamed cylinder after cylinder ({seen})', b, 'driveb_boot.png')
    r.check(b.alive(), 'the machine runs on')


def t_evo(b, r):
    """
    The ZX Evolution: EVO Reset Service from the reset, its Magic menu by
    NMI in the one-page text mode and back out of it, and NEO-DOS.
    """
    from dino import pick_machine
    pick_machine(b, MACHINES.index('Evo'), 6)
    r.check(b.alive(), 'EVO Reset Service runs', b, 'evo_ers.png')
    b.open_settings()
    b.enter(0)
    b.select_row(7)
    b.key('ENTER', gap=0.5)                    # Magic button (NMI)
    b.close_menus()
    time.sleep(3)
    def text1():                               # page 8, the TEXT1 screen: 25 rows of 80
        base = b.addr('psram_arena')           # page 8 is the arena's first
        rows = []
        for row in range(25):
            ev = bytes(b.ocd.read(base + 0x1C0 + row * 64, 8, 40))
            od = bytes(b.ocd.read(base + 0x11C0 + row * 64, 8, 40))
            rows.append(bytes(x for p in zip(ev, od) for x in p).decode('latin-1'))
        return '\n'.join(rows)
    r.check('MAGIC Service' in text1(), 'the NMI brought up EVO Magic Service')
    r.check(b.u8('atm.c:p77') & 7 == 7, f'Magic is in TEXT1 (#77 mode {b.u8("atm.c:p77") & 7})', b, 'evo_magic.png')
    b.ocd.write(b.addr('vga_late_lines'), 32, [0])
    time.sleep(2)
    r.check(b.u32('vga_late_lines') == 0, f'TEXT1 draws in time (longest line {b.u32("vga_line_max_us")} us)')
    b.key('ENTER', gap=2)                      # 0. Continue program
    r.check(b.u8('atm.c:in_nmi') == 0 and b.u8('atm.c:p77') & 7 == 3, 'Continue: out of the NMI, ZX mode again',
            b, 'evo_back.png')
    b.type('s', gap=0.3)                       # S. TR-DOS
    time.sleep(3)
    r.check(b.alive(), 'NEO-DOS runs', b, 'evo_dos.png')


TESTS = {'boot': t_boot, 'steady': t_steady, 'volume': t_volume, 'pause': t_pause, 'menu': t_menu,
         'machines': t_machines, 'snapshot': t_snapshot, 'snapshot128': t_snapshot128, 'sound': t_sound, 'tape': t_tape,
         'driveb': t_driveb, 'evo': t_evo}


def main():
    want = sys.argv[1:] or list(TESTS)
    b = Board()
    r = Result()
    try:
        for name in want:
            print(f'== {name}')
            TESTS[name](b, r)
    finally:
        b.close()
    print(f'\n{r.passes} passed, {len(r.fails)} failed')
    for f in r.fails:
        print(f'  - {f}')
    sys.exit(1 if r.fails else 0)


if __name__ == '__main__':
    main()
