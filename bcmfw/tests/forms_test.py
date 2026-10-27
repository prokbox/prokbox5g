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
"""Encoding-class gate over every bcmfw image we put on the device.

Builds the images bcmfw/mkdevtest.py embeds, the LCD DMA build, the
host-protocol build, the timing bench (BCMFW_BENCH=1) and its PLL build,
and the video build (BCMFW_VIDEO=1: the H.264 decoder and the display
path), and checks every instruction (static, not only the executed ones):

  - no 16-bit instruction in 0x7000-0x77ff (ld/st (--rs): vc2cc never
    emits it; native code that needs it must first get the kind onto
    tools/bcmre/device_verified.txt), except in a check region that
    lists the kind (the probe's checks 9-11)
  - each exact check region holds every kind it lists and no unproven
    or (--rs) kind it does not (raw .hword instructions included)
  - with Apple's corpus: every instruction's kind (vc2sim.cover_key)
    occurs in Apple's static code or is device-verified
    (tools/bcmre/vc2forms.py), except inside the check regions below,
    which may hold the kinds they test; in the video image, vector kinds
    anywhere (the decoder's and display path's vector forms Apple never
    uses: bcmfw/h264/test/vpu_forms.py lists them and the bench checks
    them on chip), reported as an informational metric; its scalar kinds
    are gated like every other image's
  - the listing's '; unproven form' markers match the scan

Usage: bcmfw/tests/forms_test.py [-v]
Skips (exit 0) without vc4-elf-gcc; without the corpus only the
0x7000-0x77ff row runs.
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
ROOT = os.path.dirname(FW)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bcmre'))
sys.path.insert(0, FW)
import vc2cc as C       # noqa: E402
import vc2dis as D      # noqa: E402
import vc2sim as S      # noqa: E402
import vc2forms as F    # noqa: E402
import build as B       # noqa: E402
import mkdevtest        # noqa: E402

OUT = os.path.join(ROOT, 'build', 'bcmfw', 'forms')
VECTOR = {'valu*', 'vmem*', 'vstr*'}
# Check regions (symbol patterns) and the kinds they may contain although
# unproven: the forms under test.  Today every scalar kind they test also
# occurs in Apple's static code except the probe's raw 0x7050 (ld (--rs),
# checks 9 and 10), and the bench's vector forms.  An exact symbol's list
# is exactly the kinds its region tests (the 'exact' row), and only a
# region listing an ldst_predec kind may hold 0x7000-0x77ff.
EXEMPT = {
    'bcmfw_selfcheck': {'alu_reg abs', 'alu_reg not', 'alu3x mulhd.ss', 'alu3 eor',
                        'alu3x addscale6', 'ldst_postinc ldh', 'mem48 st pc',
                        'mem48 ld pc', 'bcc hi', 'bcc.s hi',
                        'ldst_predec ld',                   # raw .hword 0x7050
                        'ldst_predec ldb', 'ldst_predec stb',
                        'ldst_ind ld'},                     # unaligned (check 12)
    'bench_scalar_check': {'alu3 max', 'alu_reg asr', 'bcc gt', 'bcc lt', 'bcc le',
                           'bcc ls', 'ldst_ind ldsb', 'ldst_ind ldsb idx',
                           'mem48 ldsb'},                              # bench_scalar.S
    'bench_vc_*': VECTOR,              # vector checks (bench_gen.S)
    'bench_k_*': VECTOR,               # timing kernels: the forms those checks verify
}


# Per-image extra exemptions: the video image may hold vector kinds outside
# Apple's static code in any function (they are counted, not gated).
IMAGE_EXEMPT = {'video': {'*': VECTOR},
                'jobs_merged': {'*': VECTOR},     # the video image plus the jobs (P19)
                # the visualizer image (#126, proposed, sim only):
                # its vector kinds are UNCHECKED on silicon (gen_viz.py's
                # docstring lists the forms; the image's start-up check,
                # viz_vpu.c, keeps the C FFT if they misbehave); counted
                # here, not gated.  Its scalar kinds are gated as usual
                'jobs_viz': {'*': VECTOR},
                # the same code as the display-only image's overlay
                # (J_OP_OVERLAY, proposed, sim only): counted, not gated
                'jobs_display_ovl': {'*': VECTOR}}


def is_vector(kind):
    return kind.startswith(('valu', 'vmem', 'vstr'))


def region_insns(prog, sym):
    """Every instruction from sym to the next region symbol (or the end
    of .text), decoded linearly: raw .hword instructions included."""
    import bisect
    inner = F.inner_labels(prog)
    starts = sorted(a for n, a in prog.symbols.items()
                    if n not in inner and not n.startswith(('.L', '__')))
    lo = prog.symbols[sym]
    k = bisect.bisect_right(starts, lo)
    hi = min(starts[k] if k < len(starts) else prog.ends['text'], prog.ends['text'])
    img = prog.image
    rd = lambda x: int.from_bytes(img[x - prog.base:x - prog.base + 2], 'little') \
        if prog.base <= x < prog.base + len(img) - 1 else None
    out, a = [], lo
    while a < hi:
        ins = D.decode(rd, a)
        if ins is None:
            break
        out.append(ins)
        a += ins.size
    return out


def predec(ins):
    return ins.size == 2 and 0x7000 <= ins.ws[0] <= 0x77ff


def images():
    # Every image the device test embeds (bench and bench_pll included),
    # plus the DMA build without the device test's name; no duplicates.
    out = [(n, list(d)) for n, d in mkdevtest.IMAGES]
    names = {n for n, _ in out}
    extra = [('dma', ['BCMFW_LCD_DMA=1']), ('hostproto', ['BCMFW_HOSTPROTO=1']),
             ('bench', ['BCMFW_BENCH=1'])]
    srcs = ''.join(open(os.path.join(FW, s)).read() for s in B.SOURCES)
    if 'BCMFW_BENCH_PLL' in srcs:
        extra.append(('bench_pll', ['BCMFW_BENCH=1', 'BCMFW_BENCH_PLL=1']))
    if hasattr(B, 'VIDEO_SOURCES'):
        extra.append(('video', ['BCMFW_VIDEO=1']))
    if hasattr(B, 'JOBS_SOURCES'):
        # P19: the display-only image (G9) and the merged display image
        extra.append(('jobs_display', ['BCMFW_HOSTPROTO=1', 'BCMFW_JOBS=1']))
        extra.append(('jobs_merged', ['BCMFW_VIDEO=1', 'BCMFW_JOBS=1', 'BCMFW_IDLE_SLEEP=1',
                                      'BCMFW_IDLE_GATED=1', 'BCMFW_IDLE_DOORBELL=1']))
    if hasattr(B, 'VIZ_SOURCES'):
        extra.append(('jobs_viz', ['BCMFW_HOSTPROTO=1', 'BCMFW_JOBS=1', 'BCMFW_VIZ=1']))
    out += [(n, d) for n, d in extra if n not in names]
    return out


class Row:
    def __init__(self, name):
        self.name, self.cases, self.fails = name, 0, []

    def check(self, ok, what):
        self.cases += 1
        if not ok:
            self.fails.append(what)
        return ok


def main(argv):
    verbose = '-v' in argv
    if not B.have_gcc():
        print('skip: vc4-elf-gcc not found at %s (set VC4_GCC)' % C.GCC)
        return 0
    t0 = time.time()
    apple = F.apple_kinds()
    if not apple:
        print('note: no Apple corpus under %s: kind rows skipped' % F.CORPUS_ROOT)
    rows = {k: Row(t) for k, t in (
        ('pre', 'no instruction in 0x7000-0x77ff (ld/st (--rs))'),
        ('kind', 'every kind in Apple\'s static code or device-verified (corpus)'),
        ('mark', 'listing markers match the scan (corpus)'),
        ('exact', 'check regions hold exactly their listed kinds under test'))}
    # negative control: a listing with ldh r0, (--r5) (a (--rs) width
    # neither Apple's code nor the device has run; ld (--rs) itself is
    # device-verified since sessions 2-3) and the 16-bit stb (--r0)
    # Apple's memmove uses
    neg = ('00000000 <f>\n  00000000  7250                       ldh r0, (--r5)\n'
           '  00000002  7505                       stb r5, (--r0)\n'
           '00000004 <g>\n  00000004  7250                       ldh r0, (--r5)\n')
    rows['neg'] = Row('negative control: (--rs) and an unproven kind are caught')
    rows['neg'].check(len(F.predec_sites(neg)) == 3, 'predec sites')
    if apple:
        bad = F.scan(neg, apple, {'g': {'ldst_predec ldh'}})
        rows['neg'].check([(b.addr, b.kind) for b in bad] == [(0, 'ldst_predec ldh')],
                          'scan: %s' % [b.line() for b in bad])
    metrics = []

    def programs():
        """every image, and after the display-only image its overlay"""
        for name, defs in images():
            out = os.path.join(OUT, name)
            prog, rep = B.build(out, defs, listing=True)
            yield name, prog, rep, os.path.join(out, 'bcmfw.lst')
            if hasattr(B, 'build_overlay') and B.ovl_on(defs):
                oprog, orep = B.build_overlay(prog, out, defs, listing=True)
                yield name + '_ovl', oprog, orep, os.path.join(out, B.ovl_name(defs) + '.lst')

    for name, prog, rep, lst in programs():
        # (--rs) only inside a region that lists the kind
        allow = {i.addr for sp, kinds in EXEMPT.items() if sp in prog.symbols and
                 any(k.startswith('ldst_predec') for k in kinds)
                 for i in region_insns(prog, sp) if predec(i)}
        pre = [i for i in F.predec_sites(prog) if i.addr not in allow]
        for sp, kinds in EXEMPT.items():
            if sp not in prog.symbols or any(c in sp for c in '*?['):
                continue
            got = {S.cover_key(i) for i in region_insns(prog, sp)}
            risky = {S.cover_key(i) for i in region_insns(prog, sp)
                     if predec(i) or (apple and S.cover_key(i) not in F.allowed(apple))}
            rows['exact'].check(kinds <= got and risky <= kinds,
                                '%s <%s>: listed, not present: %s; unlisted: %s' % (
                                    name, sp, sorted(kinds - got), sorted(risky - kinds)))
        rows['pre'].check(not pre, '%s: %s' % (name, '; '.join(
            '%08x %04x %s' % (i.addr, i.ws[0], i.text) for i in pre)))
        line = '%-10s %6d bytes, %4d instructions' % (name, rep['load'],
                                                      len(F.code_insns(prog)))
        if apple:
            bad = F.scan(prog, apple, dict(EXEMPT, **IMAGE_EXEMPT.get(name, {})))
            raw = F.scan(prog, apple)
            rows['kind'].check(not bad, '%s:\n%s' % (name, '\n'.join(b.line() for b in bad)))
            marked = open(lst).read().count('; unproven form')
            rows['mark'].check(marked == len(raw), '%s: %d markers, %d unproven' % (
                name, marked, len(raw)))
            line += ', %d unproven (%d in check regions)' % (len(raw), len(raw) - len(bad))
            if name in IMAGE_EXEMPT:
                vk = {b.kind for b in raw if is_vector(b.kind)}
                sk = {b.kind for b in raw if not is_vector(b.kind)}
                line += '; non-Apple kinds: %d vector (%d sites), %d scalar%s' % (
                    len(vk), sum(is_vector(b.kind) for b in raw), len(sk),
                    (' (%s)' % ', '.join(sorted(sk))) if sk else '')
            if verbose:
                for b in raw:
                    print('    ' + b.line())
        metrics.append(line)
    fails = 0
    for r in rows.values():
        if not apple and r.name.endswith('(corpus)'):
            continue
        print('%-72s %5d %5d' % (r.name[:72], r.cases, r.cases - len(r.fails)))
        for f in r.fails[:8]:
            print('    FAIL: %s' % f)
        fails += len(r.fails)
    for m in metrics:
        print('  ' + m)
    print('%d failed (%.0f s)' % (fails, time.time() - t0))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
