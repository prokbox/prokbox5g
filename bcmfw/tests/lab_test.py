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

"""The memory lab (HP_OP_LAB, bench rows B6 and B7) in vc2sim, on the
shipped video image: refusals, then each kind over SRAM and SDRAM with
its checksum against the same arithmetic here, and a store's words read
back through the host port.  The times are the simulator's (no memory
stalls modelled); only the device's mean anything.

Usage: bcmfw/tests/lab_test.py
"""
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hello_test as T      # noqa: E402
import video_test as V      # noqa: E402

K = V.K
SHIPPED = ('BCMFW_VIDEO=1', 'BCMFW_IDLE_SLEEP=1', 'BCMFW_IDLE_GATED=1',
           'BCMFW_IDLE_DOORBELL=1')
M32 = 0xFFFFFFFF


def lcg(x):
    return (x * 1664525 + 1013904223) & M32


def model(kind, mem, log2, count):
    """The checksum lab_run() returns, from mem (bytes of the region)"""
    hmask, wmask = (1 << (log2 - 1)) - 1, (1 << (log2 - 2)) - 1
    x, s = 0x2722C0DE, 0
    for i in range(count):
        if kind == K['HP_LAB_EMPTY']:
            x = lcg(x)
            s += (x >> 8) & hmask
        elif kind == K['HP_LAB_RAND16']:
            x = lcg(x)
            a = 2 * ((x >> 8) & hmask)
            s += mem[a] | mem[a + 1] << 8
        elif kind == K['HP_LAB_SEQ32']:
            a = 4 * (i & wmask)
            s += int.from_bytes(mem[a:a + 4], 'little')
    return s & M32


class Suite(V.Suite):
    def bench(self, pid, defines=SHIPPED, reset_pc=None):
        return super().bench(pid, defines, reset_pc)

    def lab(self, vp, kind, base, log2, count):
        return vp.call(K['HP_OP_LAB'], [K['HP_LAB_MAGIC'], kind, base, log2, count],
                       timeout_us=5_000_000)

    def t_lab(self):
        row = T.Row('memory lab: refusals, kinds over SRAM and SDRAM, checksums')
        self.rows.append(row)
        try:
            b, h, vp, prog = self.setup(1)
            sram, sdram = K['HP_LAB_SRAM'], K['HP_LAB_SDRAM']
            op, st, _ = vp.call(K['HP_OP_LAB'], [0, 0, sram, 12, 16])
            row.check(st == K['HP_E_INVAL'], 'no magic: %d' % st)
            op, st, _ = self.lab(vp, 0, sram + 4, 12, 16)
            row.check(st == K['HP_E_INVAL'], 'a base off the regions: %d' % st)
            op, st, _ = self.lab(vp, 0, sdram, 12, 16)
            row.check(st == K['HP_E_STATE'], 'SDRAM before any OPEN: %d' % st)
            st, p = vp.open(320, 240)
            row.check(st == K['HP_OK'], 'OPEN %d' % st)
            rng = random.Random(54)
            for base, log2, name in ((sram, 12, 'SRAM'), (sdram, 14, 'SDRAM')):
                n = 1 << log2
                data = bytes(rng.getrandbits(8) for _ in range(n))
                if base == sram:
                    b.write_bytes(base, data)
                else:
                    b.host_sdram_write(base - V.SDRAM_BCM, data)
                for kind in (K['HP_LAB_EMPTY'], K['HP_LAB_RAND16'], K['HP_LAB_SEQ32']):
                    op, st, p = self.lab(vp, kind, base, log2, 2048)
                    want = model(kind, data, log2, 2048)
                    row.check(st == K['HP_OK'] and p and p[1] == want,
                              '%s kind %d: %d %s want %08x' % (name, kind, st, p, want))
                    if st == K['HP_OK']:
                        self.metrics['%s kind %d us (sim)' % (name, kind)] = p[0]
                op, st, p = self.lab(vp, K['HP_LAB_STORE32'], base, log2, 1 << (log2 - 2))
                row.check(st == K['HP_OK'], '%s store: %d' % (name, st))
                def rd(a):
                    if a >= V.SDRAM_BCM:
                        o = V.S.SDRAM_BACKING + a - V.SDRAM_BCM
                        return int.from_bytes(b.bus.b[o:o + 4], 'little')
                    return b.r32(a)
                bad = sum(1 for i in range(0, n, 4 * 37) if rd(base + i) != i // 4)
                row.check(bad == 0, '%s store: %d words wrong' % (name, bad))
                op, st, p = self.lab(vp, K['HP_LAB_COPY32'], base, log2, 1 << (log2 - 3))
                bad = sum(1 for i in range(0, n // 2, 4 * 41)
                          if rd(base + n // 2 + i) != rd(base + i))
                row.check(st == K['HP_OK'] and bad == 0, '%s copy: %d, %d wrong' % (
                    name, st, bad))
        except (T.Fail, V.HP.Timeout) as e:
            row.check(False, str(e))

    def run(self):
        self.t_lab()
        for row in self.rows:
            print('%-72s %5d %5d' % (row.name[:72], row.cases, row.cases - len(row.fails)))
            for f in row.fails[:8]:
                print('    FAIL: %s' % f)
        for k in sorted(self.metrics):
            print('  %-28s %s' % (k, self.metrics[k]))
        fails = sum(len(r.fails) for r in self.rows)
        print('%d failed' % fails)
        return 1 if fails else 0


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--seed', type=int, default=2722)
    ap.add_argument('--vmcs', default=T.DEFAULT_VMCS)
    ap.add_argument('--png', default=os.path.join(T.ROOT, 'build', 'bcmfw', 'test-lab'))
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--only', default='')
    a = ap.parse_args(argv)
    if not V.B.have_gcc():
        print('skip: vc4-elf-gcc not found')
        return 0
    return Suite(a).run()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
