#!/usr/bin/env python3
#***************************************************************************
#               __________                   __    __________
#    Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
#    Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
#    Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
#    Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
#                                               \/        \/               \/
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
"""vc2sim: the first-use video check (apps/video/vcheck.c) against the
merged image, the PP's own C over a host port into the simulator.

The check's code and the VP client (bcmvp.c) are built for the Mac
(vcheck_shim.c) and driven by ctypes; their port calls go to vc2sim
running the merged image built from source, which must be byte for byte
the committed bcmvideo-jobs.bin.  Rows:
  - the check at the top band: PASS, all 30 pictures' sums exact;
  - the same with the host's SDRAM writes reaching the image only behind
    a host read of SDRAM or 2 ms after them (vbcm_model.h VBM_LAND_US:
    their order against the image's reads is not proven on silicon):
    PASS, the check reads its packets back before any FEED;
  - sitting 4b's sequence (each FEED straight after its packet, STATUS
    back to back), replayed under that landing: the image answers and
    takes the packet, and no picture comes for the whole 400 ms: the
    line silicon logged at every band ("vck 150 FAIL chip p0 rc-2").

Usage: bcmfw/tests/vcheck_image_test.py [-v]
"""
import ctypes as ct
import os
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hello_test as T        # noqa: E402
import hostproto_test as HP   # noqa: E402
import video_test as V        # noqa: E402

sys.path.insert(0, T.FW)
import mkvideo                # noqa: E402

K = HP.K
B, C = T.B, T.C
ROOT = T.ROOT
APPS = os.path.join(ROOT, 'apps')
OUT = os.path.join(ROOT, 'build', 'bcmfw', 'vcheck-image')
STREAM = os.path.join(T.FW, 'video', 'vcheck.bin')
SDRAM_HOST = 0xC0000000
LAND_US = 2000                  # vbcm_model.h VBM_LAND_US
PIC_WAIT_US = 400000            # vcheck.h VCHECK_PIC_WAIT_US

RD = ct.CFUNCTYPE(ct.c_int, ct.c_uint32, ct.POINTER(ct.c_uint32))
WR = ct.CFUNCTYPE(ct.c_int, ct.c_uint32, ct.c_uint32)
WRB = ct.CFUNCTYPE(ct.c_int, ct.c_uint32, ct.c_void_p, ct.c_uint32)
NOW = ct.CFUNCTYPE(ct.c_uint32)
YL = ct.CFUNCTYPE(None)
RB = ct.CFUNCTYPE(ct.c_int)


def build_lib():
    os.makedirs(OUT, exist_ok=True)
    lib = os.path.join(OUT, 'libvcheck.so')
    v = os.path.join(APPS, 'video')
    subprocess.check_call(['cc', '-shared', '-fPIC', '-O1', '-std=gnu99', '-Wall',
                           '-Wextra', '-Werror', '-I' + APPS, '-I' + v, '-o', lib,
                           os.path.join(HERE, 'vcheck_shim.c'),
                           os.path.join(v, 'vcheck.c'), os.path.join(v, 'bcmvp.c'),
                           os.path.join(v, 'vclk.c')])
    return ct.CDLL(lib)


class Port:
    """The PP's host port into a VideoBench, the core running as the PP
    waits: a read about 1 us of the core's time, a write less, a block
    by its length.  land_us > 0: the host's SDRAM writes reach the image
    only behind a host read of SDRAM or land_us after them."""

    def __init__(self, b, land_us=0):
        self.b, self.land_us, self.pending = b, land_us, []
        self.reads = 0
        run = b.run

        def run_landing(n=None):
            self.land()
            return run() if n is None else run(n)
        b.run = run_landing
        self.cbs = (RD(self.rd), WR(self.wr), WRB(self.wrb), NOW(self.now),
                    YL(self.yl), RB(lambda: -1))

    def land(self, force=False):
        now = self.b.now()
        while self.pending and (force or now - self.pending[0][0] >= self.land_us):
            _, off, data = self.pending.pop(0)
            self.b.host_sdram_write(off, data)

    def sdram_write(self, off, data):
        if self.land_us:
            self.pending.append((self.b.now(), off, data))
        else:
            self.b.host_sdram_write(off, data)

    @staticmethod
    def sdram(a):
        return SDRAM_HOST <= a < SDRAM_HOST + V.SDRAM_SIZE

    def rd(self, a, p):
        self.reads += 1
        self.b.run(150)
        if self.sdram(a):
            self.land(True)
            p[0] = struct.unpack_from('<I', self.b.bus.b,
                                      V.S.SDRAM_BACKING + a - SDRAM_HOST)[0]
        else:
            p[0] = self.b.r32(a)
        return 0

    def wr(self, a, v):
        self.b.run(50)
        if self.sdram(a):
            self.sdram_write(a - SDRAM_HOST, struct.pack('<I', v))
        else:
            self.b.w32(a, v)
        return 0

    def wrb(self, a, src, n):
        data = ct.string_at(src, 2 * n)
        self.b.run(10 + n)
        if self.sdram(a):
            self.sdram_write(a - SDRAM_HOST, data)
        else:
            for i in range(0, len(data) & ~3, 4):
                self.b.w32(a + i, struct.unpack_from('<I', data, i)[0])
        return 0

    def now(self):
        return self.b.now() & 0xFFFFFFFF

    def yl(self):
        self.b.run(500)


class Suite(V.Suite):
    def merged(self):
        b, h, vp, prog = self.setup(3)
        V.vclk_timer(b)
        return b, vp

    def check_image(self, row):
        prog, rep, img = self.image(V.DEFS)
        img = img + b'\0' * (-len(img) % 4)
        ref = open(os.path.join(T.FW, 'video', 'bcmvideo-jobs.bin'), 'rb').read()
        row.check(img == ref, 'the merged image built from source is the committed '
                  'bcmvideo-jobs.bin (%d bytes, %d committed)' % (len(img), len(ref)))

    def check_row(self, name, land_us):
        row = T.Row(name)
        self.rows.append(row)
        self.check_image(row)
        b, vp = self.merged()
        port = Port(b, land_us)
        lib = self.lib
        rc = lib.t_attach(*port.cbs)
        row.check(rc == 0, 'attach and HELLO: %d' % rc)
        data = open(STREAM, 'rb').read()
        buf = ct.create_string_buffer(data, len(data))
        pics, band, why = ct.c_int(), ct.c_int(), ct.c_int()
        t0 = b.now()
        v = lib.t_run(buf, len(data), 1, ct.byref(pics), ct.byref(band), ct.byref(why))
        self.log('%s: verdict %d, band %d, %d pictures, why %d, %d us, %d reads' % (
            name, v, band.value, pics.value, why.value, b.now() - t0, port.reads))
        row.check(v == 0 and band.value == 11 and pics.value == 30,
                  'PASS at 150 MHz with 30 exact sums: verdict %d, band %d, %d '
                  'pictures, why %d' % (v, band.value, pics.value, why.value))

    def t_check(self):
        self.check_row('vcheck: the check against the merged image', 0)

    def t_check_landing(self):
        self.check_row('vcheck: the same, SDRAM writes landing behind a read or 2 ms',
                       LAND_US)

    def t_sitting_4b(self):
        """The first build's sequence, as vcheck.c then ran it"""
        row = T.Row('vcheck: sitting 4b\'s sequence under that landing: no picture')
        self.rows.append(row)
        b, vp = self.merged()
        port = Port(b, LAND_US)
        data = open(STREAM, 'rb').read()
        n = struct.unpack_from('<I', data, 8)[0]
        len0 = struct.unpack_from('<I', data, 16)[0]
        pkt = data[16 + 4 * n:16 + 4 * n + len0]
        op, st, p = vp.call(K['HP_OP_HELLO'], [K['HP_MAJOR'] << 8 | K['HP_MINOR']])
        st, p = vp.open(320, 240, K['HP_MODE_FIT'],
                        K['HP_OPEN_OWN_DISPLAY'] | K['HP_OPEN_FAST_CLOCK'])
        row.check(st == K['HP_OK'], 'OPEN: %d' % st)
        if st != K['HP_OK']:
            return
        # the packet, then its FEED at once (bvp_feed as it was)
        port.sdram_write(vp.fifo_off, pkt + b'\0' * (-len(pkt) % 4))
        tag = vp.send(K['HP_OP_FEED'], [vp.session, 0, len0, 0, 0, K['HP_FEED_KEY']])
        row.check(tag is not None, 'FEED sent')
        t0, replies, r = b.now(), 0, []
        while b.now() - t0 <= PIC_WAIT_US:
            _, st, r = vp.call(K['HP_OP_STATUS'], [vp.session], timeout_us=200_000)
            replies += 1
            if len(r) >= 16 and r[6] >= 1 and b.r32(0x1C4) >> 24 == 0x0F:
                break
            b.run(500)
        prog, rd = b.r32(0x1C4), b.r32(K['HP_FIFO_RD']) & 0xFFFF
        self.log('sitting 4b: %d STATUS replies in %d us; decoded %d, packets %d, '
                 'MB_PROG %08x, FIFO_RD %d' % (replies, b.now() - t0, r[6], r[2], prog, rd))
        row.check(replies > 10 and len(r) >= 16 and r[6] == 0 and r[2] == 1 and
                  prog >> 24 == 0x0F and rd == (len0 + 63) >> 6,
                  'the image answered (%d replies), took the packet (FIFO_RD %d, '
                  'packets %d, MB_PROG %08x) and gave no picture (decoded %d)' % (
                      replies, rd, r[2] if r else -1, prog, r[6] if r else -1))

    def run(self):
        t0 = time.time()
        self.lib = build_lib()
        for t in (self.t_check, self.t_check_landing, self.t_sitting_4b):
            n0 = len(self.rows)
            try:
                t()
            except (T.Fail, HP.Timeout) as e:
                row = T.Row(t.__name__)
                self.rows.append(row)
                row.check(False, str(e))
            for row in self.rows[n0:]:
                print('%-72s %5d %5d' % (row.name[:72], row.cases,
                                         row.cases - len(row.fails)))
                for f in row.fails[:8]:
                    print('    FAIL: %s' % f)
        cases = sum(r.cases for r in self.rows)
        fails = sum(len(r.fails) for r in self.rows)
        print('%d rows, %d cases, %d failed (%.0f s)' % (len(self.rows), cases, fails,
                                                         time.time() - t0))
        return 1 if fails else 0


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--seed', type=int, default=2722)
    ap.add_argument('--png', default=os.path.join(ROOT, 'build', 'bcmfw', 'test-vcheck'))
    a = ap.parse_args(argv)
    if not B.have_gcc():
        print('skip: vc4-elf-gcc not found at %s (set VC4_GCC)' % C.GCC)
        return 0
    V.DEFS = tuple(mkvideo.JOBS_DEFS)
    return Suite(a).run()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
