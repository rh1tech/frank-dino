#!/usr/bin/env python3
#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# demo.py — two minutes of the board being used, for a video: machines
# booting, the settings, the file manager, a music disk playing. Driven
# through the console at a person's pace; record the HDMI meanwhile.
#
#   python3 tools/demo.py --prepare   cold 128K, sound at line level
#   python3 tools/demo.py             the demo itself
#   python3 tools/demo.py --restore   the sound settings as they were

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dino import Board, pick_machine  # noqa: E402

MACHINE = {'48K': 0, '128K': 1, 'Pentagon': 4, 'Timex': 7, 'Phoenix': 22, 'ATM 1': 23}
STATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'demo', 'saved.txt')


def beat(s=0.6):
    time.sleep(s)


def walk(b, key, n, gap=0.22):
    """Arrow presses at a reading pace."""
    for _ in range(n):
        b.key(key, gap=gap)


def show_machine(b, name, dwell):
    """Settings > Machine, scroll the list to it, Enter, out, watch it boot."""
    b.key('TAB', gap=0.9)
    b.key('SETTINGS', gap=1.2)
    b.select_row(0); beat(0.4)
    b.key('ENTER', gap=0.9)                     # into Machine
    b.select_row(0)
    b.key('RIGHT', gap=0.8)                     # the list of machines
    want = MACHINE[name]
    while b.ui_rsel() != want:
        b.key('DOWN' if b.ui_rsel() < want else 'UP', gap=0.16)
    beat(0.7)
    b.key('ENTER', gap=0.8)
    b.close_menus()
    time.sleep(dwell)


def settings_tour(b):
    b.key('TAB', gap=0.9)
    b.key('SETTINGS', gap=1.5)
    for row, depth_rows in ((2, 5), (3, 4), (5, 7)):      # Audio, Video, Storage
        b.select_row(row); beat(0.5)
        b.key('ENTER', gap=1.0)
        walk(b, 'DOWN', depth_rows, gap=0.35)
        beat(0.6)
        b.key('LEFT', gap=0.9)
    b.select_row(7); beat(0.5)                              # Emulator info
    b.key('ENTER', gap=1.5)
    walk(b, 'DOWN', 9, gap=0.25)
    beat(1.0)
    b.key('ESC', gap=0.8)
    b.close_menus()


def file_manager(b):
    b.key('TAB', gap=1.5)
    while b.browser_cwd() != '/':                           # up to the root, as it was left
        b.key('LEFT', gap=0.9)
    beat(0.8)
    walk(b, 'DOWN', 8, gap=0.12)                            # a look around the card
    beat(0.6)
    b.browser_goto('ZX', gap=0.14, open_it=True); beat(0.8)
    b.browser_goto('TRD', gap=0.25, open_it=True); beat(0.8)
    b.browser_goto('MUSIC', gap=0.25, open_it=True); beat(0.8)
    walk(b, 'DOWN', 14, gap=0.12)                           # scroll the music disks
    beat(0.5)
    b.browser_goto('bestbulba1.trd', gap=0.12)
    beat(1.2)
    b.key('ENTER', gap=1.0)                                 # mount it and boot
    b.close_menus()


def music(b, seconds):
    time.sleep(7)                                           # TR-DOS boots the Viewer
    b.key('ENTER', gap=2.5)                                 # past its title
    walk(b, 'DOWN', 3, gap=0.6)
    walk(b, 'UP', 2, gap=0.6)
    beat(0.8)
    b.key('ENTER', gap=1.0)                                 # play
    time.sleep(seconds)


def main():
    b = Board()
    try:
        if '--prepare' in sys.argv:
            os.makedirs(os.path.dirname(STATE), exist_ok=True)
            open(STATE, 'w').write(f'{b.setting("output")} {b.setting("volume")}')
            b.setting('output', 1)                          # HDMI, line level
            b.setting('volume', 5)
            pick_machine(b, MACHINE['128K'], 4)
            print('ready: a cold 128K at its menu')
            return
        if '--restore' in sys.argv:
            o, v = (int(x) for x in open(STATE).read().split())
            b.setting('output', o)
            b.setting('volume', v)
            print(f'output {o}, volume {v}')
            return

        t0 = time.time()
        beat(2.0)
        walk(b, 'DOWN', 3, gap=0.7); walk(b, 'UP', 3, gap=0.5)   # the 128's own menu
        beat(1.0)
        show_machine(b, '48K', 3.0)
        show_machine(b, 'Timex', 4.0)
        show_machine(b, 'ATM 1', 5.0)
        show_machine(b, 'Pentagon', 3.0)
        settings_tour(b)
        file_manager(b)
        music(b, 12)
        print(f'done in {time.time() - t0:.0f} s')
    finally:
        b.close()


if __name__ == '__main__':
    main()
