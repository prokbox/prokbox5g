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

"""The brief probes in vc2sim, on the shipped video image
(brief-map-peripherals.md section 11, PB-1 to PB-4; brief-map-power.md
section 9, PW1's allowlist, PW2, PW3 to PW5 and PW8):

  PEEK     the allowlist honoured (every address PB-1, PB-3 and PW1 name
           read; FIFOs, the doorbell acknowledge, DMA status and the pull
           clocks refused, with nothing read); the values are the bus's;
           the access list equals the request, two reads each, no write
  TV       PB-7 to PB-9: refused unarmed, with a wrong or used code, or
           malformed (nothing written); TIMING and OUTPUT (4 formats,
           NTSC/PAL, 640/704, bars/lines) write only their allowlist, in
           section 2.3's on order, then the off order and every register
           put back, after a normal end, a stopped chain (injected) and
           both start-wait timeouts; channel 6 reads only our line buffer
           and the FIFO gets the pattern; the progress word steps in order
  census   PINS' four-word form: 52 pins, edges per pin, no write
  PULL     only allowlisted inputs pulled, Apple's register order (mode,
           clock, both 0), each pin left pulled down, no fsel write
  CACHE    Apple's three cache values only, checksums against the model,
           the word put back; the coherence handshake (vc2sim has no
           cache: always "fresh")
  DMACPY   channel 7 sub-channel 1 copies, 0 wrong words
  PW       PW3 and PW4 (brief-map-power.md section 9): refused unarmed,
           with a wrong or used code, malformed, in a session, off the
           crystal or at a level outside 5-15 (nothing written); each
           level and each ladder point writes only its allowlist with
           Apple's values, in @5134's order (checked after every write:
           the level never below the table's for the clock, the refresh
           never too slow), then everything back and read back, after a
           normal end, an injected read-back error, a wrong self-check
           and a counter stopped in a wait; the self-check's sums
           against a Python model of both patterns; the 150 MHz point's
           frame; and two mutants (a level raised after the clock, the
           restore skipped) that the same checks must catch
  HIB      PW5 (sections 3 and 9): refused unarmed, with a wrong or used
           code, malformed, in a session or off the crystal (nothing
           written); a park writes Apple's order (PLLs B, C, D, A; the 14
           event words; SDRAM idled last when up), touches nothing but
           the counter while parked, then PLLs (N, 1 ms, the enables),
           SDRAM and the event words back and read back; the patterns
           and the bars checked, a spoiled word in each reported; no
           wake word (the image's own bound), a counter stopped while
           parked, and SDRAM's bit 15 stuck each end restored

Usage: bcmfw/tests/peek_test.py [--only NAME]
"""
import os
import random
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hello_test as T      # noqa: E402
import video_test as V      # noqa: E402

K = V.K
SHIPPED = ('BCMFW_VIDEO=1', 'BCMFW_IDLE_SLEEP=1', 'BCMFW_IDLE_GATED=1',
           'BCMFW_IDLE_DOORBELL=1')
M32 = 0xFFFFFFFF

# What the specs ask PEEK to read
PB1A = [0x10002C18, 0x10005404, 0x10006400, 0x10001C3C, 0x10004800,
        0x10002820, 0x10002824]
PB1B = ([0x10002C00] + list(range(0x10002C08, 0x10002C18, 4)) +
        list(range(0x10005484, 0x100054E0, 4)) +
        list(range(0x10001C28, 0x10001C5C, 4)) +
        [0x10004810, 0x10004814] + list(range(0x10002800, 0x10002810, 4)) +
        [0x10002860])
PW1 = ([0x10001400] + list(range(0x10001C00, 0x10001C5C, 4)) +
       [0x10000C00, 0x10000400, 0x10000408] +
       [0x10003410, 0x10003414, 0x10003420, 0x10003424, 0x10003428,
        0x10003430, 0x10001804])
# Never readable: the FIFOs, the doorbell acknowledge, DMA status words,
# the pull clocks, the SDRAM controller, unaligned, one past a range
NEVER = [0x10002C04, 0x1000400C, 0x10004804, 0x10003800, 0x10000804,
         0x10000B84, 0x10002864, 0x10002868, 0x10002C1C, 0x10001C5C,
         0x10005405, 0x100054E0, 0x10006404, 0x10002810]


# The TV block (brief-map-peripherals.md section 2.3)
TV_CTL, TV_FIFO, TV_MODE = 0x10002C00, 0x10002C04, 0x10002C18
TV_DAC, TV_MV, TV_PCLK = 0x10005404, 0x10005484, 0x10001C30
TV_FSEL1, TV_GSET, TV_GCLR = 0x10002804, 0x10002810, 0x10002818
TV_DMA6, PIN19 = 0x10000B00, 1 << 19
TV_TIMING = [[0x00F00280, 0x01E002C0, 0x09A2183F, 0x01709406],
             [0x01200280, 0x024002C0, 0x0A02383F, 0x0180B005]]
TV_ALLOW_TIMING = {TV_PCLK, TV_CTL} | set(range(0x10002C08, 0x10002C1C, 4))
TV_ALLOW_OUTPUT = TV_ALLOW_TIMING | {TV_DAC, TV_FSEL1, TV_GSET, TV_GCLR, TV_DMA6,
                                     TV_DMA6 + 4, TV_DMA6 + 0x38} | set(
                                         range(TV_MV, TV_MV + 92, 4))
TV_SAVED = sorted(TV_ALLOW_OUTPUT - {TV_GSET, TV_GCLR, TV_DMA6 + 4, TV_DMA6 + 0x38})
BARS = [(235, 128, 128), (210, 16, 146), (170, 166, 16), (145, 54, 34),
        (106, 202, 222), (81, 90, 240), (41, 240, 110), (16, 128, 128)]
BARS565 = [0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000]


# PW3 / PW4 (brief-map-power.md sections 4, 5 and 9)
PW_REG, PW_PLLM, PW_PLLN = 0x10001400, 0x10001C00, 0x10001C14
PW_CORE, PW_REF = 0x10001C28, 0x10003424
PW_ALLOW_LEVEL = {PW_REG}
PW_ALLOW_POINT = {PW_REG, PW_PLLM, PW_PLLN, PW_CORE, PW_REF}
PW_PW = 0xA5A50000
# Apple's table @141ba (section 4): (up to Hz, level), first row at or above
APPLE_LEVELS = [(160919540, 15), (154130133, 15), (147075854, 14),
                (140183656, 13), (130841121, 12), (123607364, 11),
                (116539140, 10), (102016607, 9), (87268380, 8),
                (74840572, 7), (59665292, 6), (46875000, 5)]


def apple_level(hz):
    """@141ba's lookup: the first row (from the bottom) whose frequency is
    at least hz; 5 below the table"""
    for f, lv in reversed(APPLE_LEVELS):
        if f >= hz:
            return lv
    return 15


def apple_pll(mhz):
    """Apple's rule @4cb8: M 108 (250 kHz), the VCO an integer multiple of
    the clock at 75 MHz or more -> (N, divider)"""
    div = 1
    while mhz * div < 75:
        div += 1
    return mhz * div * 4, div


PW_LADDER = [46, 74, 102, 150]


def pw_space(a):
    """What a PW run could reach: the regulator, the clock block, the SDRAM
    controller"""
    return (a == PW_REG or 0x10001C00 <= a <= 0x10001C5C or
            0x10003400 <= a < 0x10003440)


def pw_sums(p):
    """hostproto.c pw_check: pattern p's word sum and low-byte sum"""
    x = K['HP_PW_SEED_B'] if p else K['HP_PW_SEED_A']
    s = lb = 0
    for _ in range(K['HP_PW_WIN_BYTES'] // 4):
        x = lcg(x)
        s = (((s << 1) | (s >> 31)) + x) & M32
        lb += (x & 0xFF) + (x >> 16 & 0xFF)
    return s, lb & M32


def pw_invariants(writes, f0_mhz, level0, ref0):
    """Walks the (address, value) writes from the start state; after each,
    the level must be at least Apple's for the clock running and the
    refresh interval no longer than 7.8125 us (f / 128,000).  Returns the
    first breach, or None."""
    level, core, n, ref = level0, 0x0001, 0, ref0
    for i, (a, v) in enumerate(writes):
        if a == PW_REG:
            level = v & 0xF
        elif a == PW_CORE:
            core = v & 0xFFFF
        elif a == PW_PLLN:
            n = v & 0x7FF
        elif a == PW_REF:
            ref = v >> 16
        if core >> 8 & 7:                   # on PLL A: its VCO / divider
            if not n & 0x400:
                return '%d: the core on PLL A with PLL A off' % i
            hz = (n & 0x3FF) * 250000 // max(1, core & 0xFF)
        else:
            hz = f0_mhz * 1000000
        if level < apple_level(hz):
            return '%d: %08x=%08x leaves level %d at %d Hz (table %d)' % (
                i, a, v, level, hz, apple_level(hz))
        if ref is not None and ref * 128000 > hz:
            return '%d: %08x=%08x leaves refresh %d cycles at %d Hz' % (
                i, a, v, ref, hz)
    return None


# PW5 (brief-map-power.md sections 3 and 9): the PLL words in Apple's park
# order (B, C, D, A; divider then control), the 14 GPIO event words
HIB_PLL = [0x10001C04, 0x10001C18, 0x10001C08, 0x10001C1C, 0x10001C0C,
           0x10001C20, 0x10001C00, 0x10001C14]
HIB_GPIO, HIB_GPIO_N = 0x10002828, 14
HIB_SAVED = HIB_PLL + [HIB_GPIO + 4 * i for i in range(HIB_GPIO_N)]


def hib_space(a):
    """What a park could reach: the PLLs, the event words, the cache word,
    the SDRAM controller"""
    return (0x10001C00 <= a <= 0x10001C20 or HIB_GPIO <= a < HIB_GPIO + 4 * HIB_GPIO_N or
            a == 0x10000C00 or 0x10003400 <= a < 0x10003440)


def hib_panel(mirror):
    """hostproto.c hib_bars, as panel pixels"""
    return [BARS565[7 - x // 40 if mirror else x // 40] for _ in range(240)
            for x in range(320)]


def hib_sum(mirror):
    """hostproto.c hib_sum over hib_bars' words"""
    s = 0
    for _ in range(240):
        for x in range(0, 320, 2):
            c = BARS565[7 - x // 40 if mirror else x // 40]
            s = (((s << 1) | (s >> 31)) + (c | c << 16)) & M32
    return s


def tv_space(a):
    """The blocks a TV run could reach: DMA channel 6, the clocks, GPIO,
    the timing generator, the DAC"""
    return (0x10000B00 <= a < 0x10000B80 or 0x10001C00 <= a <= 0x10001C5C or
            0x10002800 <= a < 0x10002870 or 0x10002C00 <= a < 0x10002C20 or
            0x10005400 <= a < 0x10005500)


def tv_pattern(fmt, w, pat):
    """The line hostproto.c tv_fill writes, as words"""
    out = []
    step = 1 if fmt == 3 else 2
    for x in range(0, w, step):
        if pat == 1:
            b0, b1 = (0 if x % 32 == 0 else 7), 7
        else:
            b0, b1 = x * 8 // w, (x + 1) * 8 // w
        if fmt == 3:
            b1 = b0
        y0, y1 = BARS[b0][0], BARS[b1][0]
        u, v = BARS[b0][1], BARS[b0][2]
        if fmt == 0:
            out.append(u | y0 << 8 | v << 16 | y1 << 24)
        elif fmt == 1:
            out.append(y0 | u << 8 | y1 << 16 | v << 24)
        elif fmt == 2:
            out.append(BARS565[b0] | BARS565[b1] << 16)
        else:
            out.append(y0 << 24)
    return out


def allowlist():
    """peek_allow from hostproto.c (the image's one source)"""
    text = open(os.path.join(T.FW, 'hostproto.c')).read()
    body = text[text.index('peek_allow[][2]'):]
    body = body[:body.index('};')]
    return [(int(a, 16), int(b, 16)) for a, b in
            re.findall(r'\{\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+)\s*\}', body)]


class Toggle:
    """A read rule: the bits in mask flip on every read (base otherwise)"""
    def __init__(self, base, mask):
        self.v, self.mask = base, mask

    def read(self, last, since_write=0):
        self.v ^= self.mask
        return self.v


def lcg(x):
    return (x * 1664525 + 1013904223) & M32


def fill(seed, words):
    out, x = [], seed
    for _ in range(words):
        x = lcg(x)
        out.append(x)
    return out


class Suite(V.Suite):
    def bench(self, pid, defines=SHIPPED, reset_pc=None):
        return super().bench(pid, defines, reset_pc)

    def window(self, b, fn):
        """fn() with the bus log from its start; returns (result, accesses)"""
        n0 = len(b.bus.log)
        r = fn()
        return r, b.bus.log[n0:]

    def peek(self, vp, addrs, gap=0):
        return vp.call(K['HP_OP_PEEK'], [K['HP_PEEK_MAGIC'], gap] + list(addrs),
                       timeout_us=500_000)

    def t_peek(self):
        row = T.Row('PEEK: allowlist, refusals read nothing, bus values, access list')
        self.rows.append(row)
        try:
            allow = allowlist()
            row.check(len(allow) > 10, 'allowlist parsed: %d ranges' % len(allow))

            def ok(a):
                return a & 3 == 0 and any(lo <= a <= hi for lo, hi in allow)
            for a in PB1A + PB1B + PW1:
                row.check(ok(a), 'spec address %08x on the allowlist' % a)
            for a in NEVER:
                row.check(not ok(a), 'never-read %08x off the allowlist' % a)
            row.check(len(PB1B) == 1 + 4 + 23 + 13 + 2 + 4 + 1, 'PB-1b count %d' % len(PB1B))

            b, h, vp, prog = self.setup(1)
            # the counter aside: the image's own loop reads it
            hw = set(a for a in PB1A + PB1B + PW1 + NEVER) - {0x10001804}
            # magic, gap, count
            _, st, _ = vp.call(K['HP_OP_PEEK'], [0, 0, 0x10005404])
            row.check(st == K['HP_E_INVAL'], 'no magic: %d' % st)
            _, st, _ = self.peek(vp, [0x10005404], gap=1001)
            row.check(st == K['HP_E_INVAL'], 'gap over 1000: %d' % st)
            _, st, _ = self.peek(vp, [0x10005404] * 16)
            row.check(st == K['HP_E_INVAL'], '16 addresses: %d' % st)
            _, st, _ = vp.call(K['HP_OP_PEEK'], [K['HP_PEEK_MAGIC'], 0])
            row.check(st == K['HP_E_INVAL'], 'no address: %d' % st)
            # each never-read address, alone and behind a good one: refused,
            # the refused address in the reply, nothing read
            for a in NEVER:
                (op, st, p), acc = self.window(b, lambda: self.peek(vp, [0x10005404, a]))
                row.check(st == K['HP_E_INVAL'] and p == [a],
                          'refused %08x: %d %s' % (a, st, p))
                touched = [x for x in acc if x.addr in (a, 0x10005404) and
                           x.seq not in b.host_seqs]
                row.check(not touched, 'refused %08x: read nothing (%s)' % (
                    a, ', '.join(str(x) for x in touched[:3])))
            # values from the bus: fixed words, a toggling status bit
            b.bus.rule('0x10005404=0x00000001')
            b.bus.rule('0x10006400=0x01000000')
            b.bus.rule('0x10001C3C=0x00008000')
            b.bus.rule(0x10002C18, 0x10002C18, Toggle(0, 0x80000000))
            (op, st, p), acc = self.window(b, lambda: self.peek(vp, PB1A))
            row.check(st == K['HP_OK'] and len(p) == 2 * len(PB1A),
                      'PB-1a: %d, %d words' % (st, len(p or [])))
            if st == K['HP_OK'] and len(p) == 2 * len(PB1A):
                v = dict(zip(PB1A, zip(p[0::2], p[1::2])))
                row.check(v[0x10005404] == (1, 1), '5404 %s' % (v[0x10005404],))
                row.check(v[0x10006400] == (0x01000000,) * 2, '6400 %s' % (v[0x10006400],))
                row.check(v[0x10001C3C] == (0x8000, 0x8000), '1c3c %s' % (v[0x10001C3C],))
                row.check(v[0x10002C18][0] ^ v[0x10002C18][1] == 0x80000000,
                          '2c18 toggles between reads: %s' % (v[0x10002C18],))
            dev = [x for x in acc if x.seq not in b.host_seqs and x.addr in hw]
            want = [a for a in PB1A for _ in (0, 1)]
            row.check([x.addr for x in dev] == want and all(x.rw == 'R' for x in dev),
                      'access list = the request, two reads each: %s' %
                      ' '.join('%s%08x' % (x.rw, x.addr) for x in dev[:20]))
            wr = [x for x in acc if x.rw == 'W' and x.seq not in b.host_seqs and
                  0x10000000 <= x.addr < 0x10010000 and
                  any(lo <= x.addr <= hi for lo, hi in allow)]
            row.check(not wr, 'no write to an allowlisted block: %s' % [str(x) for x in wr[:3]])
            # PB-1b and PW1 in chunks of 15
            for name, lst in (('PB-1b', PB1B), ('PW1', PW1)):
                for i in range(0, len(lst), K['HP_PEEK_MAX']):
                    chunk = lst[i:i + K['HP_PEEK_MAX']]
                    _, st, p = self.peek(vp, chunk)
                    row.check(st == K['HP_OK'] and len(p) == 2 * len(chunk),
                              '%s chunk %d: %d' % (name, i, st))
            # the canary: the counter, 100 us apart
            _, st, p = self.peek(vp, [0x10001804], gap=100)
            row.check(st == K['HP_OK'] and 90 <= (p[1] - p[0]) & M32 <= 110,
                      'canary delta %s' % (p and (p[1] - p[0]) & M32))
        except (T.Fail, V.HP.Timeout) as e:
            row.check(False, str(e))

    # ---- TV (PB-7 to PB-9) ------------------------------------------------

    def tv_call(self, vp, payload, timeout_us=2_000_000):
        return vp.call(K['HP_OP_TV'], [K['HP_TV_MAGIC']] + list(payload),
                       timeout_us=timeout_us)

    def tv_unlock(self, vp):
        _, st, p = self.tv_call(vp, [K['HP_TV_UNLOCK']])
        if st != K['HP_OK'] or len(p) != 1 or not p[0]:
            raise T.Fail('UNLOCK: %d %s' % (st, p))
        return p[0]

    def tv_rules(self, b, ctl_bits=0xC0000000, half_us=1000, dac_bit0=True):
        """The TV block's model for a run: 0x2C18 bit 31 a field flipping
        every half_us of simulated time; 0x2C00 shows ctl_bits once bit 0
        (enable) is written; 0x5404 bit 0 (the DAC present) if asked"""
        timer = b.bus.timer

        class Field:
            def read(self, last, since_write=0):
                return (last & 0x7FFFFFFF) | ((timer.clo // half_us) & 1) << 31

        class Ctl:
            def read(self, last, since_write=0):
                return last | ctl_bits if last & 1 else last

        b.bus.rule(TV_MODE, TV_MODE, Field())
        b.bus.rule(TV_CTL, TV_CTL, Ctl())
        if dac_bit0:
            b.bus.rule('0x10005404|=1')

    def tv_seed(self, b, rng):
        """Distinctive values in every register a run may write, so the
        restore is seen putting them back (not merely writing 0)"""
        regs = b.bus.regs
        for a in range(0x10002C08, 0x10002C18, 4):
            regs[a] = rng.getrandbits(32)
        regs[TV_MODE] = rng.getrandbits(31)
        regs[TV_CTL] = 0
        regs[TV_DAC] = 0x10
        for i in range(23):
            regs[TV_MV + 4 * i] = 0x5A000000 | rng.getrandbits(16)
        regs[TV_PCLK] = 0x0300
        # pin 19 an input (function 0), its neighbours kept
        regs[TV_FSEL1] = 0x55555555 & ~0xC0
        regs[0x10002820] = regs.get(0x10002820, 0) & ~(1 << 19)

    def tv_pre(self, b):
        """What the image reads before its writes (rules applied)"""
        pre = {}
        for a in TV_SAVED:
            if 0x10000B00 <= a < 0x10000B80:
                pre[a] = b.dma.read(a - 0x10000800)
            else:
                v = b.bus.regs.get(a, 0)
                for lo, hi, r in b.bus.rules:
                    if lo <= a <= hi:
                        v = r.read(v, 1 << 30)
                        break
                pre[a] = v
        return pre

    def tv_expected(self, out, std, w704, pre, mv, dma):
        """(on, off, restore): the (address, value) writes a run makes"""
        pal = std == K['HP_TV_PAL']
        tim = TV_TIMING[std]
        fsel_change = (pre[TV_FSEL1] >> 6 & 3) != 1
        on = []
        if out and mv:
            on += [(TV_MV + 4 * i, 4 if i == 19 else 0) for i in range(23)]
        on.append((TV_PCLK, 0xA5A50501))
        if out:
            if fsel_change:
                on.append((TV_FSEL1, (pre[TV_FSEL1] & ~0xC0) | 0x40))
            on += [(TV_GSET, PIN19), (TV_DAC, 0xE0 if pal else 0xA0)]
        on += [(0x10002C08, tim[0] + (64 if out and w704 else 0)),
               (0x10002C0C, tim[1]), (0x10002C10, tim[2]), (0x10002C14, tim[3]),
               (TV_MODE, 0x43 if pal else 0x41), (TV_CTL, 0x00033125)]
        if dma:
            on += [(TV_DMA6, 0x0200FF01), (TV_DMA6 + 4, M32),
                   (TV_DMA6 + 0x38, K['HP_TV_CB'])]
        off = []
        if dma:
            off += [(TV_DMA6, 0), (TV_DMA6 + 4, M32)]
        off.append((TV_CTL, 0))
        if out:
            off += [(TV_DAC, 0xC0 if pal else 0x80), (TV_GCLR, PIN19)]
        off.append((TV_PCLK, 0xA5A50000))
        rest = []
        if dma:
            rest.append((TV_DMA6, pre[TV_DMA6] & 0x1FFFFFFF))
        rest += [(TV_CTL, pre[TV_CTL]), (TV_MODE, pre[TV_MODE] & 0x7FFFFFFF)]
        rest += [(a, pre[a]) for a in (0x10002C14, 0x10002C10, 0x10002C0C, 0x10002C08)]
        if out:
            rest.append((TV_DAC, pre[TV_DAC]))
            lev = b_lev19 = pre['lev19']
            rest.append((TV_GSET if lev else TV_GCLR, PIN19))
            if fsel_change:
                rest.append((TV_FSEL1, pre[TV_FSEL1]))
        rest.append((TV_PCLK, 0xA5A50000 | (pre[TV_PCLK] & 0xFFFF)))
        if out and mv:
            rest += [(TV_MV + 4 * i, pre[TV_MV + 4 * i]) for i in range(22, -1, -1)]
        return on, off, rest

    def tv_run(self, b, vp, payload, kill_at_us=None):
        """One step, the host polling the progress word as the PP would;
        returns (reply, accesses, progress words seen, ch6 sources, cbs)"""
        dma = b.dma
        srcs, dsts, cbs = set(), set(), []
        o_step, o_load = dma._step, dma._load

        def step(ch, k, st):
            if ch == 6:
                srcs.add(st['src'])
                dsts.add(st['dst'])
            return o_step(ch, k, st)

        def load(ch, k, cb):
            if ch == 6:
                cbs.append(cb)
            return o_load(ch, k, cb)
        dma._step, dma._load = step, load
        n0 = len(b.bus.log)
        prog = []
        try:
            tag = vp.send(K['HP_OP_TV'], [K['HP_TV_MAGIC']] + list(payload))
            t0, t_dma = b.now(), None
            while tag not in vp.replies:
                if b.now() - t0 > 12_000_000:
                    raise V.HP.Timeout('TV: no reply')
                b.run(300)
                w = b.bus.r32(K['HP_TV_PROGRESS'])
                if not prog or prog[-1] != w:
                    prog.append(w)
                if kill_at_us is not None and (6, 0) in dma.subs:
                    t_dma = t_dma if t_dma is not None else b.now()
                    if b.now() - t_dma >= kill_at_us:
                        del dma.subs[(6, 0)]        # the chain stops
                vp.drain()
        finally:
            dma._step, dma._load = o_step, o_load
        return vp.replies.pop(tag), b.bus.log[n0:], prog, srcs, dsts, cbs

    def tv_check(self, row, name, b, res, out, std, w704, pre, mv, dma, exit_,
                 baseline, fmt=0, pat=0, ms=0, half_us=1000):
        (op, st, p), acc, prog, srcs, dsts, cbs = res
        okst = K['HP_OK'] if exit_ == K['HP_TV_X_OK'] else K['HP_E_STATE']
        row.check(st == okst and len(p) == K['HP_TV_REPLY_WORDS'],
                  '%s: status %d, %d words' % (name, st, len(p or [])))
        if len(p or []) != K['HP_TV_REPLY_WORDS']:
            return
        row.check(p[K['HP_TV_R_EXIT']] == exit_, '%s: exit %d' % (name, p[0]))
        row.check(p[K['HP_TV_R_FLAGS']] >> 8 == 0, '%s: denied writes %d' % (
            name, p[K['HP_TV_R_FLAGS']] >> 8))
        row.check(p[K['HP_TV_R_MISMATCH']] == 0, '%s: read back after restore %x' % (
            name, p[K['HP_TV_R_MISMATCH']]))
        core = [x for x in acc if x.rw == 'W' and x.seq not in b.host_seqs and
                0x10000000 <= x.addr < 0x10010000 and x.addr != TV_FIFO]
        tvw = [(x.addr, x.value) for x in core if tv_space(x.addr)]
        other = set(x.addr for x in core if not tv_space(x.addr)) - baseline
        row.check(not other, '%s: writes outside the TV blocks: %s' % (
            name, ['%08x' % a for a in sorted(other)][:4]))
        allow = TV_ALLOW_OUTPUT if out else TV_ALLOW_TIMING
        bad = sorted(set(a for a, _ in tvw) - allow)
        row.check(not bad, '%s: off the step\'s allowlist: %s' % (
            name, ['%08x' % a for a in bad][:4]))
        on, off, rest = self.tv_expected(out, std, w704, pre, mv, dma)
        want = on + off + rest
        row.check(tvw == want, '%s: on, off and restore order' % name + (
            '' if tvw == want else ': first difference at %d: got %s want %s' % next(
                (i, ['%08x=%08x' % x for x in tvw[i:i + 2]],
                 ['%08x=%08x' % x for x in want[i:i + 2]])
                for i in range(max(len(tvw), len(want)))
                if i >= len(tvw) or i >= len(want) or tvw[i] != want[i])))
        # every register as it was (the last write is the saved value)
        regs = b.bus.regs
        for a, v in rest:
            if a in (TV_GSET, TV_GCLR):
                continue
            got = regs.get(a, 0)
            if a == TV_PCLK:
                row.check(got & 0xFFFF == pre[a] & 0xFFFF, '%s: 1c30 %08x' % (name, got))
            elif a == TV_MODE:
                row.check(got == pre[a] & 0x7FFFFFFF, '%s: 2c18 %08x' % (name, got))
            elif a == TV_DMA6:
                row.check(got == pre[a] & 0x1FFFFFFF and not any(
                    k[0] == 6 for k in b.dma.subs), '%s: channel 6 stopped' % name)
            else:
                row.check(got == pre[a], '%s: %08x is %08x, was %08x' % (name, a, got, pre[a]))
        # the progress word: in order, the window's ms counted, done last
        phases = [w >> 16 & 0xFF for w in prog if w >> 24 == 0x54]
        row.check(phases and phases[-1] == K['HP_TV_P_DONE'] and
                  all(x <= y for x, y in zip(phases, phases[1:])),
                  '%s: progress %s' % (name, phases))
        row.check(p[K['HP_TV_R_PHASE']] == (K['HP_TV_P_HOLD'] if exit_ in (
            K['HP_TV_X_OK'], K['HP_TV_X_CHAIN']) else K['HP_TV_P_WAIT31']
            if exit_ == K['HP_TV_X_TIMEOUT31'] else K['HP_TV_P_WAIT30']),
            '%s: last phase %d' % (name, p[K['HP_TV_R_PHASE']]))
        if exit_ != K['HP_TV_X_OK'] or not ms:
            return
        hold = [w & 0xFFFF for w in prog if w >> 16 & 0xFF == K['HP_TV_P_HOLD']]
        row.check(hold and max(hold) >= ms - 2, '%s: hold ms seen %s' % (name, hold[-3:]))
        us = p[K['HP_TV_R_US']]
        f = p[K['HP_TV_R_FIELDS']]
        row.check(ms * 1000 <= us < ms * 1000 + 200 and abs(f - us // half_us) <= 2,
                  '%s: %d field changes in %d us' % (name, f, us))
        smp = p[K['HP_TV_R_SAMPLES']:]
        row.check(all(smp[2 * i] & 0xFFFF == 0x3125 for i in range(K['HP_TV_SAMPLES'])),
                  '%s: control sampled %s' % (name, ['%x' % x for x in smp[0::2]]))
        if not out:
            row.check(not srcs and not cbs, '%s: no DMA' % name)
            return
        # channel 6: our block, our line buffer only, the FIFO only
        w = 704 if w704 else 640
        pattern = tv_pattern(fmt, w, pat)
        lo = K['HP_TV_BUF']
        row.check(cbs and set(cbs) == {K['HP_TV_CB']}, '%s: blocks %s' % (
            name, ['%x' % c for c in set(cbs)]))
        row.check(srcs and min(srcs) == lo and max(srcs) == lo + 4 * (len(pattern) - 1),
                  '%s: DMA read %x-%x, the line %x-%x' % (
                      name, min(srcs or [0]), max(srcs or [0]), lo,
                      lo + 4 * len(pattern) - 4))
        row.check(dsts == {TV_FIFO}, '%s: DMA wrote %s' % (name, ['%x' % d for d in dsts]))
        fifo = [x.value for x in acc if x.rw == 'W' and x.addr == TV_FIFO]
        n = min(len(fifo), 3 * len(pattern))
        row.check(n > len(pattern) and fifo[:n] == (pattern * 3)[:n],
                  '%s: the FIFO got the pattern (%d words)' % (name, len(fifo)))

    def t_tv(self):
        row = T.Row('TV (PB-7/8/9): code, allowlists, orders, restore, DMA, progress')
        self.rows.append(row)
        rng = random.Random(14)
        try:
            b, h, vp, prog = self.setup(1)
            hw_any = lambda acc: [x for x in acc if x.rw == 'W' and tv_space(x.addr) and
                                  x.seq not in b.host_seqs]
            # off by default: no code, a wrong code, a used code
            for step in (K['HP_TV_TIMING'], K['HP_TV_OUTPUT']):
                args = [step, 0, 0, 5] + ([0, 0, 0] if step else [])
                (op, st, _), acc = self.window(b, lambda: self.tv_call(vp, args))
                row.check(st == K['HP_E_UNSUPPORTED'] and not hw_any(acc),
                          'step %d unarmed: %d, nothing written' % (step, st))
                code = self.tv_unlock(vp)
                args[3] = code ^ 1
                (op, st, _), acc = self.window(b, lambda: self.tv_call(vp, args))
                row.check(st == K['HP_E_INVAL'] and not hw_any(acc),
                          'step %d wrong code: %d, nothing written' % (step, st))
                args[3] = code
                (op, st, _), acc = self.window(b, lambda: self.tv_call(vp, args))
                row.check(st == K['HP_E_UNSUPPORTED'] and not hw_any(acc),
                          'step %d code used up by the wrong try: %d' % (step, st))
            # malformed, with a good code: refused, the code gone
            for args in ([K['HP_TV_TIMING'], 2, None, 5], [K['HP_TV_TIMING'], 0, None, 0],
                         [K['HP_TV_TIMING'], 0, None, 1001],
                         [K['HP_TV_OUTPUT'], 0, None, 5, 4, 0, 0],
                         [K['HP_TV_OUTPUT'], 0, None, 5, 0, 2, 0],
                         [K['HP_TV_OUTPUT'], 0, None, 8001, 0, 0, 0],
                         [K['HP_TV_OUTPUT'], 0, None, 5, 0, 0],
                         [K['HP_TV_TIMING'], 0, None, 5, 0]):
                args[2] = self.tv_unlock(vp)
                (op, st, _), acc = self.window(b, lambda: self.tv_call(vp, args))
                row.check(st == K['HP_E_INVAL'] and not hw_any(acc),
                          'malformed %s: %d' % (args[:2] + args[3:], st))
            _, st, _ = self.tv_call(vp, [K['HP_TV_UNLOCK'], 0])
            row.check(st == K['HP_E_INVAL'], 'UNLOCK with a payload: %d' % st)
            _, st, _ = self.tv_call(vp, [K['HP_TV_STEPS']])
            row.check(st == K['HP_E_INVAL'], 'step out of the table: %d' % st)
            _, st, _ = vp.call(K['HP_OP_TV'], [0, K['HP_TV_UNLOCK']])
            row.check(st == K['HP_E_INVAL'], 'no magic: %d' % st)
            c1, c2 = self.tv_unlock(vp), self.tv_unlock(vp)
            row.check(c1 != c2, 'codes differ: %08x %08x' % (c1, c2))
            # the MMIO writes a TV request makes outside the TV blocks
            # (the image's own loop around it)
            (_, st, _), acc = self.window(b, lambda: self.tv_call(vp, [0, 0, 0, 5]))
            baseline = set(x.addr for x in acc if x.rw == 'W' and not tv_space(x.addr) and
                           0x10000000 <= x.addr < 0x10010000 and x.seq not in b.host_seqs)

            def run(name, out, std, w704=0, fmt=0, pat=0, ms=12, mv=True,
                    ctl_bits=0xC0000000, kill=None, exit_=K['HP_TV_X_OK']):
                bb, hh, vpp, _ = self.setup(1)
                self.tv_seed(bb, rng)
                self.tv_rules(bb, ctl_bits=ctl_bits, dac_bit0=mv)
                pre = self.tv_pre(bb)
                pre['lev19'] = bb.bus.regs.get(0x10002820, 0) >> 19 & 1
                code = self.tv_unlock(vpp)
                args = [K['HP_TV_OUTPUT' if out else 'HP_TV_TIMING'], std, code, ms]
                if out:
                    args += [fmt, w704, pat]
                res = self.tv_run(bb, vpp, args, kill_at_us=kill)
                dma = out and exit_ in (K['HP_TV_X_OK'], K['HP_TV_X_CHAIN'])
                self.tv_check(row, name, bb, res, out, std, w704, pre, mv, dma,
                              exit_, baseline, fmt, pat, ms)
                return res

            NTSC, PAL = K['HP_TV_NTSC'], K['HP_TV_PAL']
            run('timing ntsc', False, NTSC)
            run('timing pal', False, PAL)
            for fmt in range(K['HP_TV_ARMS']):
                run('output ntsc fmt %d' % fmt, True, NTSC, fmt=fmt)
            run('output pal 704 lines', True, PAL, w704=1, fmt=K['HP_TV_UYVY'],
                pat=K['HP_TV_PAT_LINES'])
            run('output ntsc 704 y32 lines', True, NTSC, w704=1, fmt=K['HP_TV_Y32'],
                pat=K['HP_TV_PAT_LINES'])
            run('output, 5404 bit 0 clear: no Macrovision', True, NTSC, mv=False)
            # the injected error: the chain stops in the window
            run('output, chain killed', True, NTSC, ms=20, kill=3000,
                exit_=K['HP_TV_X_CHAIN'])
            # the timeouts: bit 31 never, then bit 30 never
            run('output, bit 31 timeout', True, PAL, ctl_bits=0,
                exit_=K['HP_TV_X_TIMEOUT31'])
            run('output, bit 30 timeout', True, NTSC, ctl_bits=0x80000000,
                exit_=K['HP_TV_X_TIMEOUT30'])
            # a session open: busy, nothing written
            st, _ = vp.open(320, 240)
            code = self.tv_unlock(vp)
            (_, st, _), acc = self.window(b, lambda: self.tv_call(vp, [0, 0, code, 5]))
            row.check(st == K['HP_E_BUSY'] and not hw_any(acc), 'session open: %d' % st)
        except (T.Fail, V.HP.Timeout) as e:
            row.check(False, str(e))

    def t_census(self):
        row = T.Row('PINS census (PB-2): 52 pins, edges per pin, nothing written')
        self.rows.append(row)
        try:
            b, h, vp, prog = self.setup(1)
            gpio = set(range(0x10002800, 0x10002870, 4))
            b.bus.rule(0x10002820, 0x10002820, Toggle(0, 0x8))         # GPIO 3
            b.bus.rule(0x10002824, 0x10002824, Toggle(0x10000, 0x20000))  # 49 (TE), 48 high
            (op, st, p), acc = self.window(b, lambda: vp.call(
                K['HP_OP_PINS'], [M32, 2000, 0, 0xFFFFF], timeout_us=500_000))
            row.check(st == K['HP_OK'] and len(p) == K['HP_CENSUS_WORDS'],
                      'census: %d, %d words' % (st, len(p or [])))
            if st == K['HP_OK'] and len(p) == K['HP_CENSUS_WORDS']:
                e = [p[6 + k // 4] >> (8 * (k % 4)) & 255 for k in range(52)]
                row.check(p[0] > 4 and p[1] >= 2000, 'samples %d in %d us' % (p[0], p[1]))
                row.check(e[3] == min(255, p[0]) and e[49] == min(255, p[0]),
                          'edges on 3 and 49 every sample: %d %d of %d' % (e[3], e[49], p[0]))
                row.check(all(e[k] == 0 for k in range(52) if k not in (3, 49)),
                          'no other pin moved: %s' % [k for k in range(52) if e[k] and k not in (3, 49)])
                row.check(p[2] & 8 == 0 and p[4] & 8 and p[3] & (1 << 17) == 0 and
                          p[5] & (1 << 17), 'AND/OR show the toggling pins: %s' %
                          ['%08x' % x for x in p[2:6]])
            wr = [x for x in acc if x.rw == 'W' and x.addr in gpio]
            row.check(not wr, 'no GPIO write: %s' % [str(x) for x in wr[:3]])
            _, st, _ = vp.call(K['HP_OP_PINS'], [M32, 2000, 1, 0xFFFFF])
            row.check(st == K['HP_E_INVAL'], 'flags must be 0: %d' % st)
            _, st, _ = vp.call(K['HP_OP_PINS'], [0, 2000, 0, 0])
            row.check(st == K['HP_E_INVAL'], 'empty masks: %d' % st)
        except (T.Fail, V.HP.Timeout) as e:
            row.check(False, str(e))

    def t_pull(self):
        row = T.Row('PULL (PB-4): allowlisted inputs only, Apple\'s order, left pulled down')
        self.rows.append(row)
        try:
            b, h, vp, prog = self.setup(1)
            # a floating pin follows its pull: the clock word latches the mode
            mode = {'m': 0}
            pull = {}

            def pud(bus, a, v):
                mode['m'] = v

            def clk(base):
                def f(bus, a, v):
                    for k in range(32):
                        if v >> k & 1:
                            pull[base + k] = mode['m']
                    lo = sum(1 << k for k in range(32) if pull.get(k) == 2)
                    hi = sum(1 << (k - 32) for k in range(32, 52) if pull.get(k) == 2)
                    bus.regs[0x10002820], bus.regs[0x10002824] = lo, hi
                return f
            # GPIO 32-51 read through the panel-ID rule: keep its straps
            # (50/51, bits 19:18) and add the modelled pulls below them
            panel = next(r for lo, hi, r in b.bus.rules if lo <= 0x10002824 <= hi)

            class Hi:
                def read(self, last, since_write=0):
                    return (panel.read(last, since_write) & 0xFFFF0000) | (last & 0xFFFF)
            b.bus.rule(0x10002824, 0x10002824, Hi())
            b.bus.on_write[0x10002860] = pud
            b.bus.on_write[0x10002864] = clk(0)
            b.bus.on_write[0x10002868] = clk(32)
            # pin 5 an output (function 1): must be skipped
            b.bus.regs[0x10002800] = (b.bus.regs.get(0x10002800, 0) & ~(3 << 10)) | 1 << 10
            allowed = set([0] + list(range(4, 19)) + [20, 21, 22] + list(range(24, 48)))
            for first in range(0, 48, 8):
                (op, st, p), acc = self.window(b, lambda: vp.call(
                    K['HP_OP_PULL'], [K['HP_PULL_MAGIC'], first, 8], timeout_us=500_000))
                row.check(st == K['HP_OK'] and len(p) == 8, 'pins %d-%d: %d' % (
                    first, first + 7, st))
                if st != K['HP_OK'] or len(p) != 8:
                    continue
                for w in p:
                    pin = w & 0xFF
                    if pin not in allowed:
                        row.check(w & K['HP_PULL_SKIP_PIN'], 'gpio%d skipped' % pin)
                    elif pin == 5:
                        row.check(w & K['HP_PULL_SKIP_FSEL'], 'gpio5 (output) skipped')
                    else:
                        row.check((w >> 16 & 3) == 3 and (w >> 18 & 3) == 0,
                                  'gpio%d floats: %08x' % (pin, w))
                wr = [x for x in acc if x.rw == 'W' and 0x10002800 <= x.addr < 0x10002870]
                row.check(all(x.addr in (0x10002860, 0x10002864, 0x10002868) for x in wr),
                          'only the pull registers written: %s' % [
                              str(x) for x in wr if x.addr not in (0x10002860, 0x10002864, 0x10002868)][:3])
                # Apple's order per pin: mode, clock, mode 0, clock 0
                seq = [(x.addr & 0xF, x.value) for x in wr]
                groups = [seq[i:i + 4] for i in range(0, len(seq), 4)]
                good = all(len(g) == 4 and g[0][0] == 0 and g[1][0] in (4, 8) and
                           g[2] == (0, 0) and g[3] == (g[1][0], 0) for g in groups)
                row.check(good and len(groups) == 2 * len([w for w in p if not w & 0x300]),
                          'pins %d-%d: Apple\'s order, up then down: %s' % (
                              first, first + 7, groups[:2]))
                row.check(all(g[0][1] == (2 if i % 2 == 0 else 1)
                              for i, g in enumerate(groups)), 'up, then down per pin')
            row.check(all(pull.get(k) == 1 for k in allowed if k != 5),
                      'every pulled pin left pulled down')
            row.check(not any(k in pull for k in (1, 2, 3, 5, 19, 23, 48, 49, 50, 51)),
                      'never pulled: 1-3, 5 (output), 19, 23, 48-51: %s' % sorted(
                          k for k in (1, 2, 3, 5, 19, 23, 48, 49, 50, 51) if k in pull))
            _, st, _ = vp.call(K['HP_OP_PULL'], [K['HP_PULL_MAGIC'], 0, 9])
            row.check(st == K['HP_E_INVAL'], '9 pins: %d' % st)
            _, st, _ = vp.call(K['HP_OP_PULL'], [K['HP_PULL_MAGIC'], 48, 1])
            row.check(st == K['HP_E_INVAL'], 'first pin 48: %d' % st)
            _, st, p = vp.call(K['HP_OP_PULL'], [K['HP_PULL_MAGIC'], 44, 8], timeout_us=500_000)
            row.check(st == K['HP_OK'] and all(w & K['HP_PULL_SKIP_PIN'] for w in p[4:]),
                      '48-51 skipped: %s' % ['%x' % w for w in p or []])
        except (T.Fail, V.HP.Timeout) as e:
            row.check(False, str(e))

    def cache(self, vp, sub, cc, arg):
        return vp.call(K['HP_OP_CACHE'], [K['HP_CACHE_MAGIC'], sub, cc, arg],
                       timeout_us=60_000_000)

    def t_cache(self):
        row = T.Row('CACHE (PW2): Apple\'s values only, checksums, word restored, handshake')
        self.rows.append(row)
        try:
            b, h, vp, prog = self.setup(1)
            ctl = []
            b.bus.on_write[0x10000C00] = lambda bus, a, v: ctl.append(v)
            _, st, _ = self.cache(vp, 0, 0x80000000, 0)
            row.check(st == K['HP_E_STATE'], 'before SDRAM is up: %d' % st)
            _, st, _ = self.cache(vp, 0, 0x40000000, 0)
            row.check(st == K['HP_E_INVAL'], 'a value Apple never writes: %d' % st)
            _, st, _ = self.cache(vp, 0, 0, 3)
            row.check(st == K['HP_E_INVAL'], 'region 3: %d' % st)
            _, st, _ = self.cache(vp, 1, 0, 2)
            row.check(st == K['HP_E_INVAL'], 'coherence memory 2: %d' % st)
            st, _ = vp.open(320, 240)
            row.check(st == K['HP_OK'], 'OPEN %d' % st)
            rng = random.Random(59)
            regions = {
                K['HP_CACHE_R_SRAM']: (K['HP_CACHE_SRAM'], 16384, 1),
                K['HP_CACHE_R_SDRAM']: (K['HP_CACHE_SDRAM'], 16384, 1),
                K['HP_CACHE_R_SDRAM1M']: (K['HP_CACHE_SDRAM1M'], 1 << 20, 8),
            }
            words = {}
            for r, (base, n, step) in regions.items():
                data = rng.randbytes(n)
                if base < V.SDRAM_BCM:
                    b.write_bytes(base, data)
                else:
                    b.host_sdram_write(base - V.SDRAM_BCM, data)
                words[r] = [int.from_bytes(data[i:i + 4], 'little') for i in range(0, n, 4)]
            cases = [(0x80010000, r) for r in regions] + [(0, 0), (0x80000000, 0)]
            for cc, r in cases:
                base, n, step = regions[r]
                w = words[r]
                mask = n // 4 - 1
                want = sum(w[(i * step) & mask] for i in range(K['HP_CACHE_LOADS'])) & M32
                del ctl[:]
                _, st, p = self.cache(vp, K['HP_CACHE_TIME'], cc, r)
                row.check(st == K['HP_OK'] and len(p) == 5 and p[2] == want and p[3] == want,
                          'cc %08x region %d: %d sums %s want %08x' % (
                              cc, r, st, p and ['%08x' % x for x in p[2:4]], want))
                # the word itself, or Apple's flush: the word read (its
                # ready bit 0 included) | bit 30
                row.check(all(v in (cc, 0x10000) or (v & 0x40000000 and
                                                     v & ~0x40000001 in (cc, 0x10000))
                              for v in ctl),
                          'cc %08x: only Apple\'s values and ours: %s' % (
                              cc, ['%08x' % v for v in ctl]))
                row.check(ctl and ctl[-1] == 0x10000, 'cc %08x: word put back' % cc)
                if st == K['HP_OK']:
                    self.metrics['cc %08x r%d cold/warm us (sim)' % (cc, r)] = '%d/%d' % (p[0], p[1])
            # the coherence handshake, the host's side as the PP's will be
            for mem in (0, 1):
                for cc in (0x80010000, 0):
                    a = K['HP_CACHE_COH_SDRAM'] if mem else K['HP_CACHE_COH_SRAM']

                    def hw(addr, v):
                        if addr >= V.SDRAM_BCM:
                            b.host_sdram_write(addr - V.SDRAM_BCM, v.to_bytes(4, 'little'))
                        else:
                            b.w32(addr, v)

                    def hr(addr):
                        if addr >= V.SDRAM_BCM:
                            o = V.S.SDRAM_BACKING + addr - V.SDRAM_BCM
                            return int.from_bytes(b.bus.b[o:o + 4], 'little')
                        return b.r32(addr)
                    tag = vp.send(K['HP_OP_CACHE'], [K['HP_CACHE_MAGIC'], K['HP_CACHE_COH'], cc, mem])
                    seen, t0 = {}, b.now()
                    while tag not in vp.replies and b.now() - t0 < 2_000_000:
                        b.run(500)
                        s = b.r32(K['HP_CACHE_STEP'])
                        if s == 1 and 2 not in seen:
                            hw(a, K['HP_CACHE_V1'])
                            seen[2] = 1
                            b.w32(K['HP_CACHE_STEP'], 2)
                        elif s == 3 and 4 not in seen:
                            seen[4] = hr(a + 4)
                            b.w32(K['HP_CACHE_STEP'], 4)
                        elif s == 5 and 6 not in seen:
                            seen[6] = hr(a + 4)
                            b.w32(K['HP_CACHE_STEP'], 6)
                        vp.drain()
                    op, st, p = vp.replies.pop(tag, (0, -1, []))
                    row.check(st == K['HP_OK'] and p[:2] == [K['HP_CACHE_V1'], 0],
                              'coh mem %d cc %08x: %d %s' % (mem, cc, st, p))
                    row.check(seen.get(4) == K['HP_CACHE_V2'] and seen.get(6) == K['HP_CACHE_V2'],
                              'coh mem %d: host saw V2 %s' % (mem, seen))
            # nobody answers: every step times out, the word still comes back
            del ctl[:]
            _, st, p = self.cache(vp, K['HP_CACHE_COH'], 0x80010000, 0)
            row.check(st == K['HP_OK'] and p[1] == 7, 'no host: timeouts %s' % p)
            row.check(ctl and ctl[-1] == 0x10000, 'no host: word put back')
        except (T.Fail, V.HP.Timeout) as e:
            row.check(False, str(e))

    def t_dmacpy(self):
        row = T.Row('DMACPY (PW8): channel 7 sub 1, three copies, 0 wrong words')
        self.rows.append(row)
        try:
            b, h, vp, prog = self.setup(1)
            dma = b.dma
            # Apple's memcpy config (0x0000E802) leaves bit 0, vc2mmio's
            # enable [hypothesis], clear: count bit 1 as enable here only
            orig = dma.write

            def write(off, v):
                if off == 7 * 0x80 and v & 2:
                    v |= 1
                orig(off, v)
            dma.write = write
            starts = []
            b.bus.on_write[0x10000B80 + 0x58] = lambda bus, a, v: starts.append(v)
            _, st, _ = vp.call(K['HP_OP_DMACPY'], [K['HP_DMACPY_MAGIC'], 3, 1])
            row.check(st == K['HP_E_INVAL'], 'case 3: %d' % st)
            _, st, _ = vp.call(K['HP_OP_DMACPY'], [K['HP_DMACPY_MAGIC'], K['HP_DMACPY_SD'], 1])
            row.check(st == K['HP_E_STATE'], 'SDRAM before OPEN: %d' % st)
            _, st, p = vp.call(K['HP_OP_DMACPY'], [K['HP_DMACPY_MAGIC'], K['HP_DMACPY_SS'], 7],
                               timeout_us=5_000_000)
            row.check(st == K['HP_OK'] and p[2] == 0 and p[3] == 0 and p[5] == 0,
                      'SRAM>SRAM before OPEN: %d %s' % (st, p))
            st, _ = vp.open(320, 240)
            row.check(st == K['HP_OK'], 'OPEN %d' % st)
            n = K['HP_DMACPY_BYTES'] // 4
            for case, name in ((0, 'SRAM>SRAM'), (1, 'SRAM>SDRAM'), (2, 'SDRAM>SRAM')):
                seed = 0x2722 + case
                _, st, p = vp.call(K['HP_OP_DMACPY'], [K['HP_DMACPY_MAGIC'], case, seed],
                                   timeout_us=5_000_000)
                row.check(st == K['HP_OK'] and len(p) == 6 and p[2] == 0 and p[3] == 0 and
                          p[5] == 0 and p[4] & 0x4000,
                          '%s: %d %s' % (name, st, p))
                dst = K['HP_DMACPY_SDRAM'] if case == 1 else K['HP_DMACPY_DST']
                if dst >= V.SDRAM_BCM:
                    o = V.S.SDRAM_BACKING + dst - V.SDRAM_BCM
                else:
                    o = dst
                got = [int.from_bytes(b.bus.b[o + 4 * i:o + 4 * i + 4], 'little')
                       for i in range(0, n, 97)]
                want = fill(seed, n)[::97]
                row.check(got == want, '%s: the pattern landed' % name)
                if st == K['HP_OK']:
                    self.metrics['%s dma/core us (sim)' % name] = '%d/%d' % (p[0], p[1])
            row.check(len(starts) == 4 and len(set(starts)) == 1 and starts[0] & 31 == 0,
                      'sub-channel 1 started by one aligned block: %s' % starts)
            row.check(not dma.errors, 'engine errors: %s' % dma.errors[:2])
        except (T.Fail, V.HP.Timeout) as e:
            row.check(False, str(e))

    # ---- PW3 / PW4 (brief-map-power.md section 9) ------------------------

    def pw_setup(self, defines=SHIPPED, reg0=0xBB, core0=0x0001, sdram=True):
        """A booted video image with the regulator at reg0 and the core word
        at core0 (the clock block's reset otherwise reads 0); SDRAM up by
        an OPEN and CLOSE when asked"""
        b, h, prog = self.bench(1, defines)
        h.fb = [(i * 7) & 0xFFFF for i in range(T.W * T.H)]
        h.lcd_awake()
        h.settle()
        h.tick_task = False
        h.idle_ok = False
        vp = V.Player(b)
        vp.attach()
        b.bus.regs[PW_REG] = reg0
        if core0 is not None:
            b.bus.regs[PW_CORE] = core0
        if sdram:
            st, _ = vp.open(320, 240)
            if st != K['HP_OK']:
                raise T.Fail('OPEN for SDRAM: %d' % st)
            vp.call(K['HP_OP_CLOSE'], [vp.session], timeout_us=100_000)
        return b, h, vp

    def pw_call(self, vp, payload, timeout_us=5_000_000):
        return vp.call(K['HP_OP_PW'], [K['HP_PW_MAGIC']] + list(payload),
                       timeout_us=timeout_us)

    def pw_unlock(self, vp):
        _, st, p = self.pw_call(vp, [K['HP_PW_UNLOCK']])
        if st != K['HP_OK'] or len(p) != 1 or not p[0]:
            raise T.Fail('PW UNLOCK: %d %s' % (st, p))
        return p[0]

    def pw_pre(self, b):
        """The words the image reads at entry (rules applied)"""
        pre = {}
        for a in (PW_REG, PW_REF):
            v = b.bus.regs.get(a, 0)
            for lo, hi, r in b.bus.rules:
                if lo <= a <= hi:
                    v = r.read(v, 1 << 30)
                    break
            pre[a] = v
        for a in (PW_PLLM, PW_PLLN, PW_CORE):
            pre[a] = b.bus.regs.get(a, 0)
        return pre

    def pw_run(self, b, vp, payload):
        """One step, the host polling the progress word as the PP would;
        returns (reply, accesses, progress words seen)"""
        n0 = len(b.bus.log)
        prog = []
        tag = vp.send(K['HP_OP_PW'], [K['HP_PW_MAGIC']] + list(payload))
        t0 = b.now()
        while tag not in vp.replies:
            if b.now() - t0 > 20_000_000:
                raise V.HP.Timeout('PW: no reply')
            b.run(300)
            w = b.bus.r32(K['HP_PW_PROGRESS'])
            if not prog or prog[-1] != w:
                prog.append(w)
            vp.drain()
        return vp.replies.pop(tag), b.bus.log[n0:], prog

    def pw_expected(self, ladder, what, pre, sdram, stop=None):
        """(up, restore): the (address, value) writes a step makes; stop
        cuts the way up after that many writes (an injected stop)"""
        r0 = pre[PW_REG]
        l0 = r0 & 0xF
        up = []
        if not ladder:
            up.append((PW_REG, PW_PW | (r0 & 0xF0) | what))
            lvl = what
        else:
            mhz = PW_LADDER[what]
            n, div = apple_pll(mhz)
            lvl = apple_level(mhz * 1000000)
            if lvl > l0:
                up.append((PW_REG, PW_PW | (r0 & 0xF0) | lvl))
            up += [(PW_PLLM, PW_PW | 108), (PW_PLLN, PW_PW | n),
                   (PW_PLLN, PW_PW | n | 0x400), (PW_CORE, PW_PW | 0x100 | div)]
            if lvl < l0:
                up.append((PW_REG, PW_PW | (r0 & 0xF0) | lvl))
            if sdram:
                up.append((PW_REF, (pre[PW_REF] & 0xFFFF) | (mhz * 1000000 // 128000) << 16))
        if stop is not None:
            up = up[:stop]
        done = set(a for a, _ in up)
        cur = lvl if any(a == PW_REG for a, _ in up) else l0
        rest = []
        if PW_REF in done:
            rest.append((PW_REF, pre[PW_REF]))
        if PW_REG in done and l0 > cur:
            rest.append((PW_REG, PW_PW | (r0 & 0xFF)))
            cur = l0
        for a in (PW_CORE, PW_PLLN, PW_PLLM):
            if a in done:
                rest.append((a, PW_PW | (pre[a] & 0xFFFF)))
        if PW_REG in done and l0 != cur:
            rest.append((PW_REG, PW_PW | (r0 & 0xFF)))
        return up, rest, lvl

    def pw_check_run(self, row, name, b, res, ladder, what, pre, sdram,
                     exit_=None, stop=None, baseline=frozenset(), frame=False):
        """Every check of one run, into row"""
        exit_ = K['HP_PW_X_OK'] if exit_ is None else exit_
        (op, st, p), acc, prog = res
        okst = K['HP_OK'] if exit_ == K['HP_PW_X_OK'] else K['HP_E_STATE']
        row.check(st == okst and len(p) == K['HP_PW_REPLY_WORDS'],
                  '%s: status %d, %d words' % (name, st, len(p or [])))
        if len(p or []) != K['HP_PW_REPLY_WORDS']:
            return
        row.check(p[K['HP_PW_R_EXIT']] == exit_, '%s: exit %d' % (name, p[0]))
        row.check(p[K['HP_PW_R_FLAGS']] >> 8 == 0, '%s: denied writes %d' % (
            name, p[K['HP_PW_R_FLAGS']] >> 8))
        row.check(p[K['HP_PW_R_MISMATCH']] == 0, '%s: read back after restore %x' % (
            name, p[K['HP_PW_R_MISMATCH']]))
        core = [x for x in acc if x.rw == 'W' and x.seq not in b.host_seqs and
                0x10000000 <= x.addr < 0x10010000]
        pww = [(x.addr, x.value) for x in core if pw_space(x.addr)]
        lcdok = (lambda a: 0x10004000 <= a < 0x10004200 or 0x10000800 <= a < 0x10000C00
                 ) if frame else (lambda a: False)
        other = set(x.addr for x in core if not pw_space(x.addr) and
                    not lcdok(x.addr)) - baseline
        row.check(not other, '%s: writes outside the regulator, PLL A and the '
                  'refresh: %s' % (name, ['%08x' % a for a in sorted(other)][:4]))
        allow = PW_ALLOW_POINT if ladder else PW_ALLOW_LEVEL
        bad = sorted(set(a for a, _ in pww) - allow)
        row.check(not bad, '%s: off the step\'s allowlist: %s' % (
            name, ['%08x' % a for a in bad][:4]))
        up, rest, lvl = self.pw_expected(ladder, what, pre, sdram, stop)
        want = up + rest
        row.check(pww == want, '%s: way up and restore order' % name + (
            '' if pww == want else ': first difference at %d: got %s want %s' % next(
                (i, ['%08x=%08x' % x for x in pww[i:i + 2]],
                 ['%08x=%08x' % x for x in want[i:i + 2]])
                for i in range(max(len(pww), len(want)))
                if i >= len(pww) or i >= len(want) or pww[i] != want[i])))
        breach = pw_invariants(pww, 27, pre[PW_REG] & 0xF,
                               pre[PW_REF] >> 16 if sdram else None)
        row.check(breach is None, '%s: voltage before clock, refresh: %s' % (name, breach))
        # every register as it was
        regs = b.bus.regs
        row.check((regs.get(PW_REG, 0) ^ pre[PW_REG]) & 0xFF == 0,
                  '%s: regulator %08x, was %08x' % (name, regs.get(PW_REG, 0), pre[PW_REG]))
        for a in (PW_PLLM, PW_PLLN, PW_CORE):
            got = regs.get(a, 0)
            row.check((got ^ pre[a]) & 0xFFFF == 0, '%s: %08x is %08x, was %08x' % (
                name, a, got, pre[a]))
        if sdram:
            row.check(regs.get(PW_REF, 0) == pre[PW_REF], '%s: refresh %08x, was %08x' % (
                name, regs.get(PW_REF, 0), pre[PW_REF]))
        # progress in order, done last
        phases = [w >> 16 & 0xFF for w in prog if w >> 24 == 0x50]
        row.check(phases and phases[-1] == K['HP_PW_PH_DONE'] and
                  all(x <= y for x, y in zip(phases, phases[1:])),
                  '%s: progress %s' % (name, phases))
        # the self-check: the references against the model
        sa, la = self.pw_model[0]
        refbad = exit_ == K['HP_PW_X_REFBAD']
        row.check(p[K['HP_PW_R_SUM0']] == sa and (p[K['HP_PW_R_VSUM0']] == la) != refbad,
                  '%s: reference sums %08x/%08x, model %08x/%08x' % (
                      name, p[K['HP_PW_R_SUM0']], p[K['HP_PW_R_VSUM0']], sa, la))
        row.check(p[K['HP_PW_R_US0']] > 0, '%s: reference timed' % name)
        if exit_ != K['HP_PW_X_OK']:
            return p
        runs = p[K['HP_PW_R_RUNS']]
        last = self.pw_model[(runs - 1) & 1]
        row.check(runs & 0xFFFF >= 1 and runs >> 16 == 0 and
                  (p[K['HP_PW_R_SUM']], p[K['HP_PW_R_VSUM']]) == last,
                  '%s: %d runs at the point, last sums %08x/%08x' % (
                      name, runs & 0xFFFF, p[K['HP_PW_R_SUM']], p[K['HP_PW_R_VSUM']]))
        row.check(p[K['HP_PW_R_REG']] & 0xF == lvl, '%s: level %d at the point' % (
            name, p[K['HP_PW_R_REG']] & 0xF))
        if ladder:
            n, div = apple_pll(PW_LADDER[what])
            row.check(p[K['HP_PW_R_CORE']] >> 16 == 0x100 | div and
                      p[K['HP_PW_R_PLLA']] == (108 << 16 | n | 0x400),
                      '%s: core %08x, PLL A %08x at the point' % (
                          name, p[K['HP_PW_R_CORE']], p[K['HP_PW_R_PLLA']]))
            if sdram:
                row.check(p[K['HP_PW_R_REF']] >> 16 == PW_LADDER[what] * 1000000 // 128000,
                          '%s: refresh %08x at the point' % (name, p[K['HP_PW_R_REF']]))
            row.check(bool(p[K['HP_PW_R_FLAGS']] & K['HP_PW_F_SDRAM']) == sdram,
                      '%s: SDRAM flag %x' % (name, p[K['HP_PW_R_FLAGS']]))
        return p

    def pw_mutant_fails(self, defines, ladder, what):
        """A mutant image's run through the same checks: True if they fail"""
        b, h, vp = self.pw_setup(defines)
        pre = self.pw_pre(b)
        code = self.pw_unlock(vp)
        res = self.pw_run(b, vp, [K['HP_PW_POINT' if ladder else 'HP_PW_LEVEL'],
                                  code, what, 0])
        probe = T.Row('mutant')
        self.pw_check_run(probe, 'mutant', b, res, ladder, what, pre, True)
        return probe.fails

    def t_pw(self):
        row = T.Row('PW (PW3/PW4): code, allowlists, order, restore, self-check, mutants')
        self.rows.append(row)
        self.pw_model = [pw_sums(0), pw_sums(1)]
        try:
            # Apple's ladder against the image's table, from the rules alone
            text = open(os.path.join(T.FW, 'hostproto.c')).read()
            body = text[text.index('pw_ladder[HP_PW_POINTS]'):]
            body = body[:body.index('};')]
            rows = [tuple(int(v) for v in m) for m in re.findall(
                r'\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}', body)]
            want = [(f,) + apple_pll(f) + (apple_level(f * 1000000), f * 1000000 // 128000)
                    for f in PW_LADDER]
            row.check(rows == want, 'pw_ladder = Apple\'s rule and table: %s' % rows)
            row.check([lv for *_, lv, _ in want] == [5, 7, 9, 15], 'levels 5, 7, 9, 15')
            row.check(want[-1][1] == 0x258 and want[-1][4] == 0x493,
                      '150 MHz: clock.c\'s N and video.c\'s refresh')

            b, h, vp = self.pw_setup(sdram=False)
            hw_any = lambda acc: [x for x in acc if x.rw == 'W' and pw_space(x.addr) and
                                  x.seq not in b.host_seqs]
            L, P = K['HP_PW_LEVEL'], K['HP_PW_POINT']
            # off by default: no code, a wrong code, a used code
            for step, what in ((L, 5), (P, 0)):
                args = [step, 0, what, 0]
                (op, st, _), acc = self.window(b, lambda: self.pw_call(vp, args))
                row.check(st == K['HP_E_UNSUPPORTED'] and not hw_any(acc),
                          'step %d unarmed: %d, nothing written' % (step, st))
                code = self.pw_unlock(vp)
                args[1] = code ^ 1
                (op, st, _), acc = self.window(b, lambda: self.pw_call(vp, args))
                row.check(st == K['HP_E_INVAL'] and not hw_any(acc),
                          'step %d wrong code: %d, nothing written' % (step, st))
                args[1] = code
                (op, st, _), acc = self.window(b, lambda: self.pw_call(vp, args))
                row.check(st == K['HP_E_UNSUPPORTED'] and not hw_any(acc),
                          'step %d code used up by the wrong try: %d' % (step, st))
            # malformed, with a good code: refused, nothing written
            for args in ([L, None, 4, 0], [L, None, 16, 0], [P, None, 4, 0],
                         [L, None, 5, K['HP_PW_HOLD_MS'] + 1], [L, None, 5],
                         [P, None, 0, 0, 0]):
                args[1] = self.pw_unlock(vp)
                (op, st, _), acc = self.window(b, lambda: self.pw_call(vp, args))
                row.check(st == K['HP_E_INVAL'] and not hw_any(acc),
                          'malformed %s: %d' % (args[:1] + args[2:], st))
            _, st, _ = self.pw_call(vp, [K['HP_PW_UNLOCK'], 0])
            row.check(st == K['HP_E_INVAL'], 'UNLOCK with a payload: %d' % st)
            _, st, _ = self.pw_call(vp, [K['HP_PW_STEPS']])
            row.check(st == K['HP_E_INVAL'], 'step out of the table: %d' % st)
            _, st, _ = vp.call(K['HP_OP_PW'], [0, K['HP_PW_UNLOCK']])
            row.check(st == K['HP_E_INVAL'], 'no magic: %d' % st)
            c1, c2 = self.pw_unlock(vp), self.pw_unlock(vp)
            row.check(c1 != c2, 'codes differ: %08x %08x' % (c1, c2))
            # the state refusals: nothing written
            for name, fix in (('core on PLL A', lambda: b.bus.regs.__setitem__(PW_CORE, 0x101)),
                              ('level 3', lambda: b.bus.regs.__setitem__(PW_REG, 0xB3)),
                              ('level 0', lambda: b.bus.regs.__setitem__(PW_REG, 0xB0))):
                b.bus.regs[PW_CORE] = 1
                b.bus.regs[PW_REG] = 0xBB
                fix()
                code = self.pw_unlock(vp)
                (_, st, p), acc = self.window(b, lambda: self.pw_call(vp, [L, code, 5, 0]))
                row.check(st == K['HP_E_STATE'] and not hw_any(acc),
                          '%s: refused %d %s, nothing written' % (name, st, p))
            b.bus.regs[PW_CORE] = 1
            b.bus.regs[PW_REG] = 0xBB
            # the writes a step makes outside the PW blocks (the loop's own)
            code = self.pw_unlock(vp)
            (_, st, _), acc = self.window(b, lambda: self.pw_call(vp, [L, 0, 5, 0]))
            baseline = frozenset(x.addr for x in acc if x.rw == 'W' and not pw_space(x.addr) and
                                 0x10000000 <= x.addr < 0x10010000 and
                                 x.seq not in b.host_seqs)

            def run(name, ladder, what, sdram=True, reg0=0xBB, core0=0x0001,
                    exit_=None, stop=None, inject=None, hold=0, us_per_read=None,
                    defines=SHIPPED):
                bb, hh, vpp = self.pw_setup(defines, reg0=reg0, core0=core0, sdram=sdram)
                if inject:
                    inject(bb)
                if us_per_read:
                    bb.bus.timer.us_per_read = us_per_read
                pre = self.pw_pre(bb)
                code = self.pw_unlock(vpp)
                res = self.pw_run(bb, vpp, [P if ladder else L, code, what, hold])
                frame = ladder and what == K['HP_PW_P_150']
                p = self.pw_check_run(row, name, bb, res, ladder, what, pre, sdram,
                                      exit_=exit_, stop=stop, baseline=baseline,
                                      frame=frame)
                return bb, res, p

            # PW3: each level of the spec, and the table's top
            for lv in (11, 9, 7, 5, 15):
                run('PW3 level %d' % lv, False, lv)
            # PW4: each ladder point, with SDRAM up; one without
            for k in range(K['HP_PW_POINTS']):
                bb, res, p = run('PW4 %d MHz' % PW_LADDER[k], True, k)
            # the 150 MHz point pushed one frame with Apple's words, then
            # the bus words came back
            if p:
                row.check(p[K['HP_PW_R_FLAGS']] & K['HP_PW_F_FRAME'] and
                          p[K['HP_PW_R_FRAME']] > 0,
                          '150 MHz: a frame pushed (%d us)' % p[K['HP_PW_R_FRAME']])
                t0w = [x.value for x in res[1] if x.rw == 'W' and x.addr == 0x10004010 and
                       x.seq not in bb.host_seqs]
                row.check(len(t0w) >= 2 and t0w[0] != t0w[-1],
                          '150 MHz: the bus words changed and came back: %s' % [
                              '%08x' % v for v in t0w])
            run('PW4 74 MHz, SDRAM down', True, 1, sdram=False)
            run('PW4 102 MHz, core word 0 at entry', True, 2, core0=None)
            run('PW4 150 MHz from level 5', True, 3, reg0=0xB5)
            # a hold: several runs, both patterns by turns
            bb, res, p = run('PW3 level 5, a hold', False, 5, hold=3, us_per_read=200)
            if p:
                row.check(p[K['HP_PW_R_RUNS']] & 0xFFFF >= 2,
                          'hold: %d runs' % (p[K['HP_PW_R_RUNS']] & 0xFFFF))

            # injected: the regulator reads back its old level
            def stuck(bb):
                bb.bus.rule('0x10001400=0xBB')
            run('PW3 level 5, regulator stuck', False, 5, inject=stuck,
                exit_=K['HP_PW_X_READBACK'])

            # injected: the vector unit reads wrong once the level is down
            def vwrong(at_level):
                def inject(bb):
                    st = {'low': False}
                    orig = bb.bus.read

                    def rd(a, n):
                        d = orig(a, n)
                        lo = K['HP_PW_WIN']
                        if st['low'] and lo <= a < lo + K['HP_PW_WIN_BYTES']:
                            d = bytes([d[0] ^ 1]) + d[1:]
                        return d

                    def on_reg(bus, a, v):
                        st['low'] = v & 0xF == at_level
                    bb.bus.read = rd
                    bb.bus.on_write[PW_REG] = on_reg
                return inject
            bb, res, p = run('PW3 level 5, wrong vector sum', False, 5,
                             inject=vwrong(5), exit_=K['HP_PW_X_CHECK'])
            if p:
                row.check(p[K['HP_PW_R_RUNS']] == (1 << 16 | 1) and
                          p[K['HP_PW_R_SUM']] == self.pw_model[0][0] and
                          p[K['HP_PW_R_VSUM']] != self.pw_model[0][1],
                          'wrong sum: run 1 wrong, the core\'s sum right, the vector\'s '
                          'not: %08x %08x' % (p[K['HP_PW_R_RUNS']], p[K['HP_PW_R_VSUM']]))
            run('PW4 46 MHz, wrong vector sum', True, 0, inject=vwrong(5),
                exit_=K['HP_PW_X_CHECK'])

            # injected: CPU and vector disagree at the start: nothing written
            def vbad(bb):
                orig = bb.bus.read
                bb.bus.read = lambda a, n: bytes([orig(a, n)[0] ^ 1]) + orig(a, n)[1:]
            bb, res, p = run('PW4 150 MHz, reference bad', True, 3, inject=vbad,
                             exit_=K['HP_PW_X_REFBAD'], stop=0)

            # the counter stops in a wait (the PLL's, then the regulator's
            # settle); it runs again at the restore's first write there
            def stall(addr, nth_stop, nth_go):
                def inject(bb):
                    tm = bb.bus.timer
                    on = {'run': True, 'n': 0}
                    tm.count = tm.clo
                    tm.gate = lambda: on['run']

                    def hook(bus, a, v):
                        on['n'] += 1
                        if on['n'] == nth_stop:
                            on['run'] = False
                        elif on['n'] == nth_go:
                            on['run'] = True
                    bb.bus.on_write[addr] = hook
                return inject
            # 150: regulator up, M, N (stop), ... the restore's N write
            run('PW4 150 MHz, counter stops in the PLL wait', True, 3,
                inject=stall(PW_PLLN, 1, 2), exit_=K['HP_PW_X_STALL'], stop=3)
            # 150: regulator up (stop) ... the restore's regulator write
            bb, res, p = run('PW4 150 MHz, counter stops in the settle', True, 3,
                             inject=stall(PW_REG, 1, 2), exit_=K['HP_PW_X_STALL'],
                             stop=1)
            run('PW3 level 7, counter stops in the settle', False, 7,
                inject=stall(PW_REG, 1, 2), exit_=K['HP_PW_X_STALL'])

            # a session open: busy, nothing written
            st, _ = vp.open(320, 240)
            code = self.pw_unlock(vp)
            (_, st, _), acc = self.window(b, lambda: self.pw_call(vp, [L, code, 5, 0]))
            row.check(st == K['HP_E_BUSY'] and not hw_any(acc), 'session open: %d' % st)

            # the mutants: the same checks must fail
            f = self.pw_mutant_fails(SHIPPED + ('PW_MUTANT=1',), True, K['HP_PW_P_150'])
            row.check(any('voltage before clock' in x for x in f),
                      'mutant 1 (level after the clock) caught: %s' % f[:2])
            f = self.pw_mutant_fails(SHIPPED + ('PW_MUTANT=2',), False, 5)
            row.check(any('regulator' in x or 'restore order' in x for x in f),
                      'mutant 2 (no restore) caught: %s' % f[:2])
            f = self.pw_mutant_fails(SHIPPED + ('PW_MUTANT=2',), True, K['HP_PW_P_102'])
            row.check(sum('was' in x for x in f) >= 3,
                      'mutant 2 at 102 MHz: core, PLL and refresh left: %s' % f[:3])
        except (T.Fail, V.HP.Timeout) as e:
            row.check(False, str(e))

    # ---- PW5 (brief-map-power.md sections 3 and 9) -----------------------

    def hib_call(self, vp, payload, timeout_us=2_000_000):
        return vp.call(K['HP_OP_HIB'], [K['HP_HIB_MAGIC']] + list(payload),
                       timeout_us=timeout_us)

    def hib_unlock(self, vp):
        _, st, p = self.hib_call(vp, [K['HP_HIB_UNLOCK']])
        if st != K['HP_OK'] or len(p) != 1 or not p[0]:
            raise T.Fail('HIB UNLOCK: %d %s' % (st, p))
        return p[0]

    def hib_seed(self, b):
        """Distinct values in every word the park saves: the PLLs (B on,
        D with an N and no enable), the event words"""
        for a, v in ((0x10001C04, 0x6C), (0x10001C18, 0x258 | 0x400),
                     (0x10001C0C, 0x36), (0x10001C20, 0x120),
                     (0x10001C00, 0x6C), (0x10001C14, 0x258)):
            b.bus.regs[a] = v
        for i in range(HIB_GPIO_N):
            b.bus.regs[HIB_GPIO + 4 * i] = 0x1000 + i
        return {a: b.bus.regs.get(a, 0) for a in HIB_SAVED}

    def hib_run(self, b, vp, payload, hold_us=20_000, wake=True, during=None):
        """One park: the host polls the word as the PP would, writes the
        wake word hold_us after it saw HP_HIB_PARKED (never, without
        wake), and touches nothing else meanwhile.  Returns (reply, the
        bus log, progress words, the log index at the park and at the
        wake, the panel at the park)"""
        n0 = len(b.bus.log)
        prog, park, woke, panel = [], None, None, None
        tag = vp.send(K['HP_OP_HIB'], [K['HP_HIB_MAGIC']] + list(payload))
        t0 = b.now()
        while tag not in vp.replies:
            if b.now() - t0 > 20_000_000:
                raise V.HP.Timeout('HIB: no reply')
            b.run(300)
            w = b.bus.r32(K['HP_HIB_PROGRESS'])
            if not prog or prog[-1] != w:
                prog.append(w)
            if park is None and b.bus.r32(K['HP_HIB_WORD']) == K['HP_HIB_PARKED']:
                park = (b.now(), len(b.bus.log))
                panel = list(b.panel.fb)
                if during:
                    during(b)
            if park and wake and woke is None and b.now() - park[0] >= hold_us:
                woke = len(b.bus.log)
                b.bus.w32(K['HP_HIB_WORD'], K['HP_HIB_WAKE'])
            if park is None or woke is not None or not wake:
                vp.drain()
        return (vp.replies.pop(tag), b.bus.log[n0:], prog,
                None if park is None else park[1] - n0,
                None if woke is None else woke - n0, panel)

    def hib_check_run(self, row, name, b, res, pre, sdram, exit_=None,
                      parked=True, bad=(0, 0), frame_same=True):
        exit_ = K['HP_HIB_X_OK'] if exit_ is None else exit_
        (op, st, p), acc, prog, i_park, i_wake, panel = res
        okst = K['HP_OK'] if exit_ == K['HP_HIB_X_OK'] else K['HP_E_STATE']
        row.check(st == okst and len(p) == K['HP_HIB_REPLY_WORDS'],
                  '%s: status %d, %d words' % (name, st, len(p or [])))
        if len(p or []) != K['HP_HIB_REPLY_WORDS']:
            return None
        R = lambda k: p[K['HP_HIB_R_' + k]]
        row.check(R('EXIT') == exit_, '%s: exit %d' % (name, R('EXIT')))
        row.check(R('FLAGS') >> 8 == 0, '%s: denied writes %d' % (name, R('FLAGS') >> 8))
        row.check(R('MISMATCH') == 0, '%s: read back after restore %x' % (name, R('MISMATCH')))
        row.check(bool(R('FLAGS') & K['HP_HIB_F_SDRAM']) == sdram,
                  '%s: SDRAM flag %x' % (name, R('FLAGS')))
        dev = [x for x in acc if x.seq not in b.host_seqs and 0x10000000 <= x.addr < 0x10010000]
        wr = [x for x in dev if x.rw == 'W']
        other = sorted(set(x.addr for x in wr if not hib_space(x.addr) and
                           not 0x10004000 <= x.addr < 0x10004200 and
                           not 0x10000800 <= x.addr < 0x10000C00))
        row.check(not other, '%s: writes outside the PLLs, the event words, the '
                  'cache, SDRAM and the panel: %s' % (name, ['%08x' % a for a in other][:4]))
        hw = [(x.addr, x.value) for x in wr if hib_space(x.addr)]
        park = [(a, PW_PW) for a in HIB_PLL] + [(HIB_GPIO + 4 * i, 0) for i in range(HIB_GPIO_N)]
        row.check(hw[:len(park)] == park, '%s: Apple\'s park order (PLLs B, C, D, '
                  'A, then the 14 event words): %s' % (name, ['%08x=%08x' % x for x in hw[:3]]))
        if parked:
            row.check(i_park is not None, '%s: parked' % name)
        if i_park is not None:
            before = [(x.addr, x.value) for x in acc[:i_park] if x.rw == 'W' and
                      x.seq not in b.host_seqs and hib_space(x.addr)][len(park):]
            if sdram:
                row.check(before and before[-1] == (0x10003420, 0) and
                          all(a in (0x10000C00, 0x10003420) for a, _ in before),
                          '%s: SDRAM idled last, cache and command only: %s' % (
                              name, ['%08x=%08x' % x for x in before[-4:]]))
            else:
                row.check(not before, '%s: SDRAM down, untouched: %s' % (
                    name, ['%08x=%08x' % x for x in before[:4]]))
            end = i_wake if i_wake is not None else len(acc)
            quiet = [x for x in acc[i_park:end] if x.seq not in b.host_seqs and
                     0x10000000 <= x.addr < 0x10010000 and
                     not (x.rw == 'R' and x.addr == 0x10001804)]
            if i_wake is None and quiet:
                # no wake word: the park ends at the restore's first write
                # (PLL A's divider)
                row.check(quiet[0].rw == 'W' and quiet[0].addr == 0x10001C00,
                          '%s: the park ended by the restore: %s' % (name, quiet[0]))
                quiet = []
            row.check(not quiet, '%s: parked, the core touched only the counter: %s' % (
                name, [str(x) for x in quiet[:2]]))
            if panel is not None and R('FLAGS') & K['HP_HIB_F_PANEL']:
                row.check(panel == hib_panel(False), '%s: the bars on the panel at the park' % name)
        if not sdram:
            row.check(not any(0x10003400 <= a < 0x10003440 or a == 0x10000C00 for a, _ in hw),
                      '%s: SDRAM down: no SDRAM or cache write' % name)
        # the restore: PLLs A, D, C, B (N without the enable, then the
        # enables after 1 ms), the event words back, everything as it was
        rest = [(a, v) for a, v in hw[len(park):] if a in HIB_PLL]
        want = []
        for k in (3, 2, 1, 0):
            dv, ct = HIB_PLL[2 * k], HIB_PLL[2 * k + 1]
            want += [(dv, PW_PW | pre[dv] & 0xFFFF), (ct, PW_PW | pre[ct] & 0xFFFF & ~0x400)]
        want += [(HIB_PLL[2 * k + 1], PW_PW | pre[HIB_PLL[2 * k + 1]] & 0xFFFF)
                 for k in (3, 2, 1, 0) if pre[HIB_PLL[2 * k + 1]] & 0x400]
        row.check(rest == want, '%s: PLL restore order: %s' % (
            name, ['%08x=%08x' % x for x in rest[:3]]))
        regs = b.bus.regs
        for a in HIB_PLL:
            row.check((regs.get(a, 0) ^ pre[a]) & 0xFFFF == 0, '%s: %08x is %08x, was %08x' % (
                name, a, regs.get(a, 0), pre[a]))
        for i in range(2, HIB_GPIO_N):
            a = HIB_GPIO + 4 * i
            row.check(regs.get(a, 0) == pre[a], '%s: %08x is %08x, was %08x' % (
                name, a, regs.get(a, 0), pre[a]))
        word = b.bus.r32(K['HP_HIB_WORD'])
        row.check(word == (K['HP_HIB_AWAKE'] if exit_ == K['HP_HIB_X_OK'] else K['HP_HIB_GAVEUP']),
                  '%s: the word at the end %08x' % (name, word))
        phases = [w >> 16 & 0xFF for w in prog if w >> 24 == 0x48]
        row.check(phases and phases[-1] == K['HP_HIB_PH_DONE'] and
                  all(x <= y for x, y in zip(phases, phases[1:])),
                  '%s: progress %s' % (name, phases))
        # what the dark kept
        row.check((R('SRAM_BAD'), R('SDRAM_BAD') if sdram else 0) == bad,
                  '%s: wrong words SRAM %d SDRAM %d' % (name, R('SRAM_BAD'), R('SDRAM_BAD')))
        if not sdram:
            row.check(R('SDRAM_BAD') == M32 and R('SD') == M32, '%s: SDRAM not laid' % name)
        else:
            row.check(R('SD') == 0, '%s: SDRAM brought up again: %x' % (name, R('SD')))
        row.check(R('FRAME_PRE') == hib_sum(False) and
                  (R('FRAME_POST') == R('FRAME_PRE')) == frame_same,
                  '%s: frame sums %08x %08x, model %08x' % (
                      name, R('FRAME_PRE'), R('FRAME_POST'), hib_sum(False)))
        if R('FLAGS') & K['HP_HIB_F_PANEL']:
            row.check(list(b.panel.fb) == hib_panel(True), '%s: the bars mirrored after' % name)
        return p

    def t_hib(self):
        row = T.Row('HIB (PW5): code, refusals, Apple\'s park, quiet park, restore, checks')
        self.rows.append(row)
        try:
            b, h, vp = self.pw_setup(sdram=False)
            hw_any = lambda acc: [x for x in acc if x.rw == 'W' and hib_space(x.addr) and
                                  x.seq not in b.host_seqs]
            P = K['HP_HIB_PARK']
            for name, args in (('unarmed', [P, 0, 100]),):
                (_, st, _), acc = self.window(b, lambda: self.hib_call(vp, args))
                row.check(st == K['HP_E_UNSUPPORTED'] and not hw_any(acc),
                          '%s: %d, nothing written' % (name, st))
            code = self.hib_unlock(vp)
            (_, st, _), acc = self.window(b, lambda: self.hib_call(vp, [P, code ^ 1, 100]))
            row.check(st == K['HP_E_INVAL'] and not hw_any(acc), 'wrong code: %d' % st)
            (_, st, _), acc = self.window(b, lambda: self.hib_call(vp, [P, code, 100]))
            row.check(st == K['HP_E_UNSUPPORTED'] and not hw_any(acc),
                      'code used up by the wrong try: %d' % st)
            for args in ([P, None, 0], [P, None, K['HP_HIB_WAIT_MS'] + 1], [P, None],
                         [P, None, 100, 0]):
                args[1] = self.hib_unlock(vp)
                (_, st, _), acc = self.window(b, lambda: self.hib_call(vp, args))
                row.check(st == K['HP_E_INVAL'] and not hw_any(acc),
                          'malformed %s: %d' % (args[:1] + args[2:], st))
            _, st, _ = self.hib_call(vp, [K['HP_HIB_UNLOCK'], 0])
            row.check(st == K['HP_E_INVAL'], 'UNLOCK with a payload: %d' % st)
            _, st, _ = self.hib_call(vp, [K['HP_HIB_STEPS']])
            row.check(st == K['HP_E_INVAL'], 'step out of the table: %d' % st)
            _, st, _ = vp.call(K['HP_OP_HIB'], [0, K['HP_HIB_UNLOCK']])
            row.check(st == K['HP_E_INVAL'], 'no magic: %d' % st)
            c1, c2 = self.hib_unlock(vp), self.hib_unlock(vp)
            row.check(c1 != c2, 'codes differ: %08x %08x' % (c1, c2))
            b.bus.regs[PW_CORE] = 0x101
            code = self.hib_unlock(vp)
            (_, st, p), acc = self.window(b, lambda: self.hib_call(vp, [P, code, 100]))
            row.check(st == K['HP_E_STATE'] and not hw_any(acc) and
                      b.bus.r32(K['HP_HIB_WORD']) != K['HP_HIB_PARKED'],
                      'core on PLL A: refused %d %s, nothing written' % (st, p))
            b.bus.regs[PW_CORE] = 1
            st, _ = vp.open(320, 240)
            code = self.hib_unlock(vp)
            (_, st, _), acc = self.window(b, lambda: self.hib_call(vp, [P, code, 100]))
            row.check(st == K['HP_E_BUSY'] and not hw_any(acc), 'session open: %d' % st)

            def run(name, sdram=True, ms=1000, hold_us=20_000, wake=True, during=None,
                    inject=None, **kw):
                bb, hh, vpp = self.pw_setup(sdram=sdram)
                pre = self.hib_seed(bb)
                if inject:
                    inject(bb)
                code = self.hib_unlock(vpp)
                res = self.hib_run(bb, vpp, [P, code, ms], hold_us, wake, during)
                return bb, res, self.hib_check_run(row, name, bb, res, pre, sdram, **kw)

            bb, res, p = run('park, SDRAM up')
            if p:
                row.check(p[K['HP_HIB_R_HELD_US']] >= 20_000 and p[K['HP_HIB_R_POLLS']] > 0 and
                          p[K['HP_HIB_R_PARK_US']] > 0,
                          'held %d us, %d polls, parked in %d us' % (
                              p[K['HP_HIB_R_HELD_US']], p[K['HP_HIB_R_POLLS']],
                              p[K['HP_HIB_R_PARK_US']]))
                self.metrics['hib park us (vc2sim)'] = p[K['HP_HIB_R_PARK_US']]
                self.metrics['hib restore us (vc2sim)'] = p[K['HP_HIB_R_RESTORE_US']]
            run('park, SDRAM down', sdram=False)

            # the host writes into every window while parked: reported
            def spoil(bb):
                bb.bus.w32(K['HP_HIB_SRAM'] + 4 * 100, 0xDEADBEEF)
                bb.host_sdram_write(K['HP_HIB_SDRAM'] - 0xFE000000 + 4 * 7, b'\xEF\xBE\xAD\xDE')
                bb.bus.w32(0xE0000 + 8, 0x12345678)
            bb, res, p = run('park, words spoiled while parked', during=spoil,
                             bad=(1, 1), frame_same=False)
            if p:
                row.check(p[K['HP_HIB_R_SRAM_FIRST']] == 100 and
                          p[K['HP_HIB_R_SDRAM_FIRST']] == 7,
                          'spoiled: first wrong SRAM %d SDRAM %d' % (
                              p[K['HP_HIB_R_SRAM_FIRST']], p[K['HP_HIB_R_SDRAM_FIRST']]))
            # no wake word: the image's own bound, then everything back
            bb, res, p = run('no wake word: given up', ms=3, wake=False,
                             exit_=K['HP_HIB_X_GAVEUP'])
            if p:
                row.check(p[K['HP_HIB_R_HELD_US']] >= 3000, 'gave up after %d us' %
                          p[K['HP_HIB_R_HELD_US']])

            # the counter stops while parked (at the SDRAM idle), and runs
            # again at the restore's first PLL write
            def stall(bb):
                tm = bb.bus.timer
                on = {'run': True, 'armed': False}
                tm.count = tm.clo
                tm.gate = lambda: on['run']

                def stop(bus, a, v):
                    if v == 0:
                        on['run'], on['armed'] = False, True

                def go(bus, a, v):
                    if on['armed']:
                        on['run'] = True
                bb.bus.on_write[0x10003420] = stop
                bb.bus.on_write[0x10001C00] = go
            run('counter stopped while parked', inject=stall, wake=False,
                exit_=K['HP_HIB_X_STALL'])

            # SDRAM's bit 15 never clears: never parked, everything back
            def stuck(bb):
                bb.bus.rule('0x10003420|=0x8000')
            run('SDRAM idle stuck', inject=stuck, wake=False, parked=False,
                exit_=K['HP_HIB_X_SDIDLE'])
        except (T.Fail, V.HP.Timeout) as e:
            row.check(False, str(e))

    def run(self):
        for name in ('peek', 'tv', 'census', 'pull', 'cache', 'dmacpy', 'pw', 'hib'):
            if self.args.only and self.args.only != name:
                continue
            getattr(self, 't_' + name)()
        for row in self.rows:
            print('%-72s %5d %5d' % (row.name[:72], row.cases, row.cases - len(row.fails)))
            for f in row.fails[:8]:
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
    ap.add_argument('--png', default=os.path.join(T.ROOT, 'build', 'bcmfw', 'test-peek'))
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--only', default='')
    a = ap.parse_args(argv)
    if not V.B.have_gcc():
        print('skip: vc4-elf-gcc not found')
        return 0
    return Suite(a).run()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
