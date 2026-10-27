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

"""Generate the display jobs' vector kernels (jobs_kernels.S) for the BCM
image's executor (jobs_bcm.c, through jref.h's copy, fill, mix16 and
shade16 hooks).  They work on 16-pixel RGB565 columns, in 16-row groups
or row by row, and are bit for bit the C definitions in bcmfw/jobs/jmix.h
(FADE, AVG, SHADE) or jref.c's copies (BLIT, its key colour, SCROLL, FILL,
the animations); bcmfw/tests/jobs_image_test.py checks them against models
of those definitions in vc2sim, and --mutate breaks one on purpose so the
test can be seen to catch it.

Every row is 32-byte aligned (isa-vector.md rule 6), r7 is never a base,
a stride or a scalar B (rules 1 and v_sreg7), and results are stored only
by single 48-bit vst16s of block-0 rows at offset 0, the base moved a row
at a time: the store form the device checked (sessions 5-7, COPY/COP8).
Loads are vld16 REP 16 with a stride, or REP 1 into row 0 (session 4).
The ALU forms are the ones gen_xfade.py's kernels use (MU8R, MULR, ALUX,
AD68, ADIR, LSR8, MHR4), plus these, which only vc2sim and Apple's corpus
have shown (unproven on silicon; jobs.md section 10 lists them):
  vmul D16 A16 B16 with a vector B (SHADE's per-column factors and the
  raw FADE's, one unstepped row; stepped: the key's mask), vdist D16 A8
  Breg, vasr D16 A16 #imm, vsub D16 A16 B16 (the key).
And these, for a source out of phase (jv_phase16, jv_gather16; bench row
GTHR runs the same four ops; the start-up check, jobs_bcm.c shift_check,
keeps the C if they misbehave):
  vmov D16 B V16 rep16, both stepping: a 16 x 16 transpose (REPA ran a
  stepping V16 B on silicon, 2026-10-25);
  vadd D16 A16+rN #0 rep16 with a y offset only (+r = 64 p): REPA (y 16)
  and R48E (y 1) ran an A operand's +r y on silicon;
  vld16 REP 16 into rows 32-47 and 48-63 at x 0 (the inputs' loads use
  rows 16-31 the same way).

  The lane shift is the VPU's form of the unaligned-load idiom (AltiVec's
  lvsl/vperm, NEON's vext, SSSE3's palignr), done in the 2-D VRF: the 32
  pixels of a row pair, transposed, become 32 rows; a y offset of p rows
  is the shift; transposed back they are pixels p..p+15.  The earlier form
  (an H16 A operand at x = p and 16 + p from a register) is wrong on
  silicon: an A operand's +r x is aligned (quirks.md #30, bench
  ASHF/ASHG), as a horizontal B's is (#17b).  A plain vld16 can't do it
  either: it aligns its address down (quirks.md #17a), and the vldp/vldq
  pair is the decoder's, unchecked at REP 16 with a stride.

  jv_store16(r0, pitch r1)
      the common tail: F's 16 rows to r0, r0 + pitch, ...; entered at
      jv_store16_8 or jv_store16_12, rows 8-15 or 12-15 only (jdec's put)
  jv_group16(d r0, s r1, dpitch r2, spitch r3, cols r4, key r5)
      cols 16-pixel columns of one 16-row group from s to d; a column's
      16 rows are all loaded before any is stored.  key: 0, or
      J_BLIT_KEY | colour << 16 (jv_key16).  d is 32-byte aligned, s at
      any (2-byte) phase: jv_gather16.
  jv_key16 (jv_group16 with a key)
      per pixel t = |hi - kh| + |lo - kl| (0 exactly when s is the key),
      m = (t - 1) >> 15 (-1 or 0), d' = s + (s - d) * m: d where s is the
      key, s elsewhere, without flags or predication.
  jv_phase16(s r6, byte phase r2, not 0)
      r6 aligned down, r2 = 64 p (p the phase in pixels).
  jv_gather16(base r6, stride r3, 64 p r2)
      F = 16 rows of the 16 pixels p past r6 (two aligned loads and a lane
      shift); r6 moves 32 bytes on, so a row of columns gathers column
      after column.  Rows 16-63 x 32 and 32-63 x 0 are its scratch.
  jv_rows(d r0, s r1, dpitch r2, spitch r3, cols | rows << 16 r4, sstep r5)
      rows rows of cols columns, in order, each column loaded then stored
      (a negative pitch goes up); s moves sstep a column: 32 (a copy) or
      0 (FILL: s is one row of the colour).
  jv_mix16(d r0, a r1, b r2, dpitch | apitch << 16 r3, bpitch r4,
           wb | avg << 16 r5, k (the stack's first word))
      (a and b at any phase: jv_gather16, then moved to A or B)
      one 16x16 block: per channel (a * (32 - wb) + b * wb [- 1] + 16)
      >> 5.  FADE: wb = max(1, (weight + 4) >> 3) (32 at 256: b exactly),
      jmix_fade_px.  AVG: wb = 16 and the - 1, which is (a + b) >> 1
      exactly (AVG2), with no rounding ties.  k: 0, or (FADE only, the
      raw FADE's J_FADE_SHADE_B) 16 factors 256 - alpha, 32-byte aligned:
      each of b's channels c becomes (c * k[lane]) >> 8 first, inside the
      channel's mix (jmix_shade_px then jmix_fade_px, one pass: the
      fused form of jobs.md section 10's "raw FADE"); 256 keeps c.
  jv_shade16(p r0, pitch r1, k r2)
      one 16x16 block in place: per channel (c * k[lane]) >> 8 with k the
      16 halfwords at r2 (32-byte aligned), k = 256 - alpha, which is
      jmix_shade_px.  The >> 8 is the product's high byte, read as an H8
      operand: exact for 0 <= c * k < 65536.  k = 256 leaves c as it is.

Usage: bcmfw/video/gen_jobs.py [--check] [--mutate NAME --out DIR]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'h264'))
from vpu_asm import H8, H16, V16, Reg, alu, bitplanes, mem, out_paths, write_text  # noqa: E402

OUT_S = os.path.join(HERE, 'jobs_kernels.S')

# VRF (16-bit rows of 16 lanes; a 16-bit row's low bytes are H8 columns
# x..x+15 of the row, its high bytes x+16..x+31):
#   F  rows 0-15 x 0: the result (stored: block 0 rows only)
#   U  rows 0-15 x 32: a 16-bit channel or temporary
#   A  rows 16-31 x 0, B rows 16-31 x 32: the two inputs as loaded
#   8-bit temporaries in rows 32-47; ACC rows 48-63 x 0, TMP x 32
F, U = H16(0, 0), H16(0, 32)
A, B = H16(16, 0), H16(16, 32)
LOA, HIA, LOB, HIB = H8(16, 0), H8(16, 16), H8(16, 32), H8(16, 48)
RA, RB, GLA, GLB = H8(32, 0), H8(32, 16), H8(32, 32), H8(32, 48)
ACC, TMP = H16(48, 0), H16(48, 32)
ACC_HI = H8(48, 16)             # ACC's high bytes: ACC >> 8
B16_ = H16(32, 0)               # MIX: b's G or B as 16 bits (over RA/RB,
                                # read before it is written)
LOF, HIF = H8(0, 0), H8(0, 16)  # KEY: s's low and high bytes, s in F
# jv_gather16's scratch (free in every kernel until it gathers; A, rows
# 16-31 x 0, holds jv_mix16's a while it gathers b): two aligned 32-byte
# columns of a source, pixels 0-15 in rows 32-47 x 0 and 16-31 in rows
# 48-63 x 0; their transpose in rows 16-47 x 32; the shifted rows in
# 32-47 x 0
S0, S1 = H16(32, 0), H16(48, 0)


def st(o):
    return o.stepped()


class K:
    """One kernel's text; tail: out-of-line blocks, after its return"""
    def __init__(self):
        self.out = []
        self.tail = []

    def s(self, text):
        self.out.append(text)

    def done(self):
        return self.out + self.tail

    def phase(self, lb):
        """r2 = s's (r6's) phase in pixels, 0 when 32-byte aligned; if not,
        r6 aligned down and the flags set for jv_gather16 (jv_phase16)"""
        self.s('mov r2, r6')
        self.s('bmask r2, #5')
        self.s('cmp r2, #0')
        self.s('beq %s_p0' % lb)
        self.s('bl jv_phase16')
        self.s('%s_p0:' % lb)

    def fetch(self, lb):
        """F = 16 rows of the column at r6 (stride r3), r6 moved 32 B on:
        one load in phase (r2 = 0), else jv_gather16 (out of line)"""
        self.s('cmp r2, #0')
        self.s('bne %s_sh' % lb)
        self.load(F, 6, 3)
        self.s('add r6, #32')
        self.s('%s_f:' % lb)
        self.tail += ['%s_sh:' % lb, 'bl jv_gather16', 'b %s_f' % lb]

    def mix_in(self, t, lb):
        """t = 16 rows of the column at r6 (stride r3), at any phase: one
        load, or jv_phase16 and jv_gather16 into F, then F moved to t
        (out of line; r2 clobbered)"""
        self.s('mov r2, r6')
        self.s('bmask r2, #5')
        self.s('cmp r2, #0')
        self.s('bne %s_sh' % lb)
        self.load(t, 6, 3)
        self.s('%s_f:' % lb)
        self.tail += ['%s_sh:' % lb, 'bl jv_phase16', 'bl jv_gather16',
                      alu('vadd', st(t), st(F), 0, rep=16), 'b %s_f' % lb]

    def v(self, op, d, a, b):
        self.out.append(alu(op, st(d), st(a), b if not hasattr(b, 'kind') else st(b),
                            rep=16))

    def load(self, d, base, stride):
        self.s(mem('vld16', st(d), base, 0, stride, 16))

    def store(self, base, pitch, entries=False):
        """F's rows 0-15 to r(base), r(base) + pitch, ...: single stores,
        the base moved a row at a time (r(base) is left past the block).
        entries: global labels at rows 8 and 12 (jv_store16_8, _12: rows
        8-15 or 12-15 only, for jdec's put, gen_jdec.py jv_put)"""
        for r in range(16):
            if MUTATE == 'store' and r == 15:
                break
            if entries and r in (8, 12):
                self.s('.global jv_store16_%d' % r)
                self.s('jv_store16_%d:' % r)
            self.s(mem('vst16', H16(r, 0), base, 0))
            self.s('add r%d, r%d' % (base, pitch))

    def g16(self, d, r8, hi, lo5, m8):
        """d = (hi - R * 8) * 8 + (lo >> 5): a pixel's G as 16 bits, from
        its R (8-bit), high byte and lo >> 5; r(m8) holds -8"""
        self.v('vmul', d, r8, Reg(m8))
        self.v('vadd', d, d, hi)
        self.v('vmul', d, d, 8)
        self.v('vadd', d, d, lo5)

    def b16(self, d, lo5, lo, m32):
        """d = lo - (lo >> 5) * 32: a pixel's B as 16 bits; r(m32) = -32"""
        self.v('vmul', d, lo5, Reg(m32))
        self.v('vadd', d, d, lo)


def copy16():
    """BLIT and SCROLL: cols 16-pixel columns of one 16-row group, each
    column's 16 rows loaded before any is stored (jv_store16 stores them);
    with a key (r5 not 0), jv_key16's.  s at any phase: jv_fetch16"""
    k = K()
    k.s('cmp r5, #0')
    k.s('bne jv_key16')
    k.s('push r6, lr')
    k.s('mov r6, r1')                           # s: a base, never r7
    k.s('mov r1, r2')                           # the tail's pitch: dpitch
    k.phase('jv_group16')                       # r2: s's phase
    k.s('jv_group16_col:')
    k.fetch('jv_group16')                       # F: 16 rows of s's column
    k.s('mov r5, r0')
    k.s('bl jv_store16')
    k.s('mov r0, r5')
    k.s('add r0, #32')
    k.s('add r4, #-1')
    k.s('cmp r4, #0')
    k.s('bne jv_group16_col')
    k.s('pop r6, pc')
    return k.done()


def rows():
    """Row by row: cols 16-pixel columns of each of rows rows, a column
    loaded then stored (vld16 and vst16 of row 0, session 4's forms); the
    rows in order, each read before it is written.  s moves by sstep a
    column: 32 (a copy) or 0 (FILL: one row of the colour)"""
    k = K()
    k.s('push r6-r9')
    k.s('mov r7, r4')
    k.s('lsr r7, #16')                          # rows (a counter, not a base)
    k.s('bmask r4, #16')                        # cols
    k.s('jv_rows_row:')
    k.s('mov r6, r0')
    k.s('mov r8, r1')
    k.s('mov r9, r4')
    k.s('jv_rows_col:')
    k.s(mem('vld16', F, 1, 0))
    k.s(mem('vst16', F, 0, 0))
    k.s('add r0, #32')
    k.s('add r1, r5' if MUTATE != 'rows' else 'add r1, #32')
    k.s('add r9, #-1')
    k.s('cmp r9, #0')
    k.s('bne jv_rows_col')
    k.s('mov r0, r6')
    k.s('add r0, r2')
    k.s('mov r1, r8')
    if MUTATE != 'rowstep':
        k.s('add r1, r3')
    k.s('add r7, #-1')
    k.s('cmp r7, #0')
    k.s('bne jv_rows_row')
    k.s('pop r6-r9')
    k.s('rts')
    return k.out


def key16():
    """BLIT with J_BLIT_KEY: cols 16-pixel columns of one 16-row group,
    d = s except where s equals the key (d kept), without flags: per pixel
    t = |hi - kh| + |lo - kl| from its bytes (0 exactly when s is the key),
    m = (t - 1) >> 15 arithmetic (-1 when it is, else 0: t <= 510), then
    d' = s + (s - d) * m in 16-bit lanes (wrapping: exactly d or s).  s is
    fetched into F (any phase: jv_fetch16), d loaded into B"""
    k = K()
    k.s('push r6-r11, lr')
    k.s('mov r8, r0')                           # d's column
    k.s('mov r10, r4')                          # columns left
    k.s('mov r6, r1')                           # s's column: a base
    k.s('mov r1, r2')                           # dpitch: d's stride, the tail's
    k.s('mov r11, r5')
    k.s('lsr r11, #16')                         # the key colour
    k.phase('jv_key16')                         # r2: s's phase
    k.s('mov r4, r11')
    k.s('lsr r4, #8')                           # kh
    k.s('mov r5, r11')
    k.s('bmask r5, #8')                         # kl
    k.s('jv_key16_col:')
    k.fetch('jv_key16')                         # F: s
    k.s('mov r0, r8')
    k.load(B, 0, 1)                             # d as it is
    k.v('vdist', U, HIF, Reg(4))                # |hi - kh|
    k.v('vdist', ACC, LOF, Reg(5))              # |lo - kl|
    k.v('vadd', U, U, ACC)                      # t: 0..510, 0 iff s == key
    k.v('vadd', U, U, -1)
    k.v('vasr', U, U, 15)                       # m
    k.v('vsub', ACC, F, B)                      # s - d
    if MUTATE != 'key':
        k.v('vmul', ACC, ACC, U)                # (s - d) * m
    k.v('vadd', F, F, ACC)                      # s + (s - d) * m
    k.s('bl jv_store16')                        # F to r0 (= r8), dpitch
    k.s('add r8, #32')
    k.s('add r10, #-1')
    k.s('cmp r10, #0')
    k.s('bne jv_key16_col')
    k.s('pop r6-r11, pc')
    return k.done()


KF = H16(48, 0)                 # MIX: b's factors, one row (ACC's first)


def mix16():
    """FADE and AVG: one 16x16 block, per channel (a * wa + b * wb + 16) >> 5
    after a - 1 for AVG.  FADE: wa = 32 - w, wb = w, jmix_fade_px.  AVG:
    wa = wb = 16, and (16 (a + b) - 1 + 16) >> 5 = (a + b) >> 1, AVG2 per
    channel (the sum before the shift is never a multiple of 32 plus 16,
    so vmulhr's rounding has no ties; at a = b = 0 it rounds -1/32 to 0).
    The - 1 is a branch around one vadd #imm (ADIR), not a new form."""
    k = K()
    k.s('push r6-r9, lr')
    k.s('mov r8, r3')
    k.s('bmask r8, #16')                        # dpitch, for the tail
    k.s('lsr r3, #16')                          # apitch: a's stride
    k.s('mov r9, r5')                           # wb | avg << 16
    k.s('mov r7, r2')                           # b (a scalar: never a base)
    k.s('mov r6, r1')                           # a
    k.mix_in(A, 'jv_mix16_a')                   # A: a, at any phase
    k.s('mov r6, r7')
    k.s('mov r3, r4')                           # b, bpitch
    k.mix_in(B, 'jv_mix16_b')                   # B: b, at any phase
    k.s('mov r5, r9')
    k.s('mov r3, r5')
    k.s('lsr r3, #16')                          # AVG: the - 1
    k.s('bmask r5, #16')
    k.s('mov r2, r5')                           # wb
    k.s('mov r1, #32')
    k.s('sub r1, r5')                           # wa = 32 - wb
    k.s('mov r4, #-8')
    k.s('mov r5, #-32')
    k.s('ld r9, (sp+20)')                       # k: b's factors, or 0
    k.v('vlsr', RA, HIA, 3)
    k.v('vlsr', RB, HIB, 3)
    k.v('vlsr', GLA, LOA, 5)
    k.v('vlsr', GLB, LOB, 5)

    def mix(ca, cb, label, widen=False):
        """TMP = (ca * wa + cb' * wb [- 1] + 16) >> 5, cb' = cb, or with
        factors (r9 not 0: FADE only) (cb * k[lane]) >> 8, the product's
        high byte, as jv_shade16 does it: jmix_shade_px's channel.  The 16
        factors are loaded into ACC's first row (REP 1, from r6: a vector
        base is r0-r6) and read unstepped, jv_shade16's form; cb * k goes
        to TMP, and the two products swap places: ACC = cb' * wb, TMP =
        ca * wa.  widen: cb is 8-bit (R), made 16 first (the factors are
        a 16-bit vector B, as in jv_shade16)"""
        k.s('cmp r9, #0')
        k.s('beq %s_n' % label)
        k.s('mov r6, r9')                       # a base: r0-r6
        if MUTATE == 'klane':
            k.s('add r6, #32')
        k.s(mem('vld16', KF, 6, 0))
        c16 = cb
        if widen:
            k.s('mov r6, #1')
            k.v('vmul', TMP, cb, Reg(6))
            c16 = TMP
        if MUTATE != 'shade' or label != 'jv_mix16_b':
            k.out.append(alu('vmul', st(TMP), st(c16), KF, rep=16))
            k.v('vmul', ACC, H8(48, 48), Reg(2))    # TMP's high bytes: cb'
        else:
            k.v('vmul', ACC, cb, Reg(2))
        k.v('vmul', TMP, ca, Reg(1))
        k.s('b %s_j' % label)
        k.s('%s_n:' % label)
        k.v('vmul', ACC, ca, Reg(1))
        k.v('vmul', TMP, cb, Reg(2))
        k.s('%s_j:' % label)
        k.v('vadd', ACC, ACC, TMP)
        k.s('cmp r3, #0')
        k.s('beq ' + label)
        k.v('vadd', ACC, ACC, -1 if MUTATE != 'avg' else 0)
        k.s(label + ':')
        k.v('vmulhr', TMP, ACC, 2048)           # (acc + 16) >> 5, acc >= -1

    mix(RA, RB, 'jv_mix16_r', True)             # R
    k.s('mov r6, #2048')
    k.v('vmul', F, TMP, Reg(6))
    k.g16(U, RA, HIA, GLA, 4)                   # G
    k.g16(B16_, RB, HIB, GLB, 4)
    mix(U, B16_, 'jv_mix16_g')
    k.s('mov r6, #32')
    k.v('vmul', TMP, TMP, Reg(6))
    k.v('vadd', F, F, TMP)
    k.b16(U, GLA, LOA, 5)                       # B
    k.b16(B16_, GLB, LOB, 5)
    mix(U, B16_, 'jv_mix16_b')
    k.v('vadd', F, F, TMP)
    k.s('mov r1, r8')
    k.s('bl jv_store16')
    k.s('pop r6-r9, pc')
    return k.done()


KV = H16(48, 32)                # SHADE's factors: one row, read every REP


def shade16():
    k = K()
    k.s('push r6-r7')
    k.load(A, 0, 1)
    k.s(mem('vld16', KV, 2, 0))                 # the 16 factors, one row
    k.v('vlsr', RA, HIA, 3)
    k.v('vlsr', GLA, LOA, 5)
    k.s('mov r4, #-8')
    k.s('mov r5, #-32')
    k.s('mov r6, #1')
    kv = KV                     # not stepped: the same row every REP

    def scale(c16):
        """ACC = c * k; its high bytes (ACC_HI) are (c * k) >> 8"""
        k.out.append(alu('vmul', st(ACC), st(c16), kv, rep=16))

    k.v('vmul', U, RA, Reg(6))                  # R as 16 bits
    scale(U)
    k.s('mov r6, #2048')
    k.v('vmul', F, ACC_HI, Reg(6))              # R' << 11
    k.g16(U, RA, HIA, GLA, 4)                   # G
    scale(U)
    k.s('mov r6, #32')
    k.v('vmul', U, ACC_HI, Reg(6))              # G' << 5
    k.v('vadd', F, F, U)
    k.b16(U, GLA, LOA, 5)                       # B
    scale(U)
    k.v('vadd', F, F, ACC_HI)                   # + B'
    k.s('pop r6-r7')
    k.s('b jv_store16')                         # pitch already in r1
    return k.out


def phase16():
    """jv_phase16: s (r6) at the byte phase r2 (not 0) of its 32-byte
    line: r6 aligned down, r2 = 64 p (p the phase in pixels, 1-15: the +r
    of a y offset of p rows, jv_gather16's shift)"""
    k = K()
    k.s('sub r6, r2')
    k.s('lsl r2, #5')
    k.s('rts')
    return k.out


def gather16():
    """jv_gather16: F = 16 rows of 16 pixels from p pixels past r6 (32-byte
    aligned), r3 bytes apart, with jv_phase16's r2 = 64 p; r6 moves 32 B
    on.  Two aligned loads (S0: pixels 0-15 of each row, rows 32-47 x 0;
    S1: pixels 16-31, rows 48-63 x 0), then the lane shift done as a row
    shift: T (rows 16-47 x 32) gets the 32 pixel columns as rows (V16 B
    operands, stepping a column: the transpose the 2-D VRF gives for
    free), T's rows p .. p + 15 move to rows 32-47 x 0 (an H16 A at +r2:
    a y offset only), and F reads them back down the V16 columns.  No
    operand takes a lane offset from a register: silicon aligns an H16 A
    operand's +r x (quirks #30) and a horizontal B's (#17b), so only the
    y offset (REPA, R48E) and the stepping V16 B (REPA) are relied on"""
    k = K()
    if MUTATE == 'aplusr':
        return gather_aplusr()
    k.load(S0, 6, 3)
    k.s('add r6, #32')
    k.load(S1, 6, 3)
    ops = gather_ops()
    if MUTATE == 'trans':
        ops[1] = ops[1].replace('bc0', 'b80')
    k.out += ops
    if MUTATE == 'shift':
        k.out[-2:-2] = ['add r2, #64']
        k.out.append('add r2, #-64')
    k.s('rts')
    return k.out


def gather_aplusr():
    """--mutate aplusr (tests only): the lane shift before 2026-10-25, two
    H16 A operands at +r x = p and 16 + p, the second on lanes 16 - p ..
    15, from the columns side by side in rows 32-47.  Right only where an
    A operand's +r x is taken as given; silicon aligns it (quirks #30), so
    the start-up check must fail under every variant that fits silicon.
    r5 and the stack are its scratch (jv_group16's path, the check's)"""
    k = K()
    k.s('push r6-r7')
    k.load(S0, 6, 3)
    k.s('add r6, #32')
    k.load(H16(32, 32), 6, 3)
    k.s('mov r5, r2')
    k.s('lsr r5, #6')                           # p
    k.s('mov r6, #-1')
    k.s('lsl r6, #16')
    k.s('lsr r6, r5')
    k.s(bitplanes(Reg(6)))                      # lanes 16 - p .. 15
    k.out.append(alu('vadd', st(F), st(S0).r(5), 0, rep=16))
    k.s('add r5, #16')
    k.out.append(alu('vadd', st(F), st(S0).r(5), 0, rep=16, pred='ifnz'))
    k.s('pop r6-r7')
    k.s('add r6, #32')
    k.s('rts')
    return k.out


def gather_ops():
    """The lane shift's four ALU ops (bench row GTHR runs the same text:
    gen_bench.py gather_ops)"""
    return [alu('vmov', H16(16, 32, True), None, V16(32, 0, True), rep=16),
            alu('vmov', H16(32, 32, True), None, V16(48, 0, True), rep=16),
            alu('vadd', H16(32, 0, True), H16(16, 32, True).r(2), 0, rep=16),
            alu('vmov', H16(0, 0, True), None, V16(32, 0, True), rep=16)]


def store16():
    """The kernels' common tail: F's rows to r0, r0 + r1, ..."""
    k = K()
    k.store(0, 1, entries=True)
    k.s('rts')
    return k.out


KERNELS = (('jv_store16', store16, 'the 16 rows of F to r0, r1 bytes apart'),
           ('jv_group16', copy16, 'BLIT, SCROLL: a 16-row group of columns copied'),
           ('jv_rows', rows, 'BLIT, SCROLL, FILL, ANIM: rows of columns, in order'),
           ('jv_key16', key16, 'BLIT with a key colour: a 16-row group of columns'),
           ('jv_mix16', mix16, 'FADE and AVG: one 16x16 block'),
           ('jv_shade16', shade16, 'SHADE: one 16x16 block darkened per column'),
           ('jv_phase16', phase16, 'a source out of phase: aligned down, p, the flags'),
           ('jv_gather16', gather16, 'a 16-row column from p pixels past an aligned base'))


def gen_s():
    lines = ['/* GENERATED by bcmfw/video/gen_jobs.py: do not edit.  The display',
             ' * jobs\' vector kernels (jobs_bcm.c; jobs.md "The vector unit"). */',
             '', '    .syntax vc2', '    .text', '']
    for name, fn, what in KERNELS:
        lines += ['/* %s */' % what, '    .global ' + name, name + ':']
        lines += ['    ' + l if not l.endswith(':') else l for l in fn()] + ['']
    return '\n'.join(lines)


MUTATE = None          # --mutate NAME: a deliberately broken kernel
MUTATIONS = {'avg': 'jv_mix16 drops AVG\'s - 1 (AVG rounds half up)',
             'store': 'jv_store16 drops row 15 (FADE, AVG, SHADE, BLIT groups)',
             'rows': 'jv_rows moves s by 32 always (FILL repeats past its colour row)',
             'rowstep': 'jv_rows reads every row from the first source row',
             'key': 'jv_key16 drops its * m (a keyed BLIT copies the key too)',
             'shift': 'jv_gather16 shifts one pixel too far (p + 1)',
             'trans': 'jv_gather16 transposes pixels 16-31 from S0 (pixels 0-15 again)',
             'aplusr': 'jv_gather16 as it was: H16 A operands at +r lane offsets '
                       '(wrong on silicon, quirks #30)',
             'shade': 'jv_mix16 leaves b\'s blue unshaded in a shaded block (k 256)',
             'klane': 'jv_mix16 takes its factors 16 lanes on (the next block\'s)'}


def main(argv):
    global MUTATE
    if '--mutate' in argv:
        MUTATE = argv[argv.index('--mutate') + 1]
        assert MUTATE in MUTATIONS, 'mutations: ' + ', '.join(MUTATIONS)
    text = prokbox_header.convert(gen_s(), '.S')[0]
    if '--check' in argv:
        if not os.path.exists(OUT_S) or open(OUT_S).read() != text:
            print('gen_jobs: stale: jobs_kernels.S')
            return 1
        print('gen_jobs: up to date')
        return 0
    out_s, = out_paths(argv, [OUT_S], MUTATE, 'gen_jobs')
    write_text(out_s, text)
    if MUTATE:
        print('gen_jobs: MUTATED (%s): %s; in %s' % (MUTATE, MUTATIONS[MUTATE], out_s))
    print('gen_jobs: %s' % ', '.join('%s %d instructions' % (n, len(fn()))
                                     for n, fn, _ in KERNELS))
    return 0


# the ProkBox banner for the generated file (scripts/prokbox_header.py)
sys.path.insert(0, os.path.join(HERE, '..', '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
