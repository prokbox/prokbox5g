#!/usr/bin/env python3
#***************************************************************************
#               __________                   __    __________
#    Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
#    Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
#    Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
#    Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
#                                                \/        \/               \/
#
# Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
# Not affiliated with or endorsed by Apple Inc.
#
# Copyright (C) 2026 prokopto-dev
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.
#
#***************************************************************************
"""Real ProkBox screens through the bcmfw LCD push (CPU and BCMFW_LCD_DMA=1).

hello_test and dma_test feed random-noise frames.  This feeds committed UI
goldens (tests/ui/golden, 320x240 RGB PNGs) instead, converted to RGB565 the
way the simulator's LCD_RGBPACK does it (the value lcd-video.c copies into
the BCM framebuffer), and checks the panel model's output is pixel-identical
to the RGB565 source.  Panels 0 (DCS), 1 and 2 (index/data kinds), full
LCD_UPDATE per screen; reports instructions per frame for each push.

Usage:
  bcmfw/tests/realframes_test.py [-v] [--png DIR] [--vmcs PATH] [--seed N]

Skips cleanly (exit 0, message) when vc4-elf-gcc or the goldens are missing.
"""
import os
import struct
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hello_test as T   # noqa: E402
import dma_test as D     # noqa: E402

B, W, H = T.B, T.W, T.H
GOLDEN = os.path.join(T.ROOT, 'tests', 'ui', 'golden')
SCREENS = [                      # main menu, Now Playing with art, album list,
    'ipodui-p2-menu/menu_down.png',      # Settings, lyrics (text)
    'ipodui-p5-normal/normal.png',
    'ipodui-p4-browse/album.png',
    'ipodui-nav-panels/settings_panel.png',
    'ipodui-p5-lyrics/v23_top.png',
]
PANELS = (0, 1, 2)


def png_decode(path):
    """General 8-bit RGB/RGBA/grey PNG reader (all five filters, no
    interlace); returns (width, height, [(r, g, b)...])."""
    d = open(path, 'rb').read()
    if d[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('%s: not a PNG' % path)
    pos, idat, hdr = 8, b'', None
    while pos < len(d):
        n, t = struct.unpack('>I4s', d[pos:pos + 8])
        body = d[pos + 8:pos + 8 + n]
        if t == b'IHDR':
            hdr = struct.unpack('>IIBBBBB', body)
        elif t == b'IDAT':
            idat += body
        pos += 12 + n
    w, h, depth, ctype, _, _, interlace = hdr
    ch = {0: 1, 2: 3, 4: 2, 6: 4}.get(ctype)
    if depth != 8 or ch is None or interlace:
        raise ValueError('%s: unsupported PNG (depth %d, colour type %d, interlace %d)' % (
            path, depth, ctype, interlace))
    raw, stride = zlib.decompress(idat), w * ch
    prev, pix = bytearray(stride), []
    for y in range(h):
        f = raw[y * (stride + 1)]
        cur = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = cur[i - ch] if i >= ch else 0
            b = prev[i]
            c = prev[i - ch] if i >= ch else 0
            if f == 1:
                cur[i] = (cur[i] + a) & 255
            elif f == 2:
                cur[i] = (cur[i] + b) & 255
            elif f == 3:
                cur[i] = (cur[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 255
            elif f != 0:
                raise ValueError('%s: PNG filter %d' % (path, f))
        for x in range(w):
            p = cur[x * ch:(x + 1) * ch]
            pix.append((p[0], p[1], p[2]) if ch >= 3 else (p[0],) * 3)
        prev = cur
    return w, h, pix


def rgb565(r, g, b):
    """The simulator's LCD_RGBPACK for RGB565 (firmware/export/lcd.h)."""
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3


def load_screens():
    out = []
    for rel in SCREENS:
        p = os.path.join(GOLDEN, rel)
        if not os.path.exists(p):
            return None, 'golden %s not found' % rel
        w, h, pix = png_decode(p)
        if (w, h) != (W, H):
            return None, 'golden %s is %dx%d' % (rel, w, h)
        out.append((rel, [rgb565(*q) for q in pix]))
    return out, ''


class Suite(D.Suite):
    def t_real(self, screens):
        for pid in PANELS:
            for dma in (False, True):
                row = T.Row('real frames, %s push, panel %d' % ('DMA' if dma else 'CPU', pid))
                self.rows.append(row)
                b, h, _ = self.bench(pid, D.DMA if dma else ())
                n = 0
                try:
                    for i, (rel, fb) in enumerate(screens):
                        h.fb = list(fb)
                        s0 = b.sim.steps
                        if i == 0:
                            h.lcd_awake()     # powers the panel on, then the frame
                        else:
                            h.lcd_update()
                        h.settle()
                        steps = b.sim.steps - s0
                        tag = 'real_%s_p%d_%s' % ('dma' if dma else 'cpu', pid,
                                                  rel.replace('/', '_')[:-4])
                        self.frame_check(row, b, h, tag,
                                         png=(pid == 0 and i == 1))
                        row.check(b.panel.fb == fb, '%s: panel differs from the RGB565 source' % tag)
                        if i:
                            n += 1
                            row.instr += steps
                    if dma:
                        self.dma_check(row, b, 'real_dma_p%d' % pid)
                    k = 'real_%s_frame_instr_p%d' % ('dma' if dma else 'cpu', pid)
                    self.metrics[k] = row.instr // n
                except T.Fail as e:
                    row.check(False, str(e))

    def run(self):
        t0 = time.time()
        screens, why = load_screens()
        if screens is None:
            print('skip: %s' % why)
            return 0
        self.ref = None
        self.t_real(screens)
        self._print(self.rows)
        cases = sum(r.cases for r in self.rows)
        fails = sum(len(r.fails) for r in self.rows)
        for k in sorted(self.metrics):
            print('  %-28s %s' % (k, self.metrics[k]))
        print('%d rows, %d cases, %d failed (%.0f s); PNGs in %s' % (
            len(self.rows), cases, fails, time.time() - t0, self.png_dir))
        return 1 if fails else 0


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--seed', type=int, default=2722)
    ap.add_argument('--vmcs', default=T.DEFAULT_VMCS)
    ap.add_argument('--png', default=os.path.join(T.ROOT, 'build', 'bcmfw', 'test-real'))
    ap.add_argument('--only', default='')
    a = ap.parse_args(argv)
    if not B.have_gcc():
        print('skip: vc4-elf-gcc not found at %s (set VC4_GCC)' % T.C.GCC)
        return 0
    return Suite(a).run()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
