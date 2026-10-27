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
"""The timing bench image (BCMFW_BENCH=1) against vc2sim.

The image is started the way the device test starts the probe (upload,
start, ready), then runs to its done step.  Rows:

  - it reports ready first, then runs the bench to done, then answers
    commands; the results block has the documented header
  - every vector check and every scalar check passes (vc2sim agrees with
    the semantics gen_bench.py writes down); the probe's ten pass too
  - a wrong expected word fails exactly that check (negative control)
  - each timing entry has its tag and its counts, and each kernel's
    declared instructions per iteration equal what the simulator executes
  - the times are plausible under the simulator's timer model (100
    instructions per microsecond): every kernel near 1.0 instructions
    per tick-hundredth, the clock estimate near 100 MHz
  - the default build writes no device register but the interrupt
    controller's (no panel, GPIO, clock or DMA access)
  - the PLL build (BCMFW_BENCH_PLL=1) writes the clock block, the run
    control and the regulator in exactly vmcs.bin 1.3's order and values
    (or hardware.md's list without the corpus), then times again
  - REPA's four readings in readlog.py are the generator's, and
    readlog names vc2sim's as the saturating one (silicon 2026-10-25)
  - with Apple's corpus: each vector check's instruction under test has
    the form it names and its harness only forms the self-test runs; the
    image executes no scalar kind the self-test doesn't, except the ones
    its checks test
  - the generated files are current (gen_bench.py --check)

Built with BCMFW_BENCH_ITERS=256 (the device build's 4096 would take
minutes in vc2sim; --device runs it too); the negative controls and the
PLL build with fewer.

Usage: bcmfw/tests/bench_test.py [-v] [--device]
Skips (exit 0) without vc4-elf-gcc.
"""
import collections
import contextlib
import io
import os
import random
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
ROOT = os.path.dirname(FW)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(FW, 'bench'))
import hello_test as T      # noqa: E402  (vc2sim, models, the panel)
import gen_bench as G       # noqa: E402
B, C, S, M = T.B, T.C, T.S, T.M
import vc2dis as D          # noqa: E402
import vc2vpu as VP         # noqa: E402

OUT = os.path.join(ROOT, 'build', 'bcmfw', 'bench')
CORPUS = os.path.expanduser('~/Git/re/bcm2722/extract/1.3/vc/VideoCore')
ITERS = 256
RES = 0xC0000
STEP, PASS = 0x1C0, 0x1C8
DONE = 0x50520004
HDR = 32

# The results header (bench/bench.h)
BR = dict(magic=0, state=1, nk=2, iters=3, runs=4, flags=5, vpass=6, vall=7,
          spass=8, sall=9, ppass=10, pall=11, fail=12, got=13, want=14,
          reg0=15, reg1=16, plla_m=17, plla_ctl=18, clk_sel=19, t1=20, t2=21,
          ew=22, nvec=23, vrec=24, vrec_words=25, vlate=26,
          vpass_hi=27, vall_hi=28, vlate_hi=29, sloc=30, nsloc=31)
VREC_AT, BV_WORDS = 0x800, 12      # bench.h: the vector check records
SLOC_AT = 0xB00                    # bench.h: the store-locating records

# vmcs.bin 1.3's clock switch (hardware.md, "Clocks, PLL and regulator"):
# the device writes from the first run-control write to the core clock
# select's 0x101, in order
PLL_SEQ = [(0x10000400, 0xA5A50020), (0x10000400, 0xA5A50020),
           (0x10001C4C, 0xA5A50501), (0x10001C54, 0xA5A50000),
           (0x10001C4C, 0xA5A5051B), (0x10001C54, 0xA5A50000)] + \
    [(0x10001C00 + o, 0xA5A50000) for o in (0x2C, 0x30, 0x34, 0x38, 0x40, 0x44, 0x3C,
                                             0x48, 0x50)] + \
    [(0x10001C28, 0xA5A50001), (0x10001C58, 0xA5A50101)] + \
    [x for p in (1, 2, 3, 0) for x in ((0x10001C00 + 4 * p, 0xA5A50000),
                                       (0x10001C14 + 4 * p, 0xA5A50000))] + \
    [(0x10001400, 0xA5A5000F), (0x10001C00, 0xA5A5006C), (0x10001C14, 0xA5A50258),
     (0x10001C14, 0xA5A50658), (0x10001C28, 0xA5A50101)]


class Run:
    """One upload and start of a bench image, run to its done step."""

    def __init__(self, image, limit=20_000_000):
        self.b = b = T.Bench(image, 0, 0x200, us_per_read=1)
        b.power_on(random.Random(2722))
        b.write_bytes(0, image)
        b.w32(T.CMD_WORD, 0)
        b.w32(0x10000400, 0xA5A50002)
        self.cmd_at_steps = []
        t0 = time.time()
        n0 = b.sim.steps
        ready_seen = None
        while b.r32(STEP) != DONE:
            if b.sim.steps - n0 > limit or b.r32(T.CMD_WORD) == T.FAULT:
                raise T.Fail('bench did not finish: step %08x status %08x' % (
                    b.r32(STEP), b.r32(T.CMD_WORD)))
            b.run(5000)
            if ready_seen is None and b.r32(T.CMD_WORD) == T.DONE:
                ready_seen = b.r32(STEP)
        self.ready_step = ready_seen
        self.steps = b.sim.steps - n0
        self.wall = time.time() - t0

    def w(self, k):
        return self.b.r32(RES + 4 * k)

    def hdr(self, name):
        if name in ('vpass', 'vall', 'vlate'):      # 64 checks: low and high words
            return self.w(BR[name]) | self.w(BR[name + '_hi']) << 32
        return self.w(BR[name])

    def table(self, at):
        n = self.hdr('nk')
        out = []
        for k in range(n):
            e = [self.w(at + 8 * k + i) for i in range(8)]
            out.append(e)
        return out


def tagstr(v):
    return v.to_bytes(4, 'big').decode('ascii', 'replace')


def forms(ins):
    """(semantic) form key of one vector instruction: bcmfw/h264/test/
    vpu_forms.py's (same keys, so lists compare)."""
    vc = ins.e.vclass
    if vc in ('valu80', 'valu48'):
        vi = VP.VInsn(ins)
        name = VP.OPS[vi.op6][0]
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
            mods.append(VP.PRED[vi.pred])
        if vi.sru[1] != 7 and vi.sru[0]:
            mods.append(['sru0', 'sum', 'min', 'max'][vi.sru[0]])
        if (vi.rep or 2) > 1:
            mods.append('rep')
        ln = '80' if vc == 'valu80' else '48'
        dw = 'discard' if vi.d.discard else str(8 * vi.d.width)
        return (name, 'D' + dw, 'A' + str(8 * vi.a.width), 'B' + bw, ' '.join(mods), ln)
    if vc == 'vmem48':
        op9, d8, off, rep, base, stride = VP.mem48_fields(ins.ws)
        return ('mem48', D.vopname(op9), 'rep' if (rep or 2) > 1 else 'single')
    if vc == 'vstr80':
        return ('vstr', 'rep' if ' rep' in ins.text else 'single',
                [x for x in ins.text.split() if x.startswith('if')][0]
                if ' if' in ins.text else 'none')
    return (vc, ins.text.split()[0])


def form_of(text):
    ins = D.decode_words(D.assemble(text, 0), 0)
    return forms(ins)


def fmt_form(f):
    return ' '.join(x for x in f if x)


class Suite:
    # scalar kinds the image may execute although the self-test never
    # runs them: hello_test's accepted encodings (vc2forms), and the kinds
    # under test in the probe's and the bench's own scalar checks
    ACCEPTED = T.F.APPLE_STATIC_ONLY | T.F.DEVICE_VERIFIED
    CHECKED = {'alu_reg asr', 'bcc gt', 'bcc lt', 'bcc le', 'bcc ls',
               'ldst_ind ldsb',                                    # bench_scalar.S
               'alu_reg abs', 'alu3x mulhd.ss', 'alu3x addscale6',  # probe.S
               'ldst_postinc ldh', 'mem48 st pc',
               'ldst_predec ld',                       # probe.S checks 9 and 10
               # HWLD (gen_bench.py): every halfword load form, read
               # against both readings (quirks.md #16, #33)
               'ldst_ind ldh', 'ldst_ind ldsh', 'ldst_off16 ldh', 'ldst_off16 ldsh',
               'mem48 ldh', 'mem48 ldsh', 'gp16 ldh sp', 'gp16 ldsh sp'}

    def __init__(self, args):
        self.args = args
        self.rows = []
        self.metrics = collections.OrderedDict()
        self.kernels = G.kernels()
        self.cases = G.vcases()

    def row(self, name):
        r = T.Row(name)
        self.rows.append(r)
        return r

    def build(self, name, defs):
        prog, rep = B.build(os.path.join(OUT, name), defs, listing=False)
        return prog, rep, prog.load_image()

    # ---- rows
    def t_generated(self):
        r = self.row('generated kernels and tables are current (gen_bench --check)')
        rc = subprocess.call([sys.executable, '-I', os.path.join(FW, 'bench', 'gen_bench.py'),
                              '--check'], stdout=subprocess.DEVNULL)
        r.check(rc == 0, 'bench_gen.S / bench_tab.c are stale: run gen_bench.py')

    def t_run(self):
        r = self.row('ready first, bench to done, results header')
        self.prog, rep, self.img = self.build('sim', ['BCMFW_BENCH=1',
                                                      'BCMFW_BENCH_ITERS=%d' % ITERS])
        self.metrics['image_bytes_sim'] = rep['load']
        S.COVER = collections.Counter()
        self.run = run = Run(self.img)
        self.cover = S.COVER
        S.COVER = collections.Counter()
        self.metrics['bench_instructions_sim'] = run.steps
        self.metrics['bench_wall_s'] = round(run.wall, 1)
        r.check(run.ready_step is not None and run.ready_step != DONE,
                'ready only after the bench (step %s at ready)' % run.ready_step)
        r.check(run.hdr('magic') == 0x50424E31 and run.hdr('state') == 2,
                'magic %08x state %d' % (run.hdr('magic'), run.hdr('state')))
        r.check(run.b.r32(0x1D4) == RES, 'mailbox 0x1D4 = %08x' % run.b.r32(0x1D4))
        r.check(run.hdr('nk') == len(self.kernels), 'kernels %d' % run.hdr('nk'))
        r.check(run.hdr('iters') == ITERS and run.hdr('runs') == 3,
                'iters %d runs %d' % (run.hdr('iters'), run.hdr('runs')))
        r.check(run.hdr('flags') == 1, 'flags %x' % run.hdr('flags'))
        r.check(run.hdr('t1') == HDR and run.hdr('t2') == 0 and run.hdr('ew') == 8,
                'table offsets %d %d %d' % (run.hdr('t1'), run.hdr('t2'), run.hdr('ew')))
        b = run.b
        info = b.r32(T.MB_INFO)
        r.check(info >> 16 == 0x5042 and info & 0x200, 'info %08x' % info)
        # after the bench, commands are answered as the probe answers them
        b.w32(T.CMD_WORD, T.LCD_UPDATE)
        for _ in range(100):
            b.run(2000)
            if b.r32(T.CMD_WORD) == T.DONE:
                break
        r.check(b.r32(T.CMD_WORD) == T.DONE and b.r32(T.MB_FRAMES) == 1,
                'LCD_UPDATE after the bench: status %08x frames %d' % (
                    b.r32(T.CMD_WORD), b.r32(T.MB_FRAMES)))
        r.check(not b.panel.events, 'panel traffic: %d events' % len(b.panel.events))

    def t_algn(self):
        """Bench row ALGN (bench/algn.h): CMD_ALGN after the bench, every
        check run and recorded, every fault caught by the image (the
        exception model on, strict alignment off: the checks are misaligned
        on purpose), and readlog's readings name the modelled one for each
        scalar and vector check.  This bus has no DMA model, so the DMA
        checks only have to report (with vc2mmio's model attached they
        time out: it takes config bit 0 as the enable, and PW8's silicon
        word 0x0000E802 leaves it clear).  t_silent looks only at the bus
        log before it."""
        import algnlog as AL
        r = self.row('ALGN: CMD_ALGN after the bench, faults caught, readings as modelled')
        b = self.run.b
        self.run.algn_from = len(b.bus.log)     # t_silent: the bench's own log
        old = S.set_strict_align('')
        try:
            b.w32(T.CMD_WORD, (~0x45 & 0xFFFF) << 16 | 0x45)
            for _ in range(400):
                b.run(5000)
                if b.r32(T.CMD_WORD) == T.DONE:
                    break
        finally:
            S.set_strict_align(old)
        if not r.check(b.r32(T.CMD_WORD) == T.DONE, 'status %08x' % b.r32(T.CMD_WORD)):
            return
        A = 0xC3000
        hdr = [b.r32(A + 4 * i) for i in range(8)]
        r.check(hdr[0] == 0x414C4731 and hdr[2] == 2 and (hdr[1] >> 8 & 0xFF) == 16,
                'header %s' % ' '.join('%08x' % x for x in hdr[:3]))
        n = hdr[1] & 0xFF
        if not r.check(n == len(AL.CHECKS), '%d checks, algnlog knows %d' % (
                n, len(AL.CHECKS))):
            return
        lines = ['%6d ALGN n %d faults %d src %08x buf %08x in 0 us' % (
            0, n, hdr[4], hdr[5], hdr[6])]
        for k in range(n):
            w = [b.r32(A + 32 + 64 * k + 4 * i) for i in range(16)]
            lines.append('%6d ALGN %d %s %s' % (0, k, w[0].to_bytes(4, 'big').decode(
                'ascii', 'replace'),
                                                ' '.join('%08x' % x for x in w[1:8])))
            lines.append('%6d ALGNB %d %s' % (0, k, ' '.join('%08x' % x for x in w[8:16])))
        cur = {}
        for ln in lines:
            r.check(AL.parse_line(cur, ln), 'unparsed: %s' % ln)
        v = AL.verdicts(cur['algn'])
        for tag, kind in ((t, s[0]) for t, s in AL.CHECKS.items()):
            if tag not in v:
                r.check(False, '%s: no record' % tag)
            elif kind == 'dma':
                pass        # no DMA model on this bus: the record is enough
            else:
                r.check(v[tag][2], '%s: %s' % (tag, v[tag][1]))
        # the image goes on answering after the faults
        b.w32(T.CMD_WORD, T.LCD_UPDATE)
        for _ in range(100):
            b.run(2000)
            if b.r32(T.CMD_WORD) == T.DONE:
                break
        r.check(b.r32(T.CMD_WORD) == T.DONE, 'no answer after ALGN')

    def t_checks(self):
        run = self.run
        info = [c.tag for c in self.cases if c.info]
        r = self.row('vector checks: all %d pass (%s informational)' % (
            len(self.cases), ', '.join(info)))
        allv = (1 << len(self.cases)) - 1
        req = allv & ~sum(1 << k for k, c in enumerate(self.cases) if c.info)
        r.check(run.hdr('nvec') == len(self.cases), 'count %d' % run.hdr('nvec'))
        r.check(run.hdr('vall') == req and run.hdr('vpass') == req,
                'pass %06x of %06x; first fail %08x got %08x want %08x' % (
                    run.hdr('vpass'), run.hdr('vall'), run.hdr('fail'), run.hdr('got'),
                    run.hdr('want')))
        r.check(run.hdr('fail') == 0xFFFFFFFF, 'first fail %08x' % run.hdr('fail'))
        r = self.row('vector check records: tag, words at once and late, both pass')
        r.check(run.hdr('vrec') == VREC_AT and run.hdr('vrec_words') == BV_WORDS and
                run.hdr('vlate') == allv, 'records at %x, %d words, late %05x' % (
                    run.hdr('vrec'), run.hdr('vrec_words'), run.hdr('vlate')))
        for k, c in enumerate(self.cases):
            v = [run.w(VREC_AT + BV_WORDS * k + i) for i in range(BV_WORDS)]
            exp = c.expected()
            want = [exp[i] | exp[i + 1] << 16 for i in range(0, len(exp), 2)]
            n = len(want)
            r.check(v[0] == int.from_bytes(c.tag.encode(), 'big') and
                    v[1] == 3 | n << 8 | 0xFFFF << 16 and
                    v[2:2 + min(4, n)] == want[:4] and v[6:6 + min(4, n)] == want[:4],
                    '%s record %s' % (c.tag, ' '.join('%08x' % x for x in v)))
        r = self.row('REPA: readlog\'s four readings are the generator\'s, vc2sim saturates')
        import readlog as RL
        ra = G.repa_readings(G.REPA_A, G.REPA_B)
        r.check([list(t) for _, t in RL.REPA_TH] ==
                [ra[k][1] for k in ('saturates', 'accumulates', 'restart', 'last')],
                'readlog REPA_TH %s, generator %s' % (RL.REPA_TH, ra))
        k = [c.tag for c in self.cases].index('REPA')
        got = [run.w(VREC_AT + BV_WORDS * k + 2 + i) for i in range(4)]
        line = RL.x15_verdicts({'REPA': {'got': got, 'flags': 3}})
        r.check(line == ['REP 8 with acc %s (REPA)' % RL.REPA_TH[0][0]], 'readlog: %s' % line)
        r = self.row('store-locating check: each source lands where the silicon layout puts it')
        self.sloc_check(r, run, G.sloc_expect)
        r = self.row('scalar checks (9) and the probe\'s nine required pass')
        r.check(run.hdr('sall') == 0x1FF and run.hdr('spass') == 0x1FF,
                'scalar %03x of %03x' % (run.hdr('spass'), run.hdr('sall')))
        r.check(run.hdr('pall') == 0x1FF and run.hdr('ppass') == 0x1FF,
                'probe %03x of %03x' % (run.hdr('ppass'), run.hdr('pall')))
        r.check(run.b.r32(PASS) == 0x1FF, 'mailbox self-check %03x' % run.b.r32(PASS))

    def sloc_check(self, r, run, where):
        """Store-locating records (bench.h BS_*): where(y, x, raw) gives
        (byte offset from the target, source (y, x)) of the 32 bytes the
        record's store writes, or None for nothing."""
        r.check(run.hdr('sloc') == SLOC_AT and run.hdr('nsloc') == len(G.SLOC_RECS),
                'records at %x, %d' % (run.hdr('sloc'), run.hdr('nsloc')))
        for k, (y, x, raw) in enumerate(G.SLOC_RECS):
            v = [run.w(SLOC_AT + 8 * k + i) for i in range(8)]
            code = 0x534C0000 | G.sloc_code(y, x, raw)
            got = where(y, x, raw)
            if raw == G.SLOC_REP:
                # dev9: rows 0-15 at the stride, 8 words each
                st = G.SLOC_REP_STRIDE
                w0 = G.sloc_half(0, 0, 0) | G.sloc_half(0, 0, 1) << 16
                w7 = G.sloc_half(15, 0, 14) | G.sloc_half(15, 0, 15) << 16
                want = [code, 128, 0, 15 * st + 28, w0, w0, w7]
            elif got is not None:
                off, (sy, sx) = got
                w0 = G.sloc_half(sy, sx, 0) | G.sloc_half(sy, sx, 1) << 16
                w7 = G.sloc_half(sy, sx, 14) | G.sloc_half(sy, sx, 15) << 16
                want = [code, 8, off, off + 28, w0, w0 if off == 0 else 0xA5A5A5A5, w7]
            else:
                want = [code, 0, (-G.SLOC_TARGET) & 0xFFFFFFFF,
                        (-G.SLOC_TARGET) & 0xFFFFFFFF, 0, 0xA5A5A5A5, 0]
            want.append(G.SLOC_TARGET << 16 | (G.VBUF_BYTES - G.SLOC_TARGET))
            r.check(v == want, 'record %d: %s, want %s' % (
                k, ' '.join('%08x' % x for x in v), ' '.join('%08x' % x for x in want)))

    def profile_run(self, name, **hyp):
        old = {k: S.HYP[k] for k in hyp}
        S.HYP.update(hyp)
        try:
            _, _, img = self.build(name, ['BCMFW_BENCH=1', 'BCMFW_BENCH_ITERS=4'])
            return Run(img)
        finally:
            S.HYP.update(old)

    def t_silicon(self):
        # Device session 5 (dev4's image) is reproduced word for word by the
        # default model (the store layout 'swap' and vmulhrs without
        # saturation): 23 of 24 vector records and all four store-locating
        # records, the 24th differing only in a VRF row the check never
        # loaded (isa.md "Vector stores on silicon").  These rows are the
        # controls: each older reading must fail this image's checks.
        tags = [c.tag for c in self.cases]
        r = self.row('controls: the session-4 store readings fail the dev5 checks')
        for prof in ('model', 'drop'):
            run = self.profile_run('st_' + prof, v_st48_nonzero=prof)
            r.check(run.hdr('vpass') & (1 << tags.index('COP8')) == 0 and
                    run.hdr('vpass') & (1 << tags.index('ADIR')) == 0,
                    '%s: pass %06x' % (prof, run.hdr('vpass')))
        r = self.row('control: a saturating vmulhrs fails exactly MHRN and CLRQ')
        run = self.profile_run('mhrs_sat', v_mulhrs_sat=True)
        req = run.hdr('vall')
        r.check(run.hdr('vpass') == req & ~(1 << tags.index('MHRN') | 1 << tags.index('CLRQ')),
                'pass %06x of %06x' % (run.hdr('vpass'), req))
        # Device session 6 (MHRN lane 8): a tie rounds away from zero.  The
        # old reading (+0x8000, floor) must fail MHRN and CLRQ (both carry
        # a negative tie) and the informational RNDT and RNDU.
        r = self.row('control: rounding ties up fails MHRN, CLRQ, RNDT and RNDU')
        base = self.profile_run('round_away')
        run = self.profile_run('round_up', v_mulhr_round='up')
        lost = base.hdr('vlate') & ~run.hdr('vlate')
        want = sum(1 << tags.index(t) for t in ('MHRN', 'CLRQ', 'RNDT', 'RNDU'))
        r.check(base.hdr('vlate') == (1 << len(tags)) - 1 and lost == want,
                'late %x, lost %x, want %x' % (base.hdr('vlate'), lost, want))

    def t_negative(self):
        r = self.row('a wrong expected word fails exactly its check')
        # VMAX's first expected word: corrupt it in the image
        k = [c.tag for c in self.cases].index('VMAX')
        prog, _, base = self.build('neg', ['BCMFW_BENCH=1', 'BCMFW_BENCH_ITERS=4'])
        sym = next(v for s, v in prog.symbols.items() if s.endswith('want_vmax'))
        img = bytearray(base)
        w = struct.unpack_from('<I', img, sym)[0]
        struct.pack_into('<I', img, sym, w ^ 0x00010000)
        run = Run(bytes(img))
        allv = (1 << len(self.cases)) - 1
        req = allv & ~sum(1 << j for j, c in enumerate(self.cases) if c.info)
        r.check(run.hdr('vpass') == req & ~(1 << k), 'pass %05x' % run.hdr('vpass'))
        v = [run.w(VREC_AT + BV_WORDS * k + i) for i in range(BV_WORDS)]
        r.check(v[1] & 0xFFFF0003 == 0 and v[10] == w and v[11] == w ^ 0x00010000 and
                run.hdr('vlate') == allv & ~(1 << k),
                'VMAX record %s late %05x' % (' '.join('%08x' % x for x in v),
                                              run.hdr('vlate')))
        r.check(run.hdr('fail') == k << 16 and run.hdr('got') == w and
                run.hdr('want') == w ^ 0x00010000,
                'first fail %08x got %08x want %08x' % (run.hdr('fail'), run.hdr('got'),
                                                        run.hdr('want')))
        # and a bit flipped in a scalar check's data fails that check only
        sym = next(v for s, v in prog.symbols.items() if s.endswith('bench_sbytes'))
        img = bytearray(base)
        img[sym] = 0x81
        run = Run(bytes(img))
        r.check(run.hdr('spass') == 0x1FF & ~(1 << 6), 'scalar pass %03x' % run.hdr('spass'))

    def t_table(self):
        run = self.run
        r = self.row('timing entries: tags, instruction, op and row counts')
        tab = run.table(HDR)
        self.tab = tab
        for k, (e, kd) in enumerate(zip(tab, self.kernels)):
            r.check(tagstr(e[0]) == kd.tag, 'entry %d tag %s' % (k, tagstr(e[0])))
            r.check(e[1:4] == [ITERS * kd.per_iter, ITERS * kd.ops, ITERS * kd.rows],
                    '%s counts %s' % (kd.tag, e[1:4]))
            r.check(e[6] == kd.per_iter and 0 < e[4] <= e[5], '%s %s' % (kd.tag, e))

    def t_counts(self):
        r = self.row('declared instructions per iteration = executed (vc2sim)')
        b = self.run.b
        sim = b.sim
        stop = 0x1E0            # never executed: the return address is the stop
        for kd in self.kernels:
            got = []
            for n in (1, 2):
                sim.r[0], sim.r[26] = n, stop
                pc, steps = self.prog.symbols[kd.name], 0
                while pc != stop:
                    h = sim.code.get(pc) or sim.fetch(pc)
                    sim.r[31] = pc
                    npc = h[0]()
                    pc = (pc + h[1]) if npc is None else npc & S.MASK
                    steps += 1
                    if steps > 100_000:
                        raise T.Fail('%s does not return' % kd.name)
                got.append(steps)
            r.check(got[1] - got[0] == kd.per_iter, '%s: %d per iteration, declared %d' % (
                kd.tag, got[1] - got[0], kd.per_iter))

    def t_plausible(self):
        r = self.row('times plausible under the timer model (100 instr/us)')
        worst = 0
        for kd, e in zip(self.kernels, self.tab):
            want = e[1] / 100.0
            # time moves every 32 instructions, and once per counter read
            ok = abs(e[4] - want) <= 2 + 0.01 * want
            worst = max(worst, abs(e[4] - want) / want)
            r.check(ok, '%s: %d ticks for %d instructions' % (kd.tag, e[4], e[1]))
        alu1 = self.tab[0]
        mhz = alu1[1] / float(alu1[4])
        self.metrics['clock_estimate_MHz'] = round(mhz, 1)
        self.metrics['worst_time_error'] = '%.1f%%' % (100 * worst)
        r.check(95 <= mhz <= 105, 'clock estimate %.1f MHz' % mhz)

    def t_silent(self):
        r = self.row('default build: no device writes but the interrupt controller (counter running)')
        b = self.run.b
        log = b.bus.log[:getattr(self.run, 'algn_from', len(b.bus.log))]
        dev = [a for a in log if a.rw == 'W' and a.seq not in b.host_seqs]
        blocks = sorted({a.addr & ~0x3ff for a in dev})
        r.check(blocks in ([0x10001000], []), 'core wrote %s' % [
            '%08x' % x for x in blocks])
        reads = sorted({a.addr for a in log if a.rw == 'R' and a.seq not in
                        b.host_seqs})
        # the counter, the panel ID pins and (hello v2's timer_init) the
        # three clock registers it records before any write
        r.check(set(reads) <= {0x10001804, 0x10002824, 0x10001C4C, 0x10001C54,
                               0x10001C58}, 'core read %s' % [
            '%08x' % x for x in reads])

    def pll_reference(self):
        vmcs = os.path.join(CORPUS, 'Boot', 'vmcs.bin')
        if not os.path.exists(vmcs):
            return None
        sim = S.make_boot_sim(vmcs, S.BCM_RULES, S.BCM_ALIASES, machine=True)
        with contextlib.redirect_stdout(io.StringIO()):
            S.boot(sim, max_steps=400_000)
        w = [(a.addr, a.value) for a in sim.mem.log if a.rw == 'W']
        i = w.index((0x10000400, 0xA5A50020))
        j = w.index((0x10001C28, 0xA5A50101), i)
        return [x for x in w[i:j + 1]
                if x[0] == 0x10000400 or x[0] == 0x10001400 or x[0] >> 8 == 0x10001C]

    def t_pll(self):
        r = self.row('PLL build: Apple\'s clock switch exactly, then a second table')
        prog, rep, img = self.build('pll', ['BCMFW_BENCH=1', 'BCMFW_BENCH_PLL=1',
                                            'BCMFW_BENCH_ITERS=16'])
        self.metrics['image_bytes_pll'] = rep['load']
        run = Run(img)
        b = run.b
        seq = [(a.addr, a.value) for a in b.vpu_device_writes()
               if a.addr >> 12 in (0x10000, 0x10001) and a.addr & ~0xff != 0x10001000
               and a.addr & ~0xff != 0x10001800]
        ref = self.pll_reference()
        r.check(ref is None or ref == PLL_SEQ, 'hardware.md list differs from vmcs.bin: %s' % (
            ref and [('%08x' % a, '%08x' % v) for a, v in ref]))
        r.check(seq == PLL_SEQ, 'clock writes: %s' % [('%08x' % a, '%08x' % v)
                                                      for a, v in seq])
        self.metrics['pll_reference'] = 'vmcs.bin 1.3' if ref else 'hardware.md list'
        r.check(run.hdr('flags') == 0x7, 'flags %x' % run.hdr('flags'))
        n = run.hdr('nk')
        r.check(run.hdr('t2') == HDR + 8 * n, 'table 2 at %d' % run.hdr('t2'))
        t2 = run.table(run.hdr('t2'))
        r.check([e[0] for e in t2] == [e[0] for e in self.tab] and
                all(e[4] > 0 for e in t2), 'second table incomplete')
        r.check(run.hdr('reg1') == 0xA5A5000F and run.hdr('clk_sel') == 0xA5A50101 and
                run.hdr('plla_ctl') == 0xA5A50658 and run.hdr('plla_m') == 0xA5A5006C,
                'read-backs %08x %08x %08x %08x' % (run.hdr('reg1'), run.hdr('clk_sel'),
                                                    run.hdr('plla_ctl'), run.hdr('plla_m')))
        r.check(run.hdr('vpass') == run.hdr('vall') != 0, 'checks %x' % run.hdr('vpass'))

    def t_forms(self):
        DIAG = ('COP8', 'SUMR', 'CNST', 'CSUM', 'ALUX', 'STBK')    # harness forms only
        r = self.row('vector checks: forms under test and harness forms (corpus)')
        if not os.path.isdir(CORPUS):
            r.check(True, '')
            self.metrics['forms'] = 'skipped (no corpus)'
            return
        import vc2sim_tests as ST
        sem = collections.Counter()
        orig = VP.compile_vector

        def hooked(sim, ins):
            h = orig(sim, ins)
            f = forms(ins)

            def g():
                sem[f] += 1
                return h()
            return g
        VP.compile_vector = hooked
        S.COVER = collections.Counter()
        try:
            lib, vmcs = S.find_corpus(CORPUS)
            with contextlib.redirect_stdout(io.StringIO()):
                rows = ST.run_suite(lib, vmcs, 2722, None, False, quiet=True)
        finally:
            VP.compile_vector = orig
        apple_kinds = set(S.COVER)
        S.COVER = collections.Counter()
        r.check(all(x.n == x.ok for x in rows), 'self-test failed')
        targets = 0
        for c in self.cases:
            tested = [form_of(t) for t in c.test if t.startswith('v')]
            harness = [form_of(t) for t in c.pre + c.post if t.startswith('v')]
            harness += [('mem48', 'vld16', 'single'), ('mem48', 'vst16', 'single')]
            new = [f for f in tested if f not in sem]
            # R48E: operand addressing (a 48-bit op's +r enables) in a form
            # the self-test runs (jhoz's pair sums)
            if c.tag not in DIAG + ('COPY', 'MHRN', 'MNEG', 'CLRQ', 'MLSC', 'R48E') + \
                    tuple(t for t, *_ in G.HWLD_FORMS):
                # each names a form the self-test never runs
                r.check(new and fmt_form(new[0]).startswith(c.form.split(',')[0]),
                        '%s: tested %s, named %s' % (c.tag, [fmt_form(f) for f in tested],
                                                     c.form))
                targets += 1
            else:
                # data-range checks: forms the self-test runs, new values
                r.check(not new, '%s: unproven form %s' % (c.tag, new))
            bad = [f for f in harness if f not in sem]
            r.check(not bad, '%s: harness uses unproven %s' % (c.tag, bad))
        self.metrics['vector_forms_checked'] = targets
        # the decoder's list (bcmfw/h264/test/vpu_forms.py): every semantic form
        listed = {'mem48 vld8 rep', 'vadd D16 A16 Bimm 80', 'vadd D16 A16 Bimm rep 80',
                  'vadd D16 A8 Bimm 80', 'vadds D8 A8 B16 ifn 80', 'vmax D16 A16 B16 80',
                  'vmin D16 A16 B16 80', 'vmov D16 A8 Breg ifn 80',
                  'vmul D16 A16 Breg rep 80', 'vmul D16 A8 Breg 80',
                  'vmul D16 A8 Breg rep 80', 'vmul Ddiscard A8 Breg acc 80',
                  'vmulhr D16 A16 Bimm 80', 'vrsub D16 A16 Bimm 80',
                  'vsub Ddiscard A16 B8 setf ifnz 80',
                  # the intra kernels' and the display path's (pipeline.md)
                  'vadd D16 A16 Breg 80', 'vmov D8 A8 Breg ifnz 80',
                  'vmov D8 A8 Breg rep 80', 'vmulhr D8 A16 Bimm 80',
                  'vmulm D16 A8 Bimm rep 80', 'vlsr D8 A8 Bimm rep 80',
                  'vadd D16 A16 B8 rep 80'}
        ours = {fmt_form(form_of(t)) for c in self.cases for t in c.test if t.startswith('v')}
        r.check(listed <= ours, 'decoder forms not checked: %s' % sorted(listed - ours))
        r = self.row('scalar kinds: none unproven but the ones under test (corpus)')
        # (vector instructions are judged by their semantic forms, above)
        extra = {k for k in set(self.cover) - apple_kinds
                 if not k.startswith(('valu', 'vmem', 'vstr'))}
        self.metrics['kinds_not_in_selftest'] = ' '.join(sorted(k.replace(' ', ':')
                                                                for k in extra))
        r.check(extra <= self.ACCEPTED | self.CHECKED, 'unproven kinds: %s' % sorted(
            extra - self.ACCEPTED - self.CHECKED))

    def t_device_build(self):
        r = self.row('device build (4096 iterations): builds below the results block')
        prog, rep, img = self.build('device', ['BCMFW_BENCH=1'])
        self.metrics['image_bytes_device'] = rep['load']
        r.check(rep['end'] < 0xC0000, 'image ends at %x, past the results block' % rep['end'])
        if self.args.device:
            run = Run(img, limit=200_000_000)
            tab = run.table(HDR)
            r.check(run.hdr('vpass') == run.hdr('vall') and run.hdr('spass') == 0x1FF,
                    'checks')
            ok = all(abs(e[4] - e[1] / 100.0) <= 2 + 0.01 * e[1] / 100.0 for e in tab)
            r.check(ok, 'times')
            self.metrics['device_build_instructions'] = run.steps

    def run_all(self):
        t0 = time.time()
        for t in (self.t_generated, self.t_run, self.t_algn, self.t_checks, self.t_table,
                  self.t_counts,
                  self.t_plausible, self.t_silent, self.t_silicon, self.t_negative, self.t_pll,
                  self.t_forms, self.t_device_build):
            n0 = len(self.rows)
            t1 = time.time()
            try:
                t()
            except T.Fail as e:
                self.row(t.__name__).check(False, str(e))
            for row in self.rows[n0:]:
                print('%-72s %5d %5d %5.1fs' % (row.name[:72], row.cases,
                                                row.cases - len(row.fails), time.time() - t1))
                for f in row.fails[:8]:
                    print('    FAIL: %s' % f)
        if self.args.verbose and hasattr(self, 'tab'):
            for kd, e in zip(self.kernels, self.tab):
                print('    %s %7d instr %6d ticks  %s' % (kd.tag, e[1], e[4], kd.note))
        for k, v in self.metrics.items():
            print('  %-26s %s' % (k, v))
        cases = sum(r.cases for r in self.rows)
        fails = sum(len(r.fails) for r in self.rows)
        print('%d rows, %d cases, %d failed (%.0f s)' % (len(self.rows), cases, fails,
                                                         time.time() - t0))
        return 1 if fails else 0


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--device', action='store_true',
                    help='also run the 4096-iteration device build (minutes)')
    a = ap.parse_args(argv)
    if not B.have_gcc():
        print('skip: vc4-elf-gcc not found at %s (set VC4_GCC)' % C.GCC)
        return 0
    return Suite(a).run_all()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
