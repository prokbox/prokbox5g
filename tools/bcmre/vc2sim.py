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
"""Scalar instruction-set simulator for VideoCore II (BCM2722).

Decoding comes from vc2dis.py (the shared, table-driven encoding table);
this file adds the execution semantics, a loader for .vll libraries and
the raw vmcs.bin image, Python stubs for imported library calls, a call
interface that follows the recovered calling convention, a tracer, and
a self-test that runs Apple's own leaf functions (reference material
only, never shipped) against independent Python models.  The evidence
behind every semantic choice is in docs/research/bcm2722/sim.md.

Vector-unit instructions run on the VPU model in vc2vpu.py (VRF,
accumulators, flags, predication, REP, scalar-result unit, 48-bit
loads/stores, vstr).  Forms the model has no hypothesis for raise
VectorOp naming the instruction.

Faults (unaligned access: Misaligned, unmapped address: BusFault,
undecodable instruction: Undefined; all SimErrors) stop the run by
default.  With sim.exceptions = True they enter a VC II exception
instead (take_exception: the rti frame, vector HYP['exc_vec_*']).  A
misaligned load behaves as the silicon did in device sessions 2 and 3
and bench row ALGN (2026-10-25; isa.md, "Exceptions on silicon"): it
reads the word at the address rounded down, writes rd and any
writeback, and the exception (vector 1) is taken HYP['exc_defer_insns']
instructions later (HYP['exc_defer_insns_wb'] for (--rs)), with the
saved pc at the first instruction not run.  A misaligned store is the
same (bench row ALGN, SWR1-SHO1 [silicon]): the store lands at the
address rounded down to its size, then vector 1 comes as late.  Bus
faults and undefined instructions are precise (unmeasured): saved pc =
faulting pc + HYP['exc_pc_offset'].

Usage:
  vc2sim.py --selftest CORPUS_DIR [-v] [--seed N] [--only NAME,...]
        CORPUS_DIR is .../VideoCore (holding Library/*.vll and Boot/vmcs.bin)
        or the Library directory itself.
  vc2sim.py --call FILE.vll|vmcs.bin FUNC|ADDR [ARG ...] [--trace] [--max N]
        call one function with integer arguments and print r0 (and r1)
  vc2sim.py --exports vmcs.bin        list the vmcs.bin export table
  vc2sim.py --boot IMAGE [--bcm] [--reg RULE]... [--alias LO-HI:BASE]...
                [--max N] [--poll N] [--entry A] [--machine] [--panel ID]
                [--then FUNC[,ARG...]]... [--hide LO[-HI]]... [--summary|--lcd]
                [--trace]
        run a raw image from 0x200 against modelled peripherals (vc2mmio)
        and log every device access with its pc.  --machine adds the timer
        and interrupt controller and delivers interrupts; --bcm = --machine
        plus the status-bit rules and SDRAM window vmcs.bin needs.  --then
        calls a function after the boot (e.g. 0xf672,2: vmcs.bin 1.3's LCD
        power state 2); --panel sets the panel-ID pins; --lcd prints the
        log as panel commands, GPIO edges and delays.  RULE: ADDR=V,
        ADDR|=M[@N] (status bits set N reads after a write), ADDR&=M,
        ADDR^=M, ADDR+=N (counter); see vc2mmio.py.
"""
import atexit
import collections
import glob
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vc2dis as D  # noqa: E402  (decoder; owned by the ISA work)

MASK = 0xffffffff


def s32(v):
    v &= MASK
    return v - 0x100000000 if v & 0x80000000 else v


def sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class SimError(Exception):
    pass


# Faults that the synchronous exception model (Sim.exceptions) turns into
# a VC II exception; with the model off they are plain SimErrors.
class Misaligned(SimError):
    hyp = 'exc_vec_misaligned'


class BusFault(SimError):
    hyp = 'exc_vec_bus'


class Undefined(SimError):
    hyp = 'exc_vec_undef'


# --------------------------------------------------------------------------
# Strict alignment (docs/bcm2722-reference/memory-access.md, "Alignment";
# docs/research/bcm2722/alignment-audit.md).  A switch of its own, apart
# from the decode: STRICT_ALIGN '' (off, the model as before), 'trap'
# (an access whose silicon behaviour is not [silicon]-known raises
# AlignTrap at once) or 'log' (each such site is counted in ALIGN_LOG and
# the run goes on with the model's reading: the audit's mode).  Where the
# behaviour IS known the model already follows it (the misaligned ld,
# the plain vector load's rounding, vstr's line), and strict mode lets it
# run.  VC2SIM_STRICT_ALIGN (trap, log, 0) sets the start value;
# set_strict_align() changes it, the DMA model's too.

class AlignTrap(SimError):
    """Strict alignment: no 'hyp', so never a VC II exception."""


# --------------------------------------------------------------------------
# Strict code coherence (bench row B13, 2026-10-25; quirks.md #40).  vc2sim
# decodes an instruction once and fetches it from memory as it is then;
# silicon's instruction side after a data store (by the core or the host)
# is not known.  Apple never runs code it has just written without a
# whole-cache flush through 0x10000C00 first (vmcs.bin @88a0, @91a4; the
# flush @25a), and the host's start does the same after its upload.  A
# harness that models the flush (hello_test.Bench) calls
# Sim.cache_flushed() at each one; from then on a first fetch of bytes
# that differ from memory as it stood at the last flush raises StaleCode.
# Without a flush ever seen (Sim.code_view None) nothing is checked.

class StaleCode(SimError):
    """Code written since the last cache flush: no 'hyp', never a VC II
    exception (what silicon does there is unknown)."""


def _strict_env():
    v = os.environ.get('VC2SIM_STRICT_ALIGN', 'trap')     # trap by default (2026-10-25)
    return {'0': '', 'off': '', '1': 'trap', 'on': 'trap'}.get(v, v)


STRICT_ALIGN = _strict_env()
ALIGN_LOG = collections.OrderedDict()   # (pc, what) -> [count, first address]

# The scalar forms whose misaligned access silicon has shown: the access
# completes at the address rounded down to its size (a load writes rd, a
# store writes memory), the base writeback happens, and vector 1 comes 4
# instructions later (2 after (--rs)).  Device sessions 2-3: ld (rs) and
# ld (--rs).  Bench row ALGN, sitting 2 (bench-20261025-2257, image
# 799ff0f8d7; bcmfw/bench/algn.S): every load form LW1C-LHP1 and every
# store form SWR1-SHO1 below [silicon].  Keys: (encoding, operation,
# indexed).  Every other form and width (the gp/sp-relative 2dxx forms,
# the 16-bit ldst_off and ldst_sp forms, 48-bit stores, ldb/stb cannot be
# misaligned) is unmeasured, and strict mode traps it.
STRICT_SCALAR_KNOWN = {
    ('ldst_predec', 'ld', False),                           # S2/S3 check 9
    ('ldst_ind', 'ld', False), ('ldst_ind', 'ld', True),    # LW1C-LWR3, LWX2
    ('ldst_off16', 'ld', False), ('mem48', 'ld', False),    # LWO2, LWL2
    ('ldst_postinc', 'ld', False), ('ldst_postinc', 'ldh', False),  # LWP2, LHP1
    ('ldst_ind', 'ldh', False), ('ldst_ind', 'ldsh', False),        # LHR1/3, LSH1
    ('ldst_off16', 'ldh', False),                                   # LHO1
    ('ldst_ind', 'st', False), ('ldst_off16', 'st', False),         # SWR1-3, SWO2
    ('ldst_postinc', 'st', False),                                  # SWP2
    ('ldst_ind', 'sth', False), ('ldst_off16', 'sth', False),       # SHR1/3, SHO1
}


def set_strict_align(mode):
    """'' (off), 'trap' or 'log' (True = 'trap'); returns the old mode."""
    global STRICT_ALIGN
    old = STRICT_ALIGN
    STRICT_ALIGN = 'trap' if mode is True else (mode or '')
    M.DmaEngine.strict = STRICT_ALIGN
    return old


def align_unknown(sim, what, addr):
    """An access at addr whose silicon behaviour is unknown: nothing when
    strict mode is off, counted in 'log', AlignTrap in 'trap'.  Apple's
    code (a .vll or vmcs.bin loaded: the self-tests) is only counted,
    never trapped: its vldp/vldq pairs are misaligned by design."""
    if not STRICT_ALIGN:
        return
    pc = sim.r[31] if sim is not None else -1
    for lo, hi in getattr(sim, 'align_exempt', ()):
        if lo <= pc < hi:
            return              # misaligned on purpose (bench row ALGN)
    apple =sim is not None and (sim.vmcs is not None or any(
        str(i.path).endswith('.vll') for i in sim.images))
    if STRICT_ALIGN == 'log' or apple:
        h = sim.code.get(pc) if sim is not None else None
        key = (pc, what, h[2].text if h else '?', 'apple' if apple else 'ours')
        e = ALIGN_LOG.get(key)
        if e is None:
            ALIGN_LOG[key] = [1, addr & MASK, sim.r[26] if sim is not None else 0]
        else:
            e[0] += 1
        return
    raise AlignTrap('strict alignment: %s at 0x%08x: silicon behaviour unknown '
                    '(memory-access.md "Alignment")' % (what, addr & MASK))


ALGN_FIRST_TAG = 0x4C573143    # 'LW1C', bench/algn.S algn_tab's first entry
VC_RANGE_TAG = 0x564B4352      # 'VCKR', gen_bench.py bench_vc_range


def algn_exempt(sim, image):
    """Bench row ALGN's checks (bcmfw/bench/algn.S) are misaligned on
    purpose: find algn_tab in a raw image (its first tag, then 4-word
    entries up to a zero tag) and exempt the checks' code from strict
    alignment; the bench's vector checks too (bench_vc_range, 'VCKR':
    they probe forms off silicon's rules).  Returns ALGN's range, or None
    if the image has no table."""
    import struct as st
    w = st.unpack_from('<%dI' % (len(image) // 4), image)
    for i in range(len(w) - 2):
        # the bench's vector checks (gen_bench.py bench_vc_range): probes
        if w[i] == VC_RANGE_TAG and w[i + 1] < w[i + 2] < w[i + 1] + 0x10000:
            sim.align_exempt = list(getattr(sim, 'align_exempt', [])) + \
                [(w[i + 1], w[i + 2])]
            break
    for i in range(len(w) - 4):
        if w[i] != ALGN_FIRST_TAG:
            continue
        lo, hi, k = MASK, 0, i
        while k + 3 < len(w) and w[k]:
            if not w[k + 3] & 0x100:            # an assembly check
                lo, hi = min(lo, w[k + 1], w[k + 2]), max(hi, w[k + 1], w[k + 2])
            k += 4
        if hi:
            rng = (lo, hi + 0x80)       # the last check's few instructions on
            sim.align_exempt = list(getattr(sim, 'align_exempt', [])) + [rng]
            return rng
    return None


def _align_report():
    """'log' mode: append every counted site (and the DMA model's) to the
    file VC2SIM_ALIGN_REPORT names, one JSON object a line, for
    tools/bcmre/align_audit.py."""
    path = os.environ.get('VC2SIM_ALIGN_REPORT')
    dma = getattr(M, 'DmaEngine', None)
    dma = dma.align if dma is not None else []
    if not path or not (ALIGN_LOG or dma):
        return
    import json
    prog = os.path.basename(sys.argv[0]) if sys.argv else '?'
    with open(path, 'a') as f:
        for (pc, what, text, origin), (n, a, lr) in ALIGN_LOG.items():
            f.write(json.dumps({'test': prog, 'pc': pc, 'what': what, 'insn': text,
                                'origin': origin, 'count': n, 'addr': a,
                                'lr': lr}) + '\n')
        for msg in sorted(set(dma)):
            f.write(json.dumps({'test': prog, 'pc': -1, 'what': msg, 'insn': 'DMA',
                                'origin': 'ours', 'count': dma.count(msg), 'addr': 0,
                                'lr': 0}) + '\n')


atexit.register(_align_report)


# Semantic hypotheses that the corpus settled.  Each can be flipped to
# its most plausible alternative (vc2sim.py --selftest DIR --controls)
# to show that the self-test tells them apart (sim.md, "Controls").
HYP = {
    'mem48_offset_bits': True,   # 2fxx: hw0 bit 6 (and 5) are offset bits 1 (0)
    # The 32-bit halfword loads with an offset (offset bit 0 set): class 0
    # (0x28xx) and class 1 (0x29xx) of the 28/29/2a/2b family.
    #   'swap'  [silicon] 0x28/o1 zero-extends (ldh), 0x29/o1 sign-extends
    #           (ldsh): device session 17 (dev18's transform dump, 0x28/o1)
    #           and the 2026-10-25 Port write check (bench-20261025-1819:
    #           COMMIT's F16w, 12 of 12 logged sums fit with 0x29/o1
    #           sign-extending); quirks.md #16 and #33
    #   'zext'  both zero-extend (vc2sim 2026-10-11 to 2026-10-25)
    #   'apple' the names read from Apple's code: 0x28/o1 ldsh, 0x29/o1 ldh
    'off16_h': 'swap',
    # The 2dxx gp/sp-relative width classes 0 and 1 with offset bit 0 set:
    # 'off16' decodes them as the 28/29 family does (the same class field)
    # [hypothesis: never run on silicon with a halfword >= 0x8000; bench
    # row HWLD]; or one of off16_h's readings.  vc2cc emits neither.
    'gp16_h': 'off16',
    'index_scaled': True,        # ld rd, (rs + ri): ri scaled by access width
    'alu_sets_flags': False,     # True: every ALU result also writes Z/N
    'carry_is_borrow': True,     # cmp: C = a <u b (False: ARM, C = a >=u b)
    'cmn_carry_out': True,       # cmn: C = carry out of a + b (else inverted)
    'push_lowest_first': False,  # push: lowest register at the lowest address
                                 # (False: highest register lowest, lr on top;
                                 # vmcs.bin's interrupt save/restore needs it)
    'gp16_sp': True,             # 2dxx o=4-7 and 13 are sp-relative (else gp)
    'btst_z_when_clear': True,   # btst: Z = (bit == 0)
    'pc48_base_insn': True,      # 48-bit pc-relative: base = instruction address
                                 # (False: address of the next instruction)
    'pc48_mem_aligned': True,    # ... rounded down to a word for ld/st (not lea)
    'unary_from_rs': True,       # neg/not/msb/...: rd = op(rs) (else op(rd))
    # Synchronous exceptions (only with Sim.exceptions = True; --controls
    # skips the exc_ keys).  Device sessions 2 and 3 (probe v2 checks
    # 9-12, isa.md "Exceptions on silicon") measured the misaligned load:
    # vector 1 [H]; the load still completes from the address rounded down
    # to its size and writes rd and the (--rs)/(rs)++ writeback [H]; the
    # exception is imprecise: 4 further instructions ran after a plain
    # ld (rs), 2 after ld (--rs) [H for those two cases; M as a rule].
    # The saved pc is the first instruction not run.  Still unmeasured
    # [L]: the bus-error and undefined-instruction vectors (VC IV's table:
    # 6, 3) and whether stores, bus errors and undefined instructions are
    # precise; they are modelled precise, saved pc = faulting pc +
    # exc_pc_offset.  Vector v is the word at 4 * v.
    'exc_vec_misaligned': 1,
    'exc_vec_bus': 6,
    'exc_vec_undef': 3,
    'exc_pc_offset': 0,          # added to every saved pc (silicon: 0)
    'exc_mis_load_rounds': True,  # misaligned load: read rounded down, write rd
    # misaligned store: the bytes land at the address rounded down, then
    # vector 1 as late as a load's (bench row ALGN SWR1-SHO1, sitting 2
    # [silicon]: saved pc +10/+12 = after the 4 adds, the buffer's word
    # holds the stored value at the rounded-down address).  False: the
    # model before 2026-10-26 (precise, nothing stored)
    'exc_mis_store_rounds': True,
    'exc_defer_insns': 4,        # ... then take vector 1 after this many more
                                 # (every form ALGN ran, (rs)++ included)
    'exc_defer_insns_wb': 2,     # ... after ld (--rs) (S2/S3 check 9)
}

COVER = None                     # collections.Counter: executed instruction kinds
HYP_ALTERNATIVES = {             # multi-valued hypotheses: key -> alternatives
    'off16_h': ['swap', 'zext', 'apple'],
    'gp16_h': ['off16', 'zext', 'apple'],
}


class VectorOp(SimError):
    pass


class Unimplemented(SimError):
    pass


import vc2vpu as V  # noqa: E402  (vector unit; reads HYP at run time)

HYP.update(V.HYP_DEFAULTS)
HYP_ALTERNATIVES.update(V.HYP_ALTERNATIVES)


# --------------------------------------------------------------------------
# Memory.  One flat, little-endian, byte-addressed space.  Accesses outside
# it, and unaligned half/word accesses, raise SimError (strict on purpose:
# a wrong guess about an offset encoding shows up as an alignment fault
# instead of silently reading the wrong bytes).

MEM_SIZE = 0x2000000          # 32 MB
HEAP_BASE = 0x0800000
HEAP_END = 0x1800000
STACK_TOP = 0x1f00000         # initial sp (grows down)
STACK_LIMIT = 0x1e00000
STUB_BASE = 0x1f80000         # import stubs, 4 bytes apart
RET_MAGIC = 0x1fffff0         # return address of a top-level call


class Memory:
    def __init__(self, size=MEM_SIZE):
        self.b = bytearray(size)
        self.size = size

    def _chk(self, a, n):
        if a < 0 or a + n > self.size:
            raise BusFault('access to unmapped address 0x%08x' % a)
        if a & (n - 1):
            raise Misaligned('unaligned %d-byte access at 0x%08x' % (n, a))

    def r8(self, a):
        if not 0 <= a < self.size:
            raise BusFault('access to unmapped address 0x%08x' % a)
        return self.b[a]

    def r16(self, a):
        self._chk(a, 2)
        return self.b[a] | self.b[a + 1] << 8

    def r32(self, a):
        self._chk(a, 4)
        b = self.b
        return b[a] | b[a + 1] << 8 | b[a + 2] << 16 | b[a + 3] << 24

    def w8(self, a, v):
        if not 0 <= a < self.size:
            raise BusFault('access to unmapped address 0x%08x' % a)
        self.b[a] = v & 0xff

    def w16(self, a, v):
        self._chk(a, 2)
        self.b[a] = v & 0xff
        self.b[a + 1] = (v >> 8) & 0xff

    def w32(self, a, v):
        self._chk(a, 4)
        self.b[a:a + 4] = (v & MASK).to_bytes(4, 'little')

    def read(self, a, n):
        if a < 0 or a + n > self.size:
            raise SimError('access to unmapped range 0x%08x+%d' % (a, n))
        return bytes(self.b[a:a + n])

    def write(self, a, data):
        if a < 0 or a + len(data) > self.size:
            raise SimError('access to unmapped range 0x%08x+%d' % (a, len(data)))
        self.b[a:a + len(data)] = data

    def cstr(self, a, limit=1 << 16):
        e = self.b.find(b'\0', a, a + limit)
        if e < 0:
            raise SimError('unterminated string at 0x%08x' % a)
        return bytes(self.b[a:e])


# --------------------------------------------------------------------------
# Flags and conditions.  sr (r30) holds the flags in VC IV's layout:
# Z = 8, N = 4, C = 2, V = 1.  Only compare-type instructions (cmp, cmn,
# btst, cmp #0) write them; everything else, addcmpb included, leaves
# them alone (sim.md, "Flags").  C after cmp is a borrow (set when
# a < b unsigned), so cs = unsigned lower, hi = !C && !Z.

FZ, FN, FC, FV = 8, 4, 2, 1


def _cond(c, f):
    z, n, cy, v = f & FZ, f & FN, f & FC, f & FV
    return [bool(z), not z, bool(cy), not cy, bool(n), not n, bool(v), not v,
            not cy and not z, bool(cy) or bool(z), bool(n) == bool(v),
            bool(n) != bool(v), not z and bool(n) == bool(v),
            bool(z) or bool(n) != bool(v), True, False][c]


COND = [[_cond(c, f) for f in range(16)] for c in range(16)]


def flags_sub(a, b):
    """Flags of a - b (cmp)."""
    r = (a - b) & MASK
    f = 0
    if r == 0:
        f |= FZ
    if r & 0x80000000:
        f |= FN
    if (a < b) == HYP['carry_is_borrow']:
        f |= FC                       # borrow
    if ((a ^ b) & (a ^ r)) & 0x80000000:
        f |= FV
    return f


def flags_add(a, b):
    """Flags of a + b (cmn): C is the carry out."""
    t = a + b
    r = t & MASK
    f = 0
    if r == 0:
        f |= FZ
    if r & 0x80000000:
        f |= FN
    if (t > MASK) == HYP['cmn_carry_out']:
        f |= FC
    if (~(a ^ b) & (a ^ r)) & 0x80000000:
        f |= FV
    return f


# --------------------------------------------------------------------------
# ALU.  VC II numbering (vc2dis.ALU), semantics from VC IV unless the
# corpus said otherwise.  Each entry: fn(a, b) -> result (None: no write).
# Unary ops take their operand from b ("op rd, rs" is rd = op(rs); the
# 3-operand forms ignore ra).

def _msb(b):
    return (b.bit_length() - 1) & MASK


def _brev(b):
    return int('{:032b}'.format(b)[::-1], 2)


def _sat32(v):
    return max(-0x80000000, min(0x7fffffff, v)) & MASK


ALU_FN = {
    2: lambda a, b: a & ((1 << (b & 31)) - 1),               # bmask (extu)
    3: lambda a, b: a & ~b & MASK,                           # bic
    4: lambda a, b: (s32(a) >> (b & 31)) & MASK,             # asr
    5: lambda a, b: a ^ b,                                   # eor
    6: lambda a, b: (a << (b & 31)) & MASK,                  # lsl
    7: lambda a, b: a & b,                                   # and
    8: lambda a, b: a >> (b & 31),                           # lsr
    10: lambda a, b: (a * b) & MASK,                         # mul
    11: lambda a, b: (b - a) & MASK,                         # rsub
    12: lambda a, b: (a + b) & MASK,                         # add
    13: lambda a, b: (a - b) & MASK,                         # sub
    14: lambda a, b: b,                                      # mov
    15: lambda a, b: a | b,                                  # or
    18: lambda a, b: a if s32(a) >= s32(b) else b,           # max (signed)
    19: lambda a, b: a if s32(a) <= s32(b) else b,           # min (signed)
    20: lambda a, b: (a + 2 * b) & MASK,                     # adds2
    21: lambda a, b: (a + 4 * b) & MASK,                     # adds4
    22: lambda a, b: (a + 8 * b) & MASK,                     # adds8
    23: lambda a, b: (a + 16 * b) & MASK,                    # adds16
    24: lambda a, b: -b & MASK,                              # neg
    25: lambda a, b: _msb(b),                                # msb (-1 for 0)
    26: lambda a, b: sx(b, 8) & MASK,                        # sxtb (sim.md)
    27: lambda a, b: sx(b, 16) & MASK,                       # sxth (sim.md)
    28: lambda a, b: _brev(b),                               # brev
    30: lambda a, b: abs(s32(b)) & MASK,                     # abs (sim.md)
    31: lambda a, b: ~b & MASK,                              # not
}
UNARY = {24, 25, 26, 27, 28, 30, 31}   # neg msb sxtb sxth brev abs not
ALU_FLAGS = {
    0: lambda a, b, f: (f & ~FZ) | (0 if ((a >> (b & 31)) & 1) == HYP['btst_z_when_clear']
                                    else FZ),                           # btst
    1: lambda a, b, f: (f & ~15) | flags_add(a, b),                      # cmn
    16: lambda a, b, f: (f & ~15) | flags_sub(a, b),                     # cmp
}
# Ops seen nowhere in the corpus (or not pinned): executing them traps.
# --assume N=name enables a VC IV candidate for experiments.
ALU_CANDIDATES = {
    'ror': lambda a, b: ((a >> (b & 31)) | (a << (32 - (b & 31)))) & MASK,
    'bset': lambda a, b: a | (1 << (b & 31)),
    'bclr': lambda a, b: a & ~(1 << (b & 31)) & MASK,
    'bchg': lambda a, b: a ^ (1 << (b & 31)),
}


def _mulhd(sa, sb):
    def f(a, b):
        x = s32(a) if sa else a
        y = s32(b) if sb else b
        return ((x * y) >> 32) & MASK
    return f


def _div(sa, sb):
    def f(a, b):
        x = s32(a) if sa else a
        y = s32(b) if sb else b
        if y == 0:
            raise SimError('division by zero')
        q = abs(x) // abs(y)
        return (q if (x < 0) == (y < 0) else -q) & MASK
    return f


EXT_FN = {
    0: _mulhd(1, 1), 1: _mulhd(1, 0), 2: _mulhd(0, 1), 3: _mulhd(0, 0),
    4: _div(1, 1), 5: _div(1, 0), 6: _div(0, 1), 7: _div(0, 0),
    8: lambda a, b: _sat32(s32(a) + s32(b)),                 # adds (saturating)
    9: lambda a, b: _sat32(s32(a) - s32(b)),                 # subs (saturating)
    12: lambda a, b: (a + (b << 5)) & MASK,                  # addscale5
    13: lambda a, b: (a + (b << 6)) & MASK,
    14: lambda a, b: (a + (b << 7)) & MASK,
    15: lambda a, b: (a + (b << 8)) & MASK,
}

# Load/store kinds: (size, signed, store)
LDST = {'ld': (4, 0, 0), 'st': (4, 0, 1), 'ldh': (2, 0, 0), 'sth': (2, 0, 1),
        'ldb': (1, 0, 0), 'stb': (1, 0, 1), 'ldsh': (2, 1, 0), 'ldsb': (1, 1, 0)}
LDST_NAMES = D.LDST                       # 3-bit width field order

# 48-bit memory family 2f0d/2f4d/2f8d..2fed (sim.md, "48-bit memory ops"):
# opcode = (hw0 bit 7, hw1 bits 1:0); hw0 bit 6 is offset bit 1 and, when
# bit 7 is set, hw0 bit 5 is offset bit 0.

# 2dxx group (sim.md, "2dxx: gp/sp-relative"): o = hw0 bits 7:4.
# o 0-3: gp-relative, o 4-7: sp-relative; o & 3 picks the width class of
# the 32-bit 28/29/2a/2b family; o 12/13: lea gp/sp.
GP16_BASE = {0: 24, 1: 24, 2: 24, 3: 24, 4: 25, 5: 25, 6: 25, 7: 25}


def mem48_decode(ws):
    """(op, base_reg, offset) of a 48-bit 2fxx memory instruction
    (vc2dis.mem48_decode; HYP can drop the low offset bits)."""
    return D.mem48_decode(ws, HYP['mem48_offset_bits'])


def mem48_encode_offset(ws, off):
    """Store offset into a 2fxx memory instruction, keeping the opcode."""
    try:
        return D.mem48_encode_offset(ws, off)
    except D.EncodeError as e:
        raise SimError(str(e))


def ldst_28(ws):
    """32-bit 28/29/2a/2b family (and the 2dxx width classes): (op, off)."""
    cls, raw = (ws[0] >> 8) & 3, ws[1]
    return _ldst_class(cls, raw, 16)


# the halfword loads of width classes 0 and 1 under each reading (HYP)
_HALF_LOADS = {'swap': ('ldh', 'ldsh'), 'zext': ('ldh', 'ldh'),
               'apple': ('ldsh', 'ldh')}


def _ldst_class(cls, raw, bits, rule=None):
    o = sx(raw, bits)
    if cls < 2 and raw & 1:
        return _HALF_LOADS[rule or HYP['off16_h']][cls], o & ~1
    if cls == 0:
        return ('st' if raw & 2 else 'ld'), o & ~3
    if cls == 1:
        return 'sth', o & ~1
    return ('ldb' if cls == 2 else 'stb'), o


def gp16_rule():
    """The reading of the 2dxx halfword loads (HYP gp16_h)"""
    g = HYP['gp16_h']
    return HYP['off16_h'] if g == 'off16' else g


# --------------------------------------------------------------------------
# ELF (.vll) and raw-image loading.

class Image:
    """Something loaded into simulator memory: symbols plus import list."""
    def __init__(self, path):
        self.path = path
        self.symbols = {}          # name -> address
        self.sizes = {}            # name -> size
        self.imports = {}          # name -> list of call sites
        self.text = (0, 0)         # [start, end) of code


def load_vll(sim, path, base=None):
    """Map a .vll at its link address and apply its relocations.

    Relocation types (sim.md, "Relocations"): R1 word = S+A in data;
    R11 48-bit lea (field at instruction+2); R16/R17/R18 48-bit memory
    op, byte/halfword/word; R23 bl.  A section symbol contributes 0 (its
    addend is already the absolute target); a named symbol contributes
    its value.  Imports (undefined symbols) resolve to sim.import_addr()."""
    try:
        f = D.Vll(path)
    except ValueError as e:
        raise SimError(str(e))
    img = Image(path)
    lo = min(sc.addr for sc in f.sections if sc.addr and sc.flags & D.SHF_ALLOC)
    if base is not None and base != lo:
        raise SimError('relocating to a different base is not supported')
    for addr, data in f.alloc_sections():
        sim.mem.write(addr, data)
    img.text = (f.base, f.base + len(f.text))
    for sy in f.symtab:
        if sy.name and sy.shndx != 0:
            img.symbols[sy.name] = sy.value
            img.sizes[sy.name] = sy.size

    def target(si, add):
        sy = f.symtab[si]
        if sy.info & 0xf == D.STT_SECTION or not sy.name:
            return add & MASK
        if sy.shndx == 0:
            return (sim.import_addr(sy.name) + add) & MASK
        return (sy.value + add) & MASK

    for rl in f.rela:
        sy = f.symtab[rl.sym]
        if sy.shndx == 0 and sy.name:
            img.imports.setdefault(sy.name, []).append(rl.offset)
        apply_reloc(sim.mem, rl.type, rl.offset, target(rl.sym, rl.addend))
    sim.images.append(img)
    return img


def _ws(mem, a, n):
    return [mem.r16(a + 2 * k) for k in range(n)]


def _put_ws(mem, a, ws):
    for k, w in enumerate(ws):
        mem.w16(a + 2 * k, w)


def apply_reloc(mem, rt, off, tgt):
    if rt == 1:
        mem.w32(off, tgt)
    elif rt == 23:                               # bl s27 (halfwords)
        ws = _ws(mem, off, 2)
        if ws[0] & 0xf080 != 0x1080:
            raise SimError('R23 at 0x%x is not a bl' % off)
        dlt = tgt - off
        if dlt & 1:
            raise SimError('R23: odd target')
        v = (dlt >> 1) & ((1 << 27) - 1)
        ws[0] = (ws[0] & 0xf080) | ((v >> 23) & 15) << 8 | (v & 0x7f)
        ws[1] = (v >> 7) & 0xffff
        _put_ws(mem, off, ws)
    elif rt == 11:                               # lea48, field at insn+2
        a = off - 2
        ws = _ws(mem, a, 3)
        if ws[0] & 0xffe0 != 0x2f20:
            raise SimError('R11 at 0x%x is not a 48-bit lea' % off)
        v = (tgt - a) & 0x7ffffff
        _put_ws(mem, a, [ws[0], v & 0xffff, (ws[2] & 0xf800) | (v >> 16)])
    elif rt in (16, 17, 18):                     # 48-bit memory op
        ws = _ws(mem, off, 3)
        if ws[0] & 0xff00 != 0x2f00 or ws[0] & 0x60 in (0x20, 0x60) and \
                not ws[0] & 0x80:
            raise SimError('R%d at 0x%x is not a 48-bit memory op' % (rt, off))
        base = off & ~3 if HYP['pc48_mem_aligned'] else off
        _put_ws(mem, off, mem48_encode_offset(ws, tgt - base))
    else:
        raise SimError('unknown relocation type %d at 0x%x' % (rt, off))


def vmcs_exports(data):
    """Export table of a vmcs.bin image: (address, name pointer) pairs,
    sorted by name, preceded by zero words.  Returns {name: address}."""
    import re
    def name(p):
        if not 0 <= p < len(data):
            return None
        e = data.find(b'\0', p, p + 64)
        s = data[p:e] if e > 0 else b''
        return s.decode() if re.fullmatch(rb'[A-Za-z_][A-Za-z0-9_]{1,63}', s) else None
    best = {}
    o = 0
    while o + 16 <= len(data):
        run, p = {}, o
        while p + 8 <= len(data):
            a, n = struct.unpack_from('<II', data, p)
            s = name(n)
            if s is None or not a < len(data) or a & 1:
                break
            run[s] = a
            p += 8
        if len(run) > len(best):
            best = run
        o = p + 4 if p > o else o + 4
    return best


def load_vmcs(sim, path):
    """Map a raw vmcs.bin at 0 (entry 0x200) and read gp from its reset
    code ("mov r24, #imm32" at 0x200); exports become symbols."""
    data = open(path, 'rb').read()
    sim.mem.write(0, data)
    img = Image(path)
    img.text = (0, len(data))
    img.symbols = vmcs_exports(data)
    ins = D.decode(lambda a: sim.mem.r16(a), 0x200)
    if ins and ins.e.name == 'mov32' and (ins.ws[0] & 31) == 24:
        img.gp = D.getf(ins.ws, ins.e.fields['i'].bits)
        sim.r[24] = img.gp
    sim.images.append(img)
    sim.vmcs = img
    return img


# --------------------------------------------------------------------------
# The CPU.

class Sim:
    def __init__(self, poison=True, assume=None, trace=None):
        self.mem = Memory()
        self.r = [0] * 32
        self.vrf = V.VRF()
        self.code = {}             # addr -> (handler, size, ins)
        self.code_view = None      # memory at the last cache flush (strict
                                   # code coherence), or None: unchecked
        self.images = []
        self.vmcs = None
        self.stubs = {}            # addr -> (name, fn)
        self.stub_names = {}       # name -> addr
        self.heap = HEAP_BASE
        self.poison = poison
        self.trace = trace         # file-like or None
        self.steps = 0
        self.alu = dict(ALU_FN)
        self.unknown_imports = set()
        self.link_vmcs = set()     # import names resolved into vmcs.bin
        self.sysregs = {}          # sysN selector -> value read into rd
        self.on_sleep = None       # fn(next pc) -> pc, for boot runs
        self.irqs = []             # (vector, interrupted pc) taken so far
        self.irq_ctx = None        # {(pc, sp): r0-r23} while interrupted
        self.irq_returns = [0, 0]  # returns that restored every register / not
        self.exceptions = False    # True: faults enter VC II exceptions
        self.excs = []             # (vector, faulting pc, message) taken
        self.compiling = None      # the instruction compile_ins is on
        self.pend = []             # deferred exception: [vector, insns left,
                                   # faulting pc, message] (misaligned load)
        for spec in assume or ():
            n, _, cand = spec.partition('=')
            self.alu[int(n)] = ALU_CANDIDATES[cand]
        self.counts = collections.Counter() if trace is not None else None

    def check_irq_return(self, pc):
        """Called by rti: if this returns to an interrupted context,
        count whether r0-r23 and sp came back as they were."""
        saved = self.irq_ctx.pop((pc, self.r[25]), None)
        if saved is not None:
            self.irq_returns[0 if saved == self.r[:24] else 1] += 1

    # ---- symbols and stubs --------------------------------------------

    def sym(self, name):
        for img in self.images:
            if name in img.symbols:
                return img.symbols[name]
        raise KeyError(name)

    def import_addr(self, name):
        if name in self.link_vmcs and self.vmcs and name in self.vmcs.symbols:
            return self.vmcs.symbols[name]
        if name not in self.stub_names:
            a = STUB_BASE + 4 * len(self.stub_names)
            self.stub_names[name] = a
            fn = STUBS.get(name)
            self.stubs[a] = (name, fn)
            if fn is None:
                self.unknown_imports.add(name)
        return self.stub_names[name]

    def add_stub(self, name, fn):
        a = self.import_addr(name)
        self.stubs[a] = (name, fn)
        return a

    def malloc(self, size, align=4):
        align = max(4, align)
        a = (self.heap + align - 1) & ~(align - 1)
        if a + size > HEAP_END:
            raise SimError('simulator heap exhausted')
        self.heap = a + size
        self.mem.write(a, bytes(size))
        return a

    # ---- decode/compile -----------------------------------------------

    def fetch(self, pc):
        h = self.code.get(pc)
        if h is None:
            ins = D.decode(self._rd16, pc)
            if ins is None:
                raise Undefined('undecodable instruction at 0x%08x (%04x)' %
                                (pc, self.mem.r16(pc)))
            self.compiling = ins       # mem_op's form, for strict alignment
            fn = compile_ins(self, ins)
            self.compiling = None
            if COVER is not None:
                fn = _counted(fn, cover_key(ins))
            h = (fn, ins.size, ins)
            v = self.code_view
            if v is not None and pc + ins.size <= len(v) and \
                    v[pc:pc + ins.size] != self.mem.b[pc:pc + ins.size]:
                raise StaleCode('instruction at 0x%08x (%s) written since the '
                                'last cache flush: silicon unknown; Apple '
                                'flushes before running written code' %
                                (pc, ins.text))
            self.code[pc] = h
        return h

    def cache_flushed(self, size=0x140000):
        """A whole-cache flush (0x10000C00 bit 30): memory as it is now
        becomes what instruction fetches may read (strict code coherence),
        and every decoded instruction is dropped."""
        self.code_view = bytes(self.mem.b[0:size])   # the RAM itself, past
                                                     # any test's read hook
        self.code.clear()

    def _rd16(self, a):
        try:
            return self.mem.r16(a)
        except SimError:
            return None

    def invalidate(self, lo, hi):
        for a in [a for a in self.code if lo <= a < hi]:
            del self.code[a]

    # ---- running ------------------------------------------------------

    def run(self, pc, max_steps=10_000_000):
        r = self.r
        steps = 0
        code = self.code
        stubs = self.stubs
        tr = self.trace
        pend = self.pend
        while True:
            if pc == RET_MAGIC:
                break
            if pc in stubs:
                pc = self._call_stub(pc)
                continue
            h = None
            try:
                h = code.get(pc) or self.fetch(pc)
                r[31] = pc
                if tr is not None:
                    before = list(r)
                npc = h[0]()
            except SimError as e:
                pc = fault(self, e, pc, h)
                steps += 1
                continue
            if tr is not None:
                self._trace(pc, h[2], before)
            pc = (pc + h[1]) if npc is None else npc & MASK
            if pend:
                pend[1] -= 1
                if pend[1] <= 0:
                    pc = deferred_take(self, pc)
            steps += 1
            if steps >= max_steps:
                raise SimError('step limit (%d) reached at 0x%08x' % (max_steps, pc))
            if r[25] < STACK_LIMIT and r[25] > HEAP_END:
                raise SimError('stack overflow (sp=0x%08x)' % r[25])
        self.steps += steps
        return steps

    def _trace(self, pc, ins, before):
        d = ['%s=%08x' % (D.reg(k), v) for k, (v, b) in
             enumerate(zip(self.r, before)) if v != b and k != 31]
        self.trace.write('%08x  %-24s %-36s %s\n' % (
            pc, ' '.join('%04x' % w for w in ins.ws), ins.text, ' '.join(d)))

    def _call_stub(self, pc):
        name, fn = self.stubs[pc]
        if fn is None:
            raise Unimplemented('import %s is not simulated (no stub)' % name)
        r = self.r
        ret = fn(self, *r[0:6])
        if self.poison:                  # caller-saved registers are dead
            for k in range(1, 6):
                r[k] = 0xdead0000 | k
        if isinstance(ret, tuple):
            r[0], r[1] = ret[0] & MASK, ret[1] & MASK
        elif ret is not None:
            r[0] = ret & MASK
        elif self.poison:
            r[0] = 0xdead0000
        if self.trace is not None:
            self.trace.write('          <stub %s -> %08x>\n' % (name, r[0]))
        return r[26]

    def call(self, func, *args, stack_args=(), max_steps=10_000_000,
             check_abi=False, regs=None):
        """Call func (name or address) with args in r0-r5 and any further
        arguments on the stack (first at [sp]); return r0.  r1 is in
        self.r[1] for 64-bit results."""
        addr = self.sym(func) if isinstance(func, str) else func
        if len(args) > 6:
            stack_args = tuple(args[6:]) + tuple(stack_args)
            args = args[:6]
        r = self.r
        for k in range(6):
            r[k] = (args[k] if k < len(args) else 0xdead00a0 | k) & MASK
        for k, v in (regs or {}).items():
            r[k] = v & MASK
        sp = STACK_TOP - 4 * len(stack_args)
        for k, v in enumerate(stack_args):
            self.mem.w32(sp + 4 * k, v)
        r[25] = sp
        r[26] = RET_MAGIC
        saved = None
        # r6-r23 are callee-saved; registers passed in regs (global
        # register variables such as the bitstream cursors) are exempt.
        keep = set(regs or ()) | {24}
        if check_abi:
            for k in range(6, 24):
                if k not in keep:
                    r[k] = random.getrandbits(32)
            saved = list(r)
        self.run(addr, max_steps)
        if r[25] != sp:
            raise SimError('sp not restored: 0x%08x != 0x%08x' % (r[25], sp))
        if check_abi:
            bad = [k for k in range(6, 24) if k not in keep and r[k] != saved[k]]
            if bad:
                raise SimError('callee-saved registers clobbered: %s' %
                               ', '.join(D.reg(k) for k in bad))
        return r[0]


# --------------------------------------------------------------------------
# Instruction compiler: turns a decoded vc2dis instruction into a closure
# that executes it and returns the next pc (None = fall through).

def fault(sim, e, pc, h):
    """An instruction at pc (decoded as h, or None if fetching it failed)
    raised e.  With sim.exceptions on and e a Misaligned/BusFault/
    Undefined, enter the exception and return the handler's pc; otherwise
    re-raise e with the location appended."""
    hyp = getattr(e, 'hyp', None)
    if not (sim.exceptions and hyp) or isinstance(e, (VectorOp, Unimplemented)):
        where = h[2].text if h else 'fetch'
        raise type(e)('%s\n  at 0x%08x: %s' % (e, pc, where)) from None
    if sim.pend:
        # a deferred exception is pending: it is taken first, and this
        # instruction (not run) is where it resumes
        return deferred_take(sim, pc)
    return take_exception(sim, HYP[hyp], pc, str(e))


def take_exception(sim, vector, pc, why='', saved=None):
    """Enter exception `vector` for the instruction at pc: push sr and
    the saved pc (saved, default pc, plus HYP['exc_pc_offset']) in the
    frame rti pops (sr at sp, pc at sp + 4, as take_irq), clear the
    interrupt-enable bit (30) and jump through the word at 4 * vector.
    Taken whatever sr says (a synchronous fault).  A fault while pushing
    the frame or reading the vector is a double fault: SimError."""
    r = sim.r
    try:
        sp = (r[25] - 8) & MASK
        sim.mem.w32(sp, r[30])
        sim.mem.w32(sp + 4, ((pc if saved is None else saved)
                             + HYP['exc_pc_offset']) & MASK)
        npc = sim.mem.r32(4 * vector)
    except SimError as e2:
        raise SimError('double fault: %s (while entering exception %d for '
                       '0x%08x: %s)' % (e2, vector, pc, why)) from None
    r[25] = sp
    r[30] &= ~(1 << 30)
    sim.excs.append((vector, pc, why))
    return npc


def _counted(fn, key):
    def h():
        COVER[key] += 1
        return fn()
    return h


def cover_key(ins):
    """Instruction kind for coverage: encoding class plus operation."""
    n, t = ins.e.name, ins.text
    if n == 'mem48':
        op, b, _ = mem48_decode(ins.ws)
        return 'mem48 %s%s' % (op, ' pc' if b == 31 else '')
    if n == 'gp16':
        o4 = (ins.ws[0] >> 4) & 15
        if o4 in GP16_BASE:
            op, _ = _ldst_class(o4 & 3, ins.ws[1], 17, gp16_rule())
            return 'gp16 %s %s' % (op, D.reg(GP16_BASE[o4]))
        return 'gp16 o%d' % o4
    if n == 'ldst_off16':
        return 'ldst_off16 %s' % ldst_28(ins.ws)[0]
    if n in ('addcmpb', 'bcc', 'bcc.s', 'scc'):
        c = fval(ins, 'c')
        return '%s %s' % (n, D.COND[c] or 'al')
    if n == 'alu3' and (ins.ws[0] & 0xffe0) == 0x2e80:
        return 'alu3x %s' % t.split()[0]
    if n in ('alu_reg', 'alu_imm5', 'alu_imm16', 'alu3'):
        return '%s %s' % (n, t.split()[0])
    if n in ('ldst_ind', 'ldst_postinc'):
        return '%s %s%s' % (n, t.split()[0], ' idx' if '+r' in t else '')
    if n in ('vec48', 'vec80'):
        return '%s %s' % (getattr(ins.e, 'vclass', n), t.split()[0])
    return n + ('' if n in ('push', 'pop') else ' ' + t.split()[0])


def fval(ins, k):
    """One field as a number (vc2dis I.values()); targets masked."""
    v = ins.values()[k]
    return v & MASK if isinstance(ins.e.fields[k], D.Target) else v


def _load(sim, op):
    m = sim.mem
    size, signed, _ = LDST[op]
    if size == 4:
        return m.r32
    if size == 2:
        return (lambda a: sx(m.r16(a), 16) & MASK) if signed else m.r16
    return (lambda a: sx(m.r8(a), 8) & MASK) if signed else m.r8


def _store(sim, op):
    m = sim.mem
    return {4: m.w32, 2: m.w16, 1: m.w8}[LDST[op][0]]


def mem_op(sim, op, d, addr_fn, wb=False):
    """Closure for a load/store of register d at addr_fn().  wb: a
    pre-decrement or post-increment form (the deferral window follows the
    encoding: (--rs) 2, every other form 4)."""
    r = sim.r
    ins = sim.compiling
    form = (ins.e.name if ins is not None else '?', op,
            ins is not None and 'x' in ins.e.fields)
    known = form in STRICT_SCALAR_KNOWN
    what = '%s %s' % (op, form[0])
    predec = form[0] == 'ldst_predec'
    if LDST[op][2]:
        st = _store(sim, op)
        size = LDST[op][0]

        def h():
            a = addr_fn() & MASK
            try:
                st(a, r[d])
            except Misaligned as e:
                if not known:
                    align_unknown(sim, what, a)
                if not (sim.exceptions and HYP['exc_mis_store_rounds']):
                    raise
                # silicon (bench row ALGN, 2026-10-25): the store lands at
                # the rounded-down address; vector 1 comes later
                st(a & ~(size - 1), r[d])
                defer_exception(sim, HYP['exc_vec_misaligned'], r[31],
                                HYP['exc_defer_insns_wb' if predec else 'exc_defer_insns'],
                                str(e))
    else:
        ld = _load(sim, op)
        size = LDST[op][0]

        def h():
            a = addr_fn() & MASK
            try:
                r[d] = ld(a)
            except Misaligned as e:
                if not known:
                    align_unknown(sim, what, a)
                if not (sim.exceptions and HYP['exc_mis_load_rounds']):
                    raise
                # silicon (device sessions 2-3): the access completes at
                # the rounded-down address; vector 1 comes later
                r[d] = ld(a & ~(size - 1))
                defer_exception(sim, HYP['exc_vec_misaligned'], r[31],
                                HYP['exc_defer_insns_wb' if predec else 'exc_defer_insns'],
                                str(e))
    return h


def defer_exception(sim, vector, pc, n, why=''):
    """Arm exception `vector` for the instruction at pc, to be taken
    after it and n more instructions have run (the run loops count them
    down; deferred_take enters it).  A second fault meanwhile takes this
    one at once."""
    sim.pend[:] = [vector, n + 1, pc, why]


def deferred_take(sim, npc):
    """The pending exception's window is over (or another fault came):
    enter it with the saved pc = npc, the first instruction not run."""
    vector, _, fpc, why = sim.pend
    del sim.pend[:]
    return take_exception(sim, vector, fpc, why, saved=npc)


def compile_ins(sim, ins):
    r = sim.r
    e = ins.e
    n = e.name
    syn = e.syntax
    mn = syn.split(' ')[0]
    pc = ins.addr
    nxt = pc + ins.size
    alu = sim.alu

    def setreg(d):
        """Writer for rd, handling rd = pc (a jump)."""
        if d == 31:
            return None
        return d

    # ---- control flow
    if n == 'rts':
        return lambda: r[26]
    if n in ('nop',):
        return lambda: None
    if n in ('ei', 'di'):
        bit = 1 << 30                     # interrupt enable (bookkeeping only)

        def h():
            r[30] = (r[30] | bit) if n == 'ei' else (r[30] & ~bit)
        return h
    if n == 'misc4' and fval(ins, 's') in sim.sysregs:
        # "sysN rd" selectors that read a value into rd (boot runs model
        # them through sim.sysregs; hardware.md, "sys19")
        sel, d = fval(ins, 's'), fval(ins, 'd')

        def h():
            r[d] = sim.sysregs[sel] & MASK
        return h
    if n == 'rti':
        # Return from interrupt: sr = [sp], pc = [sp+4], sp += 8 (VC IV's
        # frame).  vmcs.bin starts its first task this way, from a frame
        # built with sr = 0x40000000 (interrupts on) above the entry point.
        mem = sim.mem

        def h():
            sp = r[25]
            r[30] = mem.r32(sp)
            r[25] = (sp + 8) & MASK
            npc = mem.r32(sp + 4)
            if sim.irq_ctx is not None:
                sim.check_irq_return(npc)
            return npc
        return h
    if n == 'sleep' and sim.on_sleep is not None:
        return lambda: sim.on_sleep(nxt)
    if n in ('sleep', 'bkpt', 'misc4', 'op2ec', 'op24'):
        def h():
            raise Unimplemented('%s not simulated: %s' % (n, ins.text))
        return h
    if n in ('vec48', 'vec80'):
        return V.compile_vector(sim, ins)
    if n == 'bl' and ins.size == 2:
        d = fval(ins, 'd')

        def h():
            r[26] = nxt
            return r[d]
        return h
    if n == 'bl':
        t = fval(ins, 't')

        def h():
            r[26] = nxt
            return t
        return h
    if n in ('bcc', 'bcc.s'):
        t, c = fval(ins, 't'), fval(ins, 'c')
        if c == 14:
            return lambda: t
        tab = COND[c]
        return lambda: t if tab[r[30] & 15] else None
    if n in ('tbb', 'tbh'):
        d = fval(ins, 'd')
        m = sim.mem
        if n == 'tbb':
            return lambda: nxt + 2 * m.r8(nxt + r[d])
        return lambda: nxt + 2 * sx(m.r16(nxt + 2 * r[d]), 16)
    if n == 'addcmpb':
        c, d, t = fval(ins, 'c'), fval(ins, 'd'), fval(ins, 't')
        tab = COND[c]
        areg = '{a}' in syn and '#{a}' not in syn
        breg = '{b}' in syn and '#{b}' not in syn
        a, b = fval(ins, 'a'), fval(ins, 'b')

        def h():
            v = (r[d] + (r[a] if areg else a)) & MASK
            r[d] = v
            return t if tab[flags_sub(v, r[b] if breg else b & MASK)] else None
        return h
    if n == 'scc':
        c, d = fval(ins, 'c'), fval(ins, 'd')
        tab = COND[c]

        def h():
            r[d] = 1 if tab[r[30] & 15] else 0
        return h

    # ---- push/pop.  Block order: highest register at the lowest address,
    # lr (pc) on top (HYP push_lowest_first = False; hardware.md, "push order")
    if n in ('push', 'pop'):
        h0 = ins.ws[0]
        lr, base, m_ = (h0 >> 7) & 1, (0, 6, 16, 24)[(h0 >> 5) & 3], h0 & 31
        regs = [] if base == 24 and m_ == 15 else [(base + k) & 31 for k in range(m_ + 1)]
        mem = sim.mem
        if not HYP['push_lowest_first']:
            regs = regs[::-1]
        if n == 'push':
            lst = regs + ([26] if lr else [])

            def h():
                sp = r[25] - 4 * len(lst)
                for k, q in enumerate(lst):
                    mem.w32(sp + 4 * k, r[q])
                r[25] = sp
            return h
        lst = regs + ([31] if lr else [])

        def h():
            sp = r[25]
            ret = None
            for k, q in enumerate(lst):
                v = mem.r32(sp + 4 * k)
                if q == 31:
                    ret = v
                else:
                    r[q] = v
            r[25] = sp + 4 * len(lst)
            return ret
        return h

    # ---- sp-relative and small helpers
    if n == 'addsp':
        i = fval(ins, 'i')

        def h():
            r[25] = (r[25] + i) & MASK
        return h
    if n == 'lea' and ins.size == 2:
        d, t = fval(ins, 'd'), fval(ins, 't')

        def h():
            r[d] = t
        return h
    if n == 'ldst_sp':
        d, o = fval(ins, 'd'), fval(ins, 'o')
        if mn == 'lea':
            def h():
                r[d] = (r[25] + o) & MASK
            return h
        if mn in ('ld', 'st'):
            return mem_op(sim, mn, d, lambda: r[25] + o)
    if n == 'cmp0':
        d = fval(ins, 'd')

        def h():
            r[30] = (r[30] & ~15) | flags_sub(r[d], 0)
        return h

    # ---- loads and stores
    if n == 'ldst_postinc':
        w, d, s = fval(ins, 'w'), fval(ins, 'd'), fval(ins, 's')
        op = LDST_NAMES[w]
        size = LDST[op][0]
        inner = mem_op(sim, op, d, lambda: r[s], wb=True)

        def h():
            inner()
            r[s] = (r[s] + size) & MASK
        return h
    if n == 'ldst_predec':
        # rs -= size, then access at rs (VC IV semantics; isa.md).  A
        # misaligned load still writes rs back (silicon, probe v2 check 9:
        # r5 0x7f -> 0x7b); a precise fault (store, bus) leaves it.
        w, d, s = fval(ins, 'w'), fval(ins, 'd'), fval(ins, 's')
        op = LDST_NAMES[w]
        size = LDST[op][0]
        cell = [0]

        def addr():
            cell[0] = (r[s] - size) & MASK
            return cell[0]
        inner = mem_op(sim, op, d, addr, wb=True)

        def h():
            inner()
            r[s] = cell[0]          # rd == rs: write-back wins, as for (rs)++
        return h
    if n == 'ldst_ind':
        w, d, s = fval(ins, 'w'), fval(ins, 'd'), fval(ins, 's')
        op = LDST_NAMES[w]
        if 'x' in e.fields:
            x = fval(ins, 'x')
            size = LDST[op][0] if HYP['index_scaled'] else 1  # scaled by width
            return mem_op(sim, op, d, lambda: r[s] + size * r[x])
        return mem_op(sim, op, d, lambda: r[s])
    if n == 'ldst_off':
        d, s, o = fval(ins, 'd'), fval(ins, 's'), fval(ins, 'o')
        return mem_op(sim, mn, d, lambda: r[s] + o)
    if n == 'ldst_off16':
        d, s = fval(ins, 'd'), fval(ins, 's')
        op, o = ldst_28(ins.ws)
        return mem_op(sim, op, d, lambda: r[s] + o)
    if n == 'gp16':
        o4 = (ins.ws[0] >> 4) & 15
        d = ins.ws[0] & 15
        raw = ins.ws[1]
        if o4 in (12, 13):
            base = 24 if o4 == 12 or not HYP['gp16_sp'] else 25
            off = raw                         # unsigned 16-bit offset

            def h():
                r[d] = (r[base] + off) & MASK
            return h
        if o4 in GP16_BASE:
            base = GP16_BASE[o4] if HYP['gp16_sp'] else 24
            op, off = _ldst_class(o4 & 3, raw, 17, gp16_rule())  # unsigned
            return mem_op(sim, op, d, lambda: r[base] + off)

        def h():
            raise Unimplemented('2dxx op %d not simulated: %s' % (o4, ins.text))
        return h
    if n == 'mem48':
        op, b, off = mem48_decode(ins.ws)
        d = ins.ws[0] & 31
        if b == 31:
            base = pc & ~3 if HYP['pc48_mem_aligned'] else pc
            a = (base + off + (0 if HYP['pc48_base_insn'] else 6)) & MASK
            return mem_op(sim, op, d, lambda: a)
        return mem_op(sim, op, d, lambda: r[b] + off)
    if n == 'lea48':
        d = fval(ins, 'd')
        if 't' in e.fields:
            t = (fval(ins, 't') + (0 if HYP['pc48_base_insn'] else 6)) & MASK

            def h():
                r[d] = t
            return h
        b, o = fval(ins, 'b'), fval(ins, 'o')

        def h():
            r[d] = ((pc if b == 31 else r[b]) + o) & MASK
        return h
    if n == 'mov32':
        d, i = fval(ins, 'd'), fval(ins, 'i') & MASK

        def h():
            r[d] = i
        return h if d != 31 else (lambda: i)

    # ---- ALU
    if n == 'add16':
        d, s, i = fval(ins, 'd'), fval(ins, 's'), fval(ins, 'i')

        def h():
            r[d] = (r[s] + i) & MASK
        return h
    if n == 'alu_imm16':
        op, d, i = fval(ins, 'op'), fval(ins, 'd'), fval(ins, 'i') & MASK
        return alu_closure(sim, [16, 10, 12, 14][op], d, d, None, i, ins)
    if n == 'alu_imm5':
        q, d, i = fval(ins, 'op'), fval(ins, 'd'), fval(ins, 'i') & MASK
        return alu_closure(sim, 2 * q, d, d, None, i, ins)
    if n == 'alu_reg':
        op, d, s = fval(ins, 'op'), fval(ins, 'd'), fval(ins, 's')
        return alu_closure(sim, op, d, d, s, None, ins)
    if n == 'alu3':
        ext = (ins.ws[0] & 0xffe0) == 0x2e80
        d = fval(ins, 'd')
        a = fval(ins, 'a') if 'a' in e.fields else d
        if '#{b}' in syn:
            b, imm = None, fval(ins, 'b') & MASK
        else:
            b, imm = fval(ins, 'b'), None
        if mn == 'mov':
            op = 14
        else:
            op = fval(ins, 'op')
        if ext:
            return ext_closure(sim, op, d, a, b, imm, ins)
        return alu_closure(sim, op, d, a, b, imm, ins)

    def h():
        raise Unimplemented('no semantics for %s (%s)' % (n, ins.text))
    return h


def alu_closure(sim, op, d, a, b, imm, ins):
    """rd = ra op (rb | imm), or a flag update for compare-type ops."""
    r = sim.r
    if op in ALU_FLAGS:
        ff = ALU_FLAGS[op]
        if b is None:
            def h():
                r[30] = ff(r[a], imm, r[30])
        else:
            def h():
                r[30] = ff(r[a], r[b], r[30])
        return h
    fn = sim.alu.get(op)
    if fn is None:
        def h():
            raise Unimplemented('ALU op %d (%s) has no known semantics' %
                                (op, ins.text))
        return h
    if op in UNARY and not HYP['unary_from_rs'] and b is not None:
        b = a
    if HYP['alu_sets_flags'] and d != 31:
        def h():
            v = fn(r[a], imm if b is None else r[b])
            r[d] = v
            r[30] = (r[30] & ~(FZ | FN)) | (FZ if v == 0 else 0) | (FN if v >> 31 else 0)
        return h
    if d == 31:
        if b is None:
            return lambda: fn(r[a], imm)
        return lambda: fn(r[a], r[b])
    if b is None:
        def h():
            r[d] = fn(r[a], imm)
    else:
        def h():
            r[d] = fn(r[a], r[b])
    return h


def ext_closure(sim, op, d, a, b, imm, ins):
    r = sim.r
    fn = EXT_FN.get(op)
    if fn is None:
        def h():
            raise Unimplemented('extended ALU op %d (%s) not simulated' % (op, ins.text))
        return h
    if b is None:
        def h():
            r[d] = fn(r[a], imm)
    else:
        def h():
            r[d] = fn(r[a], r[b])
    return h


# --------------------------------------------------------------------------
# Import stubs (Python implementations of vmcs.bin services the .vll
# libraries call).  Signature: fn(sim, r0, r1, r2, r3, r4, r5) -> r0 or
# (r0, r1) or None (void).  Argument orders were read from call sites.

def _memmove(sim, d, s, n, *_):
    sim.mem.write(d, sim.mem.read(s, n))
    return d


def _memset(sim, d, c, n, *_):
    sim.mem.write(d, bytes([c & 0xff]) * n)
    return d


def _memset2(sim, d, v, n, *_):
    sim.mem.write(d, struct.pack('<H', v & 0xffff) * n)
    return d


def _malloc_priority(sim, size, align, prio, desc, *_):
    return sim.malloc(size, align if align and align & (align - 1) == 0 else 4)


def _modu(sim, a, b, *_):
    if b == 0:
        raise SimError('__modu by zero')
    return a % b


def _rem(sim, a, b, *_):
    a, b = s32(a), s32(b)
    if b == 0:
        raise SimError('__rem by zero')
    q = abs(a) // abs(b)
    q = q if (a < 0) == (b < 0) else -q
    return (a - q * b) & MASK


STUBS = {
    'memcpy': _memmove,          # memcpy(dst, src, n) -> dst
    'memmove': _memmove,
    'vclib_memcpy': _memmove,    # overlapping use in remove_byte3: memmove
    'memset': _memset,           # memset(dst, c, n) -> dst
    'vclib_memset2': _memset2,   # (dst, halfword, count) - assumed
    'malloc_priority': _malloc_priority,  # (size, align, priority, desc)
    'malloc': lambda sim, n, *_: sim.malloc(n),
    'malloc_256bit': lambda sim, n, *_: sim.malloc(n, 32),
    'free': lambda sim, *_: None,
    'free_256bit': lambda sim, *_: None,
    '__modu': _modu,             # unsigned a % b
    '__rem': _rem,               # signed a % b (C semantics)
    'vclib_obtain_VRF': lambda sim, *_: 0,
    'vclib_release_VRF': lambda sim, *_: None,
    'vclib_dcache_flush': lambda sim, *_: None,
    'univ_setleds': lambda sim, *_: None,
    'univ_clearleds': lambda sim, *_: None,
}


# --------------------------------------------------------------------------
# Command line.

def find_corpus(path):
    """Return (library_dir, vmcs_path or None) for a VideoCore directory
    or its Library subdirectory."""
    for lib in (os.path.join(path, 'Library'), path):
        if glob.glob(os.path.join(lib, '*.vll')):
            boot = os.path.join(os.path.dirname(lib.rstrip('/')), 'Boot', 'vmcs.bin')
            return lib, boot if os.path.exists(boot) else None
    raise SystemExit('no .vll files under %s' % path)


def make_sim(path, vmcs=None, **kw):
    sim = Sim(**kw)
    if vmcs:
        load_vmcs(sim, vmcs)
    if path.endswith('.vll'):
        load_vll(sim, path)
    elif path.endswith('.bin') and not vmcs:
        load_vmcs(sim, path)
    return sim


# --------------------------------------------------------------------------
# Booting a raw image against modelled peripherals (vc2mmio.Bus).

import vc2mmio as M  # noqa: E402

Bus = M.make_bus(Memory, SimError, Misaligned)
M.DmaEngine.strict = STRICT_ALIGN       # strict alignment (set_strict_align)
M.DmaEngine.trap = AlignTrap


BOOT_MEM_SIZE = 0x4000000       # 0-32 MB as before, 32-64 MB: SDRAM backing
SDRAM_BACKING = 0x2000000


# The minimum model under which vmcs.bin boots (hardware.md, "Simulator
# model"): two status bits that real hardware sets when an operation
# completes, and the 4 MB SDRAM window.  --bcm selects it.
BCM_RULES = ('0x10000c00|=1@1',          # cache flush: bit 0 = done
             '0x10003420|=0x8000@1')     # SDRAM command: bit 15 = done
BCM_ALIASES = ('0xfe000000-0xfe3fffff:0x%x' % 0x2000000,)


def make_boot_sim(path, rules=(), aliases=(), poll_limit=2000, trace=None,
                  machine=False, us_per_read=1):
    """A Sim whose memory is a logging peripheral bus, with a raw image
    (vmcs.bin or our own) loaded at 0.  Rules are vc2mmio strings.  With
    machine=True the timer and interrupt controller are modelled
    (vc2mmio.SysTimer, IntCtl) and interrupts are delivered."""
    sim = Sim(trace=trace)
    sim.mem = Bus(sim, BOOT_MEM_SIZE, poll_limit=poll_limit)
    sim.sysregs[19] = 0          # read before clock setup; low nibble < 4 (L)
    load_vmcs(sim, path)
    for a in aliases:
        rng, _, base = a.partition(':')
        lo, _, hi = rng.partition('-')
        sim.mem.alias(int(lo, 0), int(hi, 0), int(base, 0))
    for r in rules:
        sim.mem.rule(r)
    if machine:
        t = sim.mem.attach(M.SysTimer(us_per_read))
        sim.mem.attach(M.IntCtl(t.sources))
    return sim


class Idle(Exception):
    pass


def take_irq(sim, pc):
    """Enter the highest pending interrupt if sr allows it: push sr and
    the return pc (the frame rti pops), clear the enable bit (30), jump
    through the vector table entry at 4 * vector.  Returns the new pc or
    None."""
    ic = sim.mem.device(M.IntCtl)
    if ic is None or not (sim.r[30] >> 30) & 1:
        return None
    b = ic.pending()
    if b is None:
        return None
    v = 32 + b[1]
    if sim.irq_ctx is not None:
        sim.irq_ctx[(pc, sim.r[25])] = sim.r[:24]
    sp = (sim.r[25] - 8) & MASK
    sim.mem.w32(sp, sim.r[30])
    sim.mem.w32(sp + 4, pc)
    sim.r[25] = sp
    sim.r[30] &= ~(1 << 30)
    sim.irqs.append((v, pc))
    return sim.mem.r32(4 * v)


def _sleep(sim, nxt):
    """sleep: let time pass to the next timer event and take its
    interrupt; with nothing armed (or interrupts off) the image is idle."""
    t = sim.mem.device(M.SysTimer)
    d = t.next_event() if t else None
    if d is not None:
        t.advance(d)
    npc = take_irq(sim, nxt)
    if npc is None:
        raise Idle('idle: sleep with no interrupt to come')
    return npc


def run_machine(sim, pc, max_steps, every=32, instr_per_us=100, stop=False):
    """Sim.run with time and interrupts: every `every` instructions the
    timer advances (instr_per_us instructions per microsecond) and a
    pending, enabled interrupt is taken.  With stop=True, reaching
    max_steps (rounded up to `every`) returns the pc instead of raising,
    so a harness can run the core in slices (bcmfw tests)."""
    r = sim.r
    code = sim.code
    t = sim.mem.device(M.SysTimer)
    steps = 0
    frac = 0
    tr = sim.trace
    pend = sim.pend
    while steps < max_steps:
        for _ in range(every):
            h = None
            try:
                h = code.get(pc) or sim.fetch(pc)
                r[31] = pc
                if tr is not None:
                    before = list(r)
                npc = h[0]()
            except SimError as e:
                try:
                    pc = fault(sim, e, pc, h)
                except SimError:
                    sim.steps += steps
                    raise
                continue
            if tr is not None:
                sim._trace(pc, h[2], before)
            pc = (pc + h[1]) if npc is None else npc & MASK
            if pend:
                pend[1] -= 1
                if pend[1] <= 0:
                    pc = deferred_take(sim, pc)
        steps += every
        if t is not None:
            frac += every
            if frac >= instr_per_us:
                t.advance(frac // instr_per_us)
                frac %= instr_per_us
        npc = take_irq(sim, pc)
        if npc is not None:
            pc = npc
    sim.steps += steps
    r[31] = pc
    if stop:
        return pc
    raise SimError('step limit (%d) reached at 0x%08x' % (max_steps, pc))


def boot(sim, entry=0x200, max_steps=5_000_000):
    """Run from entry until the image idles (sleep with nothing to wait
    for), stalls in a polling loop, faults, or hits the step limit.
    Returns (reason, pc)."""
    sim.r[31] = entry
    try:
        if sim.mem.device(M.SysTimer) is not None:
            sim.on_sleep = lambda nxt: _sleep(sim, nxt)
            run_machine(sim, entry, max_steps)
        else:
            sim.run(entry, max_steps)
        return 'returned', sim.r[31]
    except Idle as e:
        return str(e), sim.r[31]
    except M.PollStall as e:
        return str(e), e.acc.pc
    except Unimplemented as e:
        msg = str(e)
        if msg.startswith('sleep'):
            return 'idle (sleep)', sim.r[31]
        return msg.replace('\n', ' |'), sim.r[31]
    except SimError as e:
        return str(e).replace('\n', ' |'), sim.r[31]


def boot_main(args):
    """--boot IMAGE [options]: run a raw image from 0x200 against the
    modelled bus and print every device access (see the usage text)."""
    opts = {'--reg': [], '--alias': [], '--max': ['5000000'], '--poll': ['2000'],
            '--entry': ['0x200'], '--then': [], '--hide': [], '--panel': []}
    flags = {'--trace': False, '--summary': False, '--machine': False,
             '--bcm': False, '--lcd': False}
    rest = []
    k = 0
    while k < len(args):
        a = args[k]
        if a in opts:
            opts[a].append(args[k + 1])
            k += 2
        elif a in flags:
            flags[a] = True
            k += 1
        else:
            rest.append(a)
            k += 1
    rules, aliases = list(opts['--reg']), list(opts['--alias'])
    if flags['--bcm']:
        rules = list(BCM_RULES) + rules
        aliases = list(BCM_ALIASES) + aliases
        flags['--machine'] = True
    sim = make_boot_sim(rest[0], rules, aliases, int(opts['--poll'][-1], 0),
                        trace=sys.stdout if flags['--trace'] else None,
                        machine=flags['--machine'])
    reason, pc = boot(sim, int(opts['--entry'][-1], 0), int(opts['--max'][-1], 0))
    bus = sim.mem
    marks = [(0, 'boot from 0x%x' % int(opts['--entry'][-1], 0))]
    if opts['--panel']:
        bus.rule(panel_rule(int(opts['--panel'][-1], 0)))
    for spec in opts['--then']:
        f, *a = [int(x, 0) for x in spec.split(',')]
        marks.append((len(bus.log), 'call 0x%x(%s)' % (f, ', '.join('%d' % s32(x) for x in a))))
        sim.call(f, *[x & MASK for x in a], max_steps=50_000_000)
    hide = []
    for h in opts['--hide']:
        lo, _, hi = h.partition('-')
        hide.append((int(lo, 0), int(hi or lo, 0)))
    for i, (start, label) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else len(bus.log)
        log = [acc for acc in bus.log[start:end]
               if not any(lo <= acc.addr <= hi for lo, hi in hide)]
        print('---- %s' % label)
        if flags['--lcd']:
            for line in M.describe_lcd(log):
                print('  ' + line)
            continue
        if flags['--summary']:
            st = collections.OrderedDict()
            for acc in log:
                e = st.setdefault(acc.addr, [0, 0, []])
                e[0 if acc.rw == 'R' else 1] += 1
                if acc.rw == 'W' and acc.value not in e[2] and len(e[2]) < 8:
                    e[2].append(acc.value)
            for a, (nr, nw, vals) in st.items():
                print('%08x  R%-6d W%-6d %s' % (a, nr, nw, ' '.join('%x' % v for v in vals)))
            continue
        saved, bus.log = bus.log, log
        for acc, n, last, stride in bus.compact():
            print('%s%s' % (acc, '' if n == 1 else '  x%d%s%s' % (
                n, ' stride %+d' % stride if stride else '',
                '' if last == acc.value else ' .. %08x' % last)))
        bus.log = saved
    print('stop: %s (pc %08x, %d device accesses, %d instructions, '
          'interrupts taken: %s)' % (reason, pc, bus.seq, sim.steps,
                                     dict(collections.Counter(v for v, _ in sim.irqs))))
    return 0


def panel_rule(panel_id):
    """Register rule that makes vmcs.bin's panel probe (GPIO 51 + 2 * GPIO
    50, level register 0x10002824 bits 19/18) read panel_id."""
    return '0x10002824=0x%x' % (((panel_id >> 1) & 1) << 18 | (panel_id & 1) << 19)


def main(argv):
    args = argv[1:]
    if not args:
        sys.exit(__doc__)
    if args[0] == '--selftest':
        import vc2sim_tests
        sys.exit(vc2sim_tests.main(args[1:]))
    if args[0] == '--boot':
        return boot_main(args[1:])
    if args[0] == '--exports':
        ex = vmcs_exports(open(args[1], 'rb').read())
        for k, v in sorted(ex.items(), key=lambda kv: kv[1]):
            print('%08x  %s' % (v, k))
        return
    if args[0] == '--call':
        trace = '--trace' in args
        mx = 10_000_000
        rest = [a for a in args[1:] if a != '--trace']
        if '--max' in rest:
            k = rest.index('--max')
            mx = int(rest[k + 1], 0)
            del rest[k:k + 2]
        path, func = rest[0], rest[1]
        sim = make_sim(path, trace=sys.stdout if trace else None)
        f = int(func, 0) if func[0].isdigit() else func
        v = sim.call(f, *[int(a, 0) for a in rest[2:]], max_steps=mx)
        print('r0 = 0x%08x (%d)  r1 = 0x%08x  [%d instructions]' % (
            v, s32(v), sim.r[1], sim.steps))
        return
    sys.exit(__doc__)


if __name__ == '__main__':
    # Run as the module 'vc2sim' so vc2vpu and the self-test share one copy
    # of the classes and of HYP.
    import vc2sim
    vc2sim.main(sys.argv)
