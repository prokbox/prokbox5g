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
"""The vector unit's silicon readings against vc2sim's variants.

Bench stage 4's vector checks ran on silicon on 2026-10-25 (build A,
session log, bench-20261025 bcmtest.log, B4V 0-46; both runs read
the same words; SILICON below copies each record's first four words).
Each check's instructions run here in vc2sim alone (no image: its rows
set in the VRF, its registers set, its instructions assembled from
gen_bench.py's text), under every combination of the variant switches
the failures opened (tools/bcmre/vc2vpu.py HYP v_a16_reg_x: an H16 A
operand's +r x, ASHF/ASHG; v_acc_sat: how the accumulator saturates,
X15S/REPA), with sitting 1's scalar rule (HYP off16_h 'swap', vc2sim's default).  The
checks load no scalar halfword: their inputs come by vld16, their words
are read back as whole words (bench.c), so that rule can't move them.

Rows:
  - the variants that reproduce every logged word (LD8R aside: a memory
    load this harness doesn't run; it passed on silicon) are exactly
    vc2vpu.SILICON_FITS, and the old model is not among them;
  - vc2sim's defaults are among them;
  - ASHX and ACSA (gen_bench.py vpu_fit_cases) give a different first
    record under each variant of their switch, and readlog.py's
    variant_verdict names the variant from those words;
  - every other check reads the same under every surviving variant;
  - bench row GTHR runs jv_gather16's own four ops (gen_jobs.py).

Usage: bcmfw/tests/vpu_fit_test.py [-v]
"""
import itertools
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
ROOT = os.path.dirname(FW)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bcmre'))
sys.path.insert(0, os.path.join(FW, 'bench'))
sys.path.insert(0, os.path.join(FW, 'video'))
import vc2dis as D      # noqa: E402
import vc2sim as S      # noqa: E402
import vc2vpu as VP     # noqa: E402
import gen_bench as G   # noqa: E402
import gen_jobs as GJ   # noqa: E402
import readlog as RL    # noqa: E402

# (check, tag, its record's first four words on silicon, 2026-10-25)
SILICON = (
    (0, 'COP8', '7fff8000 0000ffff 3aa80001 fc0b4ffe'),
    (1, 'SUMR', '00000789 a5a5a5a5 a5a5a5a5 a5a5a5a5'),
    (2, 'CNST', 'fff4fff4 fff4fff4 fff4fff4 fff4fff4'),
    (3, 'CSUM', '00000f40 a5a5a5a5 a5a5a5a5 a5a5a5a5'),
    (4, 'ALUX', '7fff8000 0000ffff 3aa80001 fc0b4ffe'),
    (5, 'MXSU', '000006d4 a5a5a5a5 a5a5a5a5 a5a5a5a5'),
    (6, 'COPY', '7fff8000 0000ffff 3aa80001 fc0b4ffe'),
    (7, 'VMAX', '7fff7fff 00000000 3aa80001 fc0b4ffe'),
    (8, 'VMIN', '80008000 ffffffff 8df90001 dc714b6f'),
    (9, 'ADDI', '801f8020 0020001f 3ac80021 fc2b501e'),
    (10, 'ADIR', '14bb084e 1da7a344 cdb5c351 01b06252'),
    (11, 'AD8I', '01000001 00800081 00900002 00160042'),
    (12, 'ADSN', '07227000 41ff5600 6c00f298 e86d2eb2'),
    (13, 'MOVN', 'b455fff9 fff9fff9 fff91192 02b9d37f'),
    (14, 'MULR', '90d9a93e 4033fd12 8dc3bdf2 9471aae7'),
    (15, 'MUL8', '13ec0000 09ec0a00 0b2c0014 01a40514'),
    (16, 'MU8R', 'fb050000 fd85fd80 fc3bfffb fdf3fcc7'),
    (17, 'MACC', '0064fe66 fe680062 fec2ffa6 002cff2a'),
    (18, 'MHR4', '00000000 00010001 03fc2000 00020001'),
    (19, 'RSB0', 'ffff0000 f8300001 f8d007d0 fd9ffa83'),
    (20, 'SUBZ', '00000000 00000001 00000000 00000000'),
    (22, 'MHRN', 'be007eb0 f900fa00 5d4fc501 b90067ff'),
    (23, 'MNEG', 'fc04ff01 fd06fdff fcf3ff9d ff90fe5f'),
    (24, 'ADRG', '705e85fe 0000fb2e 1edaf65c 9852b4c5'),
    (25, 'MVNZ', '994d41a2 434deb32 4d17ac4d 888af34d'),
    (26, 'MVRP', '1cc86bc8 efc8fac8 4ac87fc8 59c80cc8'),
    (27, 'MH8I', 'e7005200 cb007101 3eff3eff ec02c801'),
    (28, 'MLMI', 'f9c20000 fce4fcde fc34fff9 fce4fbd0'),
    (29, 'LSR8', 'bf1fa400 cb0f3210 8c13e300 870f0915'),
    (30, 'AD68', '80ff7fff 007f007f e0f16c7a 70e22e46'),
    (31, 'WRAP', 'f8000000 f8000000 d8000800 f8005800'),
    (32, 'CLRQ', 'f8ff6921 180008ff 0c008a00 eaff3c01'),
    (33, 'STBK', 'e937f1b7 33f854fc 8e4312c8 02039326'),
    (34, 'RNDT', 'fffeffff ffc0fffd 00020001 ffff0000'),
    (35, 'RNDU', 'fffeffff ffc0fffd 00020001 ffff0000'),
    (36, 'MLSC', 'e500da00 c2ff2400 e0ff4dff 62ffa401'),
    (37, 'ASHF', 'be7cdd8b b5b5bd8e 7aaf12b4 76f802b3'),
    (38, 'ASHG', '6b918b77 989c8a0e a896efcf 35bcec02'),
    (39, 'X15M', 'ffff0000 00000000 fe2001df 0000ffff'),
    (40, 'X15S', 'fffa0004 03e7ffff 7ffefc17 00068000'),
    (41, 'X15Z', 'fffb0005 03e80000 7ffffc18 00078000'),
    (42, 'MHDU', '0000ffff 7ffec000 0126fed5 e5da3037'),
    (43, 'VCLP', '00000000 00ff00ff 00000000 003300ff'),
    (44, 'R48E', 'fffa4795 dc2e6cd2 f2804cbc de7fcb22'),
    (45, 'D16R', 'c36d6983 bea62293 2958e7ba d779b494'),
    (46, 'REPA', '02097fff c20401ff 01100211 01fd5bb9'),
    # sitting 2 (bench-20261025-2257/bcmtest.1.log): the put, and the two
    # rows that settle the A operand's +r x and the accumulator
    (47, 'PUTV', 'db64a505 fb26a50e b84ca5fb c96ca573'),
    (49, 'ASHX', '46034350 8d6eaadf a5ea4233 68512e4a'),
    (50, 'ACSA', '80017ffe 0bb57fff 0000ffff 0000ffff'),
)
SWITCHES = ('v_a16_reg_x', 'v_acc_sat', 'v_vd_reg_y')
MEMORY = ('LD8R',)              # loads from memory in the check itself


def run_case(c, hyp):
    """c's stored words (gen_bench.VCase.expected's layout) in vc2sim
    under hyp (overrides of vc2sim.HYP)"""
    saved = {k: S.HYP[k] for k in hyp}
    S.HYP.update(hyp)
    # the checks probe the edges on purpose (odd +r x, y 8): strict
    # mode's refusals are for our kernels, not for the bench's probes
    strict = S.set_strict_align('')
    try:
        sim = S.Sim()
        v = sim.vrf
        for r, hs in c.rows:
            y, x = G._yx(r)
            for i in range(16):
                h = hs[i] & 0xFFFF
                v.b[y * 64 + x + i] = h & 0xFF
                v.b[y * 64 + x + i + 16] = h >> 8
        for reg, val in c.regs:
            sim.r[reg] = val & 0xFFFFFFFF
        for text in c.pre + c.test + c.post:
            m = re.match(r'mov r(\d+), #(-?\w+)$', text)
            if m:
                sim.r[int(m.group(1))] = int(m.group(2), 0) & 0xFFFFFFFF
                continue
            S.compile_ins(sim, D.decode_words(D.assemble(text, 0), 0))()
        out = []
        for reg, _ in c.scal:
            val = sim.r[reg]
            out += [val & 0xFFFF, val >> 16] + [G.FILL & 0xFFFF] * 14
        for r in c.outs:
            y, x = G._yx(r)
            out += [v.b[y * 64 + x + i] | v.b[y * 64 + x + i + 16] << 8 for i in range(16)]
        return [out[i] | out[i + 1] << 16 for i in range(0, len(out), 2)]
    finally:
        S.set_strict_align(strict)
        S.HYP.update(saved)


def key(h):
    return tuple(sorted(h.items()))


class Row:
    def __init__(self, name):
        self.name, self.errs = name, []

    def check(self, ok, msg):
        if not ok:
            self.errs.append(msg)


def main(argv):
    verbose = '-v' in argv
    # the vector checks (HWLD's are scalar: bench_test runs them)
    cases = [c for c in G.vcases() if not isinstance(c, G.HCase)]
    by = {c.tag: c for c in cases}
    rows = []
    r = Row('the variants fitting all silicon records are vc2vpu.SILICON_FITS')
    rows.append(r)
    fitting = []
    for vals in itertools.product(*(VP.HYP_ALTERNATIVES[k] for k in SWITCHES)):
        h = dict(zip(SWITCHES, vals))
        bad = [tag for _, tag, words in SILICON if tag not in MEMORY and
               run_case(by[tag], dict(h, off16_h='swap'))[:4] !=
               [int(w, 16) for w in words.split()]]
        if verbose:
            print('  %-34s %s' % (' '.join('%s' % v for v in vals),
                                  'fits' if not bad else 'misses ' + ' '.join(bad)))
        if not bad and not any((k, v) in VP.SILICON_EXCLUDED for k, v in h.items()):
            fitting.append(h)
    want = [dict(zip(SWITCHES, v)) for v in itertools.product(
        *(VP.SILICON_FITS[k] for k in SWITCHES))]
    r.check(sorted(map(key, fitting)) == sorted(map(key, want)),
            'fitting %s, SILICON_FITS %s' % (fitting, want))
    r.check(key({'v_a16_reg_x': 'given', 'v_acc_sat': 'none', 'v_vd_reg_y': 'given'})
            not in map(key, fitting),
            'the old model fits')
    r = Row('vc2sim\'s defaults are a fitting variant, and the bench model is vc2sim\'s')
    rows.append(r)
    dflt = {k: S.HYP[k] for k in SWITCHES}
    r.check(key(dflt) in map(key, fitting), 'defaults %s' % dflt)
    for c in cases:
        if c.tag in MEMORY:
            continue
        exp = c.expected()
        r.check(run_case(c, {}) == [exp[i] | exp[i + 1] << 16 for i in range(0, len(exp), 2)],
                '%s: gen_bench\'s model and vc2sim differ' % c.tag)
    r = Row('ASHX, ACSA: one record per variant; readlog names it')
    rows.append(r)
    for tag, sw in (('ASHX', 'v_a16_reg_x'), ('ACSA', 'v_acc_sat')):
        seen = {}
        for val in VP.HYP_ALTERNATIVES[sw]:
            w = run_case(by[tag], {sw: val})[:4]
            seen.setdefault(tuple(w), []).append(val)
            name = RL.variant_verdict(tag, w)
            r.check(name.startswith(val + ':'), '%s under %s: readlog says %r' % (tag, val, name))
            if verbose:
                print('  %s %-8s %s' % (tag, val, ' '.join('%08x' % x for x in w)))
        r.check(len(seen) == len(VP.HYP_ALTERNATIVES[sw]),
                '%s: %s' % (tag, seen))
        r.check(RL.VARIANT_WORDS[tag][0][0] == S.HYP[sw], '%s: readlog\'s first (modelled) '
                'variant is not vc2sim\'s default' % tag)
    r = Row('every other check reads the same under every surviving variant')
    rows.append(r)
    for c in cases:
        if c.tag in MEMORY + ('ASHX', 'ACSA', 'ASHF', 'ASHG', 'X15S', 'REPA', 'PUTV'):
            continue
        ws = {tuple(run_case(c, h)) for h in fitting}
        r.check(len(ws) == 1, '%s differs between surviving variants' % c.tag)
    r = Row('GTHR runs jv_gather16\'s four ops (gen_jobs.py), exact under each variant')
    rows.append(r)
    r.check(by['GTHR'].test == GJ.gather_ops(), 'GTHR %s, gather %s' % (
        by['GTHR'].test, GJ.gather_ops()))
    exp = by['GTHR'].expected()
    exp = [exp[i] | exp[i + 1] << 16 for i in range(0, len(exp), 2)]
    for vals in itertools.product(*(VP.HYP_ALTERNATIVES[k] for k in SWITCHES)):
        h = dict(zip(SWITCHES, vals))
        r.check(run_case(by['GTHR'], h) == exp, 'GTHR differs under %s' % h)
    bad = 0
    for r in rows:
        print('%s  %s' % ('ok  ' if not r.errs else 'FAIL', r.name))
        for e in r.errs[:8]:
            print('        ' + e)
        bad += bool(r.errs)
    print('vpu_fit_test: %d of %d rows pass' % (len(rows) - bad, len(rows)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
