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
"""Vector unit (VPU) model for the VideoCore II simulator (vc2sim).

The model follows VideoCore IV's VPU (16 lanes, a 64x64-byte vector
register file, accumulators, per-lane flags, predication, REP, a
scalar-result unit) with VideoCore II's own operand encoding, recovered
from Apple's kernels (reference material only) and pinned by the
experiments in vc2vpu_tests.py.  Every uncertain decoding choice is a
key of vc2sim.HYP (prefix 'v_'), so --controls can swap it out and show
which kernels break.  docs/research/bcm2722/sim.md ("Vector unit model")
has the evidence.

Operand model (all coordinates are (y, x) byte positions in the VRF):

  element i, byte k of an H vector at (y, x): (y, x + i + 16k)
  element i, byte k of a V vector at (y, x):  (y + i, x + 16k)
  (VC IV's layout: a 16-bit vector keeps its high bytes 16 columns on.)

  D  hw2 = d8 rrr ttttt.  d8 = y5 y4 x5 x4 c3..c0, c = y (H) or x (V).
     Direction and width come from the op's .w suffix (op9[2:1]):
     0 H8, 1 H16, 2 V8, 3 V16.  A 16-bit D in x block 1 (d10) discards.
     t5 does not move D: it describes A.
  t5 bit 0: A x += 32, bit 1: A y += 16, bit 2: A y += 32,
     bit 3: A x += 16 (assumed), bit 4: see A.
  A  80-bit: 11 bits, A11[10] = 1: H8 at (dy = A[9:6], dx = A[4:0]),
     steps one row per REP iff t5 bit 4.  A11[10:8] = 0: V8 column
     A[4:0] if t5 bit 4, steps iff A[5].  A11[10:8] = 2/3: 16-bit;
     t5 bit 4: V column A[3:0], steps iff A[5]; else H row
     A[3:2]*4 + A[7:6], steps iff A11[8] (mode 3).
     48-bit: 5 bits, the row (H) or column (V) index in D's
     direction and width.
  B  vector: d8-style coordinate hw3[9:2]; hw3[0] = V, hw4[11] (.t) =
     16 bits; '++' (hw4[10]) steps per REP.  Scalar: rN + s12.  Imm16.
  +rN on any operand adds rN to the operand's byte offset y*64 + x.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vc2dis as D  # noqa: E402

VRF_ROWS = 64
VRF_COLS = 64
MASK = 0xffffffff

# Hypotheses (merged into vc2sim.HYP; booleans flip, others cycle through
# HYP_ALTERNATIVES under --controls).
HYP_DEFAULTS = {
    'v_d_dir_from_w': True,      # D direction/width from op9[2:1] (else from t5 bit 4)
    'v_d16_block1_discard': True,  # 16-bit D in x block 1 (d10) writes nothing
    'v_t5_block_bits': True,     # t5[2:0] add A's block offset (x+32, y+16, y+32)
    'v_a_mode4_step_t5': True,   # A mode 4-7 steps per REP iff t5 bit 4 (else never)
    'v_a_mode2_h_rows': 'split',  # mode 2/3 H row = A[3:2]*4 + A[7:6] ('split')
                                 # or A[3:0] ('low4') or A[7:0] ('byte')
    'v_a_mode4_dydx': True,      # mode 4-7: (dy = A[9:6], dx = A[4:0]) (else swapped)
    'v_b_v_bit0': True,          # B direction: hw3[0] = V (else hw4[11])
    'v_u8_unsigned': True,       # 8-bit elements read unsigned (else signed)
    # vmulhr/vmulhrs/vmulhru: B operand unsigned 16-bit.  Device session
    # 26 (the luma loop filter's p1, fitted; Apple's 22,163 cases pass and
    # only that kernel changes): 'not_vmulhr', vmulhr itself reads B
    # signed (#0x8000 = -1/2).  True is the old reading.
    'v_mulhr_b_unsigned': 'not_vmulhr',
    'v_sat_by_dest': True,       # saturating ops clamp to D's width (8-bit: 0..255)
    'v_pred_flags_only_enabled': True,  # setf under predication updates enabled lanes only
    'v_inc_steps_d': True,       # hw4[0] steps D per REP (else D never steps)
    'v_ld_split_lines': True,    # vldp/vldq: first/second 16-byte line of a misaligned load
    'v_r_2d': True,              # +rN adds y and x fieldwise (x wraps, no carry into y)
    'v_mem48_off_unit': 8,       # 48-bit vector memory offset unit in bytes (or 1)
    # Device session 4 (2026-10-11, bench check CSUM): the 48-bit form's
    # imm6 is signed: 'vmov.1 d08, a00, #52' wrote -12 (0xfff4) on silicon.
    'v_imm6_signed': True,
    # Device sessions 4-5: a 48-bit vst16 does not lay its fields out like
    # a load.  Session 5's store-locating check settled it ('swap', isa.md
    # "Vector stores on silicon"): the byte a load reads as D (hw2[15:8])
    # is the address offset in 32-byte units, the load's offset field
    # hw3[15:9] holds the source row (hw3[15:11]; hw3[10:9] unseen), and
    # hw2[4:0] are the source's t5 block bits.  'model' is the old corpus
    # reading (row d8 at base + offset); 'drop' is session 4's profile
    # (nothing lands unless hw2 = 0).  vst8 keeps 'model' under 'swap'
    # (its layout is not device-checked).
    'v_st48_nonzero': 'swap',
    # Device session 5 (MHRN): vmulhrs into an 8-bit D keeps the low 8 bits
    # of the rounded product (-80 -> 0xb0, 335 -> 0x4f, 256 -> 0x00); it does
    # not saturate.  True is the old reading (clamp to D's width).
    'v_mulhrs_sat': False,
    # Device session 6 (MHRN lane 8): vmulhrs #2048 on -16 (exactly -0.5)
    # gave -1, where adding 0x8000 and flooring gives 0; +0.5 (16) gave 1.
    # So a tie rounds away from zero: the product rounds as a magnitude
    # and takes its sign back ('away').  'up' is the old reading (+0x8000,
    # floor).  Device-seen on vmulhrs only; vmulhr and vmulhru are assumed
    # to share the multiplier's rounding (hypothesis, bench check RNDT).
    'v_mulhr_round': 'away',
    # Device sessions 13-14 (dev14/dev15 self-test): the colour kernel's
    # vst16 (r7)+=r4 rep16 changed low SRAM from 0x004 on with its chroma
    # (80808080), and its reads back came from there; no other kernel
    # uses base 7.  Base field 7 reads as "no register" (0), as the
    # stride field's 7 does.  'r7' is the old reading.
    'v_mem_base7': 'zero',
    # Device session 18 (dev19's self-test dump): our transform's
    # 'vmov.1 d0r, a000, b00.t+r3' (16-bit B, +r3 = 4, 8, 12 as a lane
    # offset) behaved as if +r3 were ignored: all four coefficient rows
    # came out as row 0, reproducing the chip's 16 bytes exactly.  But
    # Apple's own kernels use 16-bit B operands with +r and are right only
    # with it applied (invzigzag, vc_inverseinteger, intra 4x4 in the
    # self-test), so the true rule is narrower and unknown.  'applied'
    # stays the default; 'ignored' reproduces the S18 dump.  gen_vpu
    # refuses '.t+rN' until the rule is known.
    'v_b16_reg': 'applied',
    # Under test (device session 21, dev22's cleared-VRF dump): the
    # transform's 'vld16 d0r, (r2+8r)' (no stride register) loaded rows
    # 1-3 from (r2): the offset was ignored.  False/'applied' are the
    # modelled readings; the alternatives drop the offset of a vector
    # memory op without a stride, and the +r of a B operand without ++.
    'v_mem_off_nostride': 'applied',
    'v_b_reg_nostep': 'applied',
    'v_reg_nostep_any': 'applied',      # any operand's +r without a step
    # Device session 22 (dev23's per-kernel dumps): a plain vector load's
    # address is aligned down to the access's size (vld16: 32 bytes; the
    # transform's (r2+8)/(r2+16)/(r2+24) all read (r2)), and a horizontal
    # B operand's +r has its x part aligned down to 16 bytes (MC luma's
    # 6-tap taps, reproduced byte for byte).  Apple's code passes its
    # 22,163 self-test cases under both.  0 is the old reading.
    'v_ld_align': 'width',
    # Bench row ALGN (sitting 2, 2026-10-25, VLS4 [silicon]): a REP's rows
    # each round down on their own (row 1 of a vld16 REP 2 from an aligned
    # base at a stride of 40 read the row at 32, not at 40).  'base' is
    # the model before 2026-10-26 (the base rounds once, the stride is
    # added to the rounded base).
    'v_ld_rep_round': 'row',
    # Bench row ALGN (sitting 2, VS62/VS64/VS68, VS84/VS88 [silicon]): a
    # plain vector store off its size lands at the address rounded down,
    # as a load reads: vst16 at +2/+4/+8 and vst8 at +4/+8 all stored the
    # whole row at +0 (whether a vst8 rounds to 16 or to 32 is not told
    # apart by these offsets; 'width' rounds to its own size, as loads
    # do).  0 is the model before 2026-10-26 (the exact address).
    'v_st_align': 'width',
    'v_b_reg_x_align': 16,
    # Device session 22 (the loop filters' dumps, both fitted byte for
    # byte): a vstr whose enabled lanes cross a 16-byte line writes only
    # those in the line of its address.  Apple's 22,163 cases pass.  'all'
    # is the old reading.
    'v_vstr_line': 'first',
    # Device session 22 (intra 16x16 plane mode, fitted: the plane was
    # a - 7c off): a scalar B operand naming r7 reads as 0, as a vector
    # memory base of 7 does.  'r7' is the old reading.
    'v_sreg7': 'zero',
    # P19 (2026-10-24, apple-RE + sim): the accumulator is 32 bits per
    # lane, hi:lo.  A plain 'acc' op adds its result to the high half and
    # writes A >> 16; an 'x15' op (hw5[15]) adds at bit 0 (zero-extended
    # low 16 bits of its result, or the signed result with 'acc' too) and
    # writes A's low 16 bits, carries passing into the high half.  The
    # jhoz/jvert 32-bit 6-tap sums, expand_range_16s' 16.16 scale, a
    # 16.16 ramp at vmcs 0x153f8 and Apple's VRF context save/restore at
    # vmcs 0x1d448/0x1d4a8 all need exactly this.  'none' is the old
    # model (x15 raises).
    'v_x15': 'lo16',
    # P19: the 48-bit ALU form has one +r field (hw2[7:5]).  In every
    # non-'x' 48-bit op that names a register, t5 bit 3 or B's hw3[1] is
    # set, and never otherwise: they say which source gets the +r (bit 3:
    # A, hw3[1]: B); D takes none.  jhoz/jvert's pair sums slide over the
    # 6-tap rows by +r0 this way.  'd' is the old reading (D gets +r,
    # t5 bit 3 = A x += 16, hw3[1] raises).
    'v_r48': 'enables',
    # P19: vclip (op 37) clamps A to 0..B (H.264 Clip1 of the centre
    # half-pel j; Clip3(0, 51, qp) in vc_calc_alpha_beta).  'min' is
    # min(A, B) only.
    'v_clip_lo': 'zero',
    # P19: a 16-bit horizontal D's +r has its x part aligned down to 16
    # bytes, as a horizontal B operand's is on silicon (quirks #17b).
    # vc_interp16xN_jvert writes the vertical half-pel to d25.01+r0 with
    # r0's x = 2 or 3 (the column of h or m), and its callers read it
    # back at x 32; taken as given it also overwrites the j row's high
    # bytes.  0 is the old reading.  [apple-RE; one site]
    'v_d16_reg_x_align': 16,
    # P19: vmulhd/vmulhdsu/vmulhduu (op 18/19/51) floor the high half (an
    # exact split P = hi * 65536 + lo with the x15 low half).  'away'
    # rounds like vmulhr instead.
    'v_mulhd_round': 'floor',
    # P19 silicon (2026-10-25, bench ASHF/ASHG, bench-20261025-1430): a
    # horizontal 16-bit A operand's +r does not take its x part as given.
    # The words fix three points: x 5 and 13 read x 0, x 29 read x 32
    # (ASHG lanes 3-7 are the second column's lanes 3-7 exactly).  Two
    # rules fit every logged word: 'dbl16' (x aligned down to 16 and
    # doubled: +r's x counts lanes, aligned to 16 lanes = 32 bytes, as a
    # horizontal B aligns to 16 bytes: quirks #17b, #30) and 'near32' (x
    # rounded to the nearest 32).  They differ at x 32-63 (bench ASHX).
    # 'given' is the old model; 'align16' and 'xzero' fit no word set.
    # A +r's y is applied as given on silicon (REPA, R48E).  Bench row
    # ASHX (x 37 and 53; bench-20261025-2257, sitting 2) settled it:
    # 'near32' (x 37 read x 32, x 53 read x 0, x 16 read 32, x 48 read 0);
    # 'dbl16' is out.  Strict mode refuses an x silicon has not read
    # (A16_X_MEASURED).  [silicon]
    'v_a16_reg_x': 'near32',
    # An 8-bit horizontal A's +r x: as given.  [silicon, indirect]: the
    # display-only image's IDCT start-up check passed on silicon
    # (J_VPU 5, 2026-10-25), and its jv_put reads H8 A operands at +r x
    # 8, 16 and 24; bench rows PUTV and ASHX check it directly.
    'v_a8_reg_x': 'given',
    # P19 silicon (2026-10-25, bench X15S and REPA): the accumulator does
    # not wrap.  X15S's lane 6 (hi -32768, then -1 at bit 0) read -32768
    # where a wrapping high half gives 32767; REPA's lane 0 (514 << 16
    # plus eight 2^30 products) read 32767 where a sum mod 2^32 gives
    # 514.  Three rules fit every logged word: 'sat32' (a 32-bit
    # accumulator saturating at every add), 'read16' (a wider accumulator
    # whose high half saturates to 16 bits when written to D) and 'hi16'
    # (the high half saturates at every add, the low half kept).  Bench
    # row ACSA tells them apart.  'none' is the old model (a wide high
    # half, D keeps its low 16 bits).  ACSA (bench-20261025-2257, sitting
    # 2) read 'sat32': a 32-bit accumulator saturating at every add, as
    # Apple's VPU context save (two 16-bit halves) suggested.  [silicon]
    'v_acc_sat': 'sat32',
    # P19 silicon (2026-10-25, bench PUTV, bench-20261025-2257, sitting
    # 2): a vertical D's +r does not take its y part as given.  PUTV's V8
    # D at +r = row 8, column 17 wrote its lane 0 into row 0 (the column
    # applied, the row dropped).  'align16' (y aligned down to 16, as a
    # horizontal D's x aligns: v_d16_reg_x_align) fits it and sitting 1's
    # J_VPU 5 (jv_idct4n's 16-point pass 1, a V16 D at +r y 16, passed
    # its start-up check on 4e57b3b9a1's display-only image); 'yzero'
    # fits PUTV alone; 'given' is the old model.  Strict mode refuses a
    # vertical D's +r y that is not a multiple of 16.  [silicon: y 8 and
    # 16]
    'v_vd_reg_y': 'align16',
}
HYP_ALTERNATIVES = {
    'v_a_mode2_h_rows': ['split', 'low4', 'byte'],
    'v_mem48_off_unit': [8, 1, 2, 16],
    'v_st48_nonzero': ['swap', 'model', 'drop'],
    'v_mulhr_round': ['away', 'up'],
    'v_mem_base7': ['zero', 'r7'],
    'v_b16_reg': ['ignored', 'applied'],
    'v_mem_off_nostride': ['applied', 'ignored'],
    'v_b_reg_nostep': ['applied', 'ignored'],
    'v_reg_nostep_any': ['applied', 'ignored'],
    'v_ld_align': [0, 16, 32, 'width'],
    'v_ld_rep_round': ['row', 'base'],
    'v_st_align': ['width', 0, 32],
    'v_b_reg_x_align': [0, 16],
    'v_vstr_line': ['all', 'first'],
    'v_sreg7': ['zero', 'r7'],
    'v_mulhr_b_unsigned': ['not_vmulhr', True, 'imm', False],
    'v_x15': ['lo16', 'lo_sext', 'acc'],
    'v_r48': ['enables', 'd'],
    'v_clip_lo': ['zero', 'min'],
    'v_d16_reg_x_align': [16, 0],
    'v_mulhd_round': ['floor', 'away'],
    'v_a16_reg_x': ['near32', 'dbl16', 'given', 'align16', 'xzero'],
    'v_a8_reg_x': ['given', 'align16'],
    'v_acc_sat': ['sat32', 'read16', 'hi16', 'none'],
    'v_vd_reg_y': ['align16', 'given', 'yzero'],
}
# The variants that reproduce every silicon word of their bench rows
# (bcmfw/tests/vpu_fit_test.py checks it): the kernels must be exact under
# each, and the start-up checks must pass under each
SILICON_FITS = {
    'v_a16_reg_x': ('near32',),
    'v_acc_sat': ('sat32',),
    'v_vd_reg_y': ('align16',),
}
# Alternatives the bench records alone leave open but other silicon
# evidence rules out (vpu_fit_test.py drops them from its fit)
SILICON_EXCLUDED = {
    ('v_vd_reg_y', 'yzero'): "sitting 1's J_VPU 5 (4e57b3b9a1): jv_idct4n's "
                             "16-point pass 1 writes a V16 D at +r y 16 and passed "
                             "its start-up check",
}


class _Sim:
    """vc2sim, imported on first use (vc2sim imports this module)."""
    def __getattr__(self, name):
        import vc2sim
        return getattr(vc2sim, name)


S = _Sim()


def H():
    return S.HYP


def _base(r, base):
    """A vector memory op's base register value (HYP v_mem_base7)."""
    return 0 if base == 7 and H()['v_mem_base7'] == 'zero' else r[base]


# --------------------------------------------------------------------------
# Register file.

class VRF:
    """64 x 64 bytes, plus per-lane accumulators and flags."""
    def __init__(self):
        self.b = bytearray(VRF_ROWS * VRF_COLS)
        self.acc = [0] * 16
        self.acclo = [0] * 16      # low half of the 32-bit accumulator (x15)
        self.fz = [False] * 16
        self.fn = [False] * 16
        self.fc = [False] * 16

    def addr(self, y, x):
        return ((y & 63) << 6) | (x & 63)

    def read(self, base, vert, width, signed=True):
        """16 elements of a vector at byte offset base (y*64 + x)."""
        y, x = base >> 6, base & 63
        b = self.b
        out = []
        for i in range(16):
            v = 0
            for k in range(width):
                if vert:
                    a = (((y + i) & 63) << 6) | ((x + 16 * k) & 63)
                else:
                    a = (y << 6) | ((x + i + 16 * k) & 63)
                v |= b[a] << (8 * k)
            if signed and (width > 1 or not H()['v_u8_unsigned']):
                if v >> (8 * width - 1):
                    v -= 1 << (8 * width)
            out.append(v)
        return out

    def write(self, base, vert, width, vals, lanes):
        y, x = base >> 6, base & 63
        b = self.b
        for i in lanes:
            v = vals[i]
            for k in range(width):
                if vert:
                    a = (((y + i) & 63) << 6) | ((x + 16 * k) & 63)
                else:
                    a = (y << 6) | ((x + i + 16 * k) & 63)
                b[a] = (v >> (8 * k)) & 0xff

    def row_bytes(self, y, x, n):
        return bytes(self.b[(y << 6) + x:(y << 6) + x + n])


# --------------------------------------------------------------------------
# Decoded operands.

class Opnd:
    """A VRF operand: base offset (y*64 + x), direction, width in bytes,
    +r register (or None), step per REP (in byte offset units, 0 = fixed)."""
    __slots__ = ('base', 'vert', 'width', 'reg', 'step', 'discard', 'isb', 'isd', 'isa')

    def __init__(self, y, x, vert, width, reg=None, step=False):
        self.base = ((y & 63) << 6) | (x & 63)
        self.vert, self.width, self.reg = vert, width, reg
        self.step = (1 if vert else 64) if step else 0
        self.discard = False
        self.isb = False
        self.isd = False
        self.isa = False

    def offset(self, r, k):
        o = self.base
        if self.step:
            o = _add2d(o, k * self.step)
        if self.reg is not None and not (self.step == 0 and
                                         S.HYP['v_reg_nostep_any'] == 'ignored'):
            rv = r[self.reg]
            if self.isb and not self.vert and S.HYP['v_b_reg_x_align']:
                rv = (rv & ~63) | (rv & 63 & ~(S.HYP['v_b_reg_x_align'] - 1))
            elif self.isd and not self.vert and self.width == 2 and \
                    S.HYP['v_d16_reg_x_align']:
                rv = (rv & ~63) | (rv & 63 & ~(S.HYP['v_d16_reg_x_align'] - 1))
            elif self.isa and not self.vert:
                rv = (rv & ~63) | a_reg_x(rv & 63, S.HYP['v_a16_reg_x' if self.width == 2
                                                         else 'v_a8_reg_x'])
            elif self.isd and self.vert and S.HYP['v_vd_reg_y'] != 'given':
                yy = (rv >> 6) & 63
                yy = yy & ~15 if S.HYP['v_vd_reg_y'] == 'align16' else 0
                rv = (rv & ~0xfc0) | (yy << 6)
            o = _add2d(o, rv) if S.HYP['v_r_2d'] else o + rv
        return o & 0xfff

    def __repr__(self):
        return '%s%d(%d,%d)%s%s' % ('V' if self.vert else 'H', 8 * self.width,
                                    self.base >> 6, self.base & 63,
                                    '+r%d' % self.reg if self.reg is not None else '',
                                    '++' if self.step else '')


def a_reg_x(x, rule):
    """The x part (0-63) a horizontal A operand's +r adds, under the
    rule (HYP v_a16_reg_x, v_a8_reg_x)"""
    if rule == 'dbl16':
        return ((x & ~15) * 2) & 63
    if rule == 'near32':
        return ((x + 16) & ~31) & 63
    if rule == 'align16':
        return x & ~15
    if rule == 'xzero':
        return 0
    return x


def _add2d(o, r):
    """Add a byte offset y*64 + x to o field by field (mod 64 each)."""
    return ((o + (r & ~63)) & 0xfc0) | ((o + r) & 63)


def d8_yx(d8, vert):
    """y, x of a d8 coordinate (y5 y4 x5 x4 c3..c0)."""
    y = (d8 >> 6) * 16
    x = ((d8 >> 4) & 3) * 16
    if vert:
        x += d8 & 15
    else:
        y += d8 & 15
    return y, x


def _reg3(v):
    return None if v == 7 else v


W_DIR = [(False, 1), (False, 2), (True, 1), (True, 2)]   # .w -> (vert, bytes)


def decode_d(hw2, w, step):
    d8, rd, t5 = hw2 >> 8, (hw2 >> 5) & 7, hw2 & 31
    if H()['v_d_dir_from_w']:
        vert, width = W_DIR[w]
    else:
        vert, width = bool(t5 & 16), 2 if w & 1 else 1
    y, x = d8_yx(d8, vert)
    o = Opnd(y, x, vert, width, _reg3(rd), step and H()['v_inc_steps_d'])
    o.isd = True
    if width == 2 and (d8 >> 4) & 1 and rd == 7 and H()['v_d16_block1_discard']:
        o.discard = True
    return o


def t5_block(t5):
    if not H()['v_t5_block_bits']:
        return 0, 0
    return 16 * ((t5 >> 1) & 1) + 32 * ((t5 >> 2) & 1), 32 * (t5 & 1) + 16 * ((t5 >> 3) & 1)


class Unknown(Exception):
    """Raised at decode time for an operand form with no hypothesis."""


def decode_a80(a11, ra, t5):
    by, bx = t5_block(t5)
    v = bool(t5 & 16)
    if a11 & 0x400:
        if H()['v_a_mode4_dydx']:
            dy, dx = (a11 >> 6) & 15, a11 & 31
        else:
            dx, dy = (a11 >> 6) & 15, a11 & 31
        if a11 & 0x20:
            raise Unknown('A mode 4 with bit 5 set')
        step = v if H()['v_a_mode4_step_t5'] else False
        return Opnd(by + dy, bx + dx, False, 1, ra, step)
    m = (a11 >> 8) & 3
    if m == 0:
        if not v:
            if a11 & 0xff:
                raise Unknown('A mode 0 H with coordinate')
            return Opnd(by, bx, False, 1, ra, False)
        return Opnd(by, bx + (a11 & 31), True, 1, ra, bool(a11 & 0x20))
    if m == 1:
        raise Unknown('A mode 1')
    if v:
        if a11 & 0xd0:
            raise Unknown('A mode 2 V with bits 7:6/4')
        return Opnd(by, bx + (a11 & 15), True, 2, ra, bool(a11 & 0x20))
    mode = H()['v_a_mode2_h_rows']
    if mode == 'split':
        if a11 & 0x33:
            raise Unknown('A mode 2 H with bits 5:4/1:0')
        row = ((a11 >> 2) & 3) * 4 + ((a11 >> 6) & 3)
    elif mode == 'low4':
        row = a11 & 15
    else:
        row = a11 & 63
    return Opnd(by + row, bx, False, 2, ra, m == 3)


def decode_b80(hw3, hw4):
    """('imm', v) | ('reg', n, off) | ('vec', Opnd)."""
    if hw3 & 0x400:
        return ('imm', D.sx((hw4 >> 10) << 10 | (hw3 & 0x3ff), 16))
    if hw4 & 0x8000:
        return ('reg', (hw4 >> 12) & 7, D.sx(((hw4 >> 10) & 3) << 10 | (hw3 & 0x3ff), 12))
    b8, bt = (hw3 >> 2) & 0xff, hw3 & 3
    if bt & 2:
        raise Unknown('B type bit 1')
    if H()['v_b_v_bit0']:
        vert, width = bool(bt & 1), 2 if hw4 & 0x800 else 1
    else:
        vert, width = bool(hw4 & 0x800), 2 if bt & 1 else 1
    y, x = d8_yx(b8, vert)
    reg = _reg3((hw4 >> 12) & 7)
    if width == 2 and reg is not None and H()['v_b16_reg'] == 'ignored':
        reg = None
    if reg is not None and not hw4 & 0x400 and H()['v_b_reg_nostep'] == 'ignored':
        reg = None
    op = Opnd(y, x, vert, width, reg, bool(hw4 & 0x400))
    op.isb = True
    return ('vec', op)


# --------------------------------------------------------------------------
# Operations.  fn(a, b) on Python ints; 'sat' marks saturating variants.

def _sh(b):
    return b & 31


OPS = {
    4: ('vmov', lambda a, b: b, False),
    8: ('vadd', lambda a, b: a + b, False),
    9: ('vadds', lambda a, b: a + b, True),
    10: ('vsub', lambda a, b: a - b, False),
    11: ('vsubs', lambda a, b: a - b, True),
    12: ('vrsub', lambda a, b: b - a, False),
    13: ('vrsubs', lambda a, b: b - a, True),
    14: ('vmul', lambda a, b: a * b, False),
    16: ('vmulm', lambda a, b: (a * b) >> 8, False),
    17: ('vmulms', lambda a, b: (a * b) >> 8, True),
    20: ('vmulhr', lambda a, b: (a * b + 0x8000) >> 16, False),
    21: ('vmulhrs', lambda a, b: (a * b + 0x8000) >> 16, True),
    24: ('vshl', lambda a, b: a << _sh(b), False),
    26: ('vasr', lambda a, b: a >> _sh(b), False),
    27: ('vlsr', None, False),               # needs the width: see exec
    28: ('vmin', min, False),
    29: ('vmax', max, False),
    30: ('vdist', lambda a, b: abs(a - b), False),
    44: ('vbitplanes', None, False),
    52: ('vmulhru', lambda a, b: (a * b + 0x8000) >> 16, False),
    # P19: the high half of the product, floored (no rounding): the
    # partner of a 'vmul ... x15' low half in Apple's 32-bit sums.  The
    # op6 bit 0 / bit 5 pattern follows the scalar mulhd.ss/.su/.uu:
    # 18 both signed, 19 B unsigned (expand_range_16s' 16.16 scale,
    # RenderServer's 32-bit MAC), 51 A and B unsigned (vc_distort's 16.16
    # low half).  50 (A unsigned, B signed) is never used.
    18: ('vmulhd', lambda a, b: (a * b) >> 16, False),
    19: ('vmulhdsu', lambda a, b: (a * (b & 0xffff)) >> 16, False),
    51: ('vmulhduu', lambda a, b: ((a & 0xffff) * (b & 0xffff)) >> 16, False),
    # P19: clamp A to 0..B (H.264 Clip1 of j in jhoz; Clip3(0, 51, .) of
    # the QP index in vc_calc_alpha_beta).  HYP v_clip_lo.
    37: ('vclip', None, False),
}
MULHR = {20, 21, 52}
MULHD = {18: (False, False), 19: (False, True), 51: (True, True)}   # (A, B) unsigned


def mulhr_round(p, mode):
    """The high half of a product, rounded: 'up' adds 0x8000 and floors;
    'away' rounds ties away from zero (device session 6)."""
    if mode == 'away' and p < 0:
        return -((-p + 0x8000) >> 16)
    return (p + 0x8000) >> 16


def w16s(v):
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v


def _sat(v, width, vert=None):
    if width == 1:
        return 0 if v < 0 else 255 if v > 255 else v
    if width == 2:
        return -0x8000 if v < -0x8000 else 0x7fff if v > 0x7fff else v
    return -0x80000000 if v < -0x80000000 else 0x7fffffff if v > 0x7fffffff else v


PRED = [None, 'none', 'ifz', 'ifnz', 'ifn', 'ifnn', 'ifc', 'ifnc']


def _lanes(vrf, pred):
    if pred == 0:
        return list(range(16))
    if pred == 1:
        return []
    f = {2: vrf.fz, 3: vrf.fz, 4: vrf.fn, 5: vrf.fn, 6: vrf.fc, 7: vrf.fc}[pred]
    want = pred & 1 == 0          # ifz/ifn/ifc: flag set
    return [i for i in range(16) if f[i] == want]


REP_N = [1, 2, 4, 8, 16, 32, 64, None]


class VInsn:
    """A decoded vector ALU instruction."""
    def __init__(self, ins):
        ws = ins.ws
        self.ins = ins
        self.op9 = ws[0] & 0x1ff
        self.op6, self.w = self.op9 >> 3, (self.op9 >> 1) & 3
        self.text = ins.text
        if len(ws) == 5:
            hw5 = ws[4]
            self.setf = bool(hw5 & 0x4000)
            self.pred = (hw5 >> 11) & 7
            self.clra, self.acc = bool(hw5 & 0x400), bool(hw5 & 0x200)
            self.x15, self.x8 = bool(hw5 & 0x8000), bool(hw5 & 0x100)
            self.rep = REP_N[(hw5 >> 5) & 7]
            self.sru = ((hw5 >> 3) & 3, hw5 & 7)
            inc = bool(ws[3] & 1)
            self.d = decode_d(ws[1], self.w, inc)
            a11 = ((ws[3] >> 4) & 63) << 5 | ws[2] >> 11
            self.a = decode_a80(a11, _reg3((ws[3] >> 1) & 7), ws[1] & 31)
            self.a.isa = True
            self.b = decode_b80(ws[2], ws[3])
        else:
            hw3 = ws[2]
            b_bit1 = False
            self.x15 = self.x8 = False
            self.pred, self.clra, self.acc, self.sru = 0, False, False, (0, 7)
            if hw3 & 0x400:
                self.setf = bool(hw3 & 0x200)
                self.rep = REP_N[(hw3 >> 6) & 7]
                self.b = ('imm', D.sx(hw3 & 63, 6) if H()['v_imm6_signed'] else hw3 & 63)
            else:
                self.setf, self.rep = False, 1
                bt = hw3 & 3
                b_bit1 = bool(bt & 2)
                if b_bit1 and (H()['v_r48'] != 'enables' or self.op9 & 1):
                    raise Unknown('48-bit B type bit 1')
                self.b = None                      # filled below (needs D)
            self.d = decode_d(ws[1], self.w, self.rep > 1 if self.rep else True)
            t5 = ws[1] & 31
            areg = breg = None
            if H()['v_r48'] == 'enables' and not self.op9 & 1:
                # Non-x ops: t5 bit 3 gives the +r to A, B's hw3[1] to B;
                # D takes none.
                rr = self.d.reg
                if t5 & 8:
                    t5 &= ~8
                    areg = rr
                if b_bit1:
                    breg = rr
                if rr is not None and (areg is not None or breg is not None):
                    self.d.reg = None
                    if self.d.width == 2 and (ws[1] >> 12) & 1 and \
                            H()['v_d16_block1_discard']:
                        self.d.discard = True
            by, bx = t5_block(t5)
            c = hw3 >> 11
            d = self.d
            y, x = (by, bx + c) if d.vert else (by + c, bx)
            self.a = Opnd(y, x, d.vert, d.width, areg, False)
            self.a.isa = True
            if self.b is None:
                vert = bool(hw3 & 1)
                y, x = d8_yx((hw3 >> 2) & 0xff, vert)
                self.b = ('vec', Opnd(y, x, vert, d.width, breg, False))
                if breg is not None:
                    self.b[1].isb = True
        if self.x8:
            raise Unknown('modifier x8')
        if self.x15 and (H()['v_x15'] == 'none' or self.d.width != 2):
            raise Unknown('modifier x15')
        if self.op6 not in OPS:
            raise Unknown('operation %s' % D.vopname(self.op9))
        if self.x15 and OPS[self.op6][2]:
            raise Unknown('x15 on a saturating op')


# The H16 A +r x values silicon has read (ASHF, ASHG, ASHX) and the
# multiples of 32, where every rule agrees
A16_X_MEASURED = frozenset((0, 5, 13, 16, 21, 29, 32, 37, 48, 53))


def _strict_reg(sim, vi):
    """Strict mode (vc2sim.STRICT_ALIGN) for a vector ALU op's +r parts
    whose silicon rule is measured only at a few points: a vertical D's
    +r y off a multiple of 16 (HYP v_vd_reg_y: y 8 and 16 measured, PUTV
    and sitting 1's IDCT) and a horizontal 16-bit A's +r x silicon has not
    read (HYP v_a16_reg_x 'near32': x 5, 13, 16, 21, 29, 37, 48, 53 read, ASHF,
    ASHG, ASHX).  Our kernels keep to multiples of 32."""
    r = sim.r
    d, a = vi.d, vi.a
    if not d.discard and d.vert and d.reg is not None and (r[d.reg] >> 6) & 15:
        S.align_unknown(sim, 'V%d D +r y %d (not a multiple of 16)' % (
            8 * d.width, (r[d.reg] >> 6) & 63), r[d.reg])
    if a.isa and a.width == 2 and not a.vert and a.reg is not None and \
            (r[a.reg] & 63) not in A16_X_MEASURED:
        S.align_unknown(sim, 'H16 A +r x %d (unmeasured)' % (r[a.reg] & 63), r[a.reg])


def compile_valu(sim, ins):
    """Closure executing a vector ALU instruction (48- or 80-bit)."""
    try:
        vi = VInsn(ins)
    except Unknown as e:
        msg = 'vector op not modelled (%s): %s' % (e, ins.text)

        def h():
            raise S.VectorOp(msg)
        return h
    vrf = sim.vrf
    r = sim.r
    name, fn, sat = OPS[vi.op6]
    d, a, b = vi.d, vi.a, vi.b
    dw = d.width
    hyp = H()

    def h():
        n = vi.rep if vi.rep is not None else r[0]
        if S.STRICT_ALIGN:
            _strict_reg(sim, vi)
        for k in range(n):
            lanes = _lanes(vrf, vi.pred)
            av = vrf.read(a.offset(r, k), a.vert, a.width)
            if b[0] == 'imm':
                bv = [b[1]] * 16
            elif b[0] == 'reg':
                rv = 0 if b[1] == 7 and H()['v_sreg7'] == 'zero' else r[b[1]]
                v = (rv + b[2]) & 0xffff
                bv = [v - 0x10000 if v & 0x8000 else v] * 16
            else:
                o = b[1]
                bv = vrf.read(o.offset(r, k), o.vert, o.width)
            if vi.op6 in MULHR and (hyp['v_mulhr_b_unsigned'] is True or
                                    (hyp['v_mulhr_b_unsigned'] == 'imm' and
                                     b[0] != 'vec') or
                                    (hyp['v_mulhr_b_unsigned'] == 'not_vmulhr'
                                     and vi.op6 != 20)):
                bv = [x & 0xffff for x in bv]
            if vi.op6 == 44:                      # vbitplanes
                res = [(bv[i] >> i) & 1 for i in range(16)]
            elif vi.op6 == 27:                    # vlsr: logical on A's width
                m = (1 << (8 * a.width)) - 1
                res = [(x & m) >> _sh(y) for x, y in zip(av, bv)]
            elif vi.op6 in MULHR:
                rm = hyp['v_mulhr_round']
                res = [mulhr_round(x * y, rm) for x, y in zip(av, bv)]
            elif vi.op6 in MULHD and hyp['v_mulhd_round'] != 'floor':
                ua, ub = MULHD[vi.op6]
                res = [mulhr_round((x & 0xffff if ua else x) * (y & 0xffff if ub else y),
                                   'away') for x, y in zip(av, bv)]
            elif vi.op6 == 37:                    # vclip: clamp to 0..B
                lo = hyp['v_clip_lo'] == 'zero'
                res = [max(0, min(x, y)) if lo else min(x, y) for x, y in zip(av, bv)]
            else:
                res = [fn(x, y) for x, y in zip(av, bv)]
            if vi.clra:
                for i in lanes:
                    vrf.acc[i] = 0
                    vrf.acclo[i] = 0
            if vi.x15 and hyp['v_x15'] != 'acc':
                # The accumulator is acc[i] * 65536 + acclo[i] (32 bits on
                # silicon; the high half unbounded here, as for plain acc).
                # x15 adds at bit 0: the low 16 bits zero-extended, or the
                # signed result when 'acc' is also set (HYP v_x15); the
                # result is the whole accumulator (D keeps its low half).
                # HYP v_acc_sat: how the sum saturates (silicon: X15S,
                # REPA); 'none' lets the high half grow.
                mode, accsat = hyp['v_x15'], hyp['v_acc_sat']
                for i in lanes:
                    if vi.acc or mode == 'lo_sext':
                        v = res[i] if vi.acc else w16s(res[i])
                    else:
                        v = res[i] & 0xffff
                    if accsat == 'sat32':
                        t = _sat(vrf.acc[i] * 65536 + vrf.acclo[i] + v, 4)
                        vrf.acc[i], vrf.acclo[i] = t >> 16, t & 0xffff
                        continue
                    t = vrf.acclo[i] + v
                    vrf.acclo[i] = t & 0xffff
                    vrf.acc[i] += t >> 16
                    if accsat == 'hi16':
                        vrf.acc[i] = _sat(vrf.acc[i], 2)
                res = [vrf.acc[i] * 65536 + vrf.acclo[i] for i in range(16)]
            elif vi.acc or vi.x15:
                accsat = hyp['v_acc_sat']
                for i in lanes:
                    if accsat == 'sat32':
                        t = _sat((vrf.acc[i] + res[i]) * 65536 + vrf.acclo[i], 4)
                        vrf.acc[i], vrf.acclo[i] = t >> 16, t & 0xffff
                    elif accsat == 'hi16':
                        vrf.acc[i] = _sat(vrf.acc[i] + res[i], 2)
                    else:
                        vrf.acc[i] += res[i]
                res = list(vrf.acc)
                if accsat == 'read16':
                    res = [_sat(v, 2) for v in res]
            if sat and (vi.op6 != 21 or hyp['v_mulhrs_sat']):
                res = [_sat(v, dw if hyp['v_sat_by_dest'] else 2) for v in res]
            if vi.setf:
                fl = lanes if hyp['v_pred_flags_only_enabled'] else range(16)
                m = (1 << (8 * dw)) - 1
                for i in fl:
                    v = res[i]
                    vrf.fz[i] = (v & m) == 0
                    vrf.fn[i] = v < 0
                    vrf.fc[i] = v < 0 or v > m
            fnc, reg = vi.sru
            if reg != 7 and fnc:
                vals = [res[i] for i in lanes]
                if fnc == 1:
                    s = sum(vals)
                elif fnc == 2:
                    s = min(vals) if vals else 0
                else:
                    s = max(vals) if vals else 0
                r[reg] = s & MASK
            elif reg != 7 and fnc == 0 and vi.ins.e.vclass == 'valu80':
                raise S.VectorOp('scalar-result function 0 (sru0) not modelled: %s'
                                 % vi.text)
            if not d.discard:
                vrf.write(d.offset(r, k), d.vert, dw, res, lanes)
    return h


# --------------------------------------------------------------------------
# Memory operations (48-bit form: op9 < 0x20, plus vstr).

def st48_swap(ws):
    """(y, x, byte offset) of a 48-bit vst16 under the silicon layout
    (HYP 'v_st48_nonzero' = 'swap', device session 5): hw2[15:8] = offset
    in 32-byte units, hw2[4:0] = t5 block bits of the source, hw3[15:11] =
    its row in the block."""
    h1, h2 = ws[1], ws[2]
    t5 = h1 & 31
    y = (16 if t5 & 2 else 0) + (32 if t5 & 4 else 0) + (h2 >> 11)
    x = (32 if t5 & 1 else 0) + (16 if t5 & 8 else 0)
    return y & 63, x, 32 * (h1 >> 8)


def mem48_fields(ws):
    """(op9, d8, offset field, rep, base, stride) of a 48-bit vector memory
    op; the offset field counts v_mem48_off_unit (8) bytes."""
    h1, h2 = ws[1], ws[2]
    off = D.sx((h1 & 0xff) << 7 | h2 >> 9, 15)
    return (ws[0] & 0x1ff, h1 >> 8, off, REP_N[(h2 >> 6) & 7], (h2 >> 3) & 7,
            _reg3(h2 & 7))


# Memory op9 = kind (op9[4:3]: 0 ld, 1 st, 2 ldp, 3 ldq) . w (op9[2:1],
# as the ALU .w: 0 H8, 1 H16, 2 V8) . 0.  A V load puts byte i of memory
# into row y + i of one column.

VMEM_NAMES = {(0, False, 1): 'vld8', (0, False, 2): 'vld16', (0, True, 1): 'vld8v',
              (1, False, 1): 'vst8', (1, False, 2): 'vst16', (1, True, 1): 'vst8v',
              (2, False, 1): 'vldp8', (2, False, 2): 'vldp16', (2, True, 1): 'vldp8v',
              (3, False, 1): 'vldq8', (3, False, 2): 'vldq16', (3, True, 1): 'vldq8v'}


# Plain vector loads and stores off their size whose silicon reading is
# known (memory-access.md "Alignment"): {(kind, vertical, width): byte
# offsets mod the access size (16 bytes a row for 8-bit rows, 32 for
# 16-bit)}.  Offset 0 is the aligned access.  Loads: S18-S22 (vld16 at
# multiples of 8; quirks #17a) and bench row ALGN VL62/VL64/VL68,
# VL82/VL84/VL88 (sitting 2): each reads from the address rounded down to
# its size.  Stores: ALGN VS62/VS64/VS68, VS84/VS88 (sitting 2): each
# lands at the address rounded down; VS80 the aligned vst8.  Every other
# offset (odd ones, vld16 at 6/10/12/14 or 18-30, a store at 16-31
# for vst16 or any 8-bit vertical form) is unmeasured.
VMEM_KNOWN_OFFSETS = {
    (0, False, 2): frozenset({0, 2, 4, 8, 16, 24}),
    (0, False, 1): frozenset({0, 2, 4, 8}),
    (0, True, 1): frozenset({0}),
    (1, False, 2): frozenset({0, 2, 4, 8}),
    (1, False, 1): frozenset({0, 4, 8}),
    (1, True, 1): frozenset({0}),
}


def _strict_vmem(sim, kind, vert, width, a, n, stride):
    """Strict alignment (vc2sim.STRICT_ALIGN) for one vector memory op at
    raw address a, n rows, stride bytes apart.  Each row of a plain load
    or store is checked on its own: silicon rounds each REP row down on
    its own (bench row ALGN VLS4, sitting 2), so a row is known when its
    offset is in VMEM_KNOWN_OFFSETS.  A store REP whose stride is not a
    multiple of the size is unmeasured as a whole (only loads were run
    with such a stride).  vldp/vldq: any base with a row stride that keeps
    every row in the same 16-byte line phase is silicon-implicit; a stride
    that moves the phase is known only as bench row ALGN ran it: H8 rows
    whose phase moves by 4 bytes a row (VPG4: a lone vldp8 REP at a
    stride of 4; VPQS: a vldp8/vldq8 pair REP at a stride of 20, sitting
    2).  Every other phase step stays unknown."""
    nb = 16 * width
    name = VMEM_NAMES.get((kind, vert, width), 'vmem%d' % kind)
    if kind in (0, 1):
        known = VMEM_KNOWN_OFFSETS.get((kind, vert, width), frozenset({0}))
        if kind == 1 and n > 1 and stride % nb:
            S.align_unknown(sim, '%s rep %d stride %d (rows off %d bytes)' % (
                name, n, stride, nb), a + stride)
            return
        for k in range(max(n, 1)):
            x = (a + k * stride) & MASK
            if x % nb not in known:
                S.align_unknown(sim, '%s %s at %d mod %d' % (
                    name, 'first row' if k == 0 else 'rep row %d' % k, x % nb, nb), x)
                return
        return
    if n > 1 and stride % 16 and not (width == 1 and not vert and stride % 16 == 4):
        S.align_unknown(sim, '%s rep %d stride %d (line phase moves per row)' % (
            name, n, stride), a)


def _mem_access(sim, kind, vert, width, y, x, inc, off_fn, rep, stride):
    """Closure for a vector load/store of one 16-element vector per REP."""
    vrf, r, mem = sim.vrf, sim.r, sim.mem
    n_bytes = 16 * width

    def h():
        n = rep if rep is not None else r[0]
        a = off_fn() & MASK
        if S.STRICT_ALIGN:
            _strict_vmem(sim, kind, vert, width, a, n,
                         r[stride] if stride is not None else 0)
        al = H()['v_ld_align' if kind == 0 else 'v_st_align'] if kind in (0, 1) else 0
        al = n_bytes if al == 'width' else al
        per_row = al and H()['v_ld_rep_round'] == 'row'
        if al and not per_row:
            a &= ~(al - 1)
        for k in range(n):
            if per_row:
                row_a = a
                a &= ~(al - 1)
            if inc:
                o = ((y << 6) | ((x + k) & 63)) if vert else ((((y + k) & 63) << 6) | x)
            else:
                o = (y << 6) | x
            if kind == 1:
                vals = vrf.read(o, vert, width, signed=False)
                mem.write(a, b''.join(v.to_bytes(width, 'little') for v in vals))
            else:
                raw = mem.read(a, n_bytes)
                vals = [int.from_bytes(raw[width * i:width * i + width], 'little')
                        for i in range(16)]
                if kind == 0 or not H()['v_ld_split_lines']:
                    lanes = range(16)
                else:
                    line = a & ~15
                    same = [((a + width * i) & ~15) == line for i in range(16)]
                    lanes = [i for i in range(16) if same[i] == (kind == 2)]
                vrf.write(o, vert, width, vals, lanes)
            if per_row:
                a = row_a
            if stride is not None:
                a = (a + r[stride]) & MASK
    return h


def _vmem_unknown(ins, why):
    msg = 'vector memory op not modelled (%s): %s' % (why, ins.text)

    def h():
        raise S.VectorOp(msg)
    return h


def compile_vmem48(sim, ins):
    op9, d8, off, rep, base, stride = mem48_fields(ins.ws)
    if stride is None and H()['v_mem_off_nostride'] == 'ignored':
        off = 0
    kind, wsel = op9 >> 3, (op9 >> 1) & 3
    if op9 & 1 or wsel == 3 or kind > 3:
        return _vmem_unknown(ins, 'op9 0x%02x' % op9)
    vert, width = W_DIR[wsel]
    y, x = d8_yx(d8, vert)
    r = sim.r
    mode = H()['v_st48_nonzero']
    if kind == 1 and ins.ws[1] and mode == 'drop':
        return lambda: None
    if kind == 1 and mode == 'swap' and wsel == 1:
        y, x, off = st48_swap(ins.ws)
        return _mem_access(sim, kind, vert, width, y, x, True, lambda: _base(r, base) + off,
                           rep, stride)
    off *= H()['v_mem48_off_unit']
    return _mem_access(sim, kind, vert, width, y, x, True, lambda: _base(r, base) + off,
                       rep, stride)


def compile_vmem80(sim, ins):
    """80-bit loads of the '.1f a3f' family: d8 is the destination (no
    +r, t5 = 0x1f), hw4 = 111x 1111 1111 111i: hw4[12] loads from one byte
    further on (the left-neighbour loads at x - 2 read from x - 1), i =
    D steps per REP.  The other 80-bit memory forms (D+r0/A+r0 'bx=38'
    loaders, vc_calc_strength_load) are not modelled."""
    ws = ins.ws
    op9 = ws[0] & 0x1ff
    kind, wsel = op9 >> 3, (op9 >> 1) & 3
    if ws[1] & 0xff != 0xff or ws[3] & 0xeffe != 0xeffe or ws[4] != 0x00e7 \
            or op9 & 1 or wsel == 3 or kind not in (0, 2, 3):
        return _vmem_unknown(ins, '80-bit form')
    h3 = ws[2]
    off = D.sx(h3 >> 9, 7) + ((ws[3] >> 12) & 1)
    rep, base, stride = REP_N[(h3 >> 6) & 7], (h3 >> 3) & 7, _reg3(h3 & 7)
    if stride is None and H()['v_mem_off_nostride'] == 'ignored':
        off = 0
    vert, width = W_DIR[wsel]
    y, x = d8_yx(ws[1] >> 8, vert)
    r = sim.r
    return _mem_access(sim, kind, vert, width, y, x, bool(ws[3] & 1),
                       lambda: _base(r, base) + off, rep, stride)


def compile_vstr(sim, ins):
    """vstr (0x170, 80-bit): store the A vector's enabled lanes (bytes)
    to (base)+=stride per REP.  D's t5 supplies A's block bits."""
    ws = ins.ws
    try:
        a11 = ((ws[3] >> 4) & 63) << 5 | ws[2] >> 11
        if ws[2] & 0x200 or (ws[3] >> 10) != 0:
            raise Unknown('vstr extra bits')
        a = decode_a80(a11, _reg3((ws[3] >> 1) & 7), ws[1] & 31)
    except Unknown as e:
        msg = 'vstr not modelled (%s): %s' % (e, ins.text)

        def h():
            raise S.VectorOp(msg)
        return h
    h3, hw5 = ws[2], ws[4]
    rep = REP_N[(h3 >> 6) & 7]
    base, stride = (h3 >> 3) & 7, _reg3(h3 & 7)
    pred = (hw5 >> 11) & 7
    if (ws[0] & 0x1ff) != 0x170 or hw5 & 0xc7ff != 0x0000:
        msg = 'vstr variant not modelled: %s' % ins.text

        def h():
            raise S.VectorOp(msg)
        return h
    vrf, r, mem = sim.vrf, sim.r, sim.mem

    def h():
        n = rep if rep is not None else r[0]
        addr = _base(r, base) & MASK
        for k in range(n):
            vals = vrf.read(a.offset(r, k), a.vert, a.width, signed=False)
            line = addr & ~15
            if S.STRICT_ALIGN and a.width > 1 and (
                    addr & (a.width - 1) or any(
                        ((addr + a.width * i) & ~15) != line for i in _lanes(vrf, pred))):
                # silicon knows the 8-bit lanes' line rule (S22); a 16-bit
                # vstr off its size or across a line is unmeasured
                S.align_unknown(sim, 'vstr16 at %d mod 16' % (addr % 16), addr)
            for i in _lanes(vrf, pred):
                if H()['v_vstr_line'] == 'first' and \
                        ((addr + a.width * i) & ~15) != line:
                    continue
                for j in range(a.width):
                    mem.w8(addr + a.width * i + j, vals[i] >> (8 * j))
            if stride is not None:
                addr = (addr + r[stride]) & MASK
    return h


def _ok(o):
    return ('V' if o.vert else 'H') + str(8 * o.width)


def form_keys(ins):
    """(semantic, operand) form keys of one vector instruction, as tuples
    of strings (bcmfw/h264/test/vpu_forms.py, scripts/proven_forms.py and
    the strict forms trap use them).  semantic: operation, element widths
    of D/A/B (or B = imm / scalar reg), modifiers (clra, acc, x15, setf,
    predication, scalar result, REP), 48/80-bit.  operand: the same with
    each operand's direction, +rN and stepping."""
    vc = ins.e.vclass
    if vc in ('valu80', 'valu48'):
        try:
            vi = VInsn(ins)
        except Unknown as e:
            k = ('unmodelled', ins.text.split()[0], str(e))
            return k, k
        name = OPS[vi.op6][0]
        b = vi.b
        bw = 'imm' if b[0] == 'imm' else 'reg' if b[0] == 'reg' else str(8 * b[1].width)
        mods = []
        if vi.clra:
            mods.append('clra')
        if vi.acc:
            mods.append('acc')
        if vi.x15:
            mods.append('x15')
        if vi.setf:
            mods.append('setf')
        if vi.pred:
            mods.append(PRED[vi.pred])
        if vi.sru[1] != 7 and vi.sru[0]:
            mods.append(['sru0', 'sum', 'min', 'max'][vi.sru[0]])
        if (vi.rep or 2) > 1:
            mods.append('rep')
        ln = '80' if vc == 'valu80' else '48'
        dw = 'discard' if vi.d.discard else str(8 * vi.d.width)
        sem = (name, 'D' + dw, 'A' + str(8 * vi.a.width), 'B' + bw, ' '.join(mods), ln)
        d = 'discard' if vi.d.discard else _ok(vi.d) + ('+r' if vi.d.reg is not None else '') + \
            ('++' if vi.d.step else '')
        a = _ok(vi.a) + ('+r' if vi.a.reg is not None else '') + ('++' if vi.a.step else '')
        bo = bw if b[0] != 'vec' else _ok(b[1]) + ('+r' if b[1].reg is not None else '') + \
            ('++' if b[1].step else '')
        opd = (name, 'D' + d, 'A' + a, 'B' + bo, ' '.join(mods), ln)
        return sem, opd
    if vc == 'vmem48':
        op9, d8, off, rep, base, stride = mem48_fields(ins.ws)
        k = ('mem48', D.vopname(op9), 'rep' if (rep or 2) > 1 else 'single',
             'stride' if stride is not None else 'nostride', 'off' if off else 'nooff')
        return k[:3], k
    if vc == 'vstr80':
        t = ins.text.split()
        k = ('vstr', 'rep' if ' rep' in ins.text else 'single',
             [x for x in t if x.startswith('if')][0] if ' if' in ins.text else 'none')
        return k, k
    k = (vc, ins.text.split()[0])
    return k, k


def form_text(key):
    """One operand form key as the table writes it"""
    return ' '.join(x if x else '-' for x in key)


# Strict forms (docs/research/hardware-closure-plan.md E1; tools/bcmre/
# proven_forms.txt): a vector instruction whose operand form the table
# does not name traps when it runs (FormTrap), unless its pc is in
# sim.form_exempt (a bench check region) or the code is Apple's (the
# self-test).  VC2SIM_STRICT_FORMS: 'trap' (the default), 'log' (counted
# in FORM_LOG, the run goes on) or '0'.  The table's vector lines are
# read once, at the first vector instruction compiled.
STRICT_FORMS = {'0': '', 'off': '', '1': 'trap', 'on': 'trap'}.get(
    os.environ.get('VC2SIM_STRICT_FORMS', 'trap'),
    os.environ.get('VC2SIM_STRICT_FORMS', 'trap'))
FORM_LOG = {}
_TABLE = None


def proven_vector_forms():
    """{operand form text: (class, evidence)} from the table"""
    global _TABLE
    if _TABLE is None:
        import proven_table
        _TABLE = {k: v for (dom, k), v in proven_table.load().items() if dom == 'vector'}
    return _TABLE


def set_strict_forms(mode):
    """'' (off), 'trap' or 'log' (True = 'trap'); returns the old mode."""
    global STRICT_FORMS
    old = STRICT_FORMS
    STRICT_FORMS = 'trap' if mode is True else (mode or '')
    return old


class FormTrap(Exception):
    pass


def _form_guard(sim, ins, h):
    """Wrap handler h with the strict forms check of ins (or return h)."""
    if not STRICT_FORMS:
        return h
    key = form_text(form_keys(ins)[1])
    if key in proven_vector_forms():
        return h
    apple = sim.vmcs is not None or any(str(i.path).endswith('.vll') for i in sim.images)
    if apple:
        return h
    pc = ins.addr
    msg = 'strict forms: %s at 0x%08x (%s): no silicon evidence in ' \
          'tools/bcmre/proven_forms.txt' % (key, pc, ins.text)

    def g():
        for lo, hi in getattr(sim, 'form_exempt', ()) or getattr(sim, 'align_exempt', ()):
            if lo <= pc < hi:
                return h()
        if STRICT_FORMS == 'log':
            FORM_LOG[key] = FORM_LOG.get(key, 0) + 1
            return h()
        raise S.SimError(msg)
    return g


def compile_vector(sim, ins):
    return _form_guard(sim, ins, _compile_vector(sim, ins))


def _compile_vector(sim, ins):
    vc = getattr(ins.e, 'vclass', None)
    if vc in ('valu80', 'valu48'):
        return compile_valu(sim, ins)
    if vc == 'vmem48':
        return compile_vmem48(sim, ins)
    if vc == 'vstr80':
        return compile_vstr(sim, ins)
    if vc == 'vmem80':
        return compile_vmem80(sim, ins)
    msg = 'vector op not modelled: %s' % ins.text

    def h():
        raise S.VectorOp(msg)
    return h
