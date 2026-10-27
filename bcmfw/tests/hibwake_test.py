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

"""The instant wake after dark (power-contract.md section 6.6) in vc2sim:
the hibernate park's stub (bcmfw/hibstub.c), the jobs images' PARK hook
(HP_PARK_HIB, BCMFW_HIB_PARK) and the PP's header-only sequence and
decision rule (firmware/target/arm/ipod/video/bcm-hibwake.h, compiled
for the host and driven through ctypes).  Simulation only: vc2sim models
neither GPO32's pins nor retention, so the rig below models what PW5
would show (retain, cut, lost, stuck); silicon decides which is true.

  stub      its header first at HW_STUB, inside HW_STUB_MAX, no .bss
  refuse    no stub: HP_E_STATE, nothing written; a bad block: the word
            says HW_REFUSED, nothing parked, the image runs on
  cycle     (display-only, and the merged image built with
            BCMFW_HIB_PARK=1) a park writes Apple's order and nothing
            else (PLLs B, C, D, A to pw 0, the 14 event words to 0),
            touches only the counter while parked, restores (Apple's
            rule) and re-arms the idle tick; the sums match; J_EPOCH, a
            slot (an ALLOC hit at the same address), BACK and the panel
            path all still work after the wake
  spoiled   a word changed in BACK, or in a sampled word of a pool, while
            parked: HW_SPOILED, the image never answers again; a cold
            boot then starts a new epoch (the slot is gone)
  gaveup    no wake word within the block's bound: restored, the word
            says HP_HIB_GAVEUP, the image runs on with its epoch
  merged    the merged image: BUSY in a session, refused with SDRAM up
  pp        the PP sequence (bhw_prepare, bhw_park, bhw_wake, bhw_drop)
            against the display-only image with a model of the two pins:
            Apple's order and 5 ms windows, no port access while bit 15
            is high; retained: awake, epoch kept; cut, lost, stuck and a
            spoiled region each end in the cut
  rule      the decision: the break-even, the gate, the choice over a
            history, the drop, and the ski-rental bound (the capped hold
            never costs more than twice the better choice, per spell)
  mirror    the PP header's constants equal hibwake.h's and hostproto.h's

Usage: bcmfw/tests/hibwake_test.py [--only NAME,...] [--merged-off]
"""
import ctypes as ct
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hostproto_test as H      # noqa: E402
import hello_test as T          # noqa: E402
import jobs_image_test as JI    # noqa: E402

K, J = H.K, JI.J
B = H.B
M32 = 0xFFFFFFFF
TGT = os.path.join(T.ROOT, 'firmware', 'target', 'arm', 'ipod', 'video')
OUT = os.path.join(T.ROOT, 'build', 'bcmfw', 'test-hibwake')
DO_DEFS = JI.JDEFS
MERGED_DEFS = ('BCMFW_VIDEO=1', 'BCMFW_IDLE_SLEEP=1', 'BCMFW_IDLE_GATED=1',
               'BCMFW_IDLE_DOORBELL=1', 'BCMFW_JOBS=1', 'BCMFW_HIB_PARK=1')
PLL = [0x10001C04, 0x10001C18, 0x10001C08, 0x10001C1C, 0x10001C0C,
       0x10001C20, 0x10001C00, 0x10001C14]
GPIO, GPIO_N = 0x10002828, 14
SAVED = PLL + [GPIO + 4 * i for i in range(GPIO_N)]
PW = 0xA5A50000
RUN_PIN, RETAIN_PIN = 0x4000, 0x8000


def hw_constants():
    text = open(os.path.join(T.FW, 'hibwake.h')).read()
    return {n: int(v, 0) for n, v in
            re.findall(r'^#define\s+(HW_\w+|HP_PARK_HIB)\s+(0x[0-9A-Fa-f]+|\d+)\b', text, re.M)}


W = hw_constants()


def hw_sum(words_by_region):
    a = b = 0
    for ws in words_by_region:
        for w in ws:
            a = (a + w) & M32
            b = (b + a) & M32
    return a, b


# ---- the PP header through ctypes ----------------------------------------

class Power(ct.Structure):
    _fields_ = [('i_hold_ua', ct.c_uint32), ('e_cold_uas', ct.c_uint32)]


class Hist(ct.Structure):
    _fields_ = [('ms', ct.c_uint32 * 16), ('n', ct.c_uint8), ('pos', ct.c_uint8)]


class Region(ct.Structure):
    _fields_ = [('base', ct.c_uint32), ('bytes', ct.c_uint32), ('stride', ct.c_uint32)]


class Result(ct.Structure):
    _fields_ = [('end', ct.c_int)] + [(n, ct.c_uint32) for n in (
        'park_us', 'hold_us', 'word_after', 'wake_us', 'word_end')] + \
        [('pins', ct.c_bool), ('cut', ct.c_bool)]


RD = ct.CFUNCTYPE(ct.c_int, ct.c_uint32, ct.POINTER(ct.c_uint32))
WR = ct.CFUNCTYPE(ct.c_int, ct.c_uint32, ct.c_uint32)
PARK = ct.CFUNCTYPE(ct.c_int, ct.c_void_p)
GPO = ct.CFUNCTYPE(ct.c_uint32, ct.c_uint32, ct.c_uint32)
NOW = ct.CFUNCTYPE(ct.c_uint32)
US = ct.CFUNCTYPE(None, ct.c_uint32)
VOID = ct.CFUNCTYPE(None)


class Hal(ct.Structure):
    _fields_ = [('rd', RD), ('wr', WR), ('park', PARK), ('gpo', GPO),
                ('gpo_enable', NOW), ('now_us', NOW), ('udelay', US),
                ('sleep_ms', US), ('yield_', VOID), ('ctx', ct.c_void_p)]


BHW = {'OK': 0, 'REFUSED': -1, 'E_UPLOAD': -2, 'E_PARK': -3, 'E_DEAD': -4,
       'E_LOST': -5, 'E_WAKE': -6, 'E_SPOILED': -7}


def build_lib():
    os.makedirs(OUT, exist_ok=True)
    lib = os.path.join(OUT, 'libhibwake.so')
    subprocess.check_call(['cc', '-shared', '-fPIC', '-O1', '-std=gnu99', '-Wall', '-Wextra',
                           '-Werror', '-I' + TGT, '-o', lib,
                           os.path.join(HERE, 'hibwake_shim.c')])
    lib = ct.CDLL(lib)
    lib.t_breakeven_ms.restype = ct.c_uint32
    lib.t_hold_cost.restype = ct.c_uint64
    return lib


class Suite(JI.Suite):
    def __init__(self, args):
        super().__init__(args)
        self._stub = None
        self._lib = None

    # ---- shared ------------------------------------------------------------

    def stub(self):
        if self._stub is None:
            prog, rep = B.build_hibstub(os.path.join(OUT, 'stub'), listing=False)
            img = rep['image']
            self._stub = (prog, rep, [int.from_bytes(img[k:k + 4], 'little')
                                      for k in range(0, len(img), 4)])
        return self._stub

    def lib(self):
        if self._lib is None:
            self._lib = build_lib()
        return self._lib

    def seed_regs(self, b):
        """Distinct values in every word the park saves (peek_test's
        hib_seed): PLL B on, D with an N and no enable, the event words"""
        for a, v in ((0x10001C04, 0x6C), (0x10001C18, 0x258 | 0x400),
                     (0x10001C0C, 0x36), (0x10001C20, 0x120),
                     (0x10001C00, 0x6C), (0x10001C14, 0x258)):
            b.bus.regs[a] = v
        for i in range(GPIO_N):
            b.bus.regs[GPIO + 4 * i] = 0x1000 + i
        b.bus.regs[0x10001C28] = 0x0001         # the core on the crystal
        return {a: b.bus.regs.get(a, 0) for a in SAVED}

    def regions(self, merged):
        r = (Region * 8)()
        n = self.lib().t_default_regions(1 if merged else 0, r)
        return [(r[k].base, r[k].bytes, r[k].stride) for k in range(n)]

    def put_param(self, vp, regions, wait_ms=60_000, magic=None):
        P = W['HW_PARAM']
        vp.wr(P + W['HW_P_MAGIC'], W['HW_PARAM_MAGIC'] if magic is None else magic)
        vp.wr(P + W['HW_P_WAIT_MS'], wait_ms)
        vp.wr(P + W['HW_P_NREG'], len(regions))
        words = []
        for base, n, stride in regions:
            words += [base, n, stride]
        vp.wr_block(P + W['HW_P_REG'], words)

    def scene(self, row, b, vp, what):
        """Something worth keeping: a cover in a drift slot (committed),
        a frame in BACK; returns (slot, addr, key, the cover, BACK's words)"""
        rng = random.Random(19)
        w, hh = 64, 48
        pic = [rng.getrandbits(16) for _ in range(w * hh)]
        op, st, p = self.jcall(vp, J['J_OP_ALLOC'], [J['J_CLS_DRIFT'], JI.xy(w, hh),
                                                    J['J_FMT_RGB565'], 0xC0FE, 0])
        row.check(st == K['HP_OK'] and len(p) >= 4, '%s: ALLOC %d' % (what, st))
        slot, addr, pitch = p[0], p[1], p[2]
        JI.wr_slot(vp, addr, pitch, pic, w, hh)
        op, st, _ = self.jcall(vp, J['J_OP_COMMIT'], [slot] + JI.f16w(pic))
        row.check(st == K['HP_OK'], '%s: COMMIT %d' % (what, st))
        back = [rng.getrandbits(32) for _ in range(320 * 240 // 2)]
        vp.wr_block(0x105800, back)       # BACK (J_SURF_BACK in UI mode)
        return slot, addr, (w, hh, pitch), pic, back

    def epoch(self, b):
        return b.r32(0xDE000 + J['J_EPOCH']) & 0xFFFF

    def park_req(self, vp):
        return vp.call(K['HP_OP_PARK'], [K['HP_PARK_MAGIC'], W['HP_PARK_HIB']],
                       timeout_us=200_000)

    def run_until_word(self, b, vp, want, limit_us):
        t0 = b.now()
        while b.r32(W['HW_WORD']) not in want:
            if b.now() - t0 > limit_us:
                return None
            b.run(300)
            vp.drain()
        return b.r32(W['HW_WORD'])

    def cycle(self, row, b, vp, what, merged=False, hold_us=30_000, during=None,
              wait_ms=60_000, wake=True):
        """One park through the image's hook, the host doing what the PP
        would (minus the pins): returns a dict of what happened"""
        _, _, stub = self.stub()
        vp.wr_block(W['HW_STUB'], stub)
        self.put_param(vp, self.regions(merged), wait_ms)
        pre = self.seed_regs(b)
        b.w32(W['HW_WORD'], 0)
        n0 = len(b.bus.log)
        op, st, _ = self.park_req(vp)
        row.check(st == K['HP_OK'], '%s: PARK HIB %d' % (what, st))
        word = self.run_until_word(b, vp, (K['HP_HIB_PARKED'], W['HW_REFUSED']), 2_000_000)
        row.check(word == K['HP_HIB_PARKED'], '%s: parked word %s' % (what, word and hex(word)))
        i_park = len(b.bus.log)
        if during:
            during(b)
        b.timer.advance(hold_us)        # the hold: the core frozen (retain)
        steps0 = b.sim.steps
        i_wake = len(b.bus.log)
        if wake:
            b.w32(W['HW_WORD'], K['HP_HIB_WAKE'])
        end = self.run_until_word(b, vp, (K['HP_HIB_AWAKE'], K['HP_HIB_GAVEUP'], W['HW_SPOILED']),
                                  4_000_000 + wait_ms * 1000)
        P = W['HW_PARAM']
        res = {'pre': pre, 'word': end, 'acc': b.bus.log[n0:], 'i_park': i_park - n0,
               'i_wake': i_wake - n0, 'steps': b.sim.steps - steps0,
               'exit': b.r32(P + W['HW_P_EXIT']),
               'sums': [b.r32(P + W[k]) for k in ('HW_P_PRE_A', 'HW_P_PRE_B',
                                                    'HW_P_POST_A', 'HW_P_POST_B')],
               'park_us': b.r32(P + W['HW_P_PARK_US']), 'sum_us': b.r32(P + W['HW_P_SUM_US']),
               'restore_us': b.r32(P + W['HW_P_RESTORE_US'])}
        return res

    def check_order(self, row, b, res, what):
        """Apple's park and PW5's restore, nothing else on the device bus"""
        acc, pre = res['acc'], res['pre']
        dev = [x for x in acc if x.seq not in b.host_seqs and 0x10000000 <= x.addr < 0x10010000]
        wr = [(x.addr, x.value) for x in dev if x.rw == 'W']
        # first the image's flush of the stub the PP wrote (Apple's @25a,
        # as before any code written after the start; quirks.md #40): the
        # request over the word with the enables cleared, then that word
        flush = [(0x10000C00, 0x40000000), (0x10000C00, 0)]
        row.check(wr[:2] == flush, '%s: Apple\'s flush before the stub: %s' % (
            what, ['%08x=%08x' % x for x in wr[:2]]))
        wr = wr[2:] if wr[:2] == flush else wr
        park = [(a, PW) for a in PLL] + [(GPIO + 4 * i, 0) for i in range(GPIO_N)]
        row.check(wr[:len(park)] == park, '%s: Apple\'s park order: %s' % (
            what, ['%08x=%08x' % x for x in wr[:3]]))
        rest = []
        for k in (3, 2, 1, 0):
            dv, c = PLL[2 * k], PLL[2 * k + 1]
            rest += [(dv, PW | pre[dv] & 0xFFFF), (c, PW | pre[c] & 0xFFFF & ~0x400)]
        rest += [(PLL[2 * k + 1], PW | pre[PLL[2 * k + 1]] & 0xFFFF)
                 for k in (3, 2, 1, 0) if pre[PLL[2 * k + 1]] & 0x400]
        rest += [(GPIO + 4 * i, pre[GPIO + 4 * i]) for i in range(GPIO_N - 1, -1, -1)]
        got = wr[len(park):len(park) + len(rest)]
        row.check(got == rest, '%s: PW5\'s restore order: %s' % (
            what, ['%08x=%08x' % x for x in got[:3]]))
        tail = wr[len(park) + len(rest):]
        row.check([a for a, _ in tail[:2]] == [0x10001808 + 4, 0x10001800] and
                  tail[1][1] == 2, '%s: the idle tick re-armed: %s' % (
                      what, ['%08x=%08x' % x for x in tail[:2]]))
        other = [x for x in tail[2:]]
        quiet = [x for x in dev if res['i_park'] <= acc.index(x) < res['i_wake'] and
                 not (x.rw == 'R' and x.addr == 0x10001804)]
        row.check(not quiet, '%s: parked, the core touched only the counter: %s' % (
            what, [str(x) for x in quiet[:2]]))
        regs = b.bus.regs
        for a in PLL:
            row.check((regs.get(a, 0) ^ pre[a]) & 0xFFFF == 0, '%s: %08x is %08x, was %08x' % (
                what, a, regs.get(a, 0), pre[a]))
        for i in range(2, GPIO_N):
            a = GPIO + 4 * i
            row.check(regs.get(a, 0) == pre[a], '%s: %08x back' % (what, a))
        return other

    # ---- rows ----------------------------------------------------------------

    def t_stub(self):
        row = T.Row('stub: header at HW_STUB, inside HW_STUB_MAX, no .bss')
        self.rows.append(row)
        try:
            prog, rep, words = self.stub()
            row.check(words[0] == W['HW_STUB_MAGIC'] and words[2] == W['HW_STUB'] and
                      words[3] == W['HW_STUB'] + 4 * len(words), 'header %s' % words[:5])
            row.check(4 * len(words) <= W['HW_STUB_MAX'], 'stub %d bytes' % (4 * len(words)))
            row.check(W['HW_STUB'] + W['HW_STUB_MAX'] <= 0xDF000 and
                      W['HW_PARAM'] >= 0xDF000 + 60 * 4 and
                      W['HW_PARAM'] + W['HW_PARAM_MAX'] <= 0xE0000 and
                      W['HW_P_SAVED'] + 4 * W['HW_SAVED_N'] <= W['HW_PARAM_MAX'],
                      'stub and block past the guard and below FRONT')
            self.metrics['hibernate stub bytes'] = 4 * len(words)
        except (T.Fail, H.Timeout, B.C.Gap) as e:
            row.check(False, str(e))

    def t_refuse(self):
        row = T.Row('refuse: no stub (STATE, nothing written); a bad block (REFUSED, runs on)')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(3, defines=DO_DEFS)
            n0 = len(b.bus.log)
            op, st, _ = self.park_req(vp)
            row.check(st == K['HP_E_STATE'], 'no stub: %d' % st)
            row.check(not [x for x in b.bus.log[n0:] if x.rw == 'W' and x.addr in SAVED and
                           x.seq not in b.host_seqs], 'no stub: a register written')
            _, _, stub = self.stub()
            vp.wr_block(W['HW_STUB'], stub)
            self.put_param(vp, self.regions(False), magic=0x1234)
            b.w32(W['HW_WORD'], 0)
            n0 = len(b.bus.log)
            op, st, _ = self.park_req(vp)
            row.check(st == K['HP_OK'], 'bad block: PARK %d' % st)
            word = self.run_until_word(b, vp, (W['HW_REFUSED'], K['HP_HIB_PARKED']), 500_000)
            row.check(word == W['HW_REFUSED'], 'bad block: word %s' % (word and hex(word)))
            row.check(not [x for x in b.bus.log[n0:] if x.rw == 'W' and x.addr in SAVED and
                           x.seq not in b.host_seqs], 'bad block: a register written')
            for bad in ([(0x105800, 6, 1)], [(0x105800, 0, 1)], [(0x105800, 64, 0)],
                        [(0x13FFF0, 64, 1)]):
                self.put_param(vp, bad)
                b.w32(W['HW_WORD'], 0)
                op, st, _ = self.park_req(vp)
                word = self.run_until_word(b, vp, (W['HW_REFUSED'], K['HP_HIB_PARKED']), 500_000)
                row.check(st == K['HP_OK'] and word == W['HW_REFUSED'],
                          'region %s: %d %s' % (bad, st, word and hex(word)))
            vp.send(J['J_OP_FILL'], [J['J_SURF_FRONT'], JI.xy(0, 0), JI.xy(32, 2), 0x1234])
            row.check(self.fence(vp, 77), 'the image runs on after a refusal')
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def good_cycle(self, row, what, defines, merged):
        b, h, vp = self.setup(3, defines=defines)
        slot, addr, (w, hh, pitch), pic, back = self.scene(row, b, vp, what)
        ep = self.epoch(b)
        res = self.cycle(row, b, vp, what, merged=merged)
        row.check(res['word'] == K['HP_HIB_AWAKE'] and res['exit'] == W['HW_X_OK'],
                  '%s: end %s exit %d' % (what, res['word'] and hex(res['word']), res['exit']))
        s = res['sums']
        row.check(s[0:2] == s[2:4], '%s: sums %s' % (what, ['%08x' % x for x in s]))
        other = self.check_order(row, b, res, what)
        row.check(not other, '%s: writes after the restore: %s' % (
            what, ['%08x' % x for x, _ in other[:3]]))
        row.check(self.epoch(b) == ep, '%s: J_EPOCH %d, was %d' % (what, self.epoch(b), ep))
        op, st, p = self.jcall(vp, J['J_OP_ALLOC'], [J['J_CLS_DRIFT'], JI.xy(w, hh),
                                                    J['J_FMT_RGB565'], 0xC0FE, 0])
        row.check(st == K['HP_OK'] and p[3] & J['J_ALLOC_HIT'] and p[0] == slot and p[1] == addr,
                  '%s: the slot after the wake: %d %s' % (what, st, p[:4]))
        got = [b.r32(0x105800 + 4 * i) for i in range(0, 320 * 240 // 2, 997)]
        row.check(got == back[::997], '%s: BACK kept' % what)
        vp.send(J['J_OP_BLIT'], [slot, 0, JI.xy(w, hh), J['J_SURF_FRONT'], JI.xy(100, 90), 0])
        vp.send(J['J_OP_PRESENT'], [J['J_SURF_FRONT'], JI.xy(100, 90), JI.xy(w, hh), 0])
        row.check(self.fence(vp, 0x51), '%s: jobs run after the wake' % what)
        self.settle_dma(b)
        got = [b.panel.fb[(90 + y) * 320 + 100 + x] for y in range(hh) for x in range(w)]
        row.check(got == pic, '%s: the kept cover reaches the panel' % what)
        c1, clo = b.bus.regs.get(0x1000180C, 0), b.now()
        row.check(0 < (c1 - clo) & M32 <= 1_000_000 or c1 > clo - 100_000,
                  '%s: compare 1 at %d, counter %d' % (what, c1, clo))
        self.metrics['%s: park us (sim)' % what] = res['park_us']
        self.metrics['%s: wake sums us (sim)' % what] = res['sum_us']
        self.metrics['%s: wake instructions' % what] = res['steps']
        self.metrics['%s: wake ms at 27 MHz (floor)' % what] = round(res['steps'] / 27e3, 1)
        return b, vp

    def t_cycle(self):
        row = T.Row('cycle (display-only): Apple\'s park, quiet, restore, sums, epoch, slots kept')
        self.rows.append(row)
        try:
            self.good_cycle(row, 'display-only', DO_DEFS, False)
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def t_spoiled(self):
        row = T.Row('spoiled: a changed word -> HW_SPOILED, silent; cold boot -> new epoch')
        self.rows.append(row)
        try:
            regs = self.regions(False)
            pools = next(r for r in regs if r[0] == 0x046000)
            for what, addr, spoils in (('BACK', 0x105800 + 4 * 12345, True),
                                       ('a sampled pool word', pools[0] + 4 * 16 * 777, True),
                                       ('an unsampled pool word', pools[0] + 4 * (16 * 777 + 5),
                                        False),
                                       ('the image\'s .data', None, True)):
                b, h, vp = self.setup(3, defines=DO_DEFS)
                slot, *_ = self.scene(row, b, vp, what)
                if addr is None:
                    prog, rep, img = self.image(DO_DEFS)
                    addr = prog.ends['rodata'] & ~3
                ep = self.epoch(b)

                def during(bb, a=addr):
                    bb.bus.w32(a, bb.bus.r32(a) ^ 0x00010000)
                res = self.cycle(row, b, vp, what, during=during)
                if spoils:
                    row.check(res['word'] == W['HW_SPOILED'] and res['exit'] == W['HW_X_SPOILED'],
                              '%s: end %s exit %d' % (what, res['word'] and hex(res['word']),
                                                      res['exit']))
                    tag = vp.send(J['J_OP_FENCE'], [9])
                    try:
                        vp.wait(tag, timeout_us=300_000)
                        row.check(False, '%s: the image answered after HW_SPOILED' % what)
                    except H.Timeout:
                        pass
                    # the PP's cold boot: power cut, upload, a new epoch
                    b.power_cut(random.Random(5))
                    h.display_on = False
                    h.lcd_awake()
                    h.settle()
                    h.tick_task = False
                    h.idle_ok = False
                    vp = H.Vp(b)
                    vp.attach()
                    op, st, p = self.jcall(vp, J['J_OP_ALLOC'], [J['J_CLS_DRIFT'], JI.xy(64, 48),
                                                                J['J_FMT_RGB565'], 0xC0FE, 0])
                    row.check(st == K['HP_OK'] and not p[3] & J['J_ALLOC_HIT'],
                              '%s: after the cold boot the slot is gone (new epoch)' % what)
                else:
                    row.check(res['word'] == K['HP_HIB_AWAKE'],
                              '%s (stride 16): not seen, by design: %s' % (
                                  what, res['word'] and hex(res['word'])))
                    row.check(self.epoch(b) == ep, '%s: epoch kept' % what)
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def t_gaveup(self):
        row = T.Row('gaveup: no wake word within the bound -> restored, GAVEUP, runs on')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(3, defines=DO_DEFS)
            slot, *_ = self.scene(row, b, vp, 'gaveup')
            ep = self.epoch(b)
            res = self.cycle(row, b, vp, 'gaveup', hold_us=0, wait_ms=5, wake=False)
            row.check(res['word'] == K['HP_HIB_GAVEUP'] and res['exit'] == W['HW_X_GAVEUP'],
                      'end %s exit %d' % (res['word'] and hex(res['word']), res['exit']))
            regs = b.bus.regs
            row.check(all((regs.get(a, 0) ^ res['pre'][a]) & 0xFFFF == 0 for a in PLL),
                      'PLLs back')
            row.check(self.epoch(b) == ep and self.fence(vp, 0x61), 'runs on with its epoch')
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def t_merged(self):
        row = T.Row('merged (BCMFW_HIB_PARK=1): the cycle; BUSY in a session; SDRAM up refused')
        self.rows.append(row)
        if self.args.merged_off:
            row.check(True, 'skipped')
            return
        try:
            prog, rep, img = self.image(MERGED_DEFS)
            arena = B.config_value('BCMFW_VIDEO_ARENA', MERGED_DEFS)
            row.check(rep['end'] <= arena, 'merged image+bss to 0x%x, past 0x%x' % (
                rep['end'], arena))
            self.metrics['merged image bytes (with the hook)'] = len(img)
            self.metrics['merged image spare below the arena'] = arena - rep['end']
            b, vp = self.good_cycle(row, 'merged', MERGED_DEFS, True)
            op, st, p = self.open_null(vp)
            row.check(st == K['HP_OK'], 'OPEN %d' % st)
            op, st2, _ = self.park_req(vp)
            row.check(st2 == K['HP_E_BUSY'], 'in a session: %d' % st2)
            vp.call(K['HP_OP_CLOSE'], [p[0] if p else 0, 0])
            # SDRAM up (video_test's bench models it): an H.264 OPEN
            # brings it up and CLOSE leaves it so (the AAC tables live
            # there): the hook refuses, nothing written
            import video_test as V
            entry = B.config_value('BCMFW_ENTRY', MERGED_DEFS)
            vb = V.VideoBench(img, 3, entry, None)
            vh = T.Host(vb, random.Random(7))
            vh.fb = T.random_frame(self.rng)
            vh.lcd_awake()
            vh.settle()
            vh.tick_task = False
            vh.idle_ok = False
            pl = V.Player(vb)
            pl.attach()
            vb.bus.regs[0x10001C28] = 0x0001
            st, p = pl.open(320, 240)
            row.check(st == K['HP_OK'], 'H.264 OPEN for SDRAM: %d' % st)
            pl.call(K['HP_OP_CLOSE'], [pl.session, 0], timeout_us=200_000)
            _, _, stub = self.stub()
            pl.wr_block(W['HW_STUB'], stub)
            n0 = len(vb.bus.log)
            op, st3, _ = pl.call(K['HP_OP_PARK'], [K['HP_PARK_MAGIC'], W['HP_PARK_HIB']],
                                 timeout_us=200_000)
            row.check(st3 == K['HP_E_STATE'], 'SDRAM up after a video: %d' % st3)
            row.check(not [x for x in vb.bus.log[n0:] if x.rw == 'W' and x.addr in SAVED and
                           x.seq not in vb.host_seqs], 'SDRAM up: a register written')
        except (T.Fail, H.Timeout, B.C.Gap) as e:
            row.check(False, str(e))

    # ---- the PP header against the image -----------------------------------

    def pp_rig(self, model):
        """The display-only image, a scene, and the HAL over a pin model:
          retain  bit 15 then bit 14 low: the core stops, memory kept
          cut     bit 14 low is a cut whatever bit 15 says
          lost    retained, SRAM back scrambled (the stub with it)
          stuck   retained, the core never runs again
          spoil   retained, one BACK word changed in the dark"""
        b, h, vp = self.setup(3, defines=DO_DEFS)
        row = T.Row('scratch')
        scene = self.scene(row, b, vp, 'pp')
        self.seed_regs(b)
        rig = type('Rig', (), {})()
        rig.b, rig.vp, rig.h, rig.scene = b, vp, h, scene
        rig.gpo_v, rig.en = 0x4003, 0xC003
        rig.frozen = rig.dead = False
        rig.pins, rig.port = [], []
        rng = random.Random(2722)

        def step(us):
            end = b.now() + us
            while b.now() < end:
                if rig.frozen or rig.dead or not b.running:
                    b.timer.advance(min(1000, end - b.now()))
                else:
                    try:
                        b.run(300)
                    except T.Fail:
                        # scrambled SRAM: what the core runs is noise, and
                        # a strict trap in it (alignment, stale code) only
                        # says so; the core is as good as stopped
                        if model != 'lost':
                            raise
                        rig.frozen = True
                        continue
                    vp.drain()

        def rd(a, v):
            rig.port.append((b.now(), 'R', rig.gpo_v))
            v[0] = M32 if rig.dead else b.r32(a)
            return 0

        def wr(a, v):
            rig.port.append((b.now(), 'W', rig.gpo_v))
            if not rig.dead:
                b.w32(a, v)
            return 0

        def park(ctx):
            rig.port.append((b.now(), 'P', rig.gpo_v))
            op, st, _ = self.park_req(vp)
            return 0 if st == K['HP_OK'] else -1

        def gpo(clear, set_):
            old = rig.gpo_v
            new = (old & ~(clear & 0xC000)) | (set_ & 0xC000)
            rig.gpo_v = new
            if clear or set_:
                rig.pins.append((b.now(), clear, set_, new))
            if old & RUN_PIN and not new & RUN_PIN:
                if old & RETAIN_PIN and model != 'cut':
                    rig.frozen = True
                    if model == 'lost':
                        b.bus.b[0:0x140000] = bytes(rng.getrandbits(8)
                                                    for _ in range(256)) * (0x140000 // 256)
                        b.sim.code.clear()
                        b.sim.code_view = None  # the caches lost too
                    if model == 'spoil':
                        a = 0x105800 + 4 * 4321
                        b.bus.w32(a, b.bus.r32(a) ^ 1)
                else:
                    b.power_cut(rng)
                    rig.dead = True
            if not old & RUN_PIN and new & RUN_PIN and rig.frozen and model != 'stuck':
                rig.frozen = False
            return new

        rig.hal = Hal(rd=RD(rd), wr=WR(wr), park=PARK(park), gpo=GPO(gpo),
                      gpo_enable=NOW(lambda: rig.en), now_us=NOW(lambda: b.now() & M32),
                      udelay=US(step), sleep_ms=US(lambda ms: step(ms * 1000)),
                      yield_=VOID(lambda: step(300)), ctx=None)
        return rig

    def pp_regions(self, merged=False):
        rs = self.regions(merged)
        arr = (Region * 8)(*[Region(*r) for r in rs])
        return arr, len(rs)

    def t_pp(self):
        lib = self.lib()
        _, _, stub = self.stub()
        sw = (ct.c_uint32 * len(stub))(*stub)
        for model, want in (('retain', 'OK'), ('cut', 'E_DEAD'), ('lost', 'E_LOST'),
                            ('stuck', 'E_WAKE'), ('spoil', 'E_SPOILED')):
            row = T.Row('pp (bcm-hibwake.h), chip model "%s": %s' % (model, want))
            self.rows.append(row)
            try:
                rig = self.pp_rig(model)
                regs, n = self.pp_regions()
                ep = self.epoch(rig.b)
                rc = lib.t_prepare(ct.byref(rig.hal), sw, len(stub), regs, n, 60_000)
                row.check(rc == 0, 'prepare %d' % rc)
                got = [rig.b.r32(W['HW_STUB'] + 4 * k) for k in range(len(stub))]
                row.check(got == stub, 'the stub reads back')
                ports0 = len(rig.port)
                rc = lib.t_prepare(ct.byref(rig.hal), sw, len(stub), regs, n, 60_000)
                row.check(rc == 0 and not [p for p in rig.port[ports0:] if p[1] == 'W'
                                           and False], 'prepare again %d' % rc)
                r = Result()
                rc = lib.t_park(ct.byref(rig.hal), ct.byref(r))
                row.check(rc == 0 and r.pins, 'park %d' % rc)
                hold_t0 = rig.b.now()
                while rig.b.now() - hold_t0 < 50_000:
                    rig.b.timer.advance(1000) if rig.frozen or rig.dead else rig.b.run(300)
                rc = lib.t_wake(ct.byref(rig.hal), ct.byref(r))
                row.check(rc == BHW[want] and r.end == rc, 'wake %d, want %s' % (rc, want))
                moves = [(c, s) for _, c, s, _ in rig.pins]
                order = [(0, RETAIN_PIN), (RUN_PIN, 0), (0, RUN_PIN), (RETAIN_PIN, 0)]
                if want != 'OK':
                    order += [(RETAIN_PIN, 0), (RUN_PIN, 0)]
                row.check(moves == order, 'pin order %s' % moves)
                t = [x[0] for x in rig.pins]
                row.check(5000 <= t[1] - t[0] <= 5400 and 5000 <= t[3] - t[2] <= 5400,
                          'both pins high only for Apple\'s 5 ms')
                row.check(not [p for p in rig.port if p[2] & RETAIN_PIN],
                          'no port access while bit 15 was high')
                row.check(all(x[3] & 0x3 == 0x3 for x in rig.pins), 'other GPO32 bits kept')
                if want == 'OK':
                    row.check(r.word_after == K['HP_HIB_PARKED'] and not r.cut, 'kept, no cut')
                    row.check(self.epoch(rig.b) == ep, 'epoch kept')
                    row.check(self.fence(rig.vp, 0x71), 'jobs after the wake')
                    self.metrics['pp: wake us (pins to AWAKE, sim)'] = r.wake_us
                    self.metrics['pp: park us (request to PARKED, sim)'] = r.park_us
                    # then a park held past the break-even: the drop to C0
                    r2 = Result()
                    rc = lib.t_park(ct.byref(rig.hal), ct.byref(r2))
                    row.check(rc == 0, 'second park %d' % rc)
                    lib.t_drop(ct.byref(rig.hal), ct.byref(r2))
                    row.check(rig.gpo_v & 0xC000 == 0 and r2.cut, 'drop: both pins low')
                else:
                    row.check(r.cut and rig.gpo_v & 0xC000 == 0, 'the standard cut ran')
            except (T.Fail, H.Timeout) as e:
                row.check(False, str(e))
        row = T.Row('pp: refusals before the pins (pins untouched)')
        self.rows.append(row)
        try:
            rig = self.pp_rig('retain')
            regs, n = self.pp_regions()
            r = Result()
            rig.en = 0x4003
            row.check(lib.t_park(ct.byref(rig.hal), ct.byref(r)) == BHW['REFUSED'],
                      'GPO32_ENABLE without bit 15')
            rig.en, rig.gpo_v = 0xC003, 0x0003
            row.check(lib.t_park(ct.byref(rig.hal), ct.byref(r)) == BHW['REFUSED'],
                      'bit 14 already low')
            rig.gpo_v = 0x4003
            row.check(lib.t_prepare(ct.byref(rig.hal), sw, 0, regs, n, 1) == BHW['REFUSED'],
                      'an empty stub')
            row.check(lib.t_prepare(ct.byref(rig.hal), sw, len(stub), regs, 9, 1) ==
                      BHW['REFUSED'], 'nine regions')
            # no stub uploaded: the image refuses, the pins stay
            row.check(lib.t_park(ct.byref(rig.hal), ct.byref(r)) == BHW['E_PARK'] and
                      not rig.pins, 'no stub: E_PARK, pins untouched')
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def t_rule(self):
        row = T.Row('rule: break-even, gate, choice over the history, drop, ski-rental bound')
        self.rows.append(row)
        lib = self.lib()
        p = Power(500, 10_000)      # 0.5 mA held, 10 mA.s per cold wake
        row.check(lib.t_breakeven_ms(ct.byref(p)) == 20_000, 'T* %d' % lib.t_breakeven_ms(
            ct.byref(p)))
        row.check(lib.t_breakeven_ms(ct.byref(Power(500, 0))) == 0, 'no cold cost: never hold')
        row.check(lib.t_breakeven_ms(ct.byref(Power(0, 1))) == M32, 'free hold: forever')
        row.check(lib.t_gate(1, 1, 1) and not lib.t_gate(0, 1, 1) and not lib.t_gate(1, 0, 1)
                  and not lib.t_gate(1, 1, 0), 'gate needs all three')
        h = Hist()
        row.check(lib.t_choose(ct.byref(p), ct.byref(h)) == 1, 'no history: hold (capped)')
        for ms in (3_000, 5_000, 4_000, 8_000):
            lib.t_hist_add(ct.byref(h), ms)
        row.check(lib.t_choose(ct.byref(p), ct.byref(h)) == 1, 'short spells: hold')
        h2 = Hist()
        for ms in (3_600_000, 1_800_000, 7_200_000, 900_000):
            lib.t_hist_add(ct.byref(h2), ms)
        row.check(lib.t_choose(ct.byref(p), ct.byref(h2)) == 0, 'hour-long spells: cut')
        h3 = Hist()
        for ms in (1_000, 2_000, 600_000, 900_000, 1_500):
            lib.t_hist_add(ct.byref(h3), ms)
        cap = 20_000
        exp = sum(lib.t_hold_cost(ct.byref(p), cap, t) for t in (1_000, 2_000, 600_000,
                                                                  900_000, 1_500)) / 5
        row.check(lib.t_choose(ct.byref(p), ct.byref(h3)) == (1 if exp < 10_000_000 else 0),
                  'mixed: the expectation decides (%.0f vs %d)' % (exp, 10_000_000))
        for _ in range(20):
            lib.t_hist_add(ct.byref(h3), 5)
        row.check(h3.n == 16, 'the history keeps 16')
        row.check(not lib.t_drop_now(ct.byref(p), 19_999) and lib.t_drop_now(ct.byref(p), 20_000),
                  'drop at the break-even')
        rng = random.Random(3)
        worst = 0.0
        for _ in range(2000):
            t = int(rng.expovariate(1 / rng.choice((2_000, 30_000, 600_000))))
            got = lib.t_hold_cost(ct.byref(p), cap, t) / 1000.0
            best = min(t * p.i_hold_ua / 1000.0, p.e_cold_uas)
            worst = max(worst, got / best if best else 1.0)
        row.check(worst <= 2.0 + 1e-9, 'capped hold vs hindsight: worst ratio %.3f' % worst)
        self.metrics['rule: worst ratio to hindsight (2000 spells)'] = round(worst, 3)

    def t_mirror(self):
        row = T.Row('mirror: bcm-hibwake.h constants equal hibwake.h and hostproto.h')
        self.rows.append(row)
        text = open(os.path.join(TGT, 'bcm-hibwake.h')).read()
        pp = {n: int(v.rstrip('u'), 0) for n, v in
              re.findall(r'^#define\s+(BHW_\w+)\s+(0x[0-9A-Fa-f]+u?|\d+u?)\b', text, re.M)}
        pairs = [('BHW_STUB', W['HW_STUB']), ('BHW_STUB_MAX', W['HW_STUB_MAX']),
                 ('BHW_STUB_MAGIC', W['HW_STUB_MAGIC']), ('BHW_PARAM', W['HW_PARAM']),
                 ('BHW_PARAM_MAGIC', W['HW_PARAM_MAGIC']), ('BHW_REGIONS', W['HW_REGIONS']),
                 ('BHW_P_WAIT_MS', W['HW_P_WAIT_MS']), ('BHW_P_NREG', W['HW_P_NREG']),
                 ('BHW_P_REG', W['HW_P_REG']), ('BHW_P_EXIT', W['HW_P_EXIT']),
                 ('BHW_P_PRE_A', W['HW_P_PRE_A']), ('BHW_P_SUM_US', W['HW_P_SUM_US']),
                 ('BHW_WORD', W['HW_WORD']), ('BHW_W_PARKED', K['HP_HIB_PARKED']),
                 ('BHW_W_WAKE', K['HP_HIB_WAKE']), ('BHW_W_AWAKE', K['HP_HIB_AWAKE']),
                 ('BHW_W_GAVEUP', K['HP_HIB_GAVEUP']), ('BHW_W_SPOILED', W['HW_SPOILED']),
                 ('BHW_W_REFUSED', W['HW_REFUSED']), ('BHW_PARK_MAGIC', K['HP_PARK_MAGIC']),
                 ('BHW_PARK_HIB', W['HP_PARK_HIB']), ('BHW_WAIT_MS_MAX', W['HW_WAIT_MS_MAX']),
                 ('HW_WORD', K['HP_HIB_WORD'])]
        for name, want in pairs:
            got = pp.get(name, W.get(name))
            row.check(got == want, '%s = %s, want %s' % (name, got, want))
        # the default regions stay inside SRAM, word-aligned, off the
        # mailbox, the stack, the rings, the stub and its block
        for merged in (False, True):
            for base, n, stride in self.regions(merged):
                row.check(base % 4 == 0 and n % 4 == 0 and n > 0 and stride >= 1 and
                          base >= 0x200 and base + n <= 0x140000, 'region %x+%x' % (base, n))
                for lo, hi in ((0xCC000, 0xD4000), (W['HW_STUB'], 0xE0000)):
                    row.check(base + n <= lo or base >= hi,
                              'region %x+%x overlaps %x-%x' % (base, n, lo, hi))

    def run(self):
        import time
        t0 = time.time()
        tests = (self.t_stub, self.t_refuse, self.t_cycle, self.t_spoiled, self.t_gaveup,
                 self.t_merged, self.t_pp, self.t_rule, self.t_mirror)
        if self.args.only:
            tests = tuple(getattr(self, 't_' + n) for n in self.args.only.split(','))
        for t in tests:
            n0 = len(self.rows)
            t()
            for row in self.rows[n0:]:
                print('%-72s %5d %5d' % (row.name[:72], row.cases, row.cases - len(row.fails)))
                for f in row.fails[:20]:
                    print('    FAIL: %s' % f)
        cases = sum(r.cases for r in self.rows)
        fails = sum(len(r.fails) for r in self.rows)
        for k in sorted(self.metrics):
            print('  %-44s %s' % (k, self.metrics[k]))
        print('%d rows, %d cases, %d failed (%.0f s)' % (len(self.rows), cases, fails,
                                                         time.time() - t0))
        return 1 if fails else 0


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--seed', type=int, default=2722)
    ap.add_argument('--vmcs', default=T.DEFAULT_VMCS)
    ap.add_argument('--png', default=os.path.join(OUT, 'png'))
    ap.add_argument('--only', default='')
    ap.add_argument('--merged-off', action='store_true',
                    help='skip the merged image rows')
    ap.add_argument('--audit', action='store_true', help=argparse.SUPPRESS)
    a = ap.parse_args(argv)
    if not B.have_gcc():
        print('skip: vc4-elf-gcc not found')
        return 0
    return Suite(a).run()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
