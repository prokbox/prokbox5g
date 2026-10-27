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
"""Read the timing bench's lines (stages 4 and 4b) and probe v2's lines
(stage 1, and any probe or bench image that faulted) out of the device
test's log (bcmtest.log in the system folder) and print them as tables.

The device test writes, per bench run (bcm-devtest-core.c):

  <ms> bench step <hex>: <what>       each new step word seen (0x1C0)
  <ms> B4H <n> <8 hex words>          the results header, words n..n+7
  <ms> B4K <t> <k> <tag> <insns> <ops> <rows> <min> <max> <per_iter>
                                      timing table t (1, or 2 at 150 MHz)
  <ms> B4R ...                        the clock estimates (for reading)
  <ms> B4V <k> <tag> <flags> <got0..3> <bad> <want>
  <ms> B4VL <k> <late0..3>            vector check k's record: flags bit 0
                                      pass, bit 1 pass after a delay, 15:8
                                      words, 31:16 first bad word; out[0..3]
                                      at once and after the delay; the
                                      first bad word and its expected value
  <ms> B4V late <bits>                the late-pass bits
  <ms> B4S <k> <row> <changed> <first> <last> <w_first> <w_target> <w_last>
                                      store-locating check k (dev4): one
                                      vst16 from VRF row <row> (rows 0, 8,
                                      16, 32 hold (0xC0 + y) << 8 | lane);
                                      words of the 9 KB buffer that changed,
                                      byte offsets from the target, words

From the records it prints a table and one verdict on the vector unit
(VPU dead / its memory path dead / posted stores / ALU results lost /
the vst after an ALU op lost / row 8 addressing / encodings wrong for
named checks).  Logs from images without records (device session 3)
are read with that image's check order.

This prints, per run: the checks, the clock estimate f = insns / min of
ALU1 (MHz, assuming one cycle per dependent add), and per kernel the
cycles per measured instruction (min * f / ops), the cycles per vector
row (min * f / rows), and slowest/fastest.  A run that stopped is named
by its last step: the kernel or check, with its tag from bench_tab.c.

Probe v2 (bcm-devtest-core.c p2_block(); bringup.md, "Probe v2"):

  <ms> P2 req <p>/<r> info <p>/<i> flt <f> clo <before> <after> ticks <n>
  <ms> P2C <i> <status> <pc> <sp> <sr> <detail> <detail2>
  <ms> P2R <i> <r0> <r1> <r2> <r3> <r4> <r5>
  <ms> P2U <status> <pc> <sp> <sr> <detail> <detail2>    record U
  <ms> P2UR <r0> <r1> <r2> <r3> <r4> <r5>
  <ms> P2 none: block <ptr>, words <w0> <w1>        no valid block

All hex but <i> and <n>; status = 0xC5 << 24 | index << 16 | vector << 8
| state (0 not run, 1 pass, 2 fail, 3 fault).  Records are logged for
checks 9-12 always, for 0-8 only when they did not pass, and for U only
when set.  This prints them as a table (check name, state, vector, pc,
pc - detail for checks 9 and 12) and one verdict line for each open
question: the misaligned-access vector, the saved-pc offset, and the
pre-decrement semantics.

Usage: bcmfw/bench/readlog.py bcmtest.log
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import algnlog  # noqa: E402  (bench row ALGN)

HDR_NAMES = ('magic', 'state', 'nk', 'iters', 'runs', 'flags', 'vpass', 'vall',
             'spass', 'sall', 'ppass', 'pall', 'fail', 'got', 'want', 'reg0',
             'reg1', 'plla_m', 'plla_ctl', 'clk_sel', 't1', 't2', 'ew', 'nvec')
STEP = re.compile(r'^\s*\d+ bench step ([0-9a-f]{8}): (.*)$')
B4H = re.compile(r'^\s*\d+ B4H (\d+)((?: [0-9a-f]{8}){8})$')
B4K = re.compile(r'^\s*\d+ B4K ([12]) (\d+) (\S{4})((?: \d+){6})$')
B4V = re.compile(r'^\s*\d+ B4V (\d+) (\S{4})((?: [0-9a-f]{8}){7})$')
B4VL = re.compile(r'^\s*\d+ B4VL (\d+)((?: [0-9a-f]{8}){4})$')
B4VLATE = re.compile(r'^\s*\d+ B4V late ([0-9a-f]+)$')
B4S = re.compile(r'^\s*\d+ B4S (\d+) (\d+) (\d+) (-?\d+) (-?\d+)((?: [0-9a-f]{8}){3})$')
# The vector checks of images built before the records (eee599cddc and
# older: 18 checks, no diagnostics), for logs that name checks by index
LEGACY_VTAGS = ('COPY', 'VMAX', 'VMIN', 'ADDI', 'ADIR', 'AD8I', 'ADSN', 'MOVN', 'MULR',
                'MUL8', 'MU8R', 'MACC', 'MHR4', 'RSB0', 'SUBZ', 'LD8R', 'MHRN', 'MNEG')
DIAG_VTAGS = ('COP8', 'SUMR', 'CNST', 'CSUM', 'ALUX', 'MXSU')
FILL = 0xA5A5A5A5
START = re.compile(r'^\s*\d+ stage (4b?): timing bench')
STAGE = re.compile(r'^\s*\d+ (stage \w+|=== boot path)')
HX = r'([0-9a-f]{8})'
P2S = re.compile(r'^\s*\d+ P2 req ([0-9a-f]+)/([0-9a-f]+) info ([0-9a-f]+)/([0-9a-f]+) '
                 r'flt ([0-9a-f]+) clo %s %s ticks (\d+)$' % (HX, HX))
P2N = re.compile(r'^\s*\d+ P2 none: block %s, words %s %s$' % (HX, HX, HX))
P2C = re.compile(r'^\s*\d+ P2(?:C (\d+)|U)((?: [0-9a-f]{8}){6})$')
P2R = re.compile(r'^\s*\d+ P2(?:R (\d+)|UR)((?: [0-9a-f]{8}){6})$')
P2_NAMES = ('abs', 'not', 'mulhd.ss', 'eor (3-op)', 'addscale6', 'ldh (rs)++',
            'pc-relative ld/st', 'bhi 16-bit', 'bhi 32-bit',
            'session-1 replay: ld r0,(--r5), r5=0x7f', 'ld (--rs) aligned',
            'ldb/stb (--rs)', 'unaligned ld + 4 adds')
P2_STATES = ('not run', 'pass', 'FAIL', 'fault')


def tags():
    """Kernel tags and vector check tags in bench_tab.c order."""
    text = open(os.path.join(HERE, 'bench_tab.c')).read()
    k = re.findall(r'^\s*\{ 0x[0-9a-f]+,\s*\d+,\s*\d+,\s*\d+ \},\s*/\* (\w{4}):', text,
                   re.M)
    v = [bytes.fromhex(x).decode('ascii', 'replace') for x, _ in VDESC.findall(text)]
    return k, v


VDESC = re.compile(r'^\s*\{ 0x([0-9a-f]{8}), in_\w+, want_\w+, \d+(?:, (\d+))? \}', re.M)


def required_mask():
    """BR_VEC_ALL of today's image: the vector checks not marked
    informational in bench_tab.c (dev5)."""
    text = open(os.path.join(HERE, 'bench_tab.c')).read()
    return sum(1 << k for k, (_, info) in enumerate(VDESC.findall(text))
               if info in ('', '0'))


def parse(lines):
    """The bench runs in the log: a list of dicts with 'stage', 'steps',
    'hdr' (word list, or None), 'tables' {1: [...], 2: [...]}."""
    runs = []
    cur = None
    for line in lines:
        line = line.rstrip('\n')
        m = START.match(line)
        if m:
            cur = dict(stage=m.group(1), steps=[], hdr=None, words={}, tables={},
                       vrecs={}, vlate=None, slocs=[])
            runs.append(cur)
            continue
        if cur is None:
            continue
        m = STEP.match(line)
        if m:
            cur['steps'].append(int(m.group(1), 16))
            continue
        m = B4H.match(line)
        if m:
            n = int(m.group(1))
            for i, w in enumerate(m.group(2).split()):
                cur['words'][n + i] = int(w, 16)
            if len(cur['words']) >= 32:
                cur['hdr'] = [cur['words'][i] for i in range(32)]
            continue
        m = B4V.match(line)
        if m:
            w = [int(x, 16) for x in m.group(3).split()]
            cur['vrecs'].setdefault(int(m.group(1)), {}).update(
                tag=m.group(2), flags=w[0], got=w[1:5], bad=w[5], want=w[6])
            continue
        m = B4VL.match(line)
        if m:
            cur['vrecs'].setdefault(int(m.group(1)), {})['late'] = [
                int(x, 16) for x in m.group(2).split()]
            continue
        m = B4VLATE.match(line)
        if m:
            cur['vlate'] = int(m.group(1), 16)
            continue
        m = B4S.match(line)
        if m:
            w = [int(x, 16) for x in m.group(6).split()]
            cur['slocs'].append(dict(k=int(m.group(1)), row=int(m.group(2)),
                                     changed=int(m.group(3)), first=int(m.group(4)),
                                     last=int(m.group(5)), w_first=w[0], w_target=w[1],
                                     w_last=w[2]))
            continue
        if line.lstrip(' 0123456789').startswith('ALGN') and algnlog.parse_line(cur, line):
            continue
        m = B4K.match(line)
        if m:
            t, k = int(m.group(1)), int(m.group(2))
            nums = [int(x) for x in m.group(4).split()]
            cur['tables'].setdefault(t, []).append(
                dict(k=k, tag=m.group(3), insns=nums[0], ops=nums[1], rows=nums[2],
                     min=nums[3], max=nums[4], per_iter=nums[5]))
    return runs


def step_text(st, ktags, vtags):
    k = st & 0xFFF
    base = st & ~0xFFF
    if st == 0x50520004:
        return 'done'
    if base == 0x50521000:
        return 'vector check %d (%s)' % (k, vtags[k] if k < len(vtags) else '?')
    if base == 0x50522000:
        return 'scalar checks'
    if base in (0x50523000, 0x50525000):
        return 'timing kernel %d (%s)%s' % (k, ktags[k] if k < len(ktags) else '?',
                                            ' at 150 MHz' if base == 0x50525000 else '')
    if base == 0x50524000:
        return '150 MHz switch, step %d' % k
    if base == 0x50526000:
        return 'store-locating check %d' % k
    return 'probe start-up (%08x)' % st


SLOC_FILL = 0xA5A5A5A5


def sloc_tag(t):
    """(y, x) a tag byte names: dev4's 0xC0 + y, dev5's 0x80 | y (x 0) or
    0x40 | y (x 32); None if it is no tag."""
    if t >= 0xC0:
        return t - 0xC0, 0
    if t >= 0x80:
        return t - 0x80, 0
    if t >= 0x40:
        return t - 0x40, 32
    return None


def sloc_name(yx):
    return 'row %d' % yx[0] + (', x %d' % yx[1] if yx[1] else '')


def sloc_word(w):
    """A found word as 'row y lanes i,j' (halfwords tag << 8 | i)."""
    out = []
    for h in (w & 0xFFFF, w >> 16):
        yx = sloc_tag(h >> 8)
        if yx is None or h & 0xFF >= 16:
            return 'not a tagged row (%08x)' % w
        out += [yx, h & 0xFF]
    if out[0] == out[2]:
        return '%s lanes %d,%d' % (sloc_name(out[0]), out[1], out[3])
    return '%s / %s lanes %d,%d' % (sloc_name(out[0]), sloc_name(out[2]), out[1], out[3])


def sloc_source(s):
    """What the record stored: dev4's B4S row is y (load-style encoding);
    dev5's is y | x << 8, | 0x8000 for the load-style control."""
    c = s['row']
    if s.get('dev5'):
        if c & 0x4000:
            return 'vst16 REP 16 of rows 0-15, stride 64 (dev9, informational)'
        if c & 0x8000:
            return 'old load-style vst16 d%02x' % (c & 0xFF)
        return 'vst16 of %s (silicon layout)' % sloc_name((c & 0xFF, (c >> 8) & 0x3F))
    return 'vst16 from row %d' % c


def sloc_verdict(s):
    """One line per store-locating record."""
    if not s['changed']:
        return '%s: nothing written in the buffer' % sloc_source(s)
    where = ('at the target' if s['first'] == 0 and s['last'] == 28 else
             'at %+d..%+d bytes' % (s['first'], s['last']))
    return '%s: %d words %s; first %s, last %s' % (
        sloc_source(s), s['changed'], where, sloc_word(s['w_first']), sloc_word(s['w_last']))


def report_slocs(run, p):
    if not run.get('slocs'):
        return
    dev5 = any(s['row'] & 0x8000 for s in run['slocs'])
    for s in run['slocs']:
        s['dev5'] = dev5
    p('store-locating check (%s; isa.md "Vector stores on silicon"):' % (
        'sources tagged, then stored with the silicon layout' if dev5 else
        'rows 0, 8, 16, 32 tagged'))
    for s in run['slocs']:
        p('  ' + sloc_verdict(s))
    if dev5:
        good = sum(sloc_as_layout(s) for s in run['slocs'])
        p('  %d of %d records where the silicon layout puts them (the control: row 0 '
          'at +256)' % (good, len(run['slocs'])))


def sloc_as_layout(s):
    """A dev5 record matches the session-5 layout: its source's 32 bytes at
    the target, or for the load-style d08 control row 0's at +256."""
    c = s['row']
    if c & 0x4000:
        # dev9's REP 16: rows 0-15, 8 words each, 64 bytes apart
        return (s['changed'] == 128 and s['first'] == 0 and s['last'] == 15 * 64 + 28 and
                sloc_word(s['w_first']) == 'row 0 lanes 0,1' and
                sloc_word(s['w_last']) == 'row 15 lanes 14,15')
    off, yx = (256, (0, 0)) if c & 0x8000 else (0, (c & 0xFF, (c >> 8) & 0x3F))
    return (s['changed'] == 8 and s['first'] == off and s['last'] == off + 28 and
            sloc_word(s['w_first']) == '%s lanes 0,1' % sloc_name(yx) and
            sloc_word(s['w_last']) == '%s lanes 14,15' % sloc_name(yx))


def vtags_for(run, vtags):
    """The check order of the image that wrote this run: its records'
    tags, else the legacy order when its count matches, else today's."""
    if run['vrecs']:
        n = max(run['vrecs']) + 1
        return [run['vrecs'].get(k, {}).get('tag', '?') for k in range(n)]
    h = run['hdr']
    if h and h[23] == len(LEGACY_VTAGS) and len(vtags) != len(LEGACY_VTAGS):
        return list(LEGACY_VTAGS)
    return vtags


# Informational checks: not in the pass mask (STBK dev5; RNDT, RNDU and
# MLSC dev6)
INFO_VTAGS = ('STBK', 'RNDT', 'RNDU', 'MLSC',
              # dev6: the decoder's forms, informational on their first
              # silicon run (gen_bench.py FIRST_SILICON)
              'ADRG', 'MVNZ', 'MVRP', 'MH8I', 'MLMI', 'LSR8', 'AD68', 'WRAP', 'CLRQ',
              # P19: the jobs' lane shift (gen_jobs.py jv_gather16), an A
              # operand's +r lane offset, before it is relied on
              'ASHF', 'ASHG',
              # P19: the 32-bit accumulate (x15), the floored high-half
              # multiplies, vclip, the 48-bit +r enables and a 16-bit D's
              # +r x (gen_bench.py x15_cases; isa.md "The 32-bit accumulator")
              'X15M', 'X15S', 'X15Z', 'MHDU', 'VCLP', 'R48E', 'D16R',
              # P19: REP 8 accumulating across its repetitions, A and B
              # stepping (jv_idct8x4's products; gen_bench.py repa_case)
              'REPA',
              # P19: jdec's put and fill (gen_bench.py putv_case)
              'PUTV',
              # P19: the visualizer's vector bars (gen_bench.py barv_case)
              'BARV',
              # P19, after 2026-10-25's silicon run: the rows that tell the
              # fitting variants apart, and the jobs' new lane shift
              # (gen_bench.py vpu_fit_cases)
              'ASHX', 'ACSA', 'GTHR')
X15_VTAGS = ('X15M', 'X15S', 'X15Z', 'MHDU', 'VCLP', 'R48E', 'D16R', 'REPA')
# Lanes 0-7 of each x15 check's first stored row (gen_bench.py x15_cases)
X15M_IN = ((100, -100, 205, -205, 32767, -32768, 51, -52),
           (26, -26, 52, -51, 32767, -32768, -13, 12))
X15S_IN = ((5, -5, 0, 1000, -1000, 32767, -32768, 7),
           (-1, -2, -32768, -100, -7, -1, -1, -30000))
MHDU_IN = ((-1, 1, -32768, 32767, -300, 300, 12345, -12345),
           (-1, -1, -32768, -2, -320, -1280, -7, -30000))
VCLP_IN = (-1, -300, 255, 256, 0, -32768, 32767, 51)
# REPA: lanes 0-7 of row 49 (the high half) under each reading, from
# gen_bench.py's REPA_A, REPA_B and seed 514 (repa_readings; bench_test
# checks they agree)
REPA_TH = (('accumulates across repetitions, saturating at 32 bits (as modelled: '
             'silicon 2026-10-25; jv_dot8 keeps its partial sums inside 32 bits)',
             (32767, 521, 511, -15868, 529, 272, 23481, 509)),
           ('accumulates across repetitions, wrapping mod 2^32 (the model before '
            '2026-10-25: NOT as modelled)',
            (514, 521, 511, -15868, 529, 272, 23481, 509)),
           ('restarts each repetition (only the last product, seed lost): '
            'jv_idct8x4 needs --unrolled',
            (16384, 0, -1, -1, 0, -1, 16384, -3)),
           ('keeps the last repetition only (added to the seed): '
            'jv_idct8x4 needs --unrolled',
            (16898, 514, 513, 513, 514, 513, 16898, 511)))
# dev6's rounding checks: lanes 0-7 (the record's four words)
RND_IN = (-16, -48, -80, -2032, 16, 48, -15, -17)
MLSC_IN = (-1, -16, -17, 4095, 4096, 5000, 16, 4080)


def _lanes16(got):
    return [(w >> s) & 0xFFFF for w in got for s in (0, 16)]


def rounding_verdicts(recs):
    """dev6: what silicon did with ties (RNDT, RNDU) and with vmulms #16
    out of range (MLSC), read from the records' first four words."""
    out = []

    def s16(v):
        return v - 0x10000 if v & 0x8000 else v
    for tag, op in (('RNDT', 'vmulhrs'), ('RNDU', 'vmulhr')):
        r = recs.get(tag)
        if not r:
            continue
        got = [s16(v) for v in _lanes16(r['got'])]
        away = [-((-x * 2048 + 0x8000) >> 16) if x < 0 else (x * 2048 + 0x8000) >> 16
                for x in RND_IN]
        up = [(x * 2048 + 0x8000) >> 16 for x in RND_IN]
        how = ('ties away from zero (as modelled)' if got == away else
               'ties up (+0x8000, floor: the old model)' if got == up else
               'neither model: %s' % ' '.join('%d->%d' % p for p in zip(RND_IN, got)))
        out.append('%s #2048 rounds %s (%s)' % (op, how, tag))
    r = recs.get('MLSC')
    if r:
        got = [v & 0xFF for v in _lanes16(r['got'])]
        clamp = [min(255, max(0, (x * 16) >> 8)) for x in MLSC_IN]
        wrap = [((x * 16) >> 8) & 0xFF for x in MLSC_IN]
        how = ('clamps to 0..255 (the centre j is right on silicon)' if got == clamp else
               'keeps the low 8 bits, NO clamp: the centre j needs its own clip'
               if got == wrap else
               'neither model: %s' % ' '.join('%d->%d' % p for p in zip(MLSC_IN, got)))
        out.append('vmulms #16 into 8 bits %s (MLSC)' % how)
    return out


# HWLD (gen_bench.py HWLD_FORMS): tag -> (form, the model sign-extends)
HWLD = (('HL16', '16-bit ldh (rs)', False), ('HS16', '16-bit ldsh (rs)', True),
        ('H28O', '0x28/o1 (rs+off)', False), ('H29O', '0x29/o1 (rs+off)', True),
        ('H48H', '48-bit op 2 (rs+off)', False), ('H48S', '48-bit op 6 (rs+off)', True),
        ('HSP0', '2d4x/o1 (sp+off)', False), ('HSP1', '2d5x/o1 (sp+off)', True))
HWLD_IN = (0x8001, 0xFFFF, 0xFFFF, 0x8001)     # at +0, +2, +4, +6


def hwld_reading(got):
    """'zero-extends', 'sign-extends' or 'neither reading: ...' for a
    HWLD record's four got words (the halfwords HWLD_IN, offsets 0 and 2
    mod 4 each)"""
    if list(got) == list(HWLD_IN):
        return 'zero-extends'
    if list(got) == [h | 0xFFFF0000 for h in HWLD_IN]:
        return 'sign-extends'
    return 'neither reading: %s' % ' '.join('%08x' % g for g in got)


def hwld_verdicts(recs):
    """Each halfword load form's reading against both (quirks.md #16, #33):
    one clause per HWLD record present"""
    out = []
    for tag, form, signed in HWLD:
        r = recs.get(tag)
        if r is None:
            continue
        got = hwld_reading(r['got'])
        want = 'sign-extends' if signed else 'zero-extends'
        out.append('%s %s %s%s' % (tag, form, got, ' (as modelled)' if got == want else
                                   '' if got.startswith('neither') else
                                   ': NOT as modelled (the model %s)' % want))
    return out


def x15_verdicts(recs):
    """P19: what silicon did with the 32-bit accumulate and its partners,
    read from the records' first four words (lanes 0-7 of row 8)."""
    out = []

    def got16(tag):
        r = recs.get(tag)
        return None if r is None else [(v ^ 0x8000) - 0x8000 for v in _lanes16(r['got'])]

    def w(v):
        return ((v & 0xFFFF) ^ 0x8000) - 0x8000

    def name(g, cands):
        for label, want in cands:
            if g == [w(v) for v in want]:
                return label
        return 'neither reading: %s' % ' '.join('%d' % v for v in g)
    g = got16('X15M')
    if g is not None:
        x, y = X15M_IN
        out.append('x15 / vmulhd 32-bit sum: %s (X15M)' % name(g, (
            ('exact, carries pass from the low half (as modelled; jhoz\'s j is exact)',
             [(a * -320 + b * 1280) >> 16 for a, b in zip(x, y)]),
            ('high halves only, NO carry from the x15 low half',
             [((a * -320) >> 16) + ((b * 1280) >> 16) for a, b in zip(x, y)]))))
    h, l = X15S_IN
    for tag, what in (('X15S', "'acc x15'"), ('X15Z', "'x15' alone")):
        g = got16(tag)
        if g is not None:
            out.append('%s adds the low half %s (%s)' % (what, name(g, (
                ('signed, saturating at 32 bits' + (' (as modelled; silicon 2026-10-25)'
                                                    if tag == 'X15S' else
                                                    ': NOT as modelled'),
                 [max(-32768, a + (-1 if b < 0 else 0)) for a, b in zip(h, l)]),
                ('signed, wrapping (the model before 2026-10-25): NOT as modelled',
                 [a + (-1 if b < 0 else 0) for a, b in zip(h, l)]),
                ('zero-extended' + (': NOT as modelled' if tag == 'X15S' else
                                    ' (as modelled; the VRF restore idiom)'),
                 list(h)))), tag))
    g = got16('MHDU')
    if g is not None:
        a, b = MHDU_IN
        out.append('vmulhdsu (op 19) %s (MHDU)' % name(g, (
            ('reads B unsigned, floors (as modelled)',
             [(x * (y & 0xFFFF)) >> 16 for x, y in zip(a, b)]),
            ('reads B signed', [(x * y) >> 16 for x, y in zip(a, b)]))))
    g = got16('VCLP')
    if g is not None:
        out.append('vclip #255 %s (VCLP)' % name(g, (
            ('clamps to 0..255 (as modelled)', [max(0, min(x, 255)) for x in VCLP_IN]),
            ('is min(A, 255) only', [min(x, 255) for x in VCLP_IN]))))
    g = got16('REPA')
    if g is not None:
        out.append('REP 8 with acc %s (REPA)' % name(g, REPA_TH))
    for tag in ('ASHX', 'ACSA'):
        r = recs.get(tag)
        if r is not None:
            out.append('%s (%s)' % (variant_verdict(tag, r['got']), tag))
    for tag, ok_text, bad_text in (
            ('GTHR', "the jobs' lane shift (two transposes and a +r y shift) as modelled",
             "the jobs' lane shift (jv_gather16) NOT as modelled: the start-up check "
             'keeps the C'),
            ('R48E', "a 48-bit op's +r goes to A (t5 bit 3) and B (hw3[1]) (as modelled)",
             "a 48-bit op's +r is NOT as modelled (t5 bit 3 / B hw3[1]): jhoz's reading "
             'needs another look'),
            ('D16R', "a 16-bit H D's +r x aligns down to 16 (as modelled; jvert)",
             "a 16-bit H D's +r x is NOT aligned down to 16"),
            ('PUTV', "jdec's put and fill forms (a V8 D at +r, y 8; the masked H8 "
             'fill) as modelled',
             "jdec's put or fill forms NOT as modelled (a V8 D at +r, y 8; the "
             'masked H8 fill): the start-up check keeps the C put'),
            ('BARV', "the vector bars' forms (jv_bars16's block) as modelled",
             "the vector bars' forms NOT as modelled (jv_bars16's block): the "
             'start-up check keeps the C bars (J_VPU_BARS clear)')):
        r = recs.get(tag)
        if r is not None:
            out.append('%s (%s)' % (ok_text if r['flags'] & 3 else bad_text, tag))
    return out


# ASHX and ACSA (gen_bench.py vpu_fit_cases): their first record (lanes
# 0-7 of row 8) under each vc2sim variant (tools/bcmre/vc2vpu.py HYP
# v_a16_reg_x, v_acc_sat), as words; vpu_fit_test.py checks them against
# vc2sim.  The first is the modelled one.
VARIANT_WORDS = {
    'ASHX': (('near32', '46034350 8d6eaadf a5ea4233 68512e4a',
              "an H16 A's +r x rounds to the nearest 32 (near32, as modelled; "
              'silicon 2026-10-25, sitting 2)'),
             ('dbl16', '1e85f481 23592002 a5ea4233 6655f25a',
              "an H16 A's +r x counts lanes aligned to 16 (x 37 -> 0, 53 -> 32: "
              'dbl16): NOT as modelled (sitting 2 read near32)'),
             ('given', 'f25aa5ea 9b085166 ea6e3321 51664af2',
              "an H16 A's +r x is taken as given: NOT as modelled (ASHF/ASHG said "
              'otherwise)'),
             ('align16', '46034350 6e23df20 ea6e3321 51664af2',
              "an H16 A's +r x aligns down to 16 bytes: NOT as modelled"),
             ('xzero', '1e85f481 8d6eaadf 6eac2191 68512e4a',
              "an H16 A's +r x is dropped: NOT as modelled")),
    'ACSA': (('sat32', '80017ffe 0bb57fff 0000ffff 0000ffff',
              'the accumulator is 32 bits and saturates at every add (sat32, as '
              'modelled)'),
             ('read16', '80007fff 0bb57fff 00000000 00000000',
              'the accumulator is wider and its high half saturates when read '
              '(read16): NOT as modelled; set vc2sim v_acc_sat read16'),
             ('hi16', '80017ffe 0bb57fff 00000000 00000000',
              'the high half saturates at every add, the low half kept (hi16): NOT '
              'as modelled; set vc2sim v_acc_sat hi16'),
             ('none', '80007fff 0bb59c40 00000000 00000000',
              'the accumulator wraps (the model before 2026-10-25): NOT as '
              'modelled, and X15S/REPA said otherwise')),
}


def variant_verdict(tag, got):
    """The variant ASHX or ACSA's first four words (ints) name, as text
    that starts with the variant's name"""
    for var, words, text in VARIANT_WORDS[tag]:
        if [int(w, 16) for w in words.split()] == list(got):
            return '%s: %s' % (var, text)
    return 'neither variant: %s' % ' '.join('%08x' % w for w in got)


def tag_verdicts(recs, tags):
    """Each named check's own verdict, for tools that report check by check
    (scripts/bench_ingest.py): {tag: (state, text, modelled)} for the tags
    present in recs ({tag: record}).  state is 'pass', 'late' or 'FAIL'
    from the record's flags, or 'stale' for a FAIL whose reading is
    today's model (the image's expected value predates the model); text the clauses of x15_verdicts and
    vector_verdict that name the tag ('' if none); modelled is true when
    the check passed and no clause says otherwise."""
    clauses = list(x15_verdicts(recs)) + hwld_verdicts(recs)
    v = vector_verdict(recs)
    if v.startswith('vector unit: '):
        v = v[len('vector unit: '):]
        for c in clauses:           # their own text may hold '; '
            v = v.replace(c, '')
        clauses += [c for c in split_clauses(v) if c.strip()]
    out = {}
    for tag in tags:
        r = recs.get(tag)
        if r is None:
            continue
        state = 'pass' if r['flags'] & 1 else 'late' if r['flags'] & 2 else 'FAIL'
        text = '; '.join(c for c in clauses if re.search(r'\b%s\b' % tag, c))
        against = re.search(r'\bNOT\b|neither|not as modelled', text)
        if state == 'FAIL' and text and not against and 'as modelled' in text:
            # The image's own expected value disagrees with a reading that
            # is today's model: the image was built before the model
            # changed (sitting 1's caaa9890b0 X15S and REPA, 2026-10-25).
            # Silicon is as modelled; the flag is the image's, not ours.
            state = 'stale'
            text += (" [the image's own check failed: its expected value is older "
                     "than today's model]")
        modelled = state != 'FAIL' and not against
        out[tag] = (state, text, modelled)
    return out


def split_clauses(v):
    """vector_verdict's clauses: split at '; ' outside parentheses (a
    clause such as ASHF's holds '; quirks #30: ...' inside its own)"""
    out, depth, cur = [], 0, ''
    i = 0
    while i < len(v):
        c = v[i]
        depth += (c == '(') - (c == ')')
        if depth <= 0 and v.startswith('; ', i):
            out.append(cur)
            cur, i, depth = '', i + 2, 0
            continue
        cur += c
        i += 1
    out.append(cur)
    return out


def vector_verdict(recs):
    """One line on the vector unit from the records ({tag: record})."""
    def ok(t):
        return t in recs and recs[t]['flags'] & 1

    def late(t):
        return t in recs and not recs[t]['flags'] & 1 and recs[t]['flags'] & 2

    def scal_ok(t):         # the scalar slot is word 0: right unless word 0 is bad
        r = recs.get(t)
        return r is not None and (r['flags'] & 1 or (r['flags'] >> 16) != 0)

    def stored(t):          # the rows after the scalar slot (words 8+) right
        r = recs.get(t)
        return r is not None and (r['flags'] & 1 or (r['flags'] >> 16) < 8)
    if not recs:
        return 'vector unit: no records (an image from before the diagnostics)'
    if not any(DIAG_VTAGS[i] in recs for i in range(len(DIAG_VTAGS))):
        return 'vector unit: no diagnostic checks in this image'
    out = []
    if all(not r['flags'] & 3 for r in recs.values()):
        if not ok('SUMR') and all(g == FILL for r in recs.values() for g in r['got']):
            return 'vector unit: VPU dead (no vector result reached SRAM or the scalar unit)'
    dev5 = 'STBK' in recs
    # Images before dev5 stored rows 1.. with the load-style offset, which
    # on silicon names the source row (session 5): their failures are that
    layout = {'COP8', 'CSUM', 'MXSU', 'ADIR', 'MULR', 'MU8R', 'SUBZ', 'LD8R'}
    failed = {t for t in recs if not ok(t)}
    if not dev5 and failed and failed - {'MHRN'} <= layout and ok('COPY'):
        out.append('stores laid out like loads (images before dev5): the offset field '
                   'picks the source row and D the address, so %s stored other rows '
                   '(isa.md "Vector stores on silicon", session 5)' % ', '.join(
                       sorted(failed - {'MHRN'})))
        if 'MHRN' in failed:
            out.append('vmulhrs into an 8-bit D keeps the low 8 bits, no clamp (MHRN)')
        return 'vector unit: ' + '; '.join(out)
    if dev5 and not ok('STBK') and not late('STBK'):
        out.append('stores from rows outside block 0 (t5 bits) do not land as Apple\'s '
                   'code suggests (STBK, informational)')
    dev6 = 'RNDT' in recs
    if dev5 and not dev6 and 'MHRN' in failed:
        r = recs['MHRN']
        # dev5's expectation rounded -16 * 2048 (lane 8, word 4's low byte)
        # up to 0; session 6's silicon gave -1 (0xff): a tie rounds away
        # from zero, and dev6 expects that
        if r['flags'] >> 16 == 4 and r['bad'] ^ r['want'] == 0xFF and r['want'] & 0xFF == 0:
            out.append('vmulhrs rounds a tie away from zero (MHRN lane 8: -16 * 2048 gave '
                       '-1, dev5 expected 0; session 6, isa.md); dev6 expects it')
        else:
            out.append('vmulhrs into an 8-bit D does not keep the low 8 bits (MHRN)')
    if dev6 and 'MHRN' in failed:
        out.append('MHRN fails again: its -16 lane rounds another way than session 6 '
                   'showed, or the low 8 bits are not kept')
    rnd = (rounding_verdicts(recs) if dev6 else []) + x15_verdicts(recs) + \
        hwld_verdicts(recs)
    if ok('SUMR') and not ok('COPY') and not ok('COP8') and not late('COPY'):
        out.append('VPU memory path dead (the VRF reads back through r5; '
                   'vector stores do not land)')
    if ok('COPY') and not ok('COP8') and not late('COP8'):
        out.append('a vst16 from d08 does not land where modelled (COP8 fails, COPY '
                   'passes; isa.md "Vector stores on silicon")')
    lt = [t for t, r in recs.items() if late(t)]
    if lt:
        out.append('posted stores (late pass): %s' % ', '.join(lt))
    if 'CSUM' in recs and not ok('CSUM'):
        r = recs['CSUM']
        # word 0 (r5) wrong and one of the two readings of #52: an image
        # built for the other one (session 4's expected unsigned)
        if not scal_ok('CSUM') and r['got'][0] == 16 * 0xF4:
            out.append('imm6 is signed (CSUM r5 = 0xf40: #52 wrote -12)')
        elif not scal_ok('CSUM') and r['got'][0] == 16 * 52:
            out.append('imm6 is unsigned after all (CSUM r5 = 0x340: #52 wrote 52)')
        elif not scal_ok('CSUM'):
            out.append('ALU results lost (vmov #imm never reaches the VRF: CSUM\'s '
                       'r5 is wrong)')
        elif not late('CSUM'):
            out.append('the vst after an ALU op is lost (CSUM\'s r5 is right, '
                       'its stored row is not)')
    if 'ALUX' in recs and not ok('ALUX') and not late('ALUX') and ok('COPY'):
        out.append('an ALU op on another row stops the next vector store (ALUX)')
    if 'MXSU' in recs and not ok('MXSU') and scal_ok('MXSU') and not stored('MXSU'):
        out.append('vmax computes the right row (r5) but its store is lost')
    diag_ok = all(ok(t) for t in DIAG_VTAGS if t in recs)
    bad = [t for t, r in recs.items() if not r['flags'] & 3 and t not in DIAG_VTAGS]
    shift = [t for t in bad if t in ('ASHF', 'ASHG')]
    first = [t for t in bad if t in INFO_VTAGS[4:] and t not in shift + list(X15_VTAGS)
             + ['PUTV', 'BARV', 'ASHX', 'ACSA', 'GTHR']]
    bad = [t for t in bad if t not in INFO_VTAGS + ('MHRN',)]
    if shift:
        out.append('an A operand\'s +r lane offset is not as modelled (%s, '
                   'informational; quirks #30: near32 expected); see ASHX' % ', '.join(shift))
    if first:
        out.append('decoder forms wrong on their first silicon run (informational): %s'
                   % ', '.join(first))
    if diag_ok and bad:
        out.append('encodings wrong for: %s' % ', '.join(bad))
    if not out:
        head = 'vector unit: every check passes' if all(
            ok(t) for t in recs) else ('vector unit: every required check passes'
                                       if failed <= set(INFO_VTAGS) else
                                       'vector unit: no single cause (see the table)')
        return '; '.join([head] + rnd)
    return 'vector unit: ' + '; '.join(out + rnd)


def report_vrecs(run, p):
    recs = run['vrecs']
    if not recs:
        return
    p('vector checks (records; flags: 1 pass, 2 late pass):')
    p('  %2s %-4s %-5s %5s %-35s %-35s %8s %8s' % ('k', 'tag', 'state', 'words', 'got',
                                                  'late', 'bad', 'want'))
    for k in sorted(recs):
        r = recs[k]
        st = 'pass' if r['flags'] & 1 else ('LATE' if r['flags'] & 2 else 'FAIL')
        bad = r['flags'] >> 16
        p('  %2d %-4s %-5s %5d %-35s %-35s %8s %8s' % (
            k, r.get('tag', '?'), st, (r['flags'] >> 8) & 0xFF,
            ' '.join('%08x' % x for x in r.get('got', [])),
            ' '.join('%08x' % x for x in r.get('late', [])),
            '-' if bad == 0xFFFF else '%d:%08x' % (bad, r['bad']),
            '-' if bad == 0xFFFF else '%08x' % r['want']))
    p(vector_verdict({r['tag']: r for r in recs.values() if 'tag' in r}))


def clock_mhz(table):
    a = next((e for e in table if e['tag'] == 'ALU1' and e['min']), None)
    return a['insns'] / a['min'] if a else 0.0


def report(run, ktags, vtags, out=sys.stdout):
    p = lambda *a: print(*a, file=out)
    p('== stage %s ==' % run['stage'])
    vtags = vtags_for(run, vtags)
    if run['steps']:
        p('last step: %s' % step_text(run['steps'][-1], ktags, vtags))
    h = run['hdr']
    if not h:
        p('no results header (the bench did not finish)')
        return
    d = dict(zip(HDR_NAMES, h))
    p('state %d, %d kernels x %d iterations x %d runs, flags %x' % (
        d['state'], d['nk'], d['iters'], d['runs'], d['flags']))
    p('checks: vector %05x/%05x, scalar %03x/%03x, probe %03x/%03x' % (
        d['vpass'], d['vall'], d['spass'], d['sall'], d['ppass'], d['pall']))
    if d['fail'] != 0xFFFFFFFF:
        c = d['fail'] >> 16
        p('first mismatch: check %d (%s) word %d: got %08x want %08x' % (
            c, vtags[c] if c < len(vtags) else '?', d['fail'] & 0xFFFF, d['got'],
            d['want']))
    report_vrecs(run, p)
    report_slocs(run, p)
    algnlog.report(run.get('algn'), p)
    if d['flags'] & 2:
        p('150 MHz switch: regulator %08x -> %08x, PLL A %08x %08x, select %08x' % (
            d['reg0'], d['reg1'], d['plla_m'], d['plla_ctl'], d['clk_sel']))
    for t in sorted(run['tables']):
        tab = run['tables'][t]
        f = clock_mhz(tab)
        p('table %d: clock estimate %.1f MHz (ALU1, one cycle per add)' % (t, f))
        p('  %-4s %10s %10s %8s %8s %7s %8s %6s' % ('tag', 'ops', 'rows', 'min us',
                                                   'max us', 'cyc/op', 'cyc/row',
                                                   'max/min'))
        for e in tab:
            cyc = e['min'] * f
            p('  %-4s %10d %10d %8d %8d %7.3f %8s %6.3f' % (
                e['tag'], e['ops'], e['rows'], e['min'], e['max'],
                cyc / e['ops'] if e['ops'] else 0,
                '%.3f' % (cyc / e['rows']) if e['rows'] else '-',
                e['max'] / e['min'] if e['min'] else 0))
    if 1 in run['tables'] and 2 in run['tables']:
        a1 = next((e for e in run['tables'][1] if e['tag'] == 'ALU1'), None)
        a2 = next((e for e in run['tables'][2] if e['tag'] == 'ALU1'), None)
        if a1 and a2 and a1['min']:
            p('start-up clock: %.1f MHz (150 x %d / %d); ALU1 CPI at 150 MHz %.3f' % (
                150.0 * a2['min'] / a1['min'], a2['min'], a1['min'],
                150.0 * a2['min'] / a2['insns']))


def parse_p2(lines):
    """Probe v2 blocks in the log: a list of dicts with 'where' (the stage
    line before it), 'none' (the P2 none words, or None), 'req', 'reqall',
    'info', 'infoall', 'flt', 'clo', 'ticks' and 'recs' {index or 'U':
    dict(status, pc, sp, sr, d, d2, regs)}."""
    out, cur, where = [], None, ''
    for line in lines:
        line = line.rstrip('\n')
        m = STAGE.match(line)
        if m:
            where = m.group(1)
            continue
        m = P2S.match(line)
        if m:
            g = m.groups()
            cur = dict(where=where, none=None, req=int(g[0], 16), reqall=int(g[1], 16),
                       info=int(g[2], 16), infoall=int(g[3], 16), flt=int(g[4], 16),
                       clo=(int(g[5], 16), int(g[6], 16)), ticks=int(g[7]), recs={})
            out.append(cur)
            continue
        m = P2N.match(line)
        if m:
            out.append(dict(where=where, none=[int(x, 16) for x in m.groups()],
                            recs={}))
            cur = None
            continue
        if cur is None:
            continue
        m = P2C.match(line)
        if m:
            w = [int(x, 16) for x in m.group(2).split()]
            k = int(m.group(1)) if m.group(1) is not None else 'U'
            cur['recs'][k] = dict(status=w[0], pc=w[1], sp=w[2], sr=w[3], d=w[4],
                                  d2=w[5], regs=None)
            continue
        m = P2R.match(line)
        if m:
            k = int(m.group(1)) if m.group(1) is not None else 'U'
            if k in cur['recs']:
                cur['recs'][k]['regs'] = [int(x, 16) for x in m.group(2).split()]
    return out


def p2_state(rec):
    st = rec['status']
    return st & 0xFF, (st >> 8) & 0xFF, st >> 24 == 0xC5


def c12_insns(off):
    """Check 12: a byte offset from the 16-bit ld as instructions (the ld
    is 2 bytes, each add after it 4), or None if it falls inside one."""
    if off == 0:
        return 0
    if off >= 2 and (off - 2) % 4 == 0:
        return 1 + (off - 2) // 4
    return None


def c12_adds_ran(regs):
    """How many of check 12's four adds ran (r2-r5 = 0x111..0x444), or
    None if r2-r5 hold something else."""
    want = (0x111, 0x222, 0x333, 0x444)
    n = 0
    while n < 4 and regs[2 + n] == want[n]:
        n += 1
    return n if all(regs[2 + k] == 0 for k in range(n, 4)) else None


def p2_verdicts(blk):
    """One line per open question, from checks 9-12."""
    recs = blk['recs']
    flt = {i: recs[i] for i in (9, 12) if i in recs and p2_state(recs[i])[0] == 3}
    out = []
    vecs = sorted({p2_state(r)[1] for r in flt.values()})
    if not flt:
        out.append('misaligned vector: no fault in checks 9 or 12 (unaligned loads '
                   'do not fault?)')
    elif len(vecs) == 1:
        out.append('misaligned vector: %d (checks %s)' % (
            vecs[0], ' and '.join(str(i) for i in sorted(flt))))
    else:
        out.append('misaligned vector: checks disagree: %s' % ', '.join(
            '%d -> %d' % (i, p2_state(r)[1]) for i, r in sorted(flt.items())))
    offs = {i: (r['pc'] - r['d']) & 0xFFFFFFFF for i, r in flt.items()}
    if not offs:
        out.append('saved-pc offset: unknown (no fault in checks 9 or 12)')
    else:
        o9, o12 = offs.get(9), offs.get(12)
        i12 = c12_insns(o12) if o12 is not None else None
        n12 = c12_adds_ran(flt[12]['regs']) if 12 in flt and flt[12]['regs'] else None
        if o9 is not None and o12 is not None and o9 == o12:
            txt = 'saved pc = faulting pc + %d bytes in both checks (a byte offset)' % o9
        elif o9 is not None and o12 is not None and i12 is not None and \
                o9 % 2 == 0 and o9 // 2 == i12:
            txt = ('saved pc = %d instructions after the faulting one (9: +%d bytes, '
                   '12: +%d bytes)' % (i12, o9, o12))
        else:
            txt = 'saved pc - faulting pc: %s' % ', '.join(
                'check %d +%d bytes' % (i, o) for i, o in sorted(offs.items()))
        if n12 is not None:
            txt += '; %d of check 12\'s adds ran before the fault' % n12
            if i12 == 0 and n12 == 0:
                txt += ' (the saved pc is the faulting instruction: restartable)'
            elif i12 is not None and i12 == n12 + 1:
                txt += ' (the saved pc is the first instruction not run)'
            elif i12 is not None:
                txt += (' (the saved pc is %d instructions past the faulting one, '
                        '%d past the last that ran)' % (i12, i12 - n12))
        out.append(txt)
    parts = []
    r9 = recs.get(9)
    if r9 and r9['regs'] and p2_state(r9)[0] == 3:
        r5 = r9['regs'][5]
        parts.append('check 9 r5 %x: %s' % (
            r5, 'not decremented before the fault' if r5 == 0x7F else
            'decremented by %d before the fault' % (0x7F - r5)
            if 0 < 0x7F - r5 <= 4 else 'changed'))
    elif r9:
        parts.append('check 9 %s, r5 after %x' % (P2_STATES[p2_state(r9)[0] & 3],
                                                  r9['d2']))
    for i, what in ((10, 'aligned ld (--rs)'), (11, 'ldb/stb (--rs)')):
        r = recs.get(i)
        if r:
            st = p2_state(r)[0]
            parts.append('%s %s' % (what, {1: 'as modelled', 2: 'differs (detail %x %x)'
                                           % (r['d'], r['d2']), 3: 'faulted'}.get(
                                               st, P2_STATES[st & 3])))
    out.append('pre-decrement: ' + ('; '.join(parts) if parts else 'no record'))
    return out


def report_p2(blk, out=sys.stdout):
    p = lambda *a: print(*a, file=out)
    p('== probe v2 (%s) ==' % (blk['where'] or '?'))
    if blk['none'] is not None:
        p('no results block: pointer %08x, words %08x %08x' % tuple(blk['none']))
        return
    p('required %03x/%03x, informational %04x/%04x, faulted %04x, ticks %d '
      '(clo %08x -> %08x)' % (blk['req'], blk['reqall'], blk['info'], blk['infoall'],
                              blk['flt'], blk['ticks'], blk['clo'][0], blk['clo'][1]))
    p('  %-3s %-40s %-7s %3s %8s %8s %8s %8s %8s  %s' % (
        '#', 'check', 'state', 'vec', 'pc', 'sp', 'sr', 'detail', 'detail2', 'notes'))
    for k in sorted(blk['recs'], key=lambda x: -1 if x == 'U' else x):
        r = blk['recs'][k]
        st, vec, tagok = p2_state(r)
        name = 'unexpected fault (outside any check)' if k == 'U' else \
            P2_NAMES[k] if k < len(P2_NAMES) else '?'
        notes = []
        if not tagok:
            notes.append('bad status word %08x' % r['status'])
        if k in (9, 12) and st == 3:
            off = (r['pc'] - r['d']) & 0xFFFFFFFF
            notes.append('pc-detail +%d' % off)
        if r['regs']:
            g = r['regs']
            if k == 9 and st == 3:
                notes.append('r5 %x (%s)' % (g[5], 'not decremented' if g[5] == 0x7F
                                              else 'changed'))
            if k == 12 and st == 3:
                n = c12_adds_ran(g)
                notes.append('adds run: %s' % ('?' if n is None else n))
            notes.append('r0-r5 ' + ' '.join('%x' % x for x in g))
        p('  %-3s %-40s %-7s %3s %08x %08x %08x %08x %08x  %s' % (
            k, name[:40], P2_STATES[st & 3] if st < 4 else '%d?' % st,
            vec if st == 3 else '-', r['pc'], r['sp'], r['sr'], r['d'], r['d2'],
            '; '.join(notes)))
    for v in p2_verdicts(blk):
        p(v)


# ---- hello v2: step word, bounded waits, clock record (bcmfw/config.h)
STEP_FIXED = {1: 'start', 2: 'clock 0x4C/0x54', 3: 'clock 0x58', 4: 'counter check',
              5: 'panel ID pins', 6: 'idle (ready)', 0xF: 'command done'}
LIMITS = {1: 'a counter delay ran out on spins (counter stopped mid-wait; not fatal)',
          2: 'DMA completion never set (CPU re-send)'}
CLK_BITS = [(0x01, 'ran at start'), (0x02, 'started by 0x4C/0x54'),
            (0x04, 'started by 0x58'), (0x10, 'wrote 0x4C/0x54'),
            (0x20, 'wrote 0x58'), (0x40, 'implausible rate: spin delays')]


def hello_step(step):
    """What a hello v2 step word names (bcm-devtest-core.c step_name)."""
    if step >> 16 != 0x5053:
        return 'none' if step == 0 else 'not a hello v2 step'
    grp, lo = step >> 12 & 0xF, step & 0xFFF
    if grp == 0:
        return STEP_FIXED.get(lo, 'unknown')
    if grp == 1:
        return 'power-on step %d' % lo if lo < 4 else 'power-on sync %d' % (lo - 0x10)
    if 2 <= grp <= 4:
        return 'panel %s script record %d' % (('init', 'on', 'off')[grp - 2], lo)
    if grp == 5:
        return 'CPU push, row %d' % lo
    if grp == 6:
        return 'DMA wait, %d polls left' % (16 * lo)
    if grp == 7:
        return 'command %d received' % lo
    if grp in (8, 9):
        return 'panel light-standby %s record %d' % (('off', 'wake')[grp - 8], lo)
    return 'unknown'


def parse_hello(lines):
    """hello v2's lines: [(kind, fields)] for 'image words' and 'clock'."""
    out = []
    spa_lo = None
    for ln in lines:
        m = re.search(r'image words: step ([0-9a-f]{8})', ln)
        if m:
            out.append(('step', int(m.group(1), 16)))
            continue
        m = re.search(r'image words: limit ([0-9a-f]{8}) val ([0-9a-f]{8}) '
                      r'clk ([0-9a-f]{8})', ln)
        if m:
            out.append(('limit', tuple(int(g, 16) for g in m.groups())))
            continue
        m = re.search(r'clock ([0-9a-f]{8}): ', ln)
        if m:
            out.append(('clock', int(m.group(1), 16)))
            continue
        m = re.search(r'spin loop ticks at \+(0|16)\.\.\+(?:14|30):((?: \d+){8}); '
                      r'spin\(\) at ([0-9a-f]+)', ln)
        if m:
            t = [int(x) for x in m.group(2).split()]
            if m.group(1) == '0':
                spa_lo = t
            elif spa_lo is not None:
                out.append(('spa', (spa_lo + t, int(m.group(3), 16))))
                spa_lo = None
            continue
        m = re.search(r'clock: (\d+) ticks over spin\(1000\).*found 4c ([0-9a-f]{8}) '
                      r'54 ([0-9a-f]{8}) 58 ([0-9a-f]{8})', ln)
        if m:
            out.append(('found', tuple(int(g, 0 if k == 0 else 16)
                                       for k, g in enumerate(m.groups()))))
    return out


def spa_verdict(ticks, spin_at):
    """dev5: spin()'s loop (add 16-bit, cmp 32-bit, bne: 8 bytes) at
    offsets 0..30 of a 64-byte block.  spin()'s own loop starts 8 bytes
    into spin()."""
    lo, hi = min(ticks), max(ticks)
    head = 'spin loop at +0..+30: %s ticks' % ' '.join(str(t) for t in ticks)
    if hi <= lo * 5 // 4:
        return head + ('; no alignment effect on this 3-instruction loop (the clock line '
                       'times a different, inlined one-instruction loop: hardware.md)')
    slow = [2 * k for k, t in enumerate(ticks) if t > (lo + hi) // 2]
    return head + '; SLOW with the loop at +%s: alignment matters (spin()\'s loop is at +%d)' % (
        ','.join(map(str, slow)), (spin_at + 8) & 63)


def report_hello(items, out=sys.stdout):
    def p(s):
        print(s, file=out)
    p('== hello v2 words ==')
    for kind, f in items:
        if kind == 'step':
            p('stopped at step %08x: %s' % (f, hello_step(f)))
        elif kind == 'limit':
            lim, val, clk = f
            p('bounded wait: %s' % ('none' if not lim else '%s (count %d, last value %08x)' % (
                LIMITS.get(lim >> 24, 'unknown limit %d' % (lim >> 24)), lim & 0xFFFFFF,
                val)))
        elif kind == 'clock':
            names = [n for b, n in CLK_BITS if f & b] or ['still STOPPED']
            if not f & 0x07 and f & 0x30:
                names.append('counter still STOPPED')
            p('clock record %08x: %s; %d ticks over spin(1000)%s' % (
                f, ', '.join(names), f >> 16,
                ' (%.1f M iterations/s of a one-instruction addcmpbne loop, 2 or 4 '
                'cycles each by where it sits; not a clock reading: hardware.md, session 6)'
                % (1000.0 / (f >> 16)) if f >> 16 else ''))
        elif kind == 'spa':
            p(spa_verdict(*f))
        elif kind == 'found':
            p('clock registers at start: 4c %08x 54 %08x 58 %08x' % f[1:])


DMA_RUN = re.compile(r'^\s*\d+ (.*): run (\d+): ([0-9a-f]+) 1 poll in, ([0-9a-f]+) at end, '
                     r'ctrl ([0-9a-f]+)/([0-9a-f]+): (.*)$')


def run_verdict(n, s, e):
    """bcm-devtest-core.c dma_run_read: device session 5, the LCD run
    length register counts the transfers up to n."""
    if e == n and s < n:
        return 'counted up: all out'
    if e == n:
        return 'all out by the first poll'
    if e < n:
        return 'STOPPED SHORT'
    return 'past the length: see photo'


def parse_dma(lines):
    out = []
    for ln in lines:
        m = DMA_RUN.match(ln)
        if m:
            what, n, s, e, c0, c1, said = m.groups()
            out.append((what, int(n), int(s, 16), int(e, 16), int(c0, 16), int(c1, 16),
                        said.strip()))
    return out


def report_dma(items, out=sys.stdout):
    def p(s):
        print(s, file=out)
    p('== DMA run readback (LCD +0x04 counts up; session 5) ==')
    for what, n, s, e, c0, c1, said in items:
        v = run_verdict(n, s, e)
        p('%s: %d transfers, %d one poll in, %d at end, ctrl %03x/%03x: %s%s' % (
            what, n, s, e, c0, c1, v,
            '' if said == v else ' (the log said "%s": images before dev5 '
            'read it as a count-down)' % said))


SWEEP = re.compile(r'^\s*\d+ bus sweep( fine)? (\d+) kHz ([0-9a-f]{8})/([0-9a-f]{8}): '
                   r'frame (\d+) us, BCM (\d+) us(, DMA TIMED OUT)?$')


def parse_sweep(lines):
    out = []
    for ln in lines:
        m = SWEEP.match(ln)
        if m:
            fine, khz, t0, t1, us, bcm, tmo = m.groups()
            out.append((bool(fine), int(khz), int(t0, 16), int(t1, 16), int(us), int(bcm),
                        bool(tmo)))
    return out


def report_sweep(items, out=sys.stdout):
    """The stage 3 bus-timing sweep (dev5: bars; dev6: bars, then the fine
    pattern).  The photos are the verdict; this lists the frame times."""
    def p(s):
        print(s, file=out)
    p('== bus-timing sweep (frame times; the photos say whether the panel minds) ==')
    for fine, khz, t0, t1, us, bcm, tmo in items:
        p('%-5s %3d MHz words %08x/%08x: frame %6d us, BCM %6d us%s' % (
            'fine' if fine else 'bars', khz // 1000, t0, t1, us, bcm,
            ', DMA TIMED OUT' if tmo else ''))


# Stage 5, the everyday path (dev7): the lines that matter, as written
EVERYDAY = re.compile(r'^\s*\d+ (stage 5[: ].*|host upload, whole frame: .*|'
                      r'storm .*|full frame.*|sleep \d+: .*|wake \d+[:,] .*|'
                      r'upload lab: .*|lab .*|'
                      r'stop: .*|restore: done; fallback .*|'
                      r'boot path: .*|FAIL everyday.*|FAIL stage 5.*)$')


def parse_everyday(lines):
    return [m.group(1) for m in map(EVERYDAY.match, lines) if m]


def report_everyday(items, out=sys.stdout):
    """Stage 5 and the boot path: the storm's per-kind times (host upload
    and push), full frames, each sleep and wake, the stop and fallback;
    and the boot path's own lines (switch, fallbacks, strikes)."""
    def p(s):
        print(s, file=out)
    p('== everyday path (stage 5) and boot path ==')
    for x in items:
        p(x)


# Stage 4c, the 150 MHz clock with the panel, and 5b, the drift A/B (dev9)
CLOCK4C = re.compile(r'^\s*\d+ (stage 4c.*|kick CLOCK .*|clock (up|down): .*|'
                     r'4c[: ].*|after LCD_SLEEP: .*|PHOTO 4c.*|FAIL stage 4c.*|'
                     r'stage 5b.*|5b .*|drift: .*|controls: .*|FAIL stage 5b.*)$')
TOTALS5B = re.compile(r'^\s*\d+ 5b totals ([AB])( \(stub\))?: (\d+) frames, (\d+) late, '
                      r'PP (\d+) ms, BCM (\d+) ms, PP waited (\d+) ms')
FRAME4C = re.compile(r'^\s*\d+ 4c (crystal|PLL|crystal again): frame (\d+) us, '
                     r'BCM (\d+) us')


def drift_verdict(lines):
    """Stage 5b's totals per frame, mode A against B (the last run's)."""
    tot = {}
    for m in map(TOTALS5B.match, lines):
        if m:
            tot[m.group(1)] = (bool(m.group(2)),) + tuple(int(g) for g in m.group(3, 4, 5, 6, 7))
    if 'A' not in tot or 'B' not in tot:
        return None
    out = []
    for k in 'AB':
        stub, n, late, pp, bcm, wait = tot[k]
        out.append('5b %s%s: %d frames (%d late); per frame PP %.2f ms, BCM %.2f ms, '
                   'PP waiting %.2f ms' % (k, ' (stub)' if stub else '', n, late,
                                           pp / max(n, 1), bcm / max(n, 1),
                                           wait / max(n, 1)))
    if tot['A'][3] and not tot['B'][0]:
        out.append('5b: B takes %.0f%% of A\'s PP time; read power.csv by phase for the '
                   'current' % (100.0 * tot['B'][3] / tot['A'][3]))
    return out


def parse_4c(lines):
    items = [m.group(1) for m in map(CLOCK4C.match, lines) if m]
    frames = {}
    for m in map(FRAME4C.match, lines):
        if m:
            frames[m.group(1)] = int(m.group(3))
    return items, frames


def bus_verdict(frames):
    """Stage 4c's question: does the LCD bus follow the core onto the PLL?
    Apple's 150 MHz words cost 16 bus cycles a pixel (panel 3's later
    word): about 45 ms a frame if the bus stays at 27 MHz, about 16 ms at
    75 MHz (half the core), about 8 ms at 150 MHz."""
    f = frames.get('PLL')
    if not f:
        return None
    mhz = 76800 * 16 / f
    where = ('stays on the crystal' if mhz < 40 else
             'runs at about half the core' if mhz < 110 else 'runs at the core clock')
    return 'LCD bus on the PLL: about %.0f MHz (%s); crystal frames %s / %s us' % (
        mhz, where, frames.get('crystal', '-'), frames.get('crystal again', '-'))


def report_4c(items, frames, out=sys.stdout, lines=()):
    def p(s):
        print(s, file=out)
    p('== stage 4c (150 MHz with the panel) and 5b (drift A/B) ==')
    for x in items:
        p(x)
    v = bus_verdict(frames)
    if v:
        p(v)
    for x in drift_verdict(lines) or ():
        p(x)


# Stage 6, a clip through the real player (p14-player; apps/ipodui/devbcm.c)
STAGE6 = re.compile(r'^\s*\d+ (=== BCM test, stage 6.*|stage 6.*|=== end: stage 6.*)$')
V6 = re.compile(r'^\s*\d+ V6 (\d+) (-?\d+) (\d+) (\d+) (\d+)$')


def parse_6(lines):
    items = [m.group(1) for m in map(STAGE6.match, lines) if m]
    recs = [tuple(int(g) for g in m.groups()) for m in map(V6.match, lines) if m]
    return items, recs


def parse_6_clips(lines):
    """Stage 6 by clip (s9-int plays two: "stage 6 clip n: ..." before
    each): [(title, items, recs)].  A log without clip lines is one clip."""
    clips = []
    for ln in lines:
        m, v = STAGE6.match(ln), V6.match(ln)
        if m and m.group(1).startswith('==='):
            continue
        if m and m.group(1).startswith('stage 6 clip '):
            clips.append((m.group(1), [], []))
            continue
        if not (m or v):
            continue
        if not clips:
            clips.append(('stage 6', [], []))
        if m:
            clips[-1][1].append(m.group(1))
        else:
            clips[-1][2].append(tuple(int(g) for g in v.groups()))
    return clips


def stage6_pacing(recs, settle_ms=1000, hitch_us=15000):
    """vtrace.c's pacing figures from the V6 records: between two records
    the screen moved on by the drift's change against the pictures' own
    interval.  (intervals, max |jitter| us, mean |jitter| us, hitches,
    the hitches' pts ms)"""
    if not recs:
        return 0, 0, 0, 0, []
    t0 = recs[0][0]
    js, hits = [], []
    for prev, r in zip(recs, recs[1:]):
        if r[0] < t0 + settle_ms or r[0] <= prev[0]:
            continue
        j = r[1] - prev[1]
        js.append(abs(j))
        if j > (r[0] - prev[0]) * 500 and j >= hitch_us:
            hits.append(r[0])
    if not js:
        return 0, 0, 0, 0, []
    return len(js), max(js), sum(js) // len(js), len(hits), hits


def stage6_detail(recs):
    """From the V6 records (pts ms, drift us, chip us, MHz, shown): the
    drift by second of the clip and the slowest pictures on the chip."""
    if not recs:
        return []
    out = []
    t0 = recs[0][0]
    by_s = {}
    for pts, drift, work, mhz, shown in recs:
        by_s.setdefault((pts - t0) // 1000, []).append(abs(drift))
    out.append('A/V by second (max |drift| ms): ' + ' '.join(
        '%d:%d' % (k, max(v) // 1000) for k, v in sorted(by_s.items())))
    slow = sorted(recs, key=lambda r: -r[2])[:3]
    out.append('slowest on the chip: ' + ', '.join(
        '%d us at %d ms' % (r[2], r[0]) for r in slow))
    clocks = sorted(set(r[3] for r in recs))
    out.append('clock: %s MHz' % '/'.join(str(c) for c in clocks))
    n, jmax, jmean, nh, hits = stage6_pacing(recs)
    if n:
        out.append('pacing: %d intervals, |jitter| max %d us, mean %d us; '
                   '%d hitches%s' % (n, jmax, jmean, nh,
                                     (' (at ' + ', '.join('%d ms' % h for h in hits[:5])
                                      + (', ...' if nh > 5 else '') + ')') if nh else ''))
    return out


def report_6(lines, out=sys.stdout):
    def p(s):
        print(s, file=out)
    p('== stage 6 (clips through the player) ==')
    for title, items, recs in parse_6_clips(lines):
        if title != 'stage 6':
            p('-- ' + title)
        for x in items:
            p(x)
        for x in stage6_detail(recs):
            p(x)


def main(argv):
    if len(argv) != 1:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    ktags, vtags = tags()
    lines = open(argv[0], errors='replace').readlines()
    blocks = parse_p2(lines)
    runs = parse(lines)
    hello = parse_hello(lines)
    dma = parse_dma(lines)
    sweep = parse_sweep(lines)
    everyday = parse_everyday(lines)
    c4, f4 = parse_4c(lines)
    s6, v6 = parse_6(lines)
    if not runs and not blocks and not hello and not dma and not everyday and \
            not c4 and not s6:
        print('no bench runs, probe v2 or hello v2 lines in %s' % argv[0])
        return 1
    if hello:
        report_hello(hello)
    if dma:
        report_dma(dma)
    if sweep:
        report_sweep(sweep)
    if everyday:
        report_everyday(everyday)
    if c4:
        report_4c(c4, f4, lines=lines)
    if s6:
        report_6(lines)
    for blk in blocks:
        report_p2(blk)
    for run in runs:
        report(run, ktags, vtags)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
