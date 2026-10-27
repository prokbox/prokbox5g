#!/usr/bin/env python3 -I
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
"""Which instruction forms of our vector kernels Apple's code never runs.

Our kernels' semantics come from vc2sim's model, and the model is pinned
only where Apple's kernels exercise it in the self-test (sim.md, "Vector
experiments").  This lists every form in vpu_kernels.S and the display
path's disp_kernels.S (static: the kernels are straight-line code) that
no self-test row executes, at two levels:

  semantic  operation, element widths of D/A/B (or B = immediate /
            scalar register), accumulator use, setf, predication, scalar
            result, REP > 1, 48/80-bit: what an arithmetic surprise on
            the device would hit; also reduced to the operation and the operand
            widths alone (the modifiers are each proven on other operations)
  operand   the same plus each operand's direction, +rN and stepping:
            the addressing, whose fields the experiments pin one by one

and the scalar instruction kinds (vc2sim.cover_key) in the kernels that
the self-test never runs.  Needs the corpus (Apple's libraries are read
by the simulator only, as reference material):

  python3 -I bcmfw/h264/test/vpu_forms.py [~/Git/re/bcm2722/extract/1.3/vc/VideoCore]
"""
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(SRC))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bcmre'))
sys.path.insert(0, SRC)
import vc2dis as D            # noqa: E402
import vc2sim as S            # noqa: E402
import vc2vpu as VP           # noqa: E402
import vc2sim_tests as ST     # noqa: E402

DEFAULT_CORPUS = os.path.expanduser('~/Git/re/bcm2722/extract/1.3/vc/VideoCore')


def forms(ins):
    """(semantic, operand) form keys of one vector instruction
    (vc2vpu.form_keys)."""
    return VP.form_keys(ins)


def apple_forms(corpus):
    """Forms executed by the vc2sim self-test (every row), and the
    scalar kinds it runs."""
    sem, opd = collections.Counter(), collections.Counter()
    orig = VP.compile_vector

    def hooked(sim, ins):
        h = orig(sim, ins)
        f = forms(ins)

        def g():
            sem[f[0]] += 1
            opd[f[1]] += 1
            return h()
        return g
    VP.compile_vector = hooked
    S.COVER = collections.Counter()
    lib, vmcs = S.find_corpus(corpus)
    rows = ST.run_suite(lib, vmcs, 2722, None, False, quiet=True)
    VP.compile_vector = orig
    cover = S.COVER
    S.COVER = None
    bad = sum(r.n - r.ok for r in rows)
    return sem, opd, cover, bad


def our_forms():
    import gen_vpu as G
    sys.path.insert(0, os.path.join(ROOT, 'bcmfw', 'video'))
    import gen_disp as GD    # the display path's kernels (pipeline.md)
    sem, opd, scalar = collections.Counter(), collections.Counter(), collections.Counter()
    where = collections.defaultdict(set)
    for k in G.kernels() + GD.kernels():
        for line in k.lines:
            if line.endswith(':'):
                continue
            # a symbol immediate (a table address) assembles like any
            # 32-bit constant; the layout gives it an address in the image
            line = re.sub(r'#[A-Za-z_]\w*', '#0x20e000', line)
            ws = D.assemble(line, 0)
            ins = D.decode_words(ws, 0)
            if getattr(ins.e, 'vclass', None):
                f = forms(ins)
                sem[f[0]] += 1
                opd[f[1]] += 1
                where[f[0]].add(k.name.split('_')[1])
            else:
                key = S.cover_key(ins)
                scalar[key] += 1
                where[key].add(k.name.split('_')[1])
    return sem, opd, scalar, where


def main(argv):
    corpus = argv[0] if argv else DEFAULT_CORPUS
    if not os.path.isdir(corpus):
        print('vpu_forms: no corpus at %s: skipped' % corpus)
        return 0
    a_sem, a_opd, a_cover, bad = apple_forms(corpus)
    o_sem, o_opd, o_scalar, where = our_forms()
    coarse = lambda k: k[:4] if k[0] not in ('mem48', 'vstr', 'vmem80') else k[:2]  # noqa: E731
    a_coarse = {coarse(k) for k in a_sem}
    o_coarse = collections.Counter()
    for k, n in o_sem.items():
        o_coarse[coarse(k)] += n
        where[coarse(k)] |= where[k]
    new_coarse = [k for k in o_coarse if k not in a_coarse]
    new_sem = [k for k in o_sem if k not in a_sem]
    new_opd = [k for k in o_opd if k not in a_opd]
    new_scalar = [k for k in o_scalar if k not in a_cover]
    print('self-test: %d vector semantic forms, %d operand forms executed%s' % (
        len(a_sem), len(a_opd), '' if not bad else ' (%d FAILED cases)' % bad))
    print('our kernels: %d semantic forms (%d not in the self-test), '
          '%d operand forms (%d not in the self-test), %d scalar kinds (%d not)'
          % (len(o_sem), len(new_sem), len(o_opd), len(new_opd), len(o_scalar),
             len(new_scalar)))
    print('\noperations with operand widths the self-test never runs '
          '(%d; static count, kernels):' % len(new_coarse))
    for k in sorted(new_coarse, key=str):
        print('  %-60s %4d  %s' % (' '.join(k), o_coarse[k], ','.join(sorted(where[k]))))
    print('\nsemantic forms (with modifiers) the self-test never runs:')
    for k in sorted(new_sem, key=str):
        print('  %-60s %4d  %s' % (' '.join(k), o_sem[k], ','.join(sorted(where[k]))))
    print('\nscalar kinds the self-test never runs:')
    for k in sorted(new_scalar):
        print('  %-40s %4d  %s' % (k, o_scalar[k], ','.join(sorted(where[k]))))
    if '-v' in argv:
        print('\noperand forms the self-test never runs:')
        for k in sorted(new_opd, key=str):
            print('  %-70s %4d' % (' '.join(k), o_opd[k]))
    return 0


if __name__ == '__main__':
    sys.exit(main([a for a in sys.argv[1:]]))
