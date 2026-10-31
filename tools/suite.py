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
    'ATM 2+ xBIOS', 'ATM 3', 'ALF',
]

# The ones whose BIOS takes its time: a memory test, or a 3 s auto-boot wait.
BOOT_S = {'ATM 2': 6, 'ATM 2+': 6, 'ATM 2+ xBIOS': 14, 'ATM 3': 14}


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


TESTS = {'boot': t_boot, 'steady': t_steady, 'volume': t_volume, 'pause': t_pause, 'menu': t_menu,
         'machines': t_machines}


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
