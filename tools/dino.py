#!/usr/bin/env python3
#
# frank-dino — an RP2350B acting as the whole machine around a real Z80
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-dino
# SPDX-License-Identifier: GPL-3.0-or-later
#
# dino.py — drive the board from a script, and check what it does.
#
# Three wires, all already on the bench:
#
#   the USB console   keys in: the Spectrum's, and the menus' (Tab, \, arrows)
#   the Debug Probe   state out: any variable in the ELF, read while it runs
#   the HDMI capture  the picture, as an image
#
# so a test is "press these keys, then check the machine is still alive and
# looks like this". tools/suite.py is the tests; this is the plumbing.
#
# Needs pyserial, Pillow, ffmpeg, openocd and the arm-none-eabi binutils.

import os
import queue
import socket
import subprocess
import threading
import time

import serial
from serial.tools import list_ports

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# What the menus and the console understand, by name.
KEYS = {
    'TAB': '\t', 'ENTER': '\r', 'ESC': '\x1b', 'BS': '\x7f', 'SETTINGS': '\\',
    'UP': '\x1b[A', 'DOWN': '\x1b[B', 'RIGHT': '\x1b[C', 'LEFT': '\x1b[D',
    'PGUP': '\x1b[5~', 'PGDN': '\x1b[6~', 'HOME': '\x1b[H', 'END': '\x1b[F',
}


def find_console():
    c = [p.device for p in list_ports.comports()
         if p.vid == 0x2E8A and 'Debug Probe' not in (p.description or '')]
    if len(c) != 1:
        raise RuntimeError(f'cannot pick the board console from {c}; S1 to USB-C?')
    return c[0]


class OpenOCD:
    """A persistent openocd, spoken to over its Tcl port."""

    def __init__(self, speed=1000, port=6666):
        self.port = port
        self.proc = subprocess.Popen(
            ['openocd', '-f', 'interface/cmsis-dap.cfg', '-c', f'adapter speed {speed}',
             '-f', 'target/rp2350.cfg', '-c', f'tcl_port {port}', '-c', 'gdb_port disabled',
             '-c', 'telnet_port disabled', '-c', 'init'],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(50):
            try:
                self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError('openocd did not come up')

    def cmd(self, c):
        self.sock.sendall(c.encode() + b'\x1a')
        buf = b''
        while not buf.endswith(b'\x1a'):
            buf += self.sock.recv(4096)
        return buf[:-1].decode(errors='replace').strip()

    def read(self, addr, width, n):
        out = self.cmd(f'read_memory {addr:#x} {width} {n}')
        return [int(x, 16) for x in out.split()]

    def write(self, addr, width, vals):
        self.cmd(f'write_memory {addr:#x} {width} {{{" ".join(hex(v) for v in vals)}}}')

    def reset(self):
        self.cmd('reset run')

    def close(self):
        try:
            self.cmd('shutdown')
        except OSError:
            pass
        self.proc.wait(timeout=5)


class ConMem:
    """
    The same reads and writes as OpenOCD, over the console (src/ui/conmon.c):
    for when the SWD link has dropped out, which on this board it does.
    """

    def __init__(self, board):
        self.b = board
        self.lock = threading.Lock()

    def _ask(self, cmd):
        with self.lock:
            while not self.b.replies.empty():
                self.b.replies.get_nowait()
            self.b.ser.write(b'\x10' + cmd.encode() + b'\n')
            r = self.b.replies.get(timeout=3)
        if r.startswith('@E'):
            raise RuntimeError(f'console refused {cmd[:12]}')
        return r[3:]

    def read(self, addr, width, n):
        nb = width // 8
        out = []
        left, a = n * nb, addr
        while left:
            k = min(left, 1024)
            h = self._ask(f'R{a:08X}{k:04X}')
            out.append(bytes.fromhex(h))
            a += k
            left -= k
        raw = b''.join(out)
        return [int.from_bytes(raw[i:i + nb], 'little') for i in range(0, len(raw), nb)]

    def write(self, addr, width, vals):
        nb = width // 8
        data = b''.join(int(v).to_bytes(nb, 'little') for v in vals)
        self._ask(f'W{addr:08X}{data.hex()}')

    def reset(self):
        raise RuntimeError('no reset over the console')

    def close(self):
        pass


def settings_offsets():
    """
    Every field of settings_t, by name: compiled with the ARM compiler from
    settings.h itself, so it cannot go stale the way a table would.
    """
    import re
    import tempfile
    hdr = open(os.path.join(ROOT, 'src', 'spectrum', 'settings.h')).read()
    body = hdr[hdr.index('typedef struct'):]
    body = body[:body.index('} settings_t;')]
    names = re.findall(r'^\s*(?:uint8_t|uint16_t|uint32_t|int8_t|int16_t|int32_t|bool)\s+(\w+)\s*[;\[]',
                       body, re.M)
    src = '#include <stddef.h>\n#include "settings.h"\n' + ''.join(
        f'const unsigned off_{n} = offsetof(settings_t, {n});\n' for n in names)
    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, 'o.c')
        open(c, 'w').write(src)
        inc = [f'-I{os.path.join(ROOT, p)}' for p in ('src/spectrum', 'src/core', 'src/app', 'src')]
        asm = subprocess.run(['arm-none-eabi-gcc', '-std=gnu2x', '-S', '-o', '-', *inc, c],
                             capture_output=True, text=True)
    if asm.returncode:
        raise RuntimeError(asm.stderr[-2000:])
    out = {}
    for m in re.finditer(r'off_(\w+):\s*\n\s*\.(word|space)\s+(\d+)', asm.stdout):
        out[m.group(1)] = int(m.group(3)) if m.group(2) == 'word' else 0
    return out


class Board:
    def __init__(self, elf=None, console=None):
        self.elf = elf or os.path.join(ROOT, 'build', 'frank-dino.elf')
        # Every symbol by name, and statics also as "file.c:name" -- two
        # files can each have a static of the same name.
        self.syms = {}
        out = subprocess.run(['arm-none-eabi-objdump', '-t', self.elf],
                             capture_output=True, text=True).stdout
        src = ''
        for line in out.splitlines():
            p = line.split()
            if len(p) >= 4 and p[-2:-1] and 'df' in p and '*ABS*' in p:
                src = p[-1]
                continue
            if len(p) >= 5 and len(p[0]) == 8:
                try:
                    a = int(p[0], 16)
                except ValueError:
                    continue
                name = p[-1]
                self.syms.setdefault(name, a)
                self.syms[f'{src}:{name}'] = a
        self.ser = serial.Serial(console or find_console(), 115200, timeout=0.05)
        self.log = []
        self.replies = queue.Queue()
        self._stop = False
        threading.Thread(target=self._reader, daemon=True).start()
        # SWD if it answers, else the console; DINO_MEM=console forces it.
        self.ocd = None
        if os.environ.get('DINO_MEM') != 'console':
            try:
                self.ocd = OpenOCD()
                self.ocd.read(0x20000000, 32, 1)
            except Exception:                          # noqa: BLE001
                if self.ocd:
                    try:
                        self.ocd.proc.kill()
                    except OSError:
                        pass
                self.ocd = None
        if self.ocd is None:
            self.ocd = ConMem(self)

    # -- the console --------------------------------------------------------
    def _reader(self):
        part = b''
        while not self._stop:
            try:
                d = self.ser.read(256)
            except serial.SerialException:
                time.sleep(0.2)
                continue
            if not d:
                continue
            part += d
            while b'\n' in part:
                line, part = part.split(b'\n', 1)
                t = line.decode(errors='replace').rstrip('\r')
                if t.startswith('@M') or t.startswith('@E'):
                    self.replies.put(t)
                else:
                    self.log.append(t)

    def type(self, text, gap=0.12):
        """Characters, one at a time, with a gap the machine can see."""
        for ch in text:
            self.ser.write(ch.encode('latin-1'))
            time.sleep(gap)

    def key(self, name, times=1, gap=0.25):
        for _ in range(times):
            self.ser.write(KEYS.get(name, name).encode('latin-1'))
            time.sleep(gap)

    # -- memory -------------------------------------------------------------
    def addr(self, sym):
        return self.syms[sym]

    def u32(self, sym, n=1, off=0):
        v = self.ocd.read(self.addr(sym) + off, 32, n)
        return v[0] if n == 1 else v

    def u16(self, sym, off=0):
        return self.ocd.read(self.addr(sym) + off, 16, 1)[0]

    def u8(self, sym, n=1, off=0):
        v = self.ocd.read(self.addr(sym) + off, 8, n)
        return v[0] if n == 1 else v

    # -- settings, by field name ----------------------------------------------
    def setting(self, name, value=None):
        if not hasattr(self, '_soff'):
            self._soff = settings_offsets()
        a = self.addr('settings') + self._soff[name]
        if value is None:
            return self.ocd.read(a, 8, 1)[0]
        self.ocd.write(a, 8, [value])

    # -- the file browser, by name ---------------------------------------------
    def browser_names(self, stride=72):
        """The current folder's entries, in the browser's order."""
        n = self.u32('ui_browser.c:entry_count')
        base = self.addr('ui_browser.c:entries')
        raw = bytes(self.ocd.read(base, 8, n * stride)) if n else b''
        return [raw[i * stride:i * stride + 64].split(b'\0')[0].decode(errors='replace') for i in range(n)]

    def browser_cwd(self):
        return bytes(self.ocd.read(self.addr('ui_browser.c:cwd'), 8, 64)).split(b'\0')[0].decode()

    def browser_goto(self, name, gap=0.18, open_it=False):
        """Walk the browser's cursor to `name`, a key at a time, as a person would."""
        names = self.browser_names()
        want = next(i for i, x in enumerate(names) if x.lower() == name.lower())
        for _ in range(200):
            cur = self.u32('ui_browser.c:sel')
            if cur == want:
                break
            self.key('DOWN' if cur < want else 'UP', gap=gap)
        if open_it:
            self.key('ENTER', gap=1.2)

    # -- loading a file, as the browser would ---------------------------------
    def load(self, path, timeout=20):
        """Load a file from the card (any kind the browser takes). Returns (ok, detail)."""
        a = self.addr('harness_path')
        data = path.encode()[:95] + b'\0'
        self.ocd.write(a, 8, list(data))
        self.ocd.write(self.addr('harness_req'), 8, [1])
        if not self._wait(lambda: self.u8('harness_req') == 0, timeout):
            raise RuntimeError(f'the load of {path} did not finish')
        raw = bytes(self.ocd.read(self.addr('harness_detail'), 8, 64))
        return self.u8('harness_ok') == 1, raw.split(b'\0')[0].decode(errors='replace')

    # -- the Z80's memory, through the live page table (8 KB pages) --------
    def zread(self, addr, n):
        """n bytes of what the Z80 sees from addr (n a multiple of 4, no page crossing)."""
        page = self.ocd.read(self.addr('z80_rpage') + 4 * (addr >> 13), 32, 1)[0]
        words = self.ocd.read(page + (addr & 0x1FFF), 32, n // 4)
        return b''.join(w.to_bytes(4, 'little') for w in words)

    def zwrite(self, addr, data):
        """Write bytes where the Z80 writes (no page crossing)."""
        page = self.ocd.read(self.addr('z80_wpage') + 4 * (addr >> 13), 32, 1)[0]
        self.ocd.write(page + (addr & 0x1FFF), 8, list(data))

    # -- the picture --------------------------------------------------------
    def shot(self, path=None):
        """One frame from the capture, as a Pillow image (and a file, if asked)."""
        from PIL import Image
        dev = os.environ.get('CAPTURE_DEVICE', 'USB Video')
        raw = subprocess.run(
            ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'avfoundation',
             '-framerate', '60', '-video_size', '640x480', '-i', dev,
             '-vf', 'select=gte(n\\,8)', '-frames:v', '1', '-f', 'rawvideo',
             '-pix_fmt', 'rgb24', '-'], capture_output=True).stdout
        if len(raw) < 640 * 480 * 3:
            raise RuntimeError('no picture from the capture')
        img = Image.frombytes('RGB', (640, 480), raw[:640 * 480 * 3])
        if path:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            img.save(path)
        return img

    # -- the menus ----------------------------------------------------------
    def _wait(self, pred, timeout=3.0):
        t = time.time() + timeout
        while time.time() < t:
            if pred():
                return True
            time.sleep(0.05)
        return False

    def menu_open(self):
        return self.u8('menu.c:open_') != 0

    def settings_open(self):
        return self.u8('ui_settings_active') != 0

    def close_menus(self):
        """Out of the settings and the browser, whatever state they are in."""
        for _ in range(8):
            if self.settings_open():
                self.key('SETTINGS', gap=0.1)          # \ closes the settings
                self._wait(lambda: not self.settings_open())
            elif self.menu_open():
                self.key('TAB', gap=0.1)               # Tab closes the browser
                self._wait(lambda: not self.menu_open())
            else:
                time.sleep(0.3)
                return True
        return False

    def open_settings(self):
        """The settings tree, at its root."""
        self.close_menus()
        self.key('TAB', gap=0.1)
        if not self._wait(self.menu_open):
            raise RuntimeError('the browser did not open')
        time.sleep(0.4)
        self.key('SETTINGS', gap=0.1)
        if not self._wait(self.settings_open):
            raise RuntimeError('the settings did not open')
        time.sleep(0.3)

    # ui_S on the RP2350: five 80-byte levels (sel at 9), then depth,
    # focus, rsel. Checked against a live dump; see ui_nav.h.
    UI_LEVEL, UI_SEL, UI_DEPTH, UI_FOCUS, UI_RSEL = 80, 9, 400, 401, 402

    def _ui(self):
        base = self.addr('ui_S')
        depth = self.ocd.read(base + self.UI_DEPTH, 8, 1)[0]
        sel = self.ocd.read(base + depth * self.UI_LEVEL + self.UI_SEL, 8, 1)[0]
        return depth, sel

    def ui_focus_right(self):
        return self.ocd.read(self.addr('ui_S') + self.UI_FOCUS, 8, 1)[0] != 0

    def ui_rsel(self):
        return self.ocd.read(self.addr('ui_S') + self.UI_RSEL, 8, 1)[0]

    def choose(self, row, index):
        """
        A radio row's option `index`, the way a person picks one: the row,
        RIGHT into its list, up or down to the option, Enter.
        """
        if not self.select_row(row):
            raise RuntimeError(f'could not reach row {row}')
        self.key('RIGHT', gap=0.3)
        for _ in range(80):
            cur = self.ui_rsel()
            if cur == index:
                break
            self.key('DOWN' if cur < index else 'UP', gap=0.1)
        self.key('ENTER', gap=0.6)
        if self.ui_focus_right():
            self.key('LEFT', gap=0.3)

    def ui_depth(self):
        return self._ui()[0]

    def ui_sel(self):
        return self._ui()[1]

    def select_row(self, row):
        """Move the settings cursor to visible row `row` of the current level."""
        for _ in range(64):
            cur = self.ui_sel()
            if cur == row:
                return True
            self.key('DOWN' if cur < row else 'UP', gap=0.12)
        return False

    def enter(self, row):
        """Select a row and press Enter on it: into a submenu, or the action."""
        d = self.ui_depth()
        if not self.select_row(row):
            raise RuntimeError(f'could not reach row {row}')
        self.key('ENTER', gap=0.3)
        return self.ui_depth() != d

    def set_value(self, row, read, want, limit=64):
        """
        Step a radio or number on `row` with LEFT/RIGHT until `read()` says
        `want`, the way a person would -- through the menu, not around it.
        """
        self.select_row(row)
        for _ in range(limit):
            v = read()
            if v == want:
                return True
            self.key('RIGHT' if v < want else 'LEFT', gap=0.35)
        return read() == want

    # -- is it alive --------------------------------------------------------
    def status(self):
        """The things that say whether the machine is running."""
        frames = self.u32('spec_frames')
        # last_opfetch_addr sits after seven 32-bit counters in z80_stats.
        pc = self.u16('z80_stats', 28)
        paused = self.u8('spectrum.c:paused')
        return {'frames': frames, 'pc': pc, 'paused': paused}

    def alive(self, seconds=1.0, samples=10, since=None):
        """
        Frames advancing at about 50 Hz, the CPU fetching from more than one
        place, and -- if `since` is a z80_fetch0 reading -- no fetch from
        #0000 in between: a machine that cold-starts itself looks alive
        on every other count, and that is how a crash hid for a day.
        """
        f0 = self.u32('spec_frames')
        pcs = set()
        t0 = time.time()
        for _ in range(samples):
            pcs.add(self.u16('z80_stats', 28))
            time.sleep(seconds / samples)
        f1 = self.u32('spec_frames')
        dt = time.time() - t0
        fps = (f1 - f0) / dt
        restarts = (self.u32('z80_fetch0') - since) if since is not None else 0
        return {'fps': fps, 'pcs': len(pcs), 'restarts': restarts,
                'ok': fps > 40 and len(pcs) > 1 and restarts == 0}

    def close(self):
        self._stop = True
        self.ocd.close()
        self.ser.close()


if __name__ == '__main__':
    b = Board()
    print(b.status())
    print(b.alive())
    b.close()


def pick_machine(b, index, boot_s=3.0):
    """
    Settings > Machine > option `index`, then out and let it boot -- from
    cold every time: choosing the machine already chosen changes nothing,
    so that one gets Reset the machine instead.
    """
    b.open_settings()
    b.enter(0)
    if b.setting('machine') == index:
        b.select_row(5)
        b.key('ENTER', gap=0.6)                 # "Reset the machine?" -- No is lit
        b.key('LEFT', gap=0.3)
        b.key('ENTER', gap=0.6)
    else:
        b.choose(0, index)
    b.close_menus()
    time.sleep(boot_s)
