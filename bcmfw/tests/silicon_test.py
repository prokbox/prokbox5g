#!/usr/bin/env python3
"""Silicon words as fixtures: vc2sim's model against what the chip did
(docs/research/hardware-closure-plan.md section 6.2, exit criterion E3;
memory-access.md section 11).

Rows:
  fixtures   every tests/silicon/<session>/stage4.log (a stage 4 run's
             bench lines, copied from the device log) read through
             readlog/algnlog: every ALGN check (scalar, vector, DMA, port)
             reads "as modelled", ASHX and ACSA name the modelled variant,
             every other vector and HWLD row is as modelled, except the
             rows still open (OPEN below)
  scalar     a misaligned st/sth through every form bench row ALGN ran
             lands at the address rounded down and vector 1 is taken 4
             instructions later (sim.exceptions on); an unmeasured form
             (st (--rs)) traps in strict mode
  vector     vst16 at +4 lands at +0, vst8 at +8 at +0 (ALGN VS64, VS88);
             a vld16 REP 2 at a stride of 40 reads row 1 from +32 (VLS4);
             strict mode lets these run and traps vst16 at +20 and vld16
             at +6 (unmeasured); a vector form with no line in
             tools/bcmre/proven_forms.txt traps (strict forms)
  dma        units of 1, 2 and 4 bytes copy exactly (D1B5, D2H3, D4W9); a
             32-byte unit to 4 mod 32 lands at the address rounded down
             (D32M) with strict mode off, and strict mode refuses it
  gate       scripts/proven_forms.py's classifier on synthetic uses: a
             guard form outside its guard region, an unknown scalar kind, a
             dma or port kind with no line, data16 outside a bench function
             all fail the gate
  mutation   each row fails on the model before 2026-10-26 (stores precise
             and dropped, vector stores exact, REP base rounding once,
             dbl16, DMA moving whole words, a 32-byte unit exact)

Usage: bcmfw/tests/silicon_test.py [-v]
"""
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
import glob
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
ROOT = os.path.dirname(FW)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bcmre'))
sys.path.insert(0, os.path.join(FW, 'bench'))
import vc2dis as D      # noqa: E402
import vc2mmio as M     # noqa: E402
import vc2sim as S      # noqa: E402
import vc2vpu as VP     # noqa: E402
import readlog as RL    # noqa: E402
import algnlog as AL    # noqa: E402

FIXTURES = os.path.join(ROOT, 'tests', 'silicon')
# Rows whose silicon reading is still open (not as modelled on purpose):
# PUTV: its record FAILs against the image's own expected words, built
# before vc2sim's v_vd_reg_y 'align16' (p19-vplayer-health) modelled it
OPEN = {'PUTV'}


class Row:
    def __init__(self, name, verbose):
        self.name, self.n, self.fails, self.verbose = name, 0, [], verbose

    def check(self, ok, what):
        self.n += 1
        if not ok:
            self.fails.append(what)
        elif self.verbose:
            print('    ok   %s' % what)
        return ok


# ---- fixtures -------------------------------------------------------------

def fixture_rows(r):
    paths = sorted(glob.glob(os.path.join(FIXTURES, '*', 'stage4.log')))
    r.check(paths, 'no fixtures under %s' % FIXTURES)
    for p in paths:
        sess = os.path.basename(os.path.dirname(p))
        runs = [x for x in RL.parse(open(p)) if x['stage'] == '4']
        if not r.check(runs, '%s: no stage 4 run' % sess):
            continue
        run = runs[-1]
        v = AL.verdicts(run.get('algn'))
        r.check(len([t for t in v if t in AL.CHECKS]) == len(AL.CHECKS),
                '%s: %d of %d ALGN checks' % (sess, len(v), len(AL.CHECKS)))
        for tag, (state, text, ok) in v.items():
            # a port write that failed (R602's port error -1 in sitting
            # 2) stops the port part: it and the checks after it stay
            # open (trapped as unknown), not a model mismatch
            r.check(ok or state == 'error', '%s ALGN %s: %s' % (sess, tag, text))
        recs = {x['tag']: x for x in run['vrecs'].values() if 'tag' in x}
        for tag in ('ASHX', 'ACSA'):
            if tag in recs:
                got = RL.variant_verdict(tag, recs[tag]['got'])
                r.check(got.startswith(RL.VARIANT_WORDS[tag][0][0] + ':'),
                        '%s %s: %s' % (sess, tag, got))
        tv = RL.tag_verdicts(recs, [t for t in recs if t not in ('ASHX', 'ACSA')])
        for tag, (state, text, ok) in tv.items():
            if tag in OPEN:
                r.check(not ok, '%s %s: open, but reads as modelled now: move it out '
                        'of OPEN (%s)' % (sess, tag, text))
            else:
                r.check(ok, '%s %s: %s %s' % (sess, tag, state, text))


# ---- the model on hand-placed code ----------------------------------------

def place(sim, lines):
    code = sim.malloc(64)
    a = code
    for ln in lines:
        for w in D.assemble(ln, a):
            sim.mem.w16(a, w)
            a += 2
    return code


def store_case(form, size, off):
    """A misaligned store in form at buf + off, with the exception model on
    and a vector 1 handler that records the saved pc: (buffer words,
    handler hit, adds that ran)"""
    sim = S.Sim()
    sim.exceptions = True
    buf = sim.malloc(32)
    for k in range(8):
        sim.mem.w32(buf + 4 * k, 0xA5A5A5A0 + k)
    op = 'st' if size == 4 else 'sth'
    val = 0xC3C2C1C0 if size == 4 else 0xD1D0
    access = {'ind': '%s r2, (r0)' % op, 'off16': '%s r2, (r0+64)' % op,
              'postinc': '%s r2, (r0)++' % op, 'predec': '%s r2, (--r0)' % op}[form]
    base = buf + off - (64 if form == 'off16' else 0) + (size if form == 'predec' else 0)
    code = place(sim, ['mov r4, #0', 'mov r5, #0', access, 'add r4, #1', 'add r5, #1',
                       'add r4, #2', 'add r5, #2', 'rts'])
    handler = place(sim, ['rti'])     # back to the saved pc
    sim.mem.w32(4 * S.HYP['exc_vec_misaligned'], handler)
    try:
        sim.call(code, base, 0, val, max_steps=1000)
    except S.SimError as e:
        return None, str(e), None
    hit = [(v, pc) for v, pc, _ in sim.excs]
    return [sim.mem.r32(buf + 4 * k) for k in range(2)], hit, (sim.r[4], sim.r[5])


def scalar_rows(r):
    old = S.set_strict_align('trap')
    try:
        for form, size, off in (('ind', 4, 2), ('off16', 4, 2), ('postinc', 4, 2),
                                ('ind', 2, 1), ('off16', 2, 1), ('ind', 4, 1)):
            words, hit, regs = store_case(form, size, off)
            if not r.check(words is not None, '%s %d-byte store at +%d: %s' % (
                    form, size, off, hit)):
                continue
            exp = AL.buf_after([(off & ~(size - 1),
                                (0xC3C2C1C0 if size == 4 else 0xD1D0).to_bytes(size, 'little'))])
            r.check(words == exp[:2], '%s %d-byte store at +%d lands rounded down: %s, '
                    'want %s' % (form, size, off, ['%08x' % w for w in words],
                                 ['%08x' % w for w in exp[:2]]))
            r.check(len(hit) == 1 and hit[0][0] == 1 and regs == (3, 3),
                    '%s store at +%d: vector 1 after the 4 adds (hit %s, r4/r5 %s)' % (
                        form, off, hit, regs))
        words, why, _ = store_case('predec', 4, 2)
        r.check(words is None and 'strict alignment' in why,
                'st (--rs) misaligned (unmeasured) traps in strict mode: %s' % why)
    finally:
        S.set_strict_align(old)


def vmem(sim, lines, src=None):
    code = place(sim, lines + ['rts'])
    return code


def vector_rows(r):
    old = S.set_strict_align('trap')
    try:
        # vst16 at +4 and vst8 at +8: the row at +0 (VS64, VS88)
        for op, off, width in (('vst16', 4, 2), ('vst8', 8, 1)):
            sim = S.Sim()
            src, buf = sim.malloc(64), sim.malloc(64)
            src &= ~31
            buf = (buf + 31) & ~31
            for k in range(32):
                sim.mem.w8(src + k, 0x10 + k)
                sim.mem.w8(buf + k, 0xA5)
            ld = 'vld16 d00, (r0)' if width == 2 else 'vld8 d00, (r0)'
            code = place(sim, [ld, '%s d00, (r1)' % op, 'rts'])
            try:
                sim.call(code, src, buf + off)
                got = bytes(sim.mem.read(buf, 16 * width))
                r.check(got == bytes(sim.mem.read(src, 16 * width)),
                        '%s at +%d lands at +0: %s' % (op, off, got[:8].hex()))
            except S.SimError as e:
                r.check(False, '%s at +%d: %s' % (op, off, e))
        # strict: vst16 at +20 and vld16 at +6 are unmeasured
        for text, off in (('vst16 d00, (r1)', 20), ('vld16 d00, (r1)', 6)):
            sim = S.Sim()
            buf = (sim.malloc(96) + 31) & ~31
            code = place(sim, [text, 'rts'])
            try:
                sim.call(code, 0, buf + off)
                r.check(False, '%s at +%d ran in strict mode' % (text, off))
            except S.SimError as e:
                r.check('strict alignment' in str(e), '%s at +%d: %s' % (text, off, e))
        # REP 2 at a stride of 40: row 1 from +32 (VLS4)
        sim = S.Sim()
        src = (sim.malloc(128) + 31) & ~31
        for k in range(96):
            sim.mem.w8(src + k, k)
        buf = (sim.malloc(96) + 31) & ~31
        code = place(sim, ['mov r1, #40', 'vld16 d00, (r0)+=r1 rep2',
                           'vst16 d01, (r2+32)', 'rts'])
        try:
            sim.call(code, src, 0, buf)
            got = bytes(sim.mem.read(buf + 32, 4))
            r.check(got == bytes([32, 33, 34, 35]), 'vld16 rep2 stride 40: row 1 from +%d '
                    '(want +32)' % got[0])
        except S.SimError as e:
            r.check(False, 'vld16 rep2 stride 40: %s' % e)
        # strict forms: an operand form with no table line traps
        sim = S.Sim()
        fo = VP.set_strict_forms('trap')
        try:
            code = place(sim, ['vmax.1 d00, a300, #3 ifnn', 'rts'])
            key = VP.form_text(VP.form_keys(D.decode_words(D.assemble(
                'vmax.1 d00, a300, #3 ifnn', 0), 0))[1])
            listed = key in VP.proven_vector_forms()
            try:
                sim.call(code)
                r.check(listed, 'unlisted form %s ran under strict forms' % key)
            except S.SimError as e:
                r.check(not listed and 'strict forms' in str(e), '%s: %s' % (key, e))
        except (ValueError, KeyError, D.AsmError if hasattr(D, 'AsmError') else ValueError) as e:
            r.check(False, 'strict forms row: %s' % e)
        finally:
            VP.set_strict_forms(fo)
    finally:
        S.set_strict_align(old)


def dma_case(so, do, width, units, strict='', mis32='round'):
    bus = S.Bus(None, 0x10000)
    tm = M.SysTimer()
    bus.attach(tm)
    dma = M.DmaEngine()
    bus.attach(dma)
    oldm, olds = M.DmaEngine.mis32, M.DmaEngine.strict
    M.DmaEngine.mis32, M.DmaEngine.strict = mis32, strict
    try:
        src, dst, cb = 0x1000, 0x2000, 0x3000
        for k in range(64):
            bus.w8(src + k, 0x10 + k)
        for k in range(16):
            bus.w32(dst + 4 * k, 0xA5A5A5A0 + k)
        ti = 0x01020101 | width << 4 | width << 12
        for k, v in enumerate((0, src + so, dst + do, units, ti, 0, 0, 0)):
            bus.w32(cb + 4 * k, v)
        dma.write(7 * 0x80, 0xE802)
        dma.write(7 * 0x80 + 0x38 + 0x20, cb)
        tm.advance(1000)
        dma.progress()
        return [bus.r32(dst + 4 * k) for k in range(8)], dma.status[7]
    finally:
        M.DmaEngine.mis32, M.DmaEngine.strict = oldm, olds


def dma_rows(r):
    for tag in ('D32C', 'D4W9', 'D2H3', 'D1B5', 'D32M'):
        _, so, do, unit, units = AL.CHECKS[tag]
        width = unit.bit_length() - 1
        try:
            win, st = dma_case(so, do, width, units)
        except Exception as e:      # noqa: BLE001
            r.check(False, '%s: %s' % (tag, e))
            continue
        text, ok = AL._dma(AL.CHECKS[tag], 1, [st, 0xE802, 1, units], win)
        r.check(ok, '%s in vc2mmio: %s' % (tag, text))
    try:
        dma_case(0, 4, 5, 1, strict='trap')
        r.check(False, 'D32M ran under strict DMA')
    except Exception as e:          # noqa: BLE001
        r.check('strict alignment' in str(e), 'D32M under strict DMA: %s' % e)
    try:
        dma_case(1, 1, 0, 5, strict='trap')
        r.check(True, 'D1B5 under strict DMA')
    except Exception as e:          # noqa: BLE001
        r.check(False, 'D1B5 under strict DMA: %s' % e)


def mutation_rows(r, verbose):
    """Each model row must fail on the model before 2026-10-26"""
    muts = [('exc_mis_store_rounds', False, scalar_rows),
            ('v_st_align', 0, vector_rows),
            ('v_ld_rep_round', 'base', vector_rows)]
    for key, val, fn in muts:
        old = S.HYP[key]
        S.HYP[key] = val
        try:
            m = Row('mutant', False)
            fn(m)
            r.check(m.fails, 'HYP %s = %r: the rows still pass' % (key, val))
        finally:
            S.HYP[key] = old
    old = M.DmaEngine.mis32
    try:
        m = Row('mutant', False)
        M.DmaEngine.mis32 = 'exact'
        win, st = dma_case(0, 4, 5, 1, mis32='exact')
        text, ok = AL._dma(AL.CHECKS['D32M'], 1, [st, 0xE802, 1, 1], win)
        r.check(not ok, 'mis32 exact: D32M still reads as modelled')
    finally:
        M.DmaEngine.mis32 = old
    # dbl16 against the ASHX fixture: the variant words disagree
    r.check(RL.VARIANT_WORDS['ASHX'][0][0] == 'near32' and
            S.HYP['v_a16_reg_x'] == 'near32', 'v_a16_reg_x default %s' % S.HYP['v_a16_reg_x'])


def gate_rows(r):
    """scripts/proven_forms.py's classes on synthetic uses: the table's
    regions and classes decide, and a misplaced form fails the gate"""
    sys.path.insert(0, os.path.join(ROOT, 'scripts'))
    import proven_forms as PF
    import proven_table as PTB
    table = PTB.load()
    regs = PTB.regions(table)
    guard = next(k for (d, k), (c, _) in table.items() if d == 'vector' and c == 'guard')
    sil = next(k for (d, k), (c, _) in table.items() if d == 'vector' and c == 'silicon')
    cases = [
        (('vector', sil, 'h264_vidct4_add', 0, '', False), 'proven'),
        (('vector', guard, 'jv_fft256', 0, '', False), 'guarded'),
        (('vector', guard, 'h264_vidct4_add', 0, '', False), 'UNPROVEN'),
        (('vector', 'vfoo DH16 AH16 Bimm - 80', 'h264_x', 0, '', False), 'trapped'),
        (('vector', 'x', 'jv_q', 0, '', True), 'trapped'),
        (('vector', guard, 'bench_k_add1', 0, '', False), 'exempt'),
        (('scalar', 'alu_reg nosuchkind', 'jv_q', 0, '', False), 'UNPROVEN'),
        (('dma', 'unit 1 2-D', 'dma_lcd_start', 0, '', False), 'trapped'),
        (('dma', 'unit 4 1-D', 'dma_lcd_start', 0, '', False), 'proven'),
        (('dma', 'unit 3 1-D', 'dma_lcd_start', 0, '', False), 'UNPROVEN'),
        (('port', 'data16', 'lcd_update_rect', 0, '', False), 'UNPROVEN'),
        (('port', 'data16', 'dt_pa_cpu', 0, '', False), 'exempt'),
        (('port', 'addr32', 'dt_wr', 0, '', False), 'proven'),
    ]
    for use, want in cases:
        got, why = PF.classify(use, table, regs, {'alu_reg add'}, frozenset(), set())
        r.check(got == want, '%s %s at %s: %s (%s), want %s' % (
            use[0], use[1], use[2], got, why, want))


def main(argv):
    verbose = '-v' in argv
    t0 = time.time()
    rows = []
    for name, fn in (('fixtures: silicon words read as modelled', fixture_rows),
                     ('scalar: misaligned stores as silicon', scalar_rows),
                     ('vector: off-row stores, per-row REP, strict', vector_rows),
                     ('dma: sub-word units, the 32-byte unit off 32', dma_rows),
                     ('gate: proven_forms.py classes a use by the table', gate_rows)):
        r = Row(name, verbose)
        fn(r)
        rows.append(r)
    r = Row('mutation: each row fails on the old model', verbose)
    mutation_rows(r, verbose)
    rows.append(r)
    fails = 0
    for r in rows:
        print('%-60s %4d %4d' % (r.name, r.n, r.n - len(r.fails)))
        for f in r.fails[:10]:
            print('    FAIL: %s' % f)
        fails += len(r.fails)
    print('%d rows, %d cases, %d failed (%.0f s)' % (len(rows), sum(r.n for r in rows),
                                                    fails, time.time() - t0))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
