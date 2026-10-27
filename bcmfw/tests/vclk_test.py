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

"""vc2sim: the level bands (P19 PW4; clock.c clock_band, HP_OP_VCLK).

The 1 MHz counter here follows the core clock the image set (instructions
/ MHz, one a cycle), so clock_band's speed check sees what silicon would.
Checks, in the plain and the merged image:
  - HELLO's caps carry HP_CAP_VCLK; STAT answers the 8 words, nothing
    written;
  - every band from the crystal and back: PLL A by Apple's rule (M 108, N =
    VCO / 250 kHz, the VCO >= 75 MHz), the core word 0x100 | divider,
    Apple's level for the clock; after every single write the level is at
    least Apple's for the clock running and SDRAM's refresh no slower than
    7.8 us (peek_test's pw_invariants); back on the crystal: PLL A off,
    the level found there;
  - band to band, up and down, the same invariants;
  - refused: no magic, a band past the table: nothing written;
  - a core that runs at the wrong speed (the counter off by a third) and a
    counter that stops during PLL A's lock: both land on the crystal at
    the level found, the failure counted;
  - OPEN with HP_OPEN_FAST_CLOCK runs at the top band, CLOSE on the
    crystal;
  - the busy meter: busy never more than the counter moved.

Usage: bcmfw/tests/vclk_test.py [--merged-only]"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hello_test as T      # noqa: E402
import video_test as V      # noqa: E402
import peek_test as P       # noqa: E402

K = V.K
BANDS = [27, 46, 59, 74, 87, 102, 116, 123, 130, 140, 147, 150]
MERGED = tuple(P.SHIPPED) + ('BCMFW_JOBS=1',)


def core_mhz(regs, scale=1.0):
    """the clock the core runs at, from the words the image wrote (0: the
    core on a stopped PLL)"""
    core = regs.get(P.PW_CORE, 1) & 0xFFFF
    if not core >> 8 & 7:
        return 27.0 * scale
    n = regs.get(P.PW_PLLN, 0)
    m = regs.get(P.PW_PLLM, 0) & 0xFF
    if not n & 0x400 or not m:
        return 0.0
    return 27.0 * (n & 0x3FF) / m / max(1, core & 0xFF) * scale


def invariants(writes, regs0):
    """peek_test's pw_invariants from any start (the core on the crystal
    or on PLL A): after each write the level is at least Apple's for the
    clock running and the refresh no slower than 7.8125 us"""
    level = regs0.get(P.PW_REG, 0) & 0xF
    core = regs0.get(P.PW_CORE, 1) & 0xFFFF
    n = regs0.get(P.PW_PLLN, 0) & 0x7FF
    ref = regs0.get(P.PW_REF, 0) >> 16 or None
    for i, (a, v) in enumerate(writes):
        if a == P.PW_REG:
            level = v & 0xF
        elif a == P.PW_CORE:
            core = v & 0xFFFF
        elif a == P.PW_PLLN:
            n = v & 0x7FF
        elif a == P.PW_REF:
            ref = v >> 16
        if core >> 8 & 7:
            if not n & 0x400:
                return '%d: the core on PLL A with PLL A off' % i
            hz = (n & 0x3FF) * 250000 // max(1, core & 0xFF)
        else:
            hz = 27000000
        if level < P.apple_level(hz):
            return '%d: %08x=%08x leaves level %d at %d Hz (table %d)' % (
                i, a, v, level, hz, P.apple_level(hz))
        if ref is not None and ref * 128000 > hz:
            return '%d: %08x=%08x leaves refresh %d cycles at %d Hz' % (
                i, a, v, ref, hz)
    return None


class ClockTimer:
    """the counter at the core's clock: run_machine advances it by its
    instructions at 100 a microsecond; here at the clock the image set (one
    a cycle), and reads add nothing.  scale: a core that runs that much
    off; stop_n: the counter stops while PLL A's control holds this N"""
    def __init__(self, b):
        self.b, self.t = b, b.timer
        self.t.us_per_read = 0
        self.frac = 0.0
        self.scale = 1.0
        self.stop_n = None
        self.inrm = False
        self.orig_adv = self.t.advance
        self.t.advance = self.advance
        rm = T.S.run_machine
        if not getattr(rm, 'vclk', False):
            def wrapped(sim, *a, **k):
                ct = getattr(sim, 'vclk_timer', None)
                if ct:
                    ct.inrm = True
                try:
                    return rm(sim, *a, **k)
                finally:
                    if ct:
                        ct.inrm = False
            wrapped.vclk = True
            T.S.run_machine = wrapped
        b.sim.vclk_timer = self

    def advance(self, us):
        if self.inrm:
            regs = self.b.bus.regs
            n = regs.get(P.PW_PLLN, 0)
            if self.stop_n is not None and n & 0x3FF == self.stop_n:
                return
            f = core_mhz(regs, self.scale)
            if not f:
                return
            self.frac += us * 100.0 / f
            us = int(self.frac)
            self.frac -= us
        self.orig_adv(us)


class Suite(P.Suite):
    def vclk(self, vp, sub, mhz=None, timeout_us=20_000_000):
        return vp.call(K['HP_OP_VCLK'], [K['HP_VCLK_MAGIC'], sub] +
                       ([mhz] if mhz is not None else []), timeout_us=timeout_us)

    def writes(self, b, acc):
        return [(x.addr, x.value) for x in acc if x.rw == 'W' and
                P.pw_space(x.addr) and x.seq not in b.host_seqs]

    def set_band(self, row, b, vp, band, name, *unused, mhz=None):
        regs0 = dict(b.bus.regs)
        (op, st, p), acc = self.window(b, lambda: self.vclk(vp, band + 1, mhz))
        w = self.writes(b, acc)
        row.check(st == K['HP_OK'] and len(p) == K['HP_VCLK_WORDS'],
                  '%s: reply %d, %d words' % (name, st, len(p)))
        if st != K['HP_OK'] or len(p) < K['HP_VCLK_WORDS']:
            return None
        bad = invariants(w, regs0)
        row.check(bad is None, '%s: %s' % (name, bad))
        return p, w

    def t_bands(self, defines, label):
        row = T.Row('VCLK %s: bands, Apple\'s order and values, refusals' % label)
        self.rows.append(row)
        try:
            b, h, vp = self.pw_setup(defines=defines, reg0=0xBB, sdram=True)
            ct = ClockTimer(b)
            row.check(vp.caps & K['HP_CAP_VCLK'], 'caps %x' % vp.caps)
            (op, st, p), acc = self.window(b, lambda: self.vclk(vp, K['HP_VCLK_STAT']))
            row.check(st == K['HP_OK'] and p[K['HP_VCLK_R_BAND']] == 0 and
                      not self.writes(b, acc), 'STAT: %d %s, nothing written' % (st, p[:3]))
            for args in ([0, 1], [K['HP_VCLK_MAGIC'], K['HP_VCLK_BANDS'] + 1],
                         [K['HP_VCLK_MAGIC']]):
                (op, st, p), acc = self.window(
                    b, lambda: vp.call(K['HP_OP_VCLK'], args, timeout_us=1_000_000))
                row.check(st == K['HP_E_INVAL'] and not self.writes(b, acc),
                          'refused %s: %d' % (args, st))
            ref0 = b.bus.regs.get(P.PW_REF, 0) >> 16 or None
            fails0 = None
            for k, f in enumerate(BANDS[1:], 1):
                r = self.set_band(row, b, vp, k, '27 -> %d' % f, 27, 0xB, ref0)
                if not r:
                    continue
                p, w = r
                n, div = P.apple_pll(f)
                regs = b.bus.regs
                row.check(p[K['HP_VCLK_R_BAND']] == k and p[K['HP_VCLK_R_FAILS']] == 0,
                          '%d: band %d, fails %d' % (f, p[0], p[K['HP_VCLK_R_FAILS']]))
                row.check(regs.get(P.PW_PLLM, 0) & 0xFFFF == 108 and
                          regs.get(P.PW_PLLN, 0) & 0xFFFF == n | 0x400 and
                          regs.get(P.PW_CORE, 0) & 0xFFFF == 0x100 | div,
                          '%d: M %x N %x core %x (Apple: N %d div %d)' % (
                              f, regs.get(P.PW_PLLM, 0), regs.get(P.PW_PLLN, 0),
                              regs.get(P.PW_CORE, 0), n, div))
                row.check(regs.get(P.PW_REG, 0) & 0xFF == 0xB0 | P.apple_level(f * 1000000),
                          '%d: level %x (Apple %d)' % (f, regs.get(P.PW_REG, 0),
                                                       P.apple_level(f * 1000000)))
                row.check(regs.get(P.PW_REF, 0) >> 16 == f * 125 >> 4,
                          '%d: refresh %d' % (f, regs.get(P.PW_REF, 0) >> 16))
                lv = regs.get(P.PW_REG, 0) & 0xF
                ref = regs.get(P.PW_REF, 0) >> 16
                r = self.set_band(row, b, vp, 0, '%d -> 27' % f, 0, lv, ref)
                if r:
                    p = r[0]
                    row.check(p[K['HP_VCLK_R_BAND']] == 0 and
                              regs.get(P.PW_CORE, 0) & 0xFFFF == 1 and
                              regs.get(P.PW_PLLN, 0) & 0xFFFF == 0 and
                              regs.get(P.PW_REG, 0) & 0xFF == 0xBB and
                              regs.get(P.PW_REF, 0) >> 16 == 210,
                              '%d -> 27: core %x N %x reg %x ref %d' % (
                                  f, regs.get(P.PW_CORE, 0), regs.get(P.PW_PLLN, 0),
                                  regs.get(P.PW_REG, 0), regs.get(P.PW_REF, 0) >> 16))
                    fails0 = p[K['HP_VCLK_R_FAILS']]
            # band to band, both ways
            seq = [11, 1, 7, 10, 2, 11, 0]
            cur_f, lv, ref = 27, 0xB, 210
            for k in seq:
                r = self.set_band(row, b, vp, k, '%d -> %d' % (cur_f, BANDS[k]),
                                  cur_f, lv, ref)
                if r:
                    row.check(r[0][K['HP_VCLK_R_BAND']] == k, 'at %d' % BANDS[k])
                cur_f = BANDS[k]
                lv = b.bus.regs.get(P.PW_REG, 0) & 0xF
                ref = b.bus.regs.get(P.PW_REF, 0) >> 16
            # inside a band (the in-band policy): a clock above the band
            # below's top, the band's level, no regulator write in the band;
            # 30 MHz needs the VCO at 3 x (90 MHz); out of the band: its top
            for k, f, want_f in ((4, 80, 80), (4, 88, 87), (4, 74, 87), (4, 60, 87),
                                 (1, 30, 30), (1, 46, 46), (7, 117, 117)):
                regs0 = dict(b.bus.regs)
                same = (regs0.get(P.PW_CORE, 1) >> 8 & 7) and \
                    regs0.get(P.PW_REG, 0) & 0xF == P.apple_level(BANDS[k] * 1000000)
                r = self.set_band(row, b, vp, k, 'band %d at %d MHz' % (BANDS[k], f),
                                  mhz=f)
                if not r:
                    continue
                p, w = r
                n, div = P.apple_pll(want_f)
                regs = b.bus.regs
                row.check(p[K['HP_VCLK_R_BAND']] == k and p[K['HP_VCLK_R_MHZ']] == want_f and
                          regs.get(P.PW_PLLN, 0) & 0xFFFF == n | 0x400 and
                          regs.get(P.PW_CORE, 0) & 0xFFFF == 0x100 | div and
                          regs.get(P.PW_REF, 0) >> 16 == want_f * 125 >> 4,
                          'band %d asked %d: ran %s MHz, N %x core %x (Apple: N %d div %d)' % (
                              BANDS[k], f, p[K['HP_VCLK_R_MHZ']], regs.get(P.PW_PLLN, 0),
                              regs.get(P.PW_CORE, 0), n, div))
                if same:
                    row.check(not any(a == P.PW_REG for a, _ in w),
                              'band %d at %d: no regulator write inside the band' % (
                                  BANDS[k], f))
            self.set_band(row, b, vp, 0, 'back to 27')
            # a core at the wrong speed: the crystal, the level found
            ct.scale = 0.66
            r = self.set_band(row, b, vp, 9, 'slow core', 27, 0xB, 210)
            ct.scale = 1.0
            if r:
                p = r[0]
                row.check(p[K['HP_VCLK_R_BAND']] == 0 and
                          p[K['HP_VCLK_R_FAILS']] == (fails0 or 0) + 1 and
                          p[K['HP_VCLK_R_MHZ']] == 27 and
                          b.bus.regs.get(P.PW_CORE, 0) & 0xFFFF == 1 and
                          b.bus.regs.get(P.PW_REG, 0) & 0xFF == 0xBB,
                          'slow core: band %d fails %d MHz %d core %x reg %x' % (
                              p[0], p[K['HP_VCLK_R_FAILS']], p[K['HP_VCLK_R_MHZ']],
                              b.bus.regs.get(P.PW_CORE, 0), b.bus.regs.get(P.PW_REG, 0)))
            # the counter stops while PLL A locks (N for 102 MHz)
            ct.stop_n = 408
            r = self.set_band(row, b, vp, 5, 'counter stops', 27, 0xB, 210)
            ct.stop_n = None
            if r:
                p = r[0]
                row.check(p[K['HP_VCLK_R_BAND']] == 0 and
                          b.bus.regs.get(P.PW_CORE, 0) & 0xFFFF == 1 and
                          b.bus.regs.get(P.PW_REG, 0) & 0xFF == 0xBB,
                          'counter stops: band %d core %x reg %x' % (
                              p[0], b.bus.regs.get(P.PW_CORE, 0),
                              b.bus.regs.get(P.PW_REG, 0)))
            # a session: OPEN with the flag runs at the top, CLOSE on the
            # crystal
            st, _ = vp.open(320, 240, flags=K['HP_OPEN_OWN_DISPLAY'] |
                            K['HP_OPEN_FAST_CLOCK'])
            _, st2, p = self.vclk(vp, K['HP_VCLK_STAT'])
            row.check(st == K['HP_OK'] and p[0] == len(BANDS) - 1,
                      'OPEN fast: %d band %d' % (st, p[0] if p else -1))
            r = self.set_band(row, b, vp, 4, 'in a session 150 -> 87', 150, 0xF,
                              b.bus.regs.get(P.PW_REF, 0) >> 16)
            vp.call(K['HP_OP_CLOSE'], [vp.session], timeout_us=1_000_000)
            _, st2, p2 = self.vclk(vp, K['HP_VCLK_STAT'])
            row.check(p2 and p2[0] == 0 and b.bus.regs.get(P.PW_REG, 0) & 0xFF == 0xBB,
                      'CLOSE: band %d reg %x' % (p2[0] if p2 else -1,
                                                 b.bus.regs.get(P.PW_REG, 0)))
            # the meter
            n0 = p2[K['HP_VCLK_R_NOW']], p2[K['HP_VCLK_R_BUSY']]
            _, _, p3 = self.vclk(vp, K['HP_VCLK_STAT'])
            dn = (p3[K['HP_VCLK_R_NOW']] - n0[0]) & 0xFFFFFFFF
            db = (p3[K['HP_VCLK_R_BUSY']] - n0[1]) & 0xFFFFFFFF
            row.check(dn > 0 and db <= dn, 'meter: busy %d of %d us' % (db, dn))
            self.metrics['%s: regulator writes, PLL locks' % label] = '%d, %d' % (
                p3[K['HP_VCLK_R_REGS']], p3[K['HP_VCLK_R_PLLS']])
        except (T.Fail, V.HP.Timeout) as e:
            row.check(False, str(e))

    def t_mutants(self):
        row = T.Row('VCLK mutants (plain image): a level raised late, no speed check')
        self.rows.append(row)
        try:
            b, h, vp = self.pw_setup(defines=tuple(P.SHIPPED) + ('VCLK_MUTANT=1',), reg0=0xBB)
            ClockTimer(b)
            regs0 = dict(b.bus.regs)
            (op, st, p), acc = self.window(b, lambda: self.vclk(vp, 12))
            bad = invariants(self.writes(b, acc), regs0)
            row.check(bad is not None, 'mutant 1 caught: %s' % bad)
            b, h, vp = self.pw_setup(defines=tuple(P.SHIPPED) + ('VCLK_MUTANT=2',), reg0=0xBB)
            ct = ClockTimer(b)
            ct.scale = 0.66
            _, st, p = self.vclk(vp, 10)
            row.check(p and p[0] == 9, 'mutant 2 stays on a slow core: band %s' % (
                p[0] if p else None))
        except (T.Fail, V.HP.Timeout) as e:
            row.check(False, str(e))

    def run(self):
        self.t_mutants()
        if not self.args.merged_only:
            self.t_bands(P.SHIPPED, 'plain')
        self.t_bands(MERGED, 'merged')
        for row in self.rows:
            print('%-72s %5d %5d' % (row.name[:72], row.cases,
                                     row.cases - len(row.fails)))
            for f in row.fails[:12]:
                print('    FAIL: %s' % f)
        for k in sorted(self.metrics):
            print('  %-36s %s' % (k, self.metrics[k]))
        fails = sum(len(r.fails) for r in self.rows)
        print('%d failed' % fails)
        return 1 if fails else 0


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--seed', type=int, default=2722)
    ap.add_argument('--vmcs', default=T.DEFAULT_VMCS)
    ap.add_argument('--png', default=os.path.join(T.ROOT, 'build', 'bcmfw', 'test-vclk'))
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--only', default='')
    ap.add_argument('--merged-only', action='store_true')
    a = ap.parse_args(argv)
    if not V.B.have_gcc():
        print('skip: vc4-elf-gcc not found')
        return 0
    return Suite(a).run()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
