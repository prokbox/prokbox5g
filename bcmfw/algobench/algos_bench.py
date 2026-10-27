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

"""The algorithms survey's measurements (docs/research/bcm2722/algorithms.md):
instruction counts in vc2sim for the COMMIT hash candidates (sumbench.c)
and the JPEG entropy decoders (huffbench.c: jpeg_load.c's path replicated,
and the exact table-driven form), plus a per-function profile of the real
decoder (jdec_one).  Research scaffolding, not part of any image.

Usage: bcmfw/algobench/algos_bench.py sum
       bcmfw/algobench/algos_bench.py huff FILE.jpg:KY:KC ...   (k_need luma, chroma)
       bcmfw/algobench/algos_bench.py prof FILE.jpg:W:H ...
On the Mac: cc -O2 -std=gnu11 huffbench.c huffbench_main.c; ./a.out F KY KC
            cc -O2 fade_identity.c && ./a.out"""
import bisect
import collections
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
ROOT = os.path.dirname(FW)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bcmre'))
sys.path.insert(0, os.path.join(FW, 'tests'))
import vc2cc as C     # noqa: E402
import vc2sim as S    # noqa: E402

AT = 0x800000
LIBC = os.path.join(FW, 'jpeg', 'libc.c')


def build(srcs, extra=('-std=gnu11', '-fno-tree-loop-distribute-patterns')):
    """srcs with the image's memcpy and memset (bcmfw/lib/string.c)"""
    lib = ('string.s', C.compile_c(os.path.join(FW, 'lib', 'string.c'), '-O2',
                                   ['-I' + FW, '-std=gnu99', '-fno-tree-loop-distribute-patterns']))
    return C.build(list(srcs) + [lib], '-O2', extra=list(extra), libs=C.DEFAULT_LIBS,
                   no_float=True, conservative=True)


def profiler():
    """count executions per pc (wraps Sim.fetch)"""
    cnt = collections.Counter()
    orig = S.Sim.fetch

    def fetch(self, pc):
        fn, sz, ins = orig(self, pc)

        def w(fn=fn, pc=pc):
            cnt[pc] += 1
            return fn()
        h = (w, sz, ins)
        self.code[pc] = h
        return h
    S.Sim.fetch = fetch
    return cnt


def cmd_sum():
    prog = build([os.path.join(HERE, 'sumbench.c')])
    s = S.Sim()
    C.load_into_sim(prog, s)
    n = 18000
    random.seed(1)
    s.mem.b[AT:AT + n] = bytes(random.randrange(256) for _ in range(n))
    for f, k in (('h_fnv', n // 2), ('h_fl32', n // 4), ('h_fl32u', n // 4), ('h_f64', n // 4),
                 ('h_f64u', n // 4), ('h_f16w', n // 4), ('h_xxh32', n // 4)):
        s0 = s.steps
        rc = s.call(f, AT, k, max_steps=10 ** 9)
        st = s.steps - s0
        print('%-8s %08x  %.2f instructions a byte; 180 KB: %.2f M' % (
            f, rc & 0xffffffff, st / n, st / n * 184320 / 1e6))


def cmd_huff(args):
    prog = build([os.path.join(HERE, 'huffbench.c'), LIBC])
    s = S.Sim()
    C.load_into_sim(prog, s)
    for a in args:
        name, ky, kc = a.rsplit(':', 2)
        d = open(name, 'rb').read()
        s.mem.b[AT:AT + len(d)] = d
        assert s.call('parse', AT, len(d), max_steps=10 ** 8) == 0
        res = []
        for fn in ('dec_ref', 'dec_fast', 'dec_ref'):  # bss is not zeroed: the 2nd ref counts
            s0 = s.steps
            rc = s.call(fn, int(ky), int(kc), max_steps=4 * 10 ** 9)
            res.append((rc & 0xffffffff, s.steps - s0))
        ref, fast = res[2], res[1]
        print('%s k_need %s/%s: ref %08x %.2f M, fast %08x %.2f M, %s, x%.2f' % (
            name, ky, kc, ref[0], ref[1] / 1e6, fast[0], fast[1] / 1e6,
            'same' if ref[0] == fast[0] else 'DIFF', ref[1] / fast[1]), flush=True)


def cmd_prof(args):
    import jpeg_test as J
    prog = J.vc_build()
    cnt = profiler()
    s = S.Sim()
    C.load_into_sim(prog, s)
    syms = sorted((a, n) for n, a in prog.symbols.items() if not n.startswith(('.', '__')))
    addrs = [a for a, _ in syms]
    for a in args:
        name, w, h = a.rsplit(':', 2)
        d = open(name, 'rb').read()
        s.mem.b[J.JPG_AT:J.JPG_AT + len(d)] = d
        cnt.clear()
        rc = s.call('jdec_one', J.JPG_AT, len(d), J.OUT_AT, int(w), int(h), J.WORK_AT,
                    stack_args=(J.WORK_MAX,), max_steps=4 * 10 ** 9)
        per = collections.Counter()
        for pc, c in cnt.items():
            per[syms[bisect.bisect_right(addrs, pc) - 1][1]] += c
        tot = sum(cnt.values())
        print('%s -> %sx%s (rc %d): %.2f M instructions' % (name, w, h, rc, tot / 1e6))
        for n, c in per.most_common(14):
            print('   %-30s %6.2f M  %4.1f %%' % (n, c / 1e6, 100.0 * c / tot))


if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1] not in ('sum', 'huff', 'prof'):
        print(__doc__)
        sys.exit(2)
    {'sum': lambda: cmd_sum(), 'huff': lambda: cmd_huff(sys.argv[2:]),
     'prof': lambda: cmd_prof(sys.argv[2:])}[sys.argv[1]]()
