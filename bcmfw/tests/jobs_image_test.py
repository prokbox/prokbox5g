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

"""The display jobs in a BCM image (bcmfw/video/jobs_bcm.c), in vc2sim:
the display-only image (BCMFW_HOSTPROTO + BCMFW_JOBS: gap G9's) booted
through lcd-video.c's port, driven over the video protocol's rings as the
PP driver will: the job block and its discovery, FILL/PRESENT/FENCE to the
panel, a slot (ALLOC, the host's write, COMMIT with its F16w) blitted
and shaded, a reserved job refused (DECODE, which the display-only image
offers: decode_image_test.py).  The merged image (with the video
decoder) runs the same code; its rows join video_test.py.  The visualizer
image (BCMFW_VIZ, proposed, sim only) has its own row: SPECTRUM
and BARS against jviz.c built for the Mac (viz_test.py).

Usage: bcmfw/tests/jobs_image_test.py [-v]
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hostproto_test as H     # noqa: E402
import hello_test as T         # noqa: E402
import dma_test as D           # noqa: E402  (the bench with the BCM's DMA model)

K = H.K
S_MASK = T.S.MASK
JDEFS = ('BCMFW_HOSTPROTO=1', 'BCMFW_JOBS=1')


def jobs_constants():
    import re
    text = open(os.path.join(T.FW, 'jobs.h')).read()
    out = {}
    for name, val in re.findall(r'^#define\s+(J_\w+|HP_JOBS)\s+(0x[0-9A-Fa-f]+|\d+)\b', text, re.M):
        out[name] = int(val, 0)
    return out


J = jobs_constants()


def xy(x, y):
    return x | y << 16


def slot_pitch(cls, w):
    """jobs.h J_SLOT_PITCH: a slot's row pitch in pixels"""
    return (w + 15) & ~15           # every class (2026-10-25: was the drift's only)


def wr_slot(vp, addr, pitch_bytes, pic, w, h):
    """The PP's upload: w x h pixels, row by row at the slot's pitch"""
    if pitch_bytes == 2 * w:
        vp.wr_block(addr, [pic[i] | pic[i + 1] << 16 for i in range(0, w * h, 2)])
        return
    for y in range(h):
        row = pic[y * w:(y + 1) * w] + [0] * (w & 1)
        vp.wr_block(addr + y * pitch_bytes, [row[i] | row[i + 1] << 16
                                              for i in range(0, len(row), 2)])


def f16w(px):
    """jobs.h j_f16w: COMMIT's A and B over the w x h pixels in row order"""
    a = b = 0
    for p in px:
        a += p
        b += a
    return [a & 0xFFFFFFFF, b & 0xFFFFFFFF]


def f16w_vectors():
    """jobs.h's vector slots: (name, pixels, [A, B]) from J_F16W_*"""
    s, pic = J['J_F16W_SEED'], []
    for _ in range(300 * 300):
        s = (s * 1103515245 + 12345) & 0xFFFFFFFF
        pic.append(s >> 16)
    return (('slot', pic, [J['J_F16W_SLOT_A'], J['J_F16W_SLOT_B']]),
            ('white', [0xFFFF] * 90000, [J['J_F16W_WHITE_A'], J['J_F16W_WHITE_B']]))


def shade(p, a):
    k = 256 - a
    return ((((p >> 11) * k) >> 8) << 11) | (((((p >> 5) & 63) * k) >> 8) << 5) | (((p & 31) * k) >> 8)


ALPHA = (92, 75, 59, 45, 33, 23, 15, 8, 4, 1)


def fade_px(d, s_, weight):
    """jmix_fade_px"""
    if weight >= 256:
        return s_
    if d == s_:
        return d
    w32 = (weight + 4) >> 3
    iw = 32 - w32
    if w32 == 0:
        w32, iw = 1, 31
    d = (d | d << 16) & 0x07e0f81f
    s_ = (s_ | s_ << 16) & 0x07e0f81f
    r = (((d * iw + s_ * w32 + 0x02008010) & 0xFFFFFFFF) >> 5) & 0x07e0f81f
    return (r | r >> 16) & 0xFFFF


def avg_px(a, b):
    """jmix_avg_px (JMIX_AVG2)"""
    return ((a & b) + (((a ^ b) & 0xf7de) >> 1)) & 0xFFFF


def blit_model(surfs, src, sxy, wh, dst, dxy, flags):
    """jref's BLIT as it was pixel by pixel (rev 0.1): surfs maps a surface
    id to (pixels, pitch); one surface's list is shared, so overlaps read
    what earlier pixels wrote"""
    S_, sp = surfs[src]
    D_, dp = surfs[dst]
    w, h = wh
    key, keyed = flags >> 16, flags & J['J_BLIT_KEY']
    up = src == dst and dxy[1] > sxy[1]
    for y in range(h):
        row = h - 1 - y if up else y
        so = (sxy[1] + row) * sp + sxy[0]
        do = (dxy[1] + row) * dp + dxy[0]
        if src == dst and so == do:
            continue
        xs = range(w - 1, -1, -1) if src == dst and so < do < so + w else range(w)
        for x in xs:
            v = S_[so + x]
            if not keyed or v != key:
                D_[do + x] = v


def slide_model(surfs, out, a):
    """jref_anim_frame's SLIDE at one offset, pixel by pixel, into out (the
    front frame).  From out itself (rev 0.4) it is the frame as the ANIM
    found it: a['snap'], or out as it is now"""
    O, op = surfs[out]
    (x0, y0), (w, h), d, off = a['xy'], a['wh'], a['dir'], a['off']
    surfs = dict(surfs)
    if a['from'] == out:
        surfs[out] = (a.get('snap') or list(O), op)
    for y in range(h):
        for x in range(w):
            sx, sy, src = x, y, a['from']
            if d == 0:
                sx = x + off
                if sx >= w:
                    sx, src = sx - w, a['to']
            elif d == 1:
                sx = x - off
                if sx < 0:
                    sx, src = sx + w, a['to']
            elif d == 2:
                sy = y + off
                if sy >= h:
                    sy, src = sy - h, a['to']
            else:
                sy = y - off
                if sy < 0:
                    sy, src = sy + h, a['to']
            S_, sp = surfs[src]
            O[(y0 + y) * op + x0 + x] = S_[(y0 + sy) * sp + x0 + sx]


def slide_want(el_ms, dur_ms, w):
    """The shell's slide easing (ui_main.c slide_frame(): cubic ease-out in
    1/1024 units), in ms: the columns shown el_ms into a slide of dur_ms"""
    if el_ms >= dur_ms:
        return w
    inv = 1024 - el_ms * 1024 // dur_ms
    rest = inv * inv // 1024 * inv // 1024
    return w - (w * rest // 1024)


def slide_offsets(dur_ms, w):
    """The ANIM SLIDE table for the BCM's 33 ms frames (jobs.md "Shell
    recipe"): frame k at the shell's first frame (one tick in, 10 ms) plus
    k x 33.3 ms, up to the frame that lands on w"""
    out = []
    while not out or out[-1] < w:
        out.append(slide_want(10 + 100 * len(out) // 3, dur_ms, w))
    return out


def cpu_slide(fb, strip, rect, push, offs):
    """The shell's CPU slide, as it runs today (ui_main.c slide_shift() and
    rect_xfer()): the framebuffer rect holds the old page and moves in
    place by each newly due column count, the new page's columns come from
    the strip (the rect's width a row); the last frame copies the strip
    whole.  Yields the framebuffer after each offset in offs"""
    x0, y0, w, h = rect
    done = 0
    for want in offs:
        dx = want - done
        if dx > 0:
            for y in range(h):
                o = (y0 + y) * 320 + x0
                row, srow = fb[o:o + w], strip[y * w:(y + 1) * w]
                if want == w:
                    row = list(srow)
                elif push:
                    row = row[dx:] + srow[done:done + dx]
                else:
                    row = srow[w - done - dx:w - done] + row[:w - dx]
                fb[o:o + w] = row
            done = want
        yield list(fb)


# ---- the cost audit (--audit; jobs.md section 10, "Cost audit") ----------

# Silicon timing (isa-vector.md, S7): a vector instruction of REP n about
# n + 0.5 cycles (ALU 16.25, vld16 16.63, vst16 16.50 at REP 16), a single
# row store about 1.5; scalar instructions counted as 1 cycle each (a
# floor: loads from SRAM and taken branches may cost more)
def vec_cycles(text):
    import re
    m = re.search(r'\brep(\d+)', text)
    return int(m.group(1)) + 0.5 if m else 1.5


class Prof:
    """Instructions and estimated cycles of one call, per function"""
    def __init__(self, prog):
        import bisect
        syms = sorted((a, n) for n, a in prog.symbols.items()
                      if not n.startswith('__') or n == '__udivdi3')
        self.addrs = [a for a, _ in syms]
        self.names = [n for _, n in syms]
        self.bisect = bisect.bisect_right
        self.fn = {}            # name -> [instructions, vector, cycles]

    def name(self, pc):
        i = self.bisect(self.addrs, pc) - 1
        return self.names[i] if i >= 0 else '?'

    def total(self, k=0):
        return sum(v[k] for v in self.fn.values())


def step(b, n_max, until, prof=None):
    """Single-step the core until until(pc); with prof, count each
    instruction against its function"""
    sim, r = b.sim, b.sim.r
    pc, n = b.pc, 0
    names = {}
    while not until(pc):
        h = sim.code.get(pc) or sim.fetch(pc)
        if prof is not None:
            nm = names.get(pc)
            if nm is None:
                nm = names[pc] = prof.name(pc)
            ins = h[2]
            v = prof.fn.setdefault(nm, [0, 0, 0.0])
            v[0] += 1
            if ins.e.name in ('vec48', 'vec80'):
                v[1] += 1
                v[2] += vec_cycles(ins.text)
            else:
                v[2] += 1
        r[31] = pc
        npc = h[0]()
        pc = (pc + h[1]) if npc is None else npc & S_MASK
        n += 1
        if n > n_max:
            raise T.Fail('no end after %d instructions (at %x)' % (n_max, pc))
    b.pc = pc
    sim.steps += n
    return n

class Suite(H.Suite):
    def bench(self, pid, defines=(), reset_pc=None):
        prog, rep, img = self.image(defines)
        entry = H.B.config_value('BCMFW_ENTRY', defines)
        b = D.DmaBench(img, pid, entry, reset_pc)
        h = T.Host(b, __import__('random').Random(self.rng.getrandbits(32)))
        return b, h, prog

    def jcall(self, vp, op, payload):
        """A job that should answer: its tag carries J_TAG_REPLY"""
        vp.tag = (vp.tag + 1) & 0x7fff or 1
        tag = vp.send(op, payload, tag=vp.tag | J['J_TAG_REPLY'])
        if tag is None:
            raise H.Timeout('command ring full')
        return vp.wait(tag)

    def setup(self, pid=1, awake=True, defines=JDEFS):
        b, h, prog = self.bench(pid, defines)
        h.fb = T.random_frame(self.rng)
        h.lcd_awake()
        h.settle()
        h.tick_task = False
        h.idle_ok = False
        vp = H.Vp(b)
        vp.attach()
        return b, h, vp

    def fence(self, vp, seq):
        op, st, p = vp.call(J['J_OP_FENCE'], [seq])
        return st == K['HP_OK'] and p and p[0] == seq

    def settle_dma(self, b):
        """Let a PRESENT's transfer finish: the DMA model moves words
        lazily, when a DMA register is read (vc2mmio.DmaEngine), and the
        job loop never waits on it (the hardware runs by itself)"""
        for _ in range(400):
            b.run(500)
            b.dma.progress()

    def t_block(self):
        row = T.Row('jobs: HELLO caps, HP_JOBS, the job block, J_OPS')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(1)
            row.check(vp.caps & K['HP_CAP_JOBS_2D'], 'caps %x' % vp.caps)
            base = b.r32(J['HP_JOBS'])
            row.check(base == 0xDE000, 'HP_JOBS %x' % base)
            row.check(b.r32(base + J['J_VERSION']) == (J['J_MAGIC'] << 16 | J['J_MAJOR'] << 8 |
                                                       J['J_MINOR']), 'J_VERSION')
            ops = b.r32(base + J['J_OPS'])
            for name in ('J_OP_FENCE', 'J_OP_ALLOC', 'J_OP_BLIT', 'J_OP_FADE', 'J_OP_SHADE',
                         'J_OP_PRESENT', 'J_OP_ANIM'):
                row.check(ops >> (J[name] - 0x20) & 1, '%s missing from J_OPS' % name)
            # the display-only image offers DECODE (decode_image_test.py)
            row.check(ops >> (J['J_OP_DECODE'] - 0x20) & 1, 'DECODE missing from J_OPS')
            ep = b.r32(base + J['J_EPOCH'])
            row.check(H.valid(ep) and ep & 0xFFFF >= 1, 'epoch %08x' % ep)
            # the block clocks (config.h BCMFW_JOBS_CLK_OFF): off at start,
            # the kept ones never written
            off = H.B.config_value('BCMFW_JOBS_CLK_OFF', JDEFS)
            row.check(off == 0xE3, 'BCMFW_JOBS_CLK_OFF %s' % off)
            for k in range(8):
                v = b.bus.regs.get(0x10001C2C + 4 * k)
                row.check(v == 0xA5A50000 if off >> k & 1 else v is None,
                          'clock +%02x: %s' % (0x2C + 4 * k, v))
            prog, rep, img = self.image(JDEFS)
            self.metrics['display-only image bytes'] = len(img)
            work = H.B.config_value('BCMFW_JOBS_DEC_WORK', JDEFS)
            row.check(rep['end'] <= work, 'display-only image+bss to 0x%x, past the decode '
                      'work area (0x%x)' % (rep['end'], work))
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def t_draw(self):
        row = T.Row('jobs: FILL, PRESENT, FENCE to the panel; a reserved job refused')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(3)
            vp.send(J['J_OP_FILL'], [J['J_SURF_FRONT'], xy(16, 8), xy(100, 50), 0x07E0])
            vp.send(J['J_OP_PRESENT'], [J['J_SURF_FRONT'], xy(16, 8), xy(100, 50), 0])
            row.check(self.fence(vp, 5), 'fence 5')
            self.settle_dma(b)
            p = b.panel.fb
            row.check(all(p[y * 320 + x] == 0x07E0 for y in range(8, 58) for x in range(16, 116)),
                      'panel rect not filled')
            row.check(p[7 * 320 + 16] != 0x07E0 and p[8 * 320 + 116] != 0x07E0,
                      'fill spilled outside its rect')
            op, st, _ = vp.call(J['J_OP_DECODE'], [0])
            row.check(st == K['HP_E_INVAL'], 'a short DECODE: status %d' % st)
            op, st, _ = vp.call(J['J_OP_GLYPHS'], [0])
            row.check(st == K['HP_E_UNKNOWN'], 'GLYPHS (reserved) status %d' % st)
            # the brief probes are the plain video image's only: here the
            # TV op (PB-7 to PB-9) stays unknown, its unlock included
            for args in ([K['HP_TV_MAGIC'], K['HP_TV_UNLOCK']],
                         [K['HP_TV_MAGIC'], K['HP_TV_TIMING'], 0, 1, 5]):
                op, st, _ = vp.call(K['HP_OP_TV'], args)
                row.check(st == K['HP_E_UNKNOWN'], 'TV %d in the merged image: status %d' % (
                    args[1], st))
            # ... and so does PW3/PW4's (the core's voltage and clock)
            for args in ([K['HP_PW_MAGIC'], K['HP_PW_UNLOCK']],
                         [K['HP_PW_MAGIC'], K['HP_PW_LEVEL'], 1, 5, 0],
                         [K['HP_PW_MAGIC'], K['HP_PW_POINT'], 1, 0, 0]):
                op, st, _ = vp.call(K['HP_OP_PW'], args)
                row.check(st == K['HP_E_UNKNOWN'], 'PW %d in the merged image: status %d' % (
                    args[1], st))
            # ... and PW5's hibernate park
            for args in ([K['HP_HIB_MAGIC'], K['HP_HIB_UNLOCK']],
                         [K['HP_HIB_MAGIC'], K['HP_HIB_PARK'], 1, 100]):
                op, st, _ = vp.call(K['HP_OP_HIB'], args)
                row.check(st == K['HP_E_UNKNOWN'], 'HIB %d in the merged image: status %d' % (
                    args[1], st))
            op, st, _ = vp.call(J['J_OP_FILL'], [J['J_SURF_FRONT'], xy(300, 0), xy(40, 1), 0])
            row.check(st == J['J_E_RANGE'], 'an off-frame FILL: status %d' % st)
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def t_slot(self):
        row = T.Row('jobs: ALLOC, the host writes, COMMIT with F16w, BLIT + SHADE, PRESENT')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(3)
            w, hh = 40, 30
            pic = [(i * 37 + (i // w) * 1001) & 0xFFFF for i in range(w * hh)]
            op, st, p = self.jcall(vp, J['J_OP_ALLOC'], [J['J_CLS_DRIFT'], xy(w, hh), J['J_FMT_RGB565'],
                                                 0x1234, 0], )
            row.check(st == K['HP_OK'] and len(p) >= 4, 'ALLOC status %d' % st)
            slot, addr, pitch = p[0], p[1], p[2]
            # the display-only image's drift pool (config.h BCMFW_JOBS_DO_DRIFT)
            drift = H.B.config_value('BCMFW_JOBS_DO_DRIFT', JDEFS)
            row.check(drift <= addr < drift + J['J_DRIFT_PITCH'] * 600 and
                      pitch == 2 * slot_pitch(J['J_CLS_DRIFT'], w) == 96,
                      'slot at %x pitch %d' % (addr, pitch))
            wr_slot(vp, addr, pitch, pic, w, hh)
            op, st, p = self.jcall(vp, J['J_OP_COMMIT'], [slot] + f16w(pic))
            row.check(st == K['HP_OK'] and p[:2] == f16w(pic), 'COMMIT status %d' % st)
            op, st, p = self.jcall(vp, J['J_OP_ALLOC'], [J['J_CLS_DRIFT'], xy(w, hh), J['J_FMT_RGB565'],
                                                 0x1234, 0])
            row.check(st == K['HP_OK'] and p[3] & J['J_ALLOC_HIT'] and p[0] == slot, 'no hit')
            # jobs.h's F16w vectors: this f16w(), then the vector slot
            # through the image's COMMIT with its padding not summed
            for name, vpic, want in f16w_vectors():
                row.check(f16w(vpic) == want, 'f16w() on the %s vector' % name)
            vpic = f16w_vectors()[0][1]
            op, st, p = self.jcall(vp, J['J_OP_ALLOC'], [J['J_CLS_DRIFT'], xy(300, 300),
                                                        J['J_FMT_RGB565'], 0xF16, 0])
            row.check(st == K['HP_OK'] and p[2] == 2 * J['J_DRIFT_PITCH'], 'vector ALLOC %d' % st)
            pad = []
            for y in range(300):
                pad += vpic[y * 300:(y + 1) * 300] + [0xDEAD] * (J['J_DRIFT_PITCH'] - 300)
            wr_slot(vp, p[1], p[2], pad, J['J_DRIFT_PITCH'], 300)
            want = [J['J_F16W_SLOT_A'], J['J_F16W_SLOT_B']]
            op, st, _ = self.jcall(vp, J['J_OP_COMMIT'], [p[0], want[0], want[1] ^ 1])
            row.check(st == J['J_E_SUM'], 'vector slot, a wrong B: status %d' % st)
            op, st, q = self.jcall(vp, J['J_OP_COMMIT'], [p[0]] + want)
            row.check(st == K['HP_OK'] and q[:2] == want, 'vector slot COMMIT %d %s' % (st, q))
            self.jcall(vp, J['J_OP_FREE'], [p[0]])
            a = [0, 0, 0]
            for i, v in enumerate(ALPHA):
                a[i // 4] |= v << (8 * (i % 4))
            vp.send(J['J_OP_BLIT'], [slot, 0, xy(w, hh), J['J_SURF_FRONT'], xy(200, 100), 0])
            vp.send(J['J_OP_SHADE'], [J['J_SURF_FRONT'], xy(200, 100), xy(10, hh)] + a)
            vp.send(J['J_OP_PRESENT'], [J['J_SURF_FRONT'], xy(200, 100), xy(w, hh), 0])
            row.check(self.fence(vp, 9), 'fence 9')
            self.settle_dma(b)
            want = [shade(pic[y * w + x], ALPHA[x]) if x < 10 else pic[y * w + x]
                    for y in range(hh) for x in range(w)]
            got = [b.panel.fb[(100 + y) * 320 + 200 + x] for y in range(hh) for x in range(w)]
            bad = sum(1 for g, e in zip(got, want) if g != e)
            row.check(bad == 0, '%d of %d panel pixels differ' % (bad, len(want)))
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    VIDEO_SHIPPED = ('BCMFW_VIDEO=1', 'BCMFW_IDLE_SLEEP=1', 'BCMFW_IDLE_GATED=1',
                     'BCMFW_IDLE_DOORBELL=1')     # bcmfw/mkvideo.py's image

    def swap(self, row, b, h, vp, defines, what, progress=None):
        """One warm swap (warm-swap.md section 2) into the image built
        with defines; returns the new image's Vp, or None.  progress: the
        parking image's program, when it reports its steps
        (BCMFW_PARK_FLUSH)"""
        C = H.B.config_value
        pulses, nev, starts = b.panel.pulses, len(b.panel.events), b.starts
        shown = list(b.panel.fb)
        op, st, p = vp.call(K['HP_OP_PARK'], [K['HP_PARK_MAGIC']])
        row.check(st == K['HP_OK'] and p and p[0] & K['HP_WARM_PANEL_ON'],
                  '%s: PARK status %d %s' % (what, st, p))
        mb, stub = K['HP_WARM_WORD'], C('BCMFW_PARK_STUB', ())
        in_stub = lambda: stub <= b.pc < stub + C('BCMFW_PARK_STUB_MAX', ())
        for _ in range(200):
            b.run(500)
            if (b.r32(mb) & ~0xFF) == K['HP_WARM_PARKED']:
                break
        w = b.r32(mb)
        row.check((w & ~0xFF) == K['HP_WARM_PARKED'] and in_stub(),
                  '%s: not parked (%08x, core at %x)' % (what, w, b.pc))
        if progress:
            # B13's diagnostics: the park's last step, and the stub's
            # words where the PP reads them back
            want = K['HP_WARM_PROG_MAGIC'] << 24 | K['HP_WARM_PH_CALL'] << 16
            prog = b.r32(K['HP_WARM_PROG'])
            # the flush: no retry (bit 0 reads 0 at the request), done at
            # the first poll (vc2sim's 0x10000c00|=1@1)
            row.check(prog == want, '%s: progress %08x' % (what, prog))
            sp = progress.symbols['bcmfw_park_stub']
            row.check(b.bus.read(stub, 16) == b.bus.read(sp, 16),
                      what + ': the stub copy differs from the image\'s')
        # the host: the image over the old one, as a cold start uploads it
        # (the core runs the stub meanwhile), the ready word cleared, the
        # cache flushed, then GO with the flags
        prog, rep, img = self.image(defines)
        for i in range(0, len(img), 4096):
            b.write_bytes(i, img[i:i + 4096])
            b.run(50)
        b.sim.code.clear()      # the core reads the new bytes
        b.w32(0x1F8, 0)
        b.w32(0x10000C00, 0xC0000000)
        row.check(any(b.r32(0x10000C00) & 1 for _ in range(4)), what + ': cache flush')
        b.w32(0x10000C00, 0)
        b.run(200)              # (the upload zeroed the warm word)
        row.check(in_stub(), '%s: the upload disturbed the stub (core at %x)' % (what, b.pc))
        b.w32(mb, K['HP_WARM_GO'] | (w & 0xFF))
        for _ in range(400):
            b.run(500)
            if b.r32(0x1F8):
                break
        row.check(b.r32(0x1F8) != 0, what + ': never said ready')
        row.check(b.r32(mb) == K['HP_WARM_UP'] | (w & 0xFF), '%s: warm word %08x' % (
            what, b.r32(mb)))
        info = b.r32(C('BCMFW_MB_INFO', ()))
        row.check(info & C('BCMFW_INFO_WARM', ()) and info & C('BCMFW_INFO_PANEL_ON', ()),
                  '%s: info %08x not warm, or the panel not on' % (what, info))
        row.check(b.starts == starts, '%s: the core was restarted' % what)
        row.check(b.panel.pulses == pulses and
                  not any(e[0] == 'reset' for e in b.panel.events[nev:]),
                  '%s: GPIO 48 moved' % what)
        row.check(b.panel.fb == shown, what + ': the panel lost its frame')
        vp = H.Vp(b)
        vp.attach()
        self.lcd_frame(row, b, h, what + ': a frame after the swap')
        row.check(b.panel.pulses == pulses, what + ': the first update reset the panel')
        self.metrics['warm swap %s bytes' % what] = len(img)
        return vp

    def t_park(self):
        row = T.Row('warm swap: display-only -> video image -> display-only, panel kept')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(3)
            self.lcd_frame(row, b, h, 'display-only image frame')
            row.check(vp.caps & K['HP_CAP_PARK'], 'caps %x lack PARK' % vp.caps)
            op, st, _ = vp.call(K['HP_OP_PARK'], [0])
            row.check(st == K['HP_E_INVAL'], 'PARK without its magic: status %d' % st)
            vp = self.swap(row, b, h, vp, self.VIDEO_SHIPPED, 'to video',
                           progress=self.image(JDEFS)[0])
            row.check(vp.caps & K['HP_CAP_H264_BP'], 'the video image: caps %x' % vp.caps)
            op, st, p = vp.call(K['HP_OP_OPEN'], [K['HP_CODEC_NULL'], 320 | 240 << 16,
                                                  K['HP_MODE_FIT'], 0, 320 | 240 << 16])
            row.check(st == K['HP_OK'], 'OPEN after the swap: %d' % st)
            op, st, _ = vp.call(K['HP_OP_PARK'], [K['HP_PARK_MAGIC']])
            row.check(st == K['HP_E_BUSY'], 'PARK with a session open: %d' % st)
            op, st, _ = vp.call(K['HP_OP_CLOSE'], [p[0] if p else 0, 0])
            row.check(st == K['HP_OK'], 'CLOSE: %d' % st)
            vp = self.swap(row, b, h, vp, JDEFS, 'back')
            row.check(vp.caps & K['HP_CAP_JOBS_2D'], 'the display-only image: caps %x' % vp.caps)
            prog, rep, img = self.image(JDEFS)
            self.metrics['park stub bytes'] = (prog.symbols['bcmfw_park_stub_end'] -
                                               prog.symbols['bcmfw_park_stub'])
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def t_park_unflushed(self):
        """B13 on silicon (2026-10-25, three builds): PARK answered, the
        stub's "PRK" never came.  The park before BCMFW_PARK_FLUSH ran the
        stub it had just copied with data stores and no cache flush, which
        Apple never does (vmcs.bin @88a0, @91a4).  Strict code coherence
        (vc2sim.StaleCode) refuses that first fetch; this row fails if the
        model stops refusing it, and t_park passes only with the flush."""
        row = T.Row('warm swap: a park without Apple\'s flush is refused (B13)')
        self.rows.append(row)
        defs = JDEFS + ('BCMFW_PARK_FLUSH=0',)
        try:
            b, h, vp = self.setup(3, defines=defs)
            self.lcd_frame(row, b, h, 'display-only image frame')
            msg = ''
            try:
                # the reply goes out first; the stub's fetch follows at
                # once, often inside the call's own run
                op, st, p = vp.call(K['HP_OP_PARK'], [K['HP_PARK_MAGIC']])
                row.check(st == K['HP_OK'], 'PARK status %d' % st)
                for _ in range(200):
                    b.run(500)
            except T.Fail as e:
                msg = str(e)
            row.check('written since the last cache flush' in msg and
                      '0x000de900' in msg,
                      'the unflushed stub ran (%s; warm word %08x)' % (
                          msg or 'no refusal', b.r32(K['HP_WARM_WORD'])))
            row.check(b.r32(K['HP_WARM_WORD']) == 0,
                      'the warm word %08x' % b.r32(K['HP_WARM_WORD']))
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def t_park_sticky(self):
        """The flush when bit 0 never reads clear (silicon left it set:
        PW1 read 0x10000C00 = 0x00010001).  Apple's re-request has no
        bound; ours gives up after 15 and parks: "PRK" comes, and the
        progress word's detail says 15 retries, no poll."""
        row = T.Row('warm swap: the park\'s flush with bit 0 stuck at 1 still parks')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(3)
            self.lcd_frame(row, b, h, 'display-only image frame')
            b.bus.rule('0x10000c00|=1')
            op, st, p = vp.call(K['HP_OP_PARK'], [K['HP_PARK_MAGIC']])
            row.check(st == K['HP_OK'], 'PARK status %d' % st)
            for _ in range(200):
                b.run(500)
                if (b.r32(K['HP_WARM_WORD']) & ~0xFF) == K['HP_WARM_PARKED']:
                    break
            row.check((b.r32(K['HP_WARM_WORD']) & ~0xFF) == K['HP_WARM_PARKED'],
                      'not parked: warm word %08x' % b.r32(K['HP_WARM_WORD']))
            want = (K['HP_WARM_PROG_MAGIC'] << 24 | K['HP_WARM_PH_CALL'] << 16 |
                    15 << 12)
            row.check(b.r32(K['HP_WARM_PROG']) == want,
                      'progress %08x' % b.r32(K['HP_WARM_PROG']))
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def t_window(self):
        row = T.Row('B5: PRESENT a window of slots at pitch 304 (300 wide) and 400, odd first row')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(3)
            for cls, w, hh, wy, wh in ((J['J_CLS_DRIFT'], 300, 300, 31, 200),
                                       (J['J_CLS_STRIP'], 400, 160, 41, 100)):
                pic = [(x * 7 + y * 131 + (x ^ y)) & 0xFFFF for y in range(hh) for x in range(w)]
                op, st, p = self.jcall(vp, J['J_OP_ALLOC'], [cls, xy(w, hh), J['J_FMT_RGB565'],
                                                            0xB5 + w, 0])
                row.check(st == K['HP_OK'] and p[2] == 2 * slot_pitch(cls, w),
                          'ALLOC %d: %d %s' % (w, st, p))
                slot, addr = p[0], p[1]
                wr_slot(vp, addr, p[2], pic, w, hh)
                op, st, p = self.jcall(vp, J['J_OP_COMMIT'], [slot] + f16w(pic))
                row.check(st == K['HP_OK'], 'COMMIT %d: %d' % (w, st))
                op, st, p = self.jcall(vp, J['J_OP_PRESENT'], [slot, xy(70, wy), xy(160, wh),
                                                              J['J_PRES_WAIT']])
                row.check(st == K['HP_OK'], 'PRESENT %d: %d' % (w, st))
                self.settle_dma(b)
                bad = sum(1 for y in range(wy, wy + wh) for x in range(70, 230)
                          if b.panel.fb[y * 320 + x] != pic[y * w + x])
                row.check(bad == 0, 'pitch %d: %d of %d panel pixels differ' % (w, bad, 160 * wh))
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def t_scroll(self):
        row = T.Row('SCROLL: vector 16-row groups and the C rest equal the model')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(3)
            fb, W_ = 0xE0000, 320
            rng = __import__('random').Random(19)
            px = [rng.getrandbits(16) for _ in range(W_ * 240)]
            vp.wr_block(fb, [px[i] | px[i + 1] << 16 for i in range(0, len(px), 2)])
            cases = [(0, 8, 320, 200, dy) for dy in (1, -1, 5, -7, 16, -16, 17, -31, 100, -100)]
            cases += [(16, 4, 200, 230, 9), (16, 4, 200, 230, -40),     # vector + C columns
                      (8, 10, 64, 50, 3), (3, 0, 33, 240, -19)]         # unaligned: all C
            for x0, y0, w, hh, dy in cases:
                steps = b.sim.steps
                op, st, p = self.jcall(vp, J['J_OP_SCROLL'], [J['J_SURF_FRONT'], xy(x0, y0),
                                                             xy(w, hh), dy & 0xFFFF])
                row.check(st == K['HP_OK'], 'SCROLL %s: %d' % ((x0, y0, w, hh, dy), st))
                if x0 == 0 and w == 320 and dy == 16:
                    self.metrics['SCROLL 320x200 by 16, instructions'] = b.sim.steps - steps
                old = list(px)
                for r in range(hh):
                    if 0 <= r - dy < hh:
                        for x in range(x0, x0 + w):
                            px[(y0 + r) * W_ + x] = old[(y0 + r - dy) * W_ + x]
                got = [b.bus.r16(fb + 2 * i) if hasattr(b.bus, 'r16') else
                       b.r32(fb + 2 * i - (2 * i & 2)) >> (16 * ((2 * i & 2) // 2)) & 0xFFFF
                       for i in range(W_ * 240)]
                bad = sum(1 for a_, e_ in zip(got, px) if a_ != e_)
                row.check(bad == 0, 'SCROLL %s: %d pixels differ' % ((x0, y0, w, hh, dy), bad))
                if bad:
                    px = got
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def front(self, b):
        """The front surface (0xE0000), 320 x 240, as pixels"""
        out = []
        for i in range(0, 320 * 240, 2):
            w = b.r32(0xE0000 + 2 * i)
            out += [w & 0xFFFF, w >> 16]
        return out

    def slot(self, vp, cls, w, hh, key, rng):
        pic = [rng.getrandbits(16) for _ in range(w * hh)]
        op, st, p = self.jcall(vp, J['J_OP_ALLOC'], [cls, xy(w, hh), J['J_FMT_RGB565'], key, 0])
        if st != K['HP_OK']:
            raise T.Fail('ALLOC %d: %d' % (key, st))
        wr_slot(vp, p[1], p[2], pic, w, hh)
        op, st, _ = self.jcall(vp, J['J_OP_COMMIT'], [p[0]] + f16w(pic))
        if st != K['HP_OK']:
            raise T.Fail('COMMIT %d: %d' % (key, st))
        return p[0], pic, w

    def t_mix(self):
        row = T.Row('FADE, AVG, SHADE: vector blocks and the C rest equal jmix.h')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(3)
            rng = __import__('random').Random(26)
            px = [rng.getrandbits(16) for _ in range(320 * 240)]
            vp.wr_block(0xE0000, [px[i] | px[i + 1] << 16 for i in range(0, len(px), 2)])
            s1 = self.slot(vp, J['J_CLS_DRIFT'], 320, 96, 0x61, rng)
            s2 = self.slot(vp, J['J_CLS_STRIP'], 192, 80, 0x62, rng)
            s3 = self.slot(vp, J['J_CLS_DRIFT'], 300, 64, 0x63, rng)    # pitch 304
            FR = J['J_SURF_FRONT']
            surfs = {FR: (None, 320)}

            def pix(sid, x, y):
                if sid == FR:
                    return px[y * 320 + x]
                sl = s1 if sid == s1[0] else s3 if sid == s3[0] else s2
                return sl[1][y * sl[2] + x]

            cases = []
            for wt in (1, 3, 4, 100, 252, 255, 256):     # in place, b a slot
                cases.append(('fade', FR, (0, 16), (320, 64), FR, (0, 16), s1[0], (0, 8), wt))
            cases += [('fade', FR, (32, 100), (160, 50), s1[0], (16, 3), s2[0], (32, 17), 77),
                      ('fade', FR, (3, 7), (40, 20), FR, (3, 7), s1[0], (5, 5), 128),   # C
                      ('avg', FR, (16, 0), (160, 40), FR, (16, 0), s1[0], (0, 40), 0),
                      ('avg', FR, (48, 150), (144, 33), s1[0], (32, 9), s2[0], (16, 1), 0),
                      ('avg', FR, (0, 200), (37, 30), s2[0], (0, 0), s1[0], (1, 1), 0),
                      # a 300-wide cover at its 304 pitch, in phase: the vector unit
                      ('fade', FR, (32, 100), (160, 50), FR, (32, 100), s3[0], (16, 3), 77),
                      ('avg', FR, (64, 20), (200, 40), FR, (64, 20), s3[0], (32, 10), 0),
                      ('fade', FR, (33, 60), (100, 20), FR, (33, 60), s3[0], (16, 3), 9)]
            # FADE with b's first n columns shaded (the drift's raw fade):
            # the vector past them
            cases += [('fade', FR, (160, 16), (160, 64), FR, (160, 16), s1[0], (32, 8), 77, 10),
                      ('fade', FR, (16, 100), (100, 32), s2[0], (0, 3), s1[0], (16, 40), 200, 32),
                      ('fade', FR, (5, 150), (60, 20), FR, (5, 150), s1[0], (5, 2), 128, 4)]
            for case in cases:
                kind, dsur, dxy, wh, asur, axy, bsur, bxy, wt = case[:9]
                ncols = case[9] if len(case) > 9 else 0
                al = [rng.randrange(0, 93) for _ in range(ncols)]
                op = J['J_OP_FADE'] if kind == 'fade' else J['J_OP_AVG']
                pay = [dsur, xy(*dxy), xy(*wh), asur, xy(*axy), bsur, xy(*bxy)]
                if kind == 'fade':
                    pay.append(wt)
                if ncols:
                    words = [0] * ((ncols + 3) // 4)
                    for i, v_ in enumerate(al):
                        words[i // 4] |= v_ << (8 * (i % 4))
                    pay += [J['J_FADE_SHADE_B'] | ncols] + words
                steps = b.sim.steps
                o, st, _ = self.jcall(vp, op, pay)
                row.check(st == K['HP_OK'], '%s %s: %d' % (kind, (dxy, wh, wt), st))
                if (kind, dxy, wh, wt) == ('fade', (0, 16), (320, 64), 100):
                    self.metrics['FADE 320x64, instructions'] = b.sim.steps - steps
                new = list(px)
                for y in range(wh[1]):
                    for x in range(wh[0]):
                        av = pix(asur, axy[0] + x, axy[1] + y)
                        bv = pix(bsur, bxy[0] + x, bxy[1] + y)
                        if x < ncols:
                            bv = shade(bv, al[x])
                        new[(dxy[1] + y) * 320 + dxy[0] + x] = (
                            fade_px(av, bv, wt) if kind == 'fade' else avg_px(av, bv))
                px = new
                got = self.front(b)
                bad = sum(1 for g_, e_ in zip(got, px) if g_ != e_)
                row.check(bad == 0, '%s %s weight %d: %d pixels differ' % (kind, (dxy, wh), wt, bad))
                px = got
            for x0, y0, w, hh in ((0, 0, 32, 48), (16, 5, 20, 40), (5, 0, 10, 17), (64, 64, 16, 16),
                                  (160, 0, 10, 240), (176, 3, 7, 33), (300, 20, 20, 16)):
                al = [rng.randrange(0, 93) for _ in range(w)]
                words = [0] * ((w + 3) // 4)
                for i, v_ in enumerate(al):
                    words[i // 4] |= v_ << (8 * (i % 4))
                steps = b.sim.steps
                o, st, _ = self.jcall(vp, J['J_OP_SHADE'], [FR, xy(x0, y0), xy(w, hh)] + words)
                row.check(st == K['HP_OK'], 'SHADE %s: %d' % ((x0, y0, w, hh), st))
                if (x0, w, hh) == (0, 32, 48):
                    self.metrics['SHADE 32x48, instructions'] = b.sim.steps - steps
                for y in range(y0, y0 + hh):
                    for x in range(x0, x0 + w):
                        px[y * 320 + x] = shade(px[y * 320 + x], al[x - x0])
                got = self.front(b)
                bad = sum(1 for g_, e_ in zip(got, px) if g_ != e_)
                row.check(bad == 0, 'SHADE %s: %d pixels differ' % ((x0, y0, w, hh), bad))
                px = got
            del surfs
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def read_px(self, b, addr, n):
        out = []
        for i in range(0, n, 2):
            w = b.r32(addr + 2 * i)
            out += [w & 0xFFFF, w >> 16]
        return out[:n]

    def t_blit(self):
        row = T.Row('BLIT, FILL, ANIM: vector paths and the C rest equal the model')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(3)
            rng = __import__('random').Random(24)
            FR, BK = J['J_SURF_FRONT'], J['J_SURF_BACK']
            back = H.B.config_value('BCMFW_JOBS_BACK', JDEFS)
            KEY = 0xF81F
            surfs = {}
            for sid, addr in ((FR, 0xE0000), (BK, back)):
                px = [rng.getrandbits(16) for _ in range(320 * 240)]
                vp.wr_block(addr, [px[i] | px[i + 1] << 16 for i in range(0, len(px), 2)])
                surfs[sid] = (px, 320, addr)

            def keyed_pic(n):
                return [KEY if rng.random() < 0.35 else rng.getrandbits(16) for _ in range(n)]

            for cls, w, hh, key in ((J['J_CLS_STRIP'], 320, 96, 0xB1), (J['J_CLS_DRIFT'], 300, 120, 0xB2)):
                pic = keyed_pic(w * hh)
                op, st, p = self.jcall(vp, J['J_OP_ALLOC'], [cls, xy(w, hh), J['J_FMT_RGB565'], key, 0])
                row.check(st == K['HP_OK'], 'ALLOC %d: %d' % (w, st))
                wr_slot(vp, p[1], p[2], pic, w, hh)
                op, st, _ = self.jcall(vp, J['J_OP_COMMIT'], [p[0]] + f16w(pic))
                surfs[p[0]] = (pic, w, p[1])
                if w == 320:
                    s2 = p[0]
                else:
                    s1 = p[0]
            # the front gets key pixels too, for the keyed overlaps
            for i in range(0, 320 * 240, 3):
                surfs[FR][0][i] = KEY
            vp.wr_block(0xE0000, [surfs[FR][0][i] | surfs[FR][0][i + 1] << 16
                                  for i in range(0, 320 * 240, 2)])
            KF = J['J_BLIT_KEY'] | KEY << 16
            blits = [(s2, (0, 0), (320, 96), FR, (0, 10), 0),         # 6 groups
                     (s2, (16, 3), (160, 50), FR, (32, 100), 0),      # + the last again
                     (s2, (5, 7), (100, 20), FR, (21, 30), 0),        # a head in C
                     (s2, (0, 0), (320, 96), FR, (0, 120), KF),       # keyed groups
                     (s2, (16, 9), (160, 37), BK, (64, 3), KF),       # keyed + again
                     (s2, (0, 1), (64, 10), FR, (0, 200), KF),        # keyed, C rows
                     (FR, (0, 0), (320, 240), BK, (0, 0), 0),         # the full frame
                     (FR, (0, 0), (320, 100), FR, (0, 20), 0),        # overlap, down
                     (FR, (16, 40), (200, 100), FR, (32, 10), 0),     # overlap, up
                     (FR, (10, 50), (200, 30), FR, (13, 50), 0),      # in a row, right
                     (FR, (13, 60), (200, 30), FR, (10, 60), 0),      # in a row, left
                     (FR, (10, 92), (201, 30), FR, (12, 92), 0),      # in a row, right,
                                                                      # words (rev 0.4)
                     (FR, (11, 125), (200, 9), FR, (15, 125), 0),     # ... an odd head
                     (FR, (10, 136), (100, 10), FR, (14, 136), KF),   # ... keyed
                     (FR, (0, 0), (160, 100), FR, (160, 120), 0),     # apart, one surface
                     (FR, (0, 0), (64, 64), FR, (16, 8), KF),         # keyed overlap
                     (s1, (3, 5), (160, 40), FR, (160, 0), 0),        # pitch 304, out of
                                                                      # phase: C
                     (s1, (16, 5), (160, 40), FR, (160, 60), 0),      # pitch 304 in phase:
                                                                      # the vector unit
                     (s2, (0, 0), (320, 10), BK, (0, 5), 0)]          # 10 rows, by rows
            for bl in blits:
                src, sxy, wh, dst, dxy, fl = bl
                o, st, _ = self.jcall(vp, J['J_OP_BLIT'], [src, xy(*sxy), xy(*wh), dst, xy(*dxy), fl])
                row.check(st == K['HP_OK'], 'BLIT %s: %d' % (bl, st))
                blit_model({k: v[:2] for k, v in surfs.items()}, *bl)
                px, pitch, addr = surfs[dst]
                got = self.read_px(b, addr, len(px))
                bad = sum(1 for g_, e_ in zip(got, px) if g_ != e_)
                row.check(bad == 0, 'BLIT %s: %d pixels differ' % (bl, bad))
                surfs[dst] = (got, pitch, addr)
            for sid, fxy, wh, c in ((FR, (0, 0), (320, 240), 0x1234), (FR, (5, 7), (150, 25), 0),
                                    (FR, (16, 3), (16, 1), 0xFFFF), (FR, (3, 3), (10, 10), 0x5555),
                                    (BK, (1, 1), (318, 238), 0x0F0F), (s2, (7, 2), (200, 50), 0x8001)):
                o, st, _ = self.jcall(vp, J['J_OP_FILL'], [sid, xy(*fxy), xy(*wh), c])
                row.check(st == K['HP_OK'], 'FILL %s: %d' % ((fxy, wh), st))
                px, pitch, addr = surfs[sid]
                for y in range(fxy[1], fxy[1] + wh[1]):
                    for x in range(fxy[0], fxy[0] + wh[0]):
                        px[y * pitch + x] = c
                got = self.read_px(b, addr, len(px))
                bad = sum(1 for g_, e_ in zip(got, px) if g_ != e_)
                row.check(bad == 0, 'FILL %s: %d pixels differ' % ((fxy, wh), bad))
                surfs[sid] = (got, pitch, addr)
            # fresh pictures for the animations
            for sid in (FR, BK):
                px, pitch, addr = surfs[sid]
                px = [rng.getrandbits(16) for _ in range(320 * 240)]
                vp.wr_block(addr, [px[i] | px[i + 1] << 16 for i in range(0, len(px), 2)])
                surfs[sid] = (px, pitch, addr)
            slides = [(BK, s2, (0, 0), (320, 96), 0, 37), (BK, s2, (0, 0), (320, 96), 1, 64),
                      (BK, s2, (0, 0), (320, 96), 2, 20), (s2, BK, (0, 0), (320, 96), 3, 33),
                      (BK, s2, (16, 8), (288, 80), 0, 16), (BK, BK, (0, 0), (320, 240), 3, 100),
                      (FR, BK, (16, 100), (160, 80), 2, 7), (FR, BK, (16, 100), (160, 80), 1, 16)]
            for fr, to, sxy, wh, d, off in slides:
                a = [J['J_ANIM_SLIDE'], fr, to, xy(*sxy), xy(*wh), d, 33, 1, off]
                self.anim_run(row, b, vp, a, 'SLIDE %s' % ((fr, to, wh, d, off),))
                slide_model({k: v[:2] for k, v in surfs.items()}, FR,
                            {'xy': sxy, 'wh': wh, 'dir': d, 'off': off, 'from': fr, 'to': to})
                self.anim_check_front(row, b, surfs, 'SLIDE %s' % ((fr, to, wh, d, off),))
            for src, wxy, wh, dxy in ((s2, (5, 3), (100, 40), (21, 150)), (s2, (0, 0), (320, 96), (0, 0)),
                                      (s2, (7, 0), (120, 33), (40, 10)), (FR, (0, 0), (100, 50), (8, 150))):
                a = [J['J_ANIM_PAN'], src, xy(*wxy), xy(*wxy), xy(*wh), xy(*dxy), 100, 0]
                self.anim_run(row, b, vp, a, 'PAN %s' % ((wxy, wh, dxy),))
                O = surfs[FR][0]
                S_, sp = surfs[src][:2]
                S_ = list(S_)           # rev 0.4: a BLIT, from FR too (apart from
                                        # its window: each frame draws the same)
                for y in range(wh[1]):
                    for x in range(wh[0]):
                        O[(dxy[1] + y) * 320 + dxy[0] + x] = S_[(wxy[1] + y) * sp + wxy[0] + x]
                self.anim_check_front(row, b, surfs, 'PAN %s' % ((wxy, wh, dxy),))
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def t_retained(self):
        """The retained screen (19-4; jobs.md "Shell recipe"): a push and a
        pop slide out of the front frame itself at the shell's offsets, each
        frame the BCM draws checked against the shell's own CPU slide; the
        four directions against the model; a glide as SCROLL by the
        shell's steps with the newly shown rows and the scrollbar column
        uploaded into the back frame and BLITted, PRESENT of the list's
        rect (and an unaligned one) checked on the panel"""
        row = T.Row('retained: SLIDE at the shell\'s offsets, SCROLL by its glide, PRESENT rect')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(3)
            prog = self.image(JDEFS)[0]
            frames_at = prog.symbols['anim_frames']
            rng = __import__('random').Random(194)
            FR, BK = J['J_SURF_FRONT'], J['J_SURF_BACK']
            back = H.B.config_value('BCMFW_JOBS_BACK', JDEFS)

            def put(addr, px):
                vp.wr_block(addr, [px[i] | px[i + 1] << 16 for i in range(0, len(px), 2)])

            def rect_px(px, rect):
                x0, y0, w, hh = rect
                return [px[(y0 + y) * 320 + x0 + x] for y in range(hh) for x in range(w)]

            # -- slides: the shell's (push 190 ms, pop 290 ms into a split
            # screen; the list column of a split screen), then the four
            # directions of a band against the model
            slides = [((0, 20, 320, 220), 0, 190), ((0, 20, 320, 220), 1, 290),
                      ((0, 0, 320, 240), 0, 250), ((0, 20, 160, 220), 1, 190),
                      ((16, 60, 200, 96), 2, 190), ((16, 60, 200, 96), 3, 190)]
            for rect, d, dur in slides:
                x0, y0, w, hh = rect
                ext = w if d < 2 else hh
                offs = slide_offsets(dur, ext)
                old = [rng.getrandbits(16) for _ in range(320 * 240)]
                new = [rng.getrandbits(16) for _ in range(320 * 240)]
                put(0xE0000, old)               # the old page, on the panel
                put(back, new)                  # the new page, uploaded once
                strip = rect_px(new, rect)
                if d < 2:
                    want = list(cpu_slide(list(old), strip, rect, d == 0, offs))
                else:
                    want = []
                    for off in offs:
                        f = list(old)
                        slide_model({FR: (f, 320), BK: (new, 320)}, FR,
                                    {'xy': rect[:2], 'wh': rect[2:], 'dir': d, 'off': off,
                                     'from': FR, 'to': BK, 'snap': old})
                        want.append(f)
                a = [J['J_ANIM_SLIDE'], FR, BK, xy(x0, y0), xy(w, hh), d, 33, len(offs)]
                a += [offs[k] | (offs[k + 1] if k + 1 < len(offs) else 0) << 16
                      for k in range(0, len(offs), 2)]
                what = 'SLIDE %s dir %d %d ms' % (rect, d, dur)
                o, st, _ = self.jcall(vp, J['J_OP_ANIM'], a)
                row.check(st == K['HP_OK'], '%s: %d' % (what, st))
                seen, drawn = b.r32(frames_at), 0
                for _ in range(40000):
                    b.run(500)
                    n = b.r32(frames_at)
                    if n == seen:
                        continue
                    seen = n
                    got = self.read_px(b, 0xE0000, 320 * 240)
                    # the k-th frame the BCM draws is offset k (33 ms
                    # frames, its 1 MHz counter): the CPU's at that offset
                    k = min(n - 1, len(offs) - 1)
                    bad = sum(1 for g_, e_ in zip(got, want[k]) if g_ != e_)
                    row.check(bad == 0, '%s: frame %d (offset %d): %d pixels differ from '
                              'the CPU\'s' % (what, k, offs[k], bad))
                    drawn += 1
                    if n >= len(offs):
                        break
                row.check(drawn == len(offs), '%s: %d of %d frames' % (what, drawn, len(offs)))
                for _ in range(2000):           # the end event follows the last push
                    b.run(500)
                    vp.drain()
                    if any(e[0] == J['J_EV_ANIM'] for e in vp.events):
                        break
                row.check(any(e[0] == J['J_EV_ANIM'] for e in vp.events), what + ': no end')
                vp.events = []
                self.settle_dma(b)
                bad = sum(1 for y in range(y0, y0 + hh) for x in range(x0, x0 + w)
                          if b.panel.fb[y * 320 + x] != new[y * 320 + x])
                row.check(bad == 0, '%s: the panel\'s last frame: %d pixels differ' % (what, bad))

            # -- a glide: the list (rows of 25, view y 20 h 220, scrollbar
            # at x 312), the shell's steps (scroll.c ui_scroll_step(), 3
            # ticks a frame), each frame SCROLL + the new rows and the
            # scrollbar column through the back frame + PRESENT (WAIT)
            VY, VH, RH, SBX = 20, 220, 25, 312
            lst = [rng.getrandbits(16) for _ in range(320 * RH * 40)]

            def cpu_frame(pos):
                f = list(fb0)
                for r in range(VH):
                    f[(VY + r) * 320:(VY + r + 1) * 320] = lst[(pos + r) * 320:(pos + r + 1) * 320]
                for r in range(VH):             # the bar: blended, per frame
                    for x in range(SBX, 320):
                        f[(VY + r) * 320 + x] = (pos * 977 + r * 31 + x) & 0xFFFF
                return f

            fb0 = self.read_px(b, 0xE0000, 320 * 240)
            pos, target, cur = 0, 0, cpu_frame(0)
            put(0xE0000, cur)
            steps = []
            for move in (RH, RH, 3 * RH, 2 * RH, -RH, -4 * RH, 5 * RH):
                target = max(0, min(target + move, RH * 40 - VH))
                while pos != target:
                    for _ in range(3):          # one frame: dt = 3 ticks
                        if pos == target:
                            break
                        dd = target - pos
                        m_ = (abs(dd) + 2) // 3
                        pos += m_ if dd > 0 else -m_
                    steps.append(pos)
            prev, seq, frames, bad_frames = 0, 100, 0, 0
            for pos in steps:
                d = pos - prev                  # the list moves up by d
                new = cpu_frame(pos)
                o, st, _ = self.jcall(vp, J['J_OP_SCROLL'], [FR, xy(0, VY), xy(320, VH),
                                                             (-d) & 0xFFFF])
                row.check(st == K['HP_OK'], 'SCROLL by %d: %d' % (-d, st))
                ny, nh = (VY + VH - d, d) if d > 0 else (VY, -d)
                bands = [(0, ny, 320, nh), (SBX, VY, 320 - SBX, VH)]
                for bx, by, bw, bh in bands:    # the PP's upload, then BLIT
                    for y in range(by, by + bh):
                        put(back + 2 * (y * 320 + bx), new[y * 320 + bx:y * 320 + bx + bw])
                    o, st, _ = self.jcall(vp, J['J_OP_BLIT'], [BK, xy(bx, by), xy(bw, bh), FR,
                                                              xy(bx, by), 0])
                    row.check(st == K['HP_OK'], 'BLIT a band: %d' % st)
                o, st, _ = self.jcall(vp, J['J_OP_PRESENT'], [FR, xy(0, VY), xy(320, VH),
                                                              J['J_PRES_WAIT']])
                seq += 1
                row.check(self.fence(vp, seq), 'fence %d' % seq)
                got = self.read_px(b, 0xE0000, 320 * 240)
                bad = sum(1 for g_, e_ in zip(got, new) if g_ != e_)
                row.check(bad == 0, 'glide to %d (by %d): %d pixels differ from the CPU\'s '
                          'frame' % (pos, d, bad))
                pbad = sum(1 for y in range(VY, VY + VH) for x in range(320)
                           if b.panel.fb[y * 320 + x] != new[y * 320 + x])
                bad_frames += pbad != 0
                frames += 1
                prev = pos
            row.check(bad_frames == 0, 'glide: %d of %d frames not on the panel after '
                      'PRESENT WAIT' % (bad_frames, frames))
            self.metrics['retained glide frames'] = frames
            # an unaligned PRESENT: only its rect reaches the panel
            before = list(b.panel.fb)
            o, st, _ = self.jcall(vp, J['J_OP_FILL'], [FR, xy(5, 37), xy(150, 25), 0x5AA5])
            o, st, _ = self.jcall(vp, J['J_OP_PRESENT'], [FR, xy(5, 37), xy(150, 25),
                                                          J['J_PRES_WAIT']])
            row.check(st == K['HP_OK'], 'PRESENT rect: %d' % st)
            self.settle_dma(b)
            bad = sum(1 for y in range(240) for x in range(320)
                      if b.panel.fb[y * 320 + x] != (0x5AA5 if 5 <= x < 155 and 37 <= y < 62
                                                      else before[y * 320 + x]))
            row.check(bad == 0, 'PRESENT rect: %d panel pixels wrong' % bad)
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def t_marquee(self):
        """PAN's stepped form (jobs.h J_PAN_STEP, rev 0.3: the marquee):
        single frames at the rest and at steps 1, 2, the middle and the
        last, after a wrap, for labels wider than the window at several
        widths, heights, destinations (phases) and steps; then one whole
        lap at 1 ms steps, its frame count and its rest at the end.  Each
        frame against a Python model of jref's window (jtest checks that
        window against F2's own timing)"""
        row = T.Row('stepped PAN (the marquee): frames, wrap and pause equal the model')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(3)
            prog = self.image(JDEFS)[0]
            frames_at = prog.symbols['anim_frames']
            rng = __import__('random').Random(1955)
            FR = J['J_SURF_FRONT']
            px = [rng.getrandbits(16) for _ in range(320 * 240)]
            vp.wr_block(0xE0000, [px[i] | px[i + 1] << 16 for i in range(0, len(px), 2)])
            front = px
            STEP, PAUSE = 50, 1000

            def window(pic, sw, sxy, wh, dxy):
                for y in range(wh[1]):
                    for x in range(wh[0]):
                        front[(dxy[1] + y) * 320 + dxy[0] + x] = \
                            pic[(sxy[1] + y) * sw + sxy[0] + x]

            def compare(what):
                got = self.read_px(b, 0xE0000, 320 * 240)
                bad = sum(1 for g_, e_ in zip(got, front) if g_ != e_)
                row.check(bad == 0, '%s: %d pixels differ' % (what, bad))
                front[:] = got

            # (window w, h, the label's lap L, step px, rest band below,
            # destination, slot width rounded up to 16 px): widths from
            # the split list's to the full row, 16 and 18 rows (one group,
            # a group and its last again), an odd and an even lap,
            # destinations at three phases.  A strip's pitch is its width:
            # rounded to 16 px its rows stay 32-byte aligned and the
            # window goes to the vector unit (jv_gather16 at every phase);
            # an odd width takes the C copy, exact too
            cases = [(271, 18, 301, 2, False, (9, 50), True),
                     (200, 16, 37 + 40, 2, True, (16, 100), True),
                     (150, 18, 160, 3, False, (21, 150), False),
                     (271, 18, 1240, 2, True, (9, 200), True)]
            for tw, rh, L, sp, below, dxy, r16 in cases:
                sw = tw + L + tw
                if r16:
                    sw = (sw + 15) & ~15
                sh = 2 * rh if below else rh
                op, st, p = self.jcall(vp, J['J_OP_ALLOC'], [J['J_CLS_STRIP'], xy(sw, sh),
                                                            J['J_FMT_RGB565'], 0x5E0 + L, 0])
                row.check(st == K['HP_OK'], 'ALLOC %d x %d: %d' % (sw, sh, st))
                if st != K['HP_OK']:
                    continue
                pic = [rng.getrandbits(16) for _ in range(sw * sh)]
                wr_slot(vp, p[1], p[2], pic, sw, sh)
                op, st, _ = self.jcall(vp, J['J_OP_COMMIT'], [p[0]] + f16w(pic))
                slot = p[0]
                x0 = 0 if below else tw     # the lap starts after the rest cell
                rest = (0, rh) if below else (0, 0)
                steps = (L - 1) // sp       # K - 1: the steps a lap shows
                lap = PAUSE + steps * STEP
                for k in (0, 1, 2, steps // 2, steps, 'wrap'):
                    start = (0 if k == 0 else
                             lap + PAUSE + 2 * STEP if k == 'wrap' else PAUSE + (k - 1) * STEP)
                    kk = 3 if k == 'wrap' else k
                    a = [J['J_ANIM_PAN'], slot, xy(x0, 0), xy(x0 + L, 0), xy(tw, rh), xy(*dxy),
                         STEP, J['J_PAN_STEP'] | 1, sp | PAUSE << 16, xy(*rest), start]
                    what = 'w %d h %d lap %d px %d step %s' % (tw, rh, L, sp, k)
                    o, st, _ = self.jcall(vp, J['J_OP_ANIM'], a)
                    row.check(st == K['HP_OK'], '%s: ANIM %d' % (what, st))
                    for _ in range(4000):
                        b.run(500)
                        if b.r32(frames_at):
                            break
                    o, st, rp = self.jcall(vp, J['J_OP_ANIM_STOP'], [0])
                    row.check(st == K['HP_OK'] and rp and rp[0] == 1,
                              '%s: ANIM_STOP %d, frames %s' % (what, st, rp))
                    self.settle_dma(b)
                    window(pic, sw, rest if kk == 0 else (x0 + kk * sp, 0), (tw, rh), dxy)
                    compare(what)
                self.jcall(vp, J['J_OP_FREE'], [slot])

            # one whole lap, loop 0: steps of 1 ms, a 2 ms pause; the
            # event counts lap + 1 frames and the last shows the rest
            tw, rh, L, sp = 64, 16, 37, 5
            sw = tw + L + tw
            slot, pic, _ = self.slot(vp, J['J_CLS_STRIP'], sw, rh, 0x5EF, rng)
            steps = (L - 1) // sp
            a = [J['J_ANIM_PAN'], slot, xy(tw, 0), xy(tw + L, 0), xy(tw, rh), xy(40, 20),
                 1, J['J_PAN_STEP'], sp | 2 << 16, 0, 0]
            o, st, _ = self.jcall(vp, J['J_OP_ANIM'], a)
            row.check(st == K['HP_OK'], 'one lap: ANIM %d' % st)
            ev = None
            for _ in range(20000):
                b.run(500)
                vp.drain()
                ev = [e for e in vp.events if e[0] == J['J_EV_ANIM']]
                if ev:
                    break
            row.check(bool(ev), 'one lap: no end event')
            if ev:
                row.check(ev[0][2][0] == 2 + steps + 1,
                          'one lap: %s frames, want %d' % (ev[0][2][:1], 2 + steps + 1))
            vp.events = []
            self.settle_dma(b)
            window(pic, sw, (0, 0), (tw, rh), (40, 20))
            compare('one lap ends at rest')
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def t_phase(self):
        """Sources out of phase with the destination (jv_gather16): every
        phase 0-15 for BLIT, keyed BLIT, FADE and AVG, against the pixel-
        by-pixel models; odd widths, unaligned heads, the last rows (a
        group again, or C), three-surface rects, a source beside its
        destination in one surface"""
        row = T.Row('out of phase: BLIT, KEY, FADE, raw FADE, AVG at every phase 0-15')
        self.rows.append(row)
        try:
            b, h, vp = self.setup(3)
            rng = __import__('random').Random(1615)
            FR, BK = J['J_SURF_FRONT'], J['J_SURF_BACK']
            back = H.B.config_value('BCMFW_JOBS_BACK', JDEFS)
            KEY = 0x07E0
            surfs = {}
            for sid, addr in ((FR, 0xE0000), (BK, back)):
                px = [rng.getrandbits(16) for _ in range(320 * 240)]
                vp.wr_block(addr, [px[i] | px[i + 1] << 16 for i in range(0, len(px), 2)])
                surfs[sid] = (px, 320, addr)
            slots = {}
            for name, cls, w, hh, keyed in (('strip', J['J_CLS_STRIP'], 320, 64, False),
                                            ('keyed', J['J_CLS_STRIP'], 320, 40, True),
                                            ('cover', J['J_CLS_DRIFT'], 300, 64, False)):
                pic = [KEY if keyed and rng.random() < 0.35 else rng.getrandbits(16)
                       for _ in range(w * hh)]
                op, st, p = self.jcall(vp, J['J_OP_ALLOC'], [cls, xy(w, hh), J['J_FMT_RGB565'],
                                                            0x160 + len(slots), 0])
                row.check(st == K['HP_OK'], 'ALLOC %s: %d' % (name, st))
                wr_slot(vp, p[1], p[2], [v for v in pic], w, hh)
                op, st, _ = self.jcall(vp, J['J_OP_COMMIT'], [p[0]] + f16w(pic))
                pitch = p[2] // 2
                full = []
                for y in range(hh):
                    full += pic[y * w:(y + 1) * w] + [0] * (pitch - w)
                surfs[p[0]] = (full, pitch, p[1])
                slots[name] = p[0]
            sA, sK, sC = slots['strip'], slots['keyed'], slots['cover']
            KF = J['J_BLIT_KEY'] | KEY << 16

            def check(what, sid):
                px, pitch, addr = surfs[sid]
                got = self.read_px(b, addr, len(px))
                bad = sum(1 for g_, e_ in zip(got, px) if g_ != e_)
                row.check(bad == 0, '%s: %d pixels differ' % (what, bad))
                surfs[sid] = (got, pitch, addr)

            for p_ in range(16):
                dx = 32 if p_ % 2 == 0 else 35             # an aligned and an unaligned head
                blits = [(sA, (16 + p_, 1), (53 + p_, 37), BK, (dx, 4 + p_), 0),
                         (sC, (p_, 3), (160, 33), FR, (160, 2 * p_), 0),     # pitch 304
                         (sK, (32 + p_, 2), (47 + 2 * p_, 21), FR, (dx + 64, 100), KF),
                         (FR, (16 + p_, 150), (64, 40), FR, (96, 196), 0)]   # apart, one surface
                for bl in blits:
                    src, sxy, wh, dst, dxy, fl = bl
                    o, st, _ = self.jcall(vp, J['J_OP_BLIT'], [src, xy(*sxy), xy(*wh), dst,
                                                              xy(*dxy), fl])
                    row.check(st == K['HP_OK'], 'BLIT %s: %d' % (bl, st))
                    blit_model({k_: v[:2] for k_, v in surfs.items()}, *bl)
                    check('phase %d BLIT %s' % (p_, bl), dst)
                pb = (7 * p_ + 3) % 16
                mixes = [('fade', FR, (32, 40), (49 + p_, 33), sA, (16 + p_, 3), sC, (pb, 5),
                          [1, 77, 128, 256][p_ % 4]),                    # three surfaces
                         ('fade', FR, (160, 100), (160, 20), FR, (160, 100), sC, (64 + p_, 30),
                          100),                                          # the drift's step
                         ('avg', BK, (dx, 200), (160, 17), sC, (64 + p_, 2), sC, (65 + p_, 2),
                          0),                                            # two framings
                         ('avg', FR, (0, 150), (51, 32), sA, (p_, 9), sA, (pb, 31), 0)]
                for kind, dsur, dxy, wh, asur, axy, bsur, bxy, wt in mixes:
                    op = J['J_OP_FADE'] if kind == 'fade' else J['J_OP_AVG']
                    pay = [dsur, xy(*dxy), xy(*wh), asur, xy(*axy), bsur, xy(*bxy)]
                    if kind == 'fade':
                        pay.append(wt)
                    o, st, _ = self.jcall(vp, op, pay)
                    row.check(st == K['HP_OK'], '%s %s: %d' % (kind, pay, st))
                    A_, ap = surfs[asur][:2]
                    B_, bp = surfs[bsur][:2]
                    D_, dp = surfs[dsur][:2]
                    new = list(D_)
                    for y in range(wh[1]):
                        for x in range(wh[0]):
                            av = A_[(axy[1] + y) * ap + axy[0] + x]
                            bv = B_[(bxy[1] + y) * bp + bxy[0] + x]
                            new[(dxy[1] + y) * dp + dxy[0] + x] = (
                                fade_px(av, bv, wt) if kind == 'fade' else avg_px(av, bv))
                    surfs[dsur] = (new,) + surfs[dsur][1:]
                    check('phase %d %s %s' % (p_, kind, (dxy, wh, axy, bxy)), dsur)
                # the raw FADE (b's first n columns shaded, jv_mix16 with
                # factors): 0-32 shaded columns, in place and three
                # surfaces, odd widths, aligned and unaligned heads (one or
                # two factor blocks), the last rows in C
                al = lambda n: [rng.randrange(256) for _ in range(n)]
                raws = [('fade', FR, (128, 40), (151 + p_, 37), FR, (128, 40), sC, (64 + p_, 2),
                         [1, 100, 200, 256][p_ % 4], al(p_)),
                        ('fade', BK, (dx, 120), (45 + 3 * p_, 33), sA, (16 + p_, 20), sC,
                         (pb, 20), [77, 128, 255, 3][p_ % 4], al(16 - p_)),
                        ('fade', FR, (dx + 128, 180), (40 + p_, 18), FR, (dx + 128, 180), sA,
                         (p_ + 1, 40), 128, al(17 + p_))]
                for case in raws:
                    op, pay, dsur = self.mix_model(surfs, case)
                    o, st, _ = self.jcall(vp, op, pay)
                    row.check(st == K['HP_OK'], 'raw fade %s: %d' % (pay, st))
                    check('phase %d raw fade %s, %d shaded' % (p_, case[2:8], len(case[9])),
                          dsur)
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def t_variants(self):
        """The lane shift's start-up check under every rule for an H16 A
        operand's +r x (vc2sim HYP v_a16_reg_x; dbl16 and near32 fit
        silicon, bench ASHF/ASHG, quirks #30): the gather (a transpose and
        a +r y shift) passes it under each, so J_VPU_SHIFT is set; the
        gather as it was (gen_jobs.py --mutate aplusr: A operands at +r
        lane offsets) passes only where x is taken as given, and the check
        keeps the C everywhere else"""
        import mutant
        import vc2vpu as VP
        row = T.Row('lane-shift self-check under each A +r x rule: new gather passes, old fails')
        self.rows.append(row)
        # the mutant goes to this process's builds only, never over the
        # tracked jobs_kernels.S, which concurrent benches build from
        regen = mutant.switcher(os.path.join(T.FW, 'video', 'gen_jobs.py'))
        saved = T.S.HYP['v_a16_reg_x']
        try:
            for mut in (None, 'aplusr'):
                regen(mut)
                # the old gather is longer: its image drops the overlay
                # stub to fit (and builds apart from the shipped one)
                defs = JDEFS + (('BCMFW_JOBS_OVL=0',) if mut else ())
                for rule in VP.HYP_ALTERNATIVES['v_a16_reg_x']:
                    T.S.HYP['v_a16_reg_x'] = rule
                    try:
                        b, h, vp = self.setup(3, defines=defs)
                        got = b.r32(b.r32(J['HP_JOBS']) + J['J_VPU'])
                    except (T.Fail, H.Timeout) as e:
                        row.check(False, '%s %s: %s' % (mut, rule, e))
                        continue
                    shift = bool(got & J['J_VPU_SHIFT'])
                    want = mut is None or rule == 'given'
                    row.check(shift == want and got & J['J_VPU_CHECKED'],
                              '%s under %s: J_VPU %x' % (mut or 'gather', rule, got))
        finally:
            T.S.HYP['v_a16_reg_x'] = saved
            regen()

    def t_gate(self):
        """The lane shift's start-up self-check (J_VPU): it passes in
        vc2sim, and out-of-phase jobs gather on the vector unit; built to
        fail (BCMFW_JOBS_SHIFT_FAIL), J_VPU_SHIFT stays clear and the same
        jobs take the C path (jv_gather16 never runs), the pixels exact
        either way"""
        row = T.Row('lane-shift self-check: J_VPU, vector or C, exact either way')
        self.rows.append(row)
        for fail in (False, True):
            defs = JDEFS + (('BCMFW_JOBS_SHIFT_FAIL=1',) if fail else ())
            what = 'check forced to fail' if fail else 'check passes'
            try:
                b, h, vp = self.setup(3, defines=defs)
                prog = self.image(defs)[0]
                jb = b.r32(J['HP_JOBS'])
                # the display-only image decodes: its IDCT check passes too
                # (decode_image_test.py's gate row)
                want = J['J_VPU_CHECKED'] | (0 if fail else J['J_VPU_SHIFT']) | J['J_VPU_MAC'] | \
                    J['J_VPU_YUV']
                got = b.r32(jb + J['J_VPU'])
                row.check(got == want, '%s: J_VPU %x, want %x' % (what, got, want))
                rng = __import__('random').Random(77)
                FR, BK = J['J_SURF_FRONT'], J['J_SURF_BACK']
                back = H.B.config_value('BCMFW_JOBS_BACK', defs)
                surfs = {}
                for sid, addr in ((FR, 0xE0000), (BK, back)):
                    px = [rng.getrandbits(16) for _ in range(320 * 240)]
                    vp.wr_block(addr, [px[i] | px[i + 1] << 16 for i in range(0, len(px), 2)])
                    surfs[sid] = (px, 320, addr)
                pic = [rng.getrandbits(16) for _ in range(300 * 64)]
                op, st, p = self.jcall(vp, J['J_OP_ALLOC'], [J['J_CLS_DRIFT'], xy(300, 64),
                                                            J['J_FMT_RGB565'], 0x170, 0])
                wr_slot(vp, p[1], p[2], pic, 300, 64)
                op, st, _ = self.jcall(vp, J['J_OP_COMMIT'], [p[0]] + f16w(pic))
                pitch = p[2] // 2
                full = []
                for y in range(64):
                    full += pic[y * 300:(y + 1) * 300] + [0] * (pitch - 300)
                sC = p[0]
                surfs[sC] = (full, pitch, p[1])
                jobs = [('blit', (sC, (69, 3), (160, 33), FR, (160, 20), 0)),
                        ('fade', ('fade', FR, (160, 100), (160, 20), FR, (160, 100), sC,
                                  (69, 30), 100, 0)),
                        ('fade', ('fade', FR, (32, 60), (100, 17), BK, (21, 7), sC, (13, 5),
                                  200, 0)),
                        ('avg', ('avg', BK, (64, 200), (160, 17), sC, (69, 2), sC, (70, 2),
                                 0, 0))]
                for kind, a_ in jobs:
                    if kind == 'blit':
                        src, sxy, wh, dst, dxy, fl = a_
                        op, pay = J['J_OP_BLIT'], [src, xy(*sxy), xy(*wh), dst, xy(*dxy), fl]
                        blit_model({k_: v[:2] for k_, v in surfs.items()}, *a_)
                    else:
                        op, pay, dst = self.mix_model(surfs, a_)
                    pr = self.audit_job(b, vp, prog, op, pay)
                    ran = 'jv_gather16' in pr.fn
                    row.check(ran != fail, '%s: %s %s: jv_gather16 %s' % (
                        what, kind, a_[1:3], 'ran' if ran else 'never ran'))
                    px, pt, addr = surfs[dst]
                    got = self.read_px(b, addr, len(px))
                    bad = sum(1 for g_, e_ in zip(got, px) if g_ != e_)
                    row.check(bad == 0, '%s: %s %s: %d pixels differ' % (what, kind, a_[1:3], bad))
                    surfs[dst] = (got, pt, addr)
            except (T.Fail, H.Timeout) as e:
                row.check(False, '%s: %s' % (what, e))

    def viz_job(self, b, vp, prog, op, payload):
        """audit_job's profile, and the reply"""
        step(b, 2_000_000, lambda pc: pc == prog.symbols['hp_poll'])
        vp.tag = (vp.tag + 1) & 0x7fff or 1
        tag = vp.send(op, payload, tag=vp.tag | J['J_TAG_REPLY'])
        p = self.prof_poll(b, prog)
        o, st, rep = vp.wait(tag)
        return p, st, rep

    def viz_run(self, row, b, vp, prog, what, fail, rng, lib):
        """SPECTRUM through the ring at 256 and 512 samples against jviz.c
        built for the Mac (the levels), checking the vector FFT ran unless
        fail; then BARS, the v1 frame and bare ones, against jviz_bars_draw
        on the Mac, pixel for pixel, checking the vector bars ran on every
        frame they take unless fail.  prog: the symbols to
        profile with (an overlay's added).  Returns every reply and frame,
        so two images can be compared case for case"""
        import ctypes
        import viz_test as V
        out = []
        op, st, p = self.jcall(vp, J['J_OP_ALLOC'], [J['J_CLS_STRIP'], xy(32, 16),
                                                    J['J_FMT_RGB565'], 0x5EC, 0])
        row.check(st == K['HP_OK'], '%s: ALLOC %d' % (what, st))
        slot, addr = p[0], p[1]
        op, st, _ = self.jcall(vp, J['J_OP_COMMIT'], [slot, 0, 0, J['J_COMMIT_NOCHECK']])
        for n in (256, 512):
            nb = 24 if n == 512 else 12
            edges = [1 + i * i * (n // 2 - 1) // (nb * nb) for i in range(nb + 1)]
            sigs = V.signals(n, rng)
            for cls, name, x in sigs[::6]:
                pcm = V.split(x) + [0] * (512 - n)
                wr_slot(vp, addr, 64, [v & 0xFFFF for v in pcm], 32, 16)
                pay = [slot, n | nb << 16] + [
                    edges[i] | (edges[i + 1] << 16 if i + 1 <= nb else 0)
                    for i in range(0, nb + 1, 2)]
                pr, st, rp = self.viz_job(b, vp, prog, J['J_OP_SPECTRUM'], pay)
                pw = (ctypes.c_uint32 * 256)()
                lib.jviz_power_ref(pw, (ctypes.c_int16 * 512)(*pcm[:512]), n)
                l2 = (ctypes.c_uint16 * 32)()
                lib.jviz_bands(l2, pw, n // 2, (ctypes.c_uint16 * 33)(*edges), nb)
                got = [(rp[k // 2] >> (16 * (k & 1))) & 0xFFFF for k in range(nb)] \
                    if st == K['HP_OK'] else None
                out.append(('SPECTRUM', n, name, st, got))
                row.check(got == list(l2[:nb]), '%s: SPECTRUM %d %s: %s, want %s' % (
                    what, n, name, got, list(l2[:nb])))
                ran = 'jv_fft%d' % n in pr.fn
                row.check(ran != fail, '%s: SPECTRUM %d: jv_fft%d %s' % (
                    what, n, n, 'ran' if ran else 'never ran'))
                if name.startswith('sine full'):
                    self.metrics['SPECTRUM %d (%s%s), 27 MHz' % (
                        n, 'C' if fail else 'vector', what[:8])] = \
                        '%d instructions, %d vector, ~%.0f cycles: %.2f ms' % (
                            pr.total(0), pr.total(1), pr.total(2), pr.total(2) / 27000)
        op, st, _ = vp.call(J['J_OP_SPECTRUM'], [slot, 384 | 4 << 16, 1, 2, 3])
        out.append(('SPECTRUM 384', st))
        row.check(st == K['HP_E_INVAL'], '%s: SPECTRUM of 384 samples: %d' % (what, st))
        cases = [(w, hh, bars, 0 if w > 300 else 7, 60 if w > 300 else 30)
                 for w, hh, bars in V.bars_cases(rng)[:4]]
        # J_BARS_FLIP: the shell's Mirror at full screen (the bars, then the
        # reflection upside down under the gap row) and a bare frame at x 7
        cases += [(320, hh, bars, 0, y) for y, hh, bars in V.mirror_frames()]
        w, hh, bars = V.bars_cases(V.random.Random(2))[1]
        cases.append((w, hh, dict(bars, flags=bars['flags'] | V.FLIP), 7, 30))
        for w, hh, bars, x0, y0 in cases:
            bars = dict(bars)
            front = self.front(b)
            pay = [J['J_SURF_FRONT'], xy(x0, y0), xy(w, hh),
                   bars['n'] | bars['barw'] << 8 | bars['gap'] << 16 |
                   bars['cap_h'] << 24, bars['bot'] | bars['top'] << 16,
                   bars['cap'] | bars['bg'] << 16, bars['flags']]
            words = (bars['n'] + 3) // 4
            for arr in (bars['height'], bars['peak']):
                for k in range(words):
                    pay.append(sum(arr[4 * k + i] << (8 * i) for i in range(4)))
            pr, st, _ = self.viz_job(b, vp, prog, J['J_OP_BARS'], pay)
            want_px = list(front)
            rc = -1                 # a rect past the frame: J_E_RANGE, no pixel
            if x0 + w <= 320 and y0 + hh <= 240:
                sub = [want_px[(y0 + y) * 320 + x0 + x] for y in range(hh)
                       for x in range(320 - x0)]
                rc, sub = V.mac_bars(lib, sub, 320 - x0, w, hh, bars)
                for y in range(hh):
                    want_px[(y0 + y) * 320 + x0:(y0 + y + 1) * 320] = \
                        sub[y * (320 - x0):(y + 1) * (320 - x0)]
            st_want = K['HP_OK'] if rc == 0 else J['J_E_RANGE']
            got = self.front(b)
            out.append(('BARS', w, hh, st, got))
            bad = sum(1 for g_, e_ in zip(got, want_px) if g_ != e_)
            row.check(st == st_want and bad == 0, '%s: BARS %dx%d n %d flags %d: status %d '
                      '(want %d), %d pixels differ' % (what, w, hh, bars['n'], bars['flags'],
                                                       st, st_want, bad))
            # the vector bars (J_VPU_BARS) take every frame of 16 to 256
            # rows that fits; the C the rest, and all of them if the check
            # failed
            ran = 'jv_bars16' in pr.fn
            vec = not fail and rc == 0 and 16 <= hh <= 256
            row.check(ran == vec, '%s: BARS %dx%d: jv_bars16 %s' % (
                what, w, hh, 'ran' if ran else 'never ran'))
            if w == 320:
                self.metrics['BARS v1 frame (320x150, %s%s), 27 MHz' % (
                    'C' if fail else 'vector', what[:8])] = \
                    '%d instructions, %d vector, ~%.0f cycles: %.2f ms' % (
                        pr.total(0), pr.total(1), pr.total(2), pr.total(2) / 27000)
        out += self.lines_run(row, b, vp, prog, what, fail, slot, addr, lib)
        return out

    def lines_run(self, row, b, vp, prog, what, fail, slot, addr, lib):
        """POLYLINE, SEGMENTS and PALROW (#126) through the ring: the shell's
        Scope (FILL, glow, core: from SPECTRUM's slot, split order), Radial
        and a Spectrogram row (its LUT a slot), full screen at 240 rows and
        under the volume overlay's 198, against jviz.c on the Mac pixel for
        pixel; offered (J_OPS) unless the start-up check is built to fail.
        The per-op fixed cost: a 1 x 1 FILL's whole job, ring to reply"""
        import viz_test as V
        rng = V.random.Random(9)
        out = []
        jb = b.r32(J['HP_JOBS'])
        ops = b.r32(jb + J['J_OPS'])
        bits = 7 << (J['J_OP_POLYLINE'] - 0x20)
        row.check(ops & bits == (0 if fail else bits), '%s: J_OPS %08x: the line ops %s' % (
            what, ops, 'offered' if ops & bits else 'not offered'))
        if fail:
            return out
        pr, st, _ = self.viz_job(b, vp, prog, J['J_OP_FILL'], [J['J_SURF_FRONT'], 0,
                                                               xy(1, 1), 0])
        self.metrics['per-op fixed cost (FILL 1x1, ring to reply), 27 MHz'] = \
            '%d instructions, ~%.0f cycles: %.3f ms' % (pr.total(0), pr.total(2),
                                                       pr.total(2) / 27000)
        op, st, p = self.jcall(vp, J['J_OP_ALLOC'], [J['J_CLS_STRIP'], xy(16, 16),
                                                    J['J_FMT_RGB565'], 0x107, 0])
        lslot, laddr = p[0], p[1]
        lut = V.pal_frame(rng)['lut']
        wr_slot(vp, laddr, 32, lut, 16, 16)
        op, st, _ = self.jcall(vp, J['J_OP_COMMIT'], [lslot] + f16w(lut))
        row.check(st == K['HP_OK'], '%s: the LUT slot: %d' % (what, st))
        for h in (240, 198):
            win = V.window(rng)
            wr_slot(vp, addr, 64, [v & 0xFFFF for v in win], 32, 16)
            frames = [('poly', dict(p_, src=win)) for p_ in V.scope_frame(rng, h)]
            frames.append(('segs', V.radial_frame(rng, h)))
            frames.append(('pal', dict(V.pal_frame(rng), lut=lut)))
            self.fill_front(vp)
            mac = self.front(b)
            for kind, a in frames:
                op = {'poly': 'J_OP_POLYLINE', 'segs': 'J_OP_SEGMENTS',
                      'pal': 'J_OP_PALROW'}[kind]
                hh = 4 if kind == 'pal' else h
                pr, st, _ = self.viz_job(b, vp, prog, J[op], V.wire(
                    kind, J['J_SURF_FRONT'], 0, 0, 320, hh, a,
                    lslot if kind == 'pal' else slot))
                rc, sub = V.draw_mac(lib, kind, mac[:320 * hh], 320, 320, hh, a)
                mac[:320 * hh] = sub
                got = self.front(b)
                out.append((kind, h, st, got))
                bad = sum(1 for g_, e_ in zip(got, mac) if g_ != e_)
                row.check(st == K['HP_OK'] and rc == 0 and bad == 0,
                          '%s: %s 320x%d: status %d, %d pixels differ from the Mac' % (
                              what, op, hh, st, bad))
                self.metrics['%s 320x%d (%s), 27 MHz' % (op[5:], hh, kind)] = \
                    '%d instructions, ~%.0f cycles: %.2f ms' % (
                        pr.total(0), pr.total(2), pr.total(2) / 27000)
        return out

    def t_viz(self):
        """The visualizer image (BCMFW_VIZ; #126, proposed, sim
        only): J_OPS and J_VPU; SPECTRUM through the ring at 256 and 512
        samples equals jviz.c built for the Mac (the levels), on the vector
        FFT when its start-up check passes and on the C when it is built to
        fail (BCMFW_VIZ_FFT_FAIL); BARS (the v1 frame and a bare one) equals
        jviz_bars_draw on the Mac, pixel for pixel; the costs"""
        import viz_test as V
        row = T.Row('viz image: SPECTRUM and BARS equal jviz.c, vector FFT or C')
        self.rows.append(row)
        lib = V.mac_lib()
        rng = __import__('random').Random(126)
        for fail in (False, True):
            defs = JDEFS + ('BCMFW_VIZ=1',) + (('BCMFW_VIZ_FFT_FAIL=1',) if fail else ())
            what = 'check forced to fail' if fail else 'check passes'
            try:
                b, h, vp = self.setup(3, defines=defs)
                prog, rep, img = self.image(defs)
                if not fail:
                    self.metrics['viz image bytes, image+bss end'] = '%d, 0x%x' % (
                        len(img), rep['end'])
                jb = b.r32(J['HP_JOBS'])
                ops = b.r32(jb + J['J_OPS'])
                for name in ('J_OP_SPECTRUM', 'J_OP_BARS', 'J_OP_FILL', 'J_OP_PRESENT'):
                    row.check(ops >> (J[name] - 0x20) & 1, '%s: %s missing' % (what, name))
                row.check(not ops >> (J['J_OP_DECODE'] - 0x20) & 1, 'DECODE in the viz image')
                want = J['J_VPU_CHECKED'] | J['J_VPU_SHIFT'] | J['J_VPU_FLIP'] | \
                    (0 if fail else J['J_VPU_FFT'] | J['J_VPU_BARS'])
                got = b.r32(jb + J['J_VPU'])
                row.check(got == want, '%s: J_VPU %x, want %x' % (what, got, want))
                self.viz_run(row, b, vp, prog, what, fail, rng, lib)
            except (T.Fail, H.Timeout) as e:
                row.check(False, '%s: %s' % (what, e))

    def overlay(self, defines=JDEFS, abi=None):
        """The visualizer's overlay for the display-only image built with
        these defines: (Program, report, the PP's upload as words)"""
        key = ('ovl', tuple(sorted(defines)), abi)
        if key not in self.builds:
            res = self.image(defines)[0]
            name = '-'.join(d.replace('BCMFW_', '').replace('=', '').lower()
                            for d in sorted(defines))
            out = os.path.join(T.ROOT, 'build', 'bcmfw', 'test-ovl-' + name +
                               ('' if abi is None else '-abi%d' % abi))
            prog, rep = H.B.build_overlay(res, out, list(defines), listing=False, abi=abi)
            img = open(os.path.join(out, 'viz-ovl.bin'), 'rb').read()
            words = [int.from_bytes(img[i:i + 4], 'little') for i in range(0, len(img), 4)]
            self.builds[key] = (prog, rep, words)
        return self.builds[key]

    def fill_front(self, vp):
        """The front frame one colour: two images' frames compare alike"""
        op, st, _ = self.jcall(vp, J['J_OP_FILL'], [J['J_SURF_FRONT'], 0, xy(320, 240), 0x39E7])
        if st != K['HP_OK']:
            raise T.Fail('FILL: %d' % st)

    def ovl_load(self, vp, words, sums=None, n=None, defines=JDEFS):
        """The PP's load: the words at the region's base, then OVERLAY"""
        vp.wr_block(H.B.config_value('BCMFW_JOBS_OVL', defines), words)
        if sums is None:
            sums = f16w([h for w in words for h in (w & 0xFFFF, w >> 16)])
        vp.tag = (vp.tag + 1) & 0x7fff or 1
        tag = vp.send(J['J_OP_OVERLAY'], [len(words) if n is None else n] + sums,
                      tag=vp.tag | J['J_TAG_REPLY'])
        t0, s0 = vp.b.now(), vp.b.sim.steps
        op, st, p = vp.wait(tag, timeout_us=2_000_000)      # init: the FFT's check
        if st == K['HP_OK'] and 'OVERLAY load (sum, init, FFT check)' not in self.metrics:
            self.metrics['OVERLAY load (sum, init, FFT check)'] = \
                '%.2f M instructions, %.1f ms simulated' % (
                    (vp.b.sim.steps - s0) / 1e6, (vp.b.now() - t0) / 1000)
        return st

    # ---- the staged upload (jobs rev 0.5, candidate images; #67) ----------

    STAGED = JDEFS + ('BCMFW_JOBS_STAGED=1',)
    MERGED_STAGED = VIDEO_SHIPPED + ('BCMFW_JOBS=1', 'BCMFW_JOBS_STAGED=1')
    # every class at its sizes: CR2's four, then the other pools' (the
    # display-only image's J_CLS_APP is 51.5 KB, so 120 x 200 at most)
    STAGED_CASES = ((J['J_CLS_DRIFT'], 300, 300), (J['J_CLS_STRIP'], 400, 160),
                    (J['J_CLS_STRIP'], 301, 7), (J['J_CLS_DRIFT'], 8, 1),
                    (J['J_CLS_DRIFT'], 301, 7), (J['J_CLS_THUMB'], 55, 55),
                    (J['J_CLS_APP'], 120, 200), (J['J_CLS_SCRATCH'], 128, 128),
                    (J['J_CLS_STRIP'], 1, 1))

    def staged_setup(self, defines, dma=True):
        """A staged image booted on a bench with SDRAM (video_test's), the
        DMA engine's channel 7 enabled by its config's bit 1 as on silicon
        (PW8's words; vc2mmio's model) unless dma is False: then a start
        goes nowhere and COMMIT's piece times out"""
        import random
        import video_test as V
        prog, rep, img = self.image(defines)
        b = V.VideoBench(img, 3, H.B.config_value('BCMFW_ENTRY', defines), None)
        h = T.Host(b, random.Random(self.rng.getrandbits(32)))
        h.fb = T.random_frame(self.rng)
        h.lcd_awake()
        h.settle()
        h.tick_task = False
        h.idle_ok = False
        if not dma:
            # vc2mmio takes bit 1 as an enable too (PW8's E802); a channel 7
            # config with both enables cleared makes the start go nowhere
            orig = b.dma.write

            def write(off, v):
                if off == 7 * 0x80:
                    v &= ~3
                orig(off, v)
            b.dma.write = write
        vp = H.Vp(b)
        vp.attach()
        return b, h, vp, prog

    def staged_put(self, b, vp, cls, w, hh, key, pic, stage=True):
        """ALLOC (asking for the window when stage) and the PP's rows where
        the reply says: the window in SDRAM through the host port's view,
        or the slot.  Returns (status, slot, addr, pitch, flags)"""
        import video_test as V
        a = [cls, xy(w, hh), J['J_FMT_RGB565'], key, 0]
        if stage:
            a.append(J['J_ALLOC_F_STAGE'])
        op, st, p = self.jcall(vp, J['J_OP_ALLOC'], a)
        if st != K['HP_OK']:
            return st, None, None, None, None
        slot, addr, pitch, flags = p[:4]
        if addr >= V.SDRAM_HOST:
            for y in range(hh):
                row = pic[y * w:(y + 1) * w]
                b.host_sdram_write(addr - V.SDRAM_HOST + y * pitch,
                                   b''.join(v.to_bytes(2, 'little') for v in row))
        else:
            wr_slot(vp, addr, pitch, pic, w, hh)
        return st, slot, addr, pitch, flags

    def staged_shown(self, b, vp, slot, w, hh, pic):
        """PRESENT the slot's top-left (at most the panel) and compare"""
        pw, ph = min(w, 320) & ~1, min(hh, 240)
        if not pw:
            return True
        op, st, _ = self.jcall(vp, J['J_OP_PRESENT'], [slot, 0, xy(pw, ph), J['J_PRES_WAIT']])
        if st != K['HP_OK']:
            return False
        self.settle_dma(b)
        return all(b.panel.fb[y * 320 + x] == pic[y * w + x]
                   for y in range(ph) for x in range(pw))

    def staged_rows(self, row, b, vp, what, how_want, rng):
        win_host = H.B.config_value('BCMFW_JOBS_STAGE_HOST', self.STAGED)
        for k, (cls, w, hh) in enumerate(self.STAGED_CASES):
            pic = [rng.getrandbits(16) for _ in range(w * hh)]
            st, slot, addr, pitch, flags = self.staged_put(b, vp, cls, w, hh, 0x5700 + k, pic)
            tag = '%s class %d %dx%d' % (what, cls, w, hh)
            row.check(st == K['HP_OK'] and flags & J['J_ALLOC_STAGED'] and addr == win_host and
                      pitch == 2 * slot_pitch(cls, w),
                      '%s: ALLOC %s %s' % (tag, st, (addr, pitch, flags)))
            if st != K['HP_OK']:
                continue
            op, st, p = self.jcall(vp, J['J_OP_COMMIT'], [slot] + f16w(pic))
            row.check(st == K['HP_OK'] and p == f16w(pic) + [how_want],
                      '%s: COMMIT %d %s' % (tag, st, p))
            row.check(self.staged_shown(b, vp, slot, w, hh, pic), tag + ': the panel')
            self.jcall(vp, J['J_OP_FREE'], [slot])
        # the direct path on the same image: no flag, the slot itself
        pic = [rng.getrandbits(16) for _ in range(40 * 30)]
        st, slot, addr, pitch, flags = self.staged_put(b, vp, J['J_CLS_DRIFT'], 40, 30, 0x57FF,
                                                       pic, stage=False)
        row.check(st == K['HP_OK'] and not flags & J['J_ALLOC_STAGED'] and addr < 0x140000,
                  '%s: direct ALLOC %s' % (what, (st, addr, flags)))
        op, st, p = self.jcall(vp, J['J_OP_COMMIT'], [slot] + f16w(pic))
        row.check(st == K['HP_OK'] and p == f16w(pic) + [J['J_HOW_DIRECT']],
                  '%s: direct COMMIT %d %s' % (what, st, p))
        self.jcall(vp, J['J_OP_FREE'], [slot])

    def t_staged(self):
        """The staged upload in the display-only candidate (jobs_staged):
        J_CAPS; every class and size, odd widths, through the window and
        the DMA; the direct path beside it; an epoch change mid-upload; a
        window taken by a second ALLOC; the J_E_SUM reply's words for a
        wrong sum and for a torn window; the core's copy after a DMA
        timeout"""
        import random
        row = T.Row('staged upload (display-only candidate): classes, epoch, J_E_SUM, DMA')
        self.rows.append(row)
        rng = random.Random(0x57A6)
        try:
            b, h, vp, prog = self.staged_setup(self.STAGED)
            base = b.r32(J['HP_JOBS'])
            caps = b.r32(base + J['J_CAPS'])
            row.check(caps == J['J_CAPS_MAGIC'] | J['J_CAP_STAGED'], 'J_CAPS %08x' % caps)
            row.check(b.r32(base + J['J_VERSION']) & 0xFF == J['J_MINOR'], 'J_VERSION minor')
            prog_, rep_, img = self.image(self.STAGED)
            row.check(rep_['end'] <= H.B.config_value('BCMFW_JOBS_DEC_WORK', self.STAGED),
                      'image+bss to 0x%x' % rep_['end'])
            self.metrics['staged display-only image bytes'] = len(img)
            self.staged_rows(row, b, vp, 'DMA', J['J_HOW_DMA'], rng)
            row.check(not b.dma.errors, 'engine errors: %s' % b.dma.errors[:2])
            # an epoch change mid-upload: the slot is gone (J_E_SLOT), the
            # window free, and the next staged upload works
            pic = [rng.getrandbits(16) for _ in range(64 * 32)]
            st, slot, addr, pitch, flags = self.staged_put(b, vp, J['J_CLS_STRIP'], 64, 32,
                                                           0x5801, pic[:64 * 16] + [0] * 64 * 16)
            self.jcall(vp, J['J_OP_RESET'], [])
            op, st, p = self.jcall(vp, J['J_OP_COMMIT'], [slot] + f16w(pic))
            row.check(st == J['J_E_SLOT'], 'COMMIT after RESET: %d' % st)
            st, slot, addr, pitch, flags = self.staged_put(b, vp, J['J_CLS_STRIP'], 64, 32,
                                                           0x5802, pic)
            op, st, p = self.jcall(vp, J['J_OP_COMMIT'], [slot] + f16w(pic))
            row.check(st == K['HP_OK'] and p[2] == J['J_HOW_DMA'],
                      'staged after the epoch change: %d %s' % (st, p))
            # the window taken by a second staged ALLOC before the first's
            # COMMIT: the first is checked as its slot stands (J_HOW_DIRECT)
            p1 = [rng.getrandbits(16) for _ in range(32 * 8)]
            p2 = [rng.getrandbits(16) for _ in range(32 * 8)]
            s1 = self.staged_put(b, vp, J['J_CLS_THUMB'], 32, 8, 0x5803, p1)[1]
            s2 = self.staged_put(b, vp, J['J_CLS_THUMB'], 32, 8, 0x5804, p2)[1]
            op, st, p = self.jcall(vp, J['J_OP_COMMIT'], [s1] + f16w(p1))
            row.check(st == J['J_E_SUM'] and len(p) == 5 and p[3] == J['J_HOW_DIRECT'] and
                      p[4] == 0, 'the first of two: %d %s' % (st, p))
            op, st, p = self.jcall(vp, J['J_OP_COMMIT'], [s2] + f16w(p2))
            row.check(st == K['HP_OK'] and p == f16w(p2) + [J['J_HOW_DMA']],
                      'the window\'s owner: %d %s' % (st, p))
            # a wrong sum: J_E_SUM with the BCM's A, B, the first word, how
            # and the window's B, all of them the pixels' own
            pic = [rng.getrandbits(16) for _ in range(300 * 300)]
            st, slot, addr, pitch, flags = self.staged_put(b, vp, J['J_CLS_DRIFT'], 300, 300,
                                                           0x5805, pic)
            want = f16w(pic)
            op, st, p = self.jcall(vp, J['J_OP_COMMIT'], [slot, want[0], want[1] ^ 1])
            row.check(st == J['J_E_SUM'] and p == want + [pic[0] | pic[1] << 16,
                                                          J['J_HOW_DMA'], want[1]],
                      'a wrong B: %d %s' % (st, p))
            self.jcall(vp, J['J_OP_FREE'], [slot])
            # a torn window (a word lost on the host's side): the window's B
            # differs from the PP's too, so the reply blames the upload
            torn = list(pic)
            torn[1000] ^= 0x0101
            st, slot, addr, pitch, flags = self.staged_put(b, vp, J['J_CLS_DRIFT'], 300, 300,
                                                           0x5806, torn)
            op, st, p = self.jcall(vp, J['J_OP_COMMIT'], [slot] + want)
            row.check(st == J['J_E_SUM'] and p[:2] == f16w(torn) and p[4] == f16w(torn)[1] and
                      p[4] != want[1], 'a torn window: %d %s' % (st, p))
            # the core's copy when the DMA never finishes
            b2, h2, vp2, _ = self.staged_setup(self.STAGED, dma=False)
            pic = [rng.getrandbits(16) for _ in range(301 * 7)]
            st, slot, addr, pitch, flags = self.staged_put(b2, vp2, J['J_CLS_STRIP'], 301, 7,
                                                           0x5807, pic)
            op, st, p = vp2.wait(vp2.send(J['J_OP_COMMIT'], [slot] + f16w(pic),
                                          tag=0x5807 | J['J_TAG_REPLY']), timeout_us=400_000)
            row.check(st == K['HP_OK'] and p == f16w(pic) + [J['J_HOW_CORE']],
                      'DMA timed out, the core copied: %d %s' % (st, p))
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def t_staged_merged(self):
        """The staged upload in the merged candidate
        (bcmvideo-jobs-staged.bin): its image below the arena, every class
        and size by the core's copy, then a video OPEN with SDRAM already up
        (the arena taken: the epoch moves, the window's owner goes)"""
        import random
        import video_test as V
        row = T.Row('staged upload (merged candidate): classes by the core, then OPEN')
        self.rows.append(row)
        rng = random.Random(0x57A7)
        try:
            prog_, rep_, img = self.image(self.MERGED_STAGED)
            row.check(rep_['end'] <= H.B.config_value('BCMFW_VIDEO_ARENA', self.MERGED_STAGED),
                      'image+bss to 0x%x' % rep_['end'])
            self.metrics['staged merged image bytes'] = len(img)
            b, h, vp, prog = self.staged_setup(self.MERGED_STAGED)
            base = b.r32(J['HP_JOBS'])
            row.check(b.r32(base + J['J_CAPS']) == J['J_CAPS_MAGIC'] | J['J_CAP_STAGED'],
                      'J_CAPS %08x' % b.r32(base + J['J_CAPS']))
            self.staged_rows(row, b, vp, 'core', J['J_HOW_CORE'], rng)
            pic = [rng.getrandbits(16) for _ in range(64 * 32)]
            st, slot, addr, pitch, flags = self.staged_put(b, vp, J['J_CLS_STRIP'], 64, 32,
                                                           0x5901, pic)
            pv = V.Player(b)
            pv.attach()
            pv.tag = vp.tag
            st2, _ = pv.open(320, 240)
            row.check(st2 == K['HP_OK'], 'OPEN with SDRAM up: %d' % st2)
            pv.call(K['HP_OP_CLOSE'], [pv.session], timeout_us=100_000)
            op, st, p = self.jcall(pv, J['J_OP_COMMIT'], [slot] + f16w(pic))
            row.check(st in (J['J_E_SLOT'], J['J_E_MODE']),
                      'COMMIT across a video session: %d' % st)
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def t_ovl(self):
        """The display-only image's overlay (J_OP_OVERLAY; #126, proposed,
        sim only): J_OPS bit 21 and J_OVL; SPECTRUM and BARS refused until
        it is loaded; loaded, they equal jviz.c built for the Mac and the
        BCMFW_VIZ image's replies and frames, case for case, the FFT on the
        vector unit (or the C, its check built to fail); a corrupt, short,
        oversized, truncated or foreign (another ABI) overlay is refused and
        offers nothing; an epoch change (RESET) drops the ops until the next
        load (decode_image_test.py: a DECODE beside it)"""
        import random
        import viz_test as V
        row = T.Row('overlay: load, check, SPECTRUM/BARS = jviz.c = viz image, epoch')
        self.rows.append(row)
        lib = V.mac_lib()
        bits = 1 << (J['J_OP_SPECTRUM'] - 0x20) | 1 << (J['J_OP_BARS'] - 0x20)
        lbits = 7 << (J['J_OP_POLYLINE'] - 0x20)      # the line ops, if checked
        base = H.B.config_value('BCMFW_JOBS_OVL', JDEFS)
        size = H.B.config_value('BCMFW_JOBS_OVL_SIZE', JDEFS)
        results = {}
        for fail in (False, True):
            defs = JDEFS + (('BCMFW_VIZ_FFT_FAIL=1',) if fail else ())
            what = 'overlay, check fails' if fail else 'overlay'
            try:
                b, h, vp = self.setup(3, defines=defs)
                res, rrep, _ = self.image(defs)
                oprog, orep, words = self.overlay(defs)
                both = type('Syms', (), {'symbols': dict(res.symbols, **oprog.symbols)})
                if not fail:
                    self.metrics['overlay bytes, end, spare'] = '%d, 0x%x, %d' % (
                        4 * len(words), orep['end'], orep['spare'])
                    self.metrics['display-only image+bss end'] = '0x%x (%d below the '\
                        'decode work area)' % (rrep['end'], H.B.config_value(
                            'BCMFW_JOBS_DEC_WORK', defs) - rrep['end'])
                jb = b.r32(J['HP_JOBS'])
                ops0 = b.r32(jb + J['J_OPS'])
                jops = lambda: b.r32(jb + J['J_OPS'])   # noqa: E731
                row.check(ops0 >> (J['J_OP_OVERLAY'] - 0x20) & 1 and not ops0 & bits and
                          ops0 >> (J['J_OP_DECODE'] - 0x20) & 1,
                          '%s: J_OPS %08x at start' % (what, ops0))
                row.check(b.r32(jb + J['J_OVL']) == base | size >> 12,
                          '%s: J_OVL %08x' % (what, b.r32(jb + J['J_OVL'])))
                vpu0 = b.r32(jb + J['J_VPU'])
                op, st, _ = vp.call(J['J_OP_SPECTRUM'], [0, 256 | 1 << 16, 1, 2])
                row.check(st == K['HP_E_UNKNOWN'], '%s: SPECTRUM before a load: %d' % (what, st))
                # refused loads: each leaves no ops
                bad = list(words)
                bad[len(words) // 2] ^= 0x10        # a flipped bit, the true sums
                sums = f16w([hw for w in words for hw in (w & 0xFFFF, w >> 16)])
                foreign = self.overlay(defs, abi=J['J_OVL_ABI'] + 1)[2]
                patched = list(words)
                patched[1] ^= 1                     # the ABI word, sums recomputed
                cases = [('a flipped bit', bad, sums, None, J['J_E_SUM']),
                         ('another ABI version', foreign, None, None, K['HP_E_VERSION']),
                         ('its ABI word patched', patched, None, None, K['HP_E_VERSION']),
                         ('truncated', words[:-1], None, None, K['HP_E_VERSION']),
                         ('under the header', words[:6], None, None, K['HP_E_INVAL']),
                         ('over the region', words, sums, size // 4 + 1, K['HP_E_INVAL'])]
                for name, ws, sm, n, want in cases:
                    st = self.ovl_load(vp, ws, sm, n)
                    row.check(st == want and not jops() & (bits | lbits),
                              '%s: %s: status %d (want %d), J_OPS %08x' % (
                                  what, name, st, want, jops()))
                    op, st, _ = vp.call(J['J_OP_BARS'], [J['J_SURF_FRONT'], 0, xy(8, 8),
                                                         1 | 2 << 8, 0, 0, 0, 0, 0])
                    row.check(st == K['HP_E_UNKNOWN'], '%s: %s: BARS %d' % (what, name, st))
                # a good load, after the refused ones: the ops, J_VPU_FFT
                st = self.ovl_load(vp, words)
                row.check(st == K['HP_OK'] and jops() == ops0 | bits | (0 if fail else lbits),
                          '%s: load: status %d, J_OPS %08x' % (what, st, jops()))
                vpu = b.r32(jb + J['J_VPU'])
                row.check(vpu == vpu0 | J['J_VPU_FLIP'] |
                          (0 if fail else J['J_VPU_FFT'] | J['J_VPU_BARS']),
                          '%s: J_VPU %x (before %x)' % (what, vpu, vpu0))
                self.fill_front(vp)
                results[fail] = self.viz_run(row, b, vp, both, what, fail, random.Random(126), lib)
                # an epoch change drops the ops; a reload brings them back
                ep = b.r32(jb + J['J_EPOCH']) & 0xFFFF
                op, st, _ = self.jcall(vp, J['J_OP_RESET'], [])
                row.check(st == K['HP_OK'] and not jops() & (bits | lbits) and
                          b.r32(jb + J['J_EPOCH']) & 0xFFFF != ep and
                          jops() == ops0, '%s: RESET: J_OPS %08x' % (what, jops()))
                op, st, _ = vp.call(J['J_OP_BARS'], [J['J_SURF_FRONT'], 0, xy(8, 8),
                                                     1 | 2 << 8, 0, 0, 0, 0, 0])
                row.check(st == K['HP_E_UNKNOWN'], '%s: BARS after RESET: %d' % (what, st))
                st = self.ovl_load(vp, words)
                row.check(st == K['HP_OK'] and jops() == ops0 | bits | (0 if fail else lbits),
                          '%s: reload: status %d, J_OPS %08x' % (what, st, jops()))
                op, st, _ = self.jcall(vp, J['J_OP_BARS'], [J['J_SURF_FRONT'], xy(8, 8), xy(40, 20),
                                                     4 | 6 << 8 | 2 << 16, 0xF800 | 0x1F << 16,
                                                     0, J['J_BARS_BG'], 0x14100A05, 0])
                row.check(st == K['HP_OK'], '%s: BARS after the reload: %d' % (what, st))
            except (T.Fail, H.Timeout) as e:
                row.check(False, '%s: %s' % (what, e))
        # the BCMFW_VIZ image on the same cases: replies and frames alike
        for fail in (False, True):
            defs = JDEFS + ('BCMFW_VIZ=1',) + (('BCMFW_VIZ_FFT_FAIL=1',) if fail else ())
            try:
                b, h, vp = self.setup(3, defines=defs)
                prog = self.image(defs)[0]
                self.fill_front(vp)
                got = self.viz_run(row, b, vp, prog, 'viz image', fail, random.Random(126), lib)
                same = fail in results and results[fail] == got
                row.check(same, 'overlay%s and viz image differ (%d of %d cases)' % (
                    ', check fails' if fail else '',
                    sum(1 for x, y in zip(results.get(fail, []), got) if x != y), len(got)))
            except (T.Fail, H.Timeout) as e:
                row.check(False, 'viz image: %s' % e)

    # the merged image (bcmfw/mkvideo.py's bcmvideo-jobs.bin) with its
    # overlay region (decode_image_test.py MERGED_OVL)
    MERGED_OVL = VIDEO_SHIPPED + ('BCMFW_JOBS=1', 'BCMFW_JOBS_MERGED_OVL=1')

    def merged_overlay(self, kind):
        """An overlay of kind ('viz', 'decode') linked against the merged
        image: (Program, report, the PP's upload as words)"""
        key = ('movl', kind)
        if key not in self.builds:
            res = self.image(self.MERGED_OVL)[0]
            out = os.path.join(T.ROOT, 'build', 'bcmfw', 'test-movl-' + kind)
            prog, rep = H.B.build_overlay(res, out, list(self.MERGED_OVL), listing=False,
                                          kind=kind)
            img = open(os.path.join(out, ('viz-ovl' if kind == 'viz' else 'dec-ovl') +
                                    '.bin'), 'rb').read()
            words = [int.from_bytes(img[i:i + 4], 'little') for i in range(0, len(img), 4)]
            self.builds[key] = (prog, rep, words)
        return self.builds[key]

    def t_ovl_merged(self):
        """The visualizer's overlay for the merged image (viz-ovl-m.bin;
        visualizer.md 8.5): the image unchanged, the overlay linked against
        its map and exports, into the region DECODE's overlay uses
        (0x12B000).  SPECTRUM and BARS refused before a load and for the
        display-only image's overlay; loaded, they equal jviz.c on the Mac,
        on the vector FFT and bars; the swap: a DECODE load drops them (and
        DECODE is offered), a viz load drops DECODE and J_CAP_DEC_LENT
        again; RESET drops everything"""
        import random
        import viz_test as V
        row = T.Row('merged image: viz overlay, SPECTRUM/BARS = jviz.c, DEC/VIZ swap')
        self.rows.append(row)
        lib = V.mac_lib()
        defs = self.MERGED_OVL
        bits = 1 << (J['J_OP_SPECTRUM'] - 0x20) | 1 << (J['J_OP_BARS'] - 0x20)
        lbits = 7 << (J['J_OP_POLYLINE'] - 0x20)
        dbit = 1 << (J['J_OP_DECODE'] - 0x20)
        what = 'merged viz overlay'
        try:
            b, h, vp = self.setup(3, defines=defs)
            res = self.image(defs)[0]
            oprog, orep, words = self.merged_overlay('viz')
            dwords = self.merged_overlay('decode')[2]
            both = type('Syms', (), {'symbols': dict(res.symbols, **oprog.symbols)})
            self.metrics['merged viz overlay bytes, spare'] = '%d, %d' % (
                orep['load'], orep['spare'])
            jb = b.r32(J['HP_JOBS'])
            jops = lambda: b.r32(jb + J['J_OPS'])   # noqa: E731
            ops0 = jops()
            row.check(ops0 >> (J['J_OP_OVERLAY'] - 0x20) & 1 and not ops0 & (bits | dbit),
                      'J_OPS %08x at start' % ops0)
            op, st, _ = vp.call(J['J_OP_SPECTRUM'], [0, 256 | 1 << 16, 1, 2])
            row.check(st == K['HP_E_UNKNOWN'], 'SPECTRUM before a load: %d' % st)
            # the display-only image's overlay (another base, another ABI)
            foreign = self.overlay(JDEFS)[2]
            st = self.ovl_load(vp, foreign, defines=defs)
            row.check(st != K['HP_OK'] and not jops() & (bits | lbits),
                      'the display-only overlay: status %d, J_OPS %08x' % (st, jops()))
            st = self.ovl_load(vp, words, defines=defs)
            row.check(st == K['HP_OK'] and jops() == ops0 | bits | lbits,
                      'load: status %d, J_OPS %08x' % (st, jops()))
            vpu = b.r32(jb + J['J_VPU'])
            row.check(vpu & J['J_VPU_FFT'] and vpu & J['J_VPU_BARS'] and vpu & J['J_VPU_FLIP'],
                      'J_VPU %x' % vpu)
            self.fill_front(vp)
            self.viz_run(row, b, vp, both, what, False, random.Random(126), lib)
            # the swap: DECODE in, the visualizer's ops out
            st = self.ovl_load(vp, dwords, defines=defs)
            caps = b.r32(jb + J['J_CAPS'])
            row.check(st == K['HP_OK'] and jops() == ops0 | dbit and caps & J['J_CAP_DEC_LENT'],
                      'DECODE load: status %d, J_OPS %08x, J_CAPS %08x' % (st, jops(), caps))
            op, st, _ = vp.call(J['J_OP_BARS'], [J['J_SURF_FRONT'], 0, xy(8, 8),
                                                 1 | 2 << 8, 0, 0, 0, 0, 0])
            row.check(st == K['HP_E_UNKNOWN'], 'BARS with DECODE loaded: %d' % st)
            # ... and back
            st = self.ovl_load(vp, words, defines=defs)
            caps = b.r32(jb + J['J_CAPS'])
            row.check(st == K['HP_OK'] and jops() == ops0 | bits | lbits and
                      not caps & J['J_CAP_DEC_LENT'],
                      'viz reload: status %d, J_OPS %08x, J_CAPS %08x' % (st, jops(), caps))
            op, st, _ = self.jcall(vp, J['J_OP_BARS'], [J['J_SURF_FRONT'], xy(8, 8), xy(40, 20),
                                                        4 | 6 << 8 | 2 << 16, 0xF800 | 0x1F << 16,
                                                        0, J['J_BARS_BG'], 0x14100A05, 0])
            row.check(st == K['HP_OK'], 'BARS after the swap back: %d' % st)
            op, st, _ = self.jcall(vp, J['J_OP_RESET'], [])
            row.check(st == K['HP_OK'] and jops() == ops0, 'RESET: J_OPS %08x' % jops())
        except (T.Fail, H.Timeout) as e:
            row.check(False, '%s: %s' % (what, e))

    def mix_model(self, surfs, case):
        """A FADE or AVG case (kind, dst, dxy, wh, a, axy, b, bxy, weight,
        alpha bytes or 0): its op and payload, and surfs updated by the
        jmix.h models (b's first len(alpha) columns shaded); returns
        (op, payload, dst)"""
        kind, dsur, dxy, wh, asur, axy, bsur, bxy, wt, al = case
        al = al or []
        op = J['J_OP_FADE'] if kind == 'fade' else J['J_OP_AVG']
        pay = [dsur, xy(*dxy), xy(*wh), asur, xy(*axy), bsur, xy(*bxy)]
        if kind == 'fade':
            pay.append(wt)
        if al:
            words = [0] * ((len(al) + 3) // 4)
            for i, v_ in enumerate(al):
                words[i // 4] |= v_ << (8 * (i % 4))
            pay += [J['J_FADE_SHADE_B'] | len(al)] + words
        A_, ap = surfs[asur][:2]
        B_, bp = surfs[bsur][:2]
        D_, dp = surfs[dsur][:2]
        new = list(D_)
        for y in range(wh[1]):
            for x in range(wh[0]):
                av = A_[(axy[1] + y) * ap + axy[0] + x]
                bv = B_[(bxy[1] + y) * bp + bxy[0] + x]
                if x < len(al):
                    bv = shade(bv, al[x])
                new[(dxy[1] + y) * dp + dxy[0] + x] = (
                    fade_px(av, bv, wt) if kind == 'fade' else avg_px(av, bv))
        surfs[dsur] = (new,) + surfs[dsur][1:]
        return op, pay, dsur

    def anim_run(self, row, b, vp, a, what):
        o, st, _ = self.jcall(vp, J['J_OP_ANIM'], a)
        row.check(st == K['HP_OK'], '%s: %d' % (what, st))
        for _ in range(20000):
            b.run(500)
            vp.drain()
            if any(e[0] == J['J_EV_ANIM'] for e in vp.events):
                break
        row.check(any(e[0] == J['J_EV_ANIM'] for e in vp.events), what + ': no end event')
        vp.events = []
        self.settle_dma(b)

    def anim_check_front(self, row, b, surfs, what):
        px, pitch, addr = surfs[J['J_SURF_FRONT']]
        got = self.read_px(b, addr, len(px))
        bad = sum(1 for g_, e_ in zip(got, px) if g_ != e_)
        row.check(bad == 0, '%s: %d pixels differ' % (what, bad))
        surfs[J['J_SURF_FRONT']] = (got, pitch, addr)

    # -- the cost audit
    def prof_poll(self, b, prog, want='jobs_dispatch', limit=40_000_000):
        """Profile hp_poll passes until one runs want (a job's dispatch, or
        with want='jobs_tick' an animation frame: a jobs_tick call that
        draws); returns its Prof"""
        S_ = prog.symbols
        entry = S_['hp_poll'] if want == 'jobs_dispatch' else S_['jobs_tick']
        for _ in range(200000):
            step(b, 2_000_000, lambda pc: pc == entry)
            ret = b.sim.r[26]
            p = Prof(prog)
            step(b, limit, lambda pc: pc == ret, p)
            if want in p.fn and (want == 'jobs_dispatch' or p.total() > 300):
                return p
        raise T.Fail('%s never ran' % want)

    def audit_job(self, b, vp, prog, op, payload):
        # the core to hp_poll's entry first: stopped inside a pass (after
        # heartbeat(), before the ring's index is read) it would run the
        # job unprofiled, before prof_poll's first pass begins
        step(b, 2_000_000, lambda pc: pc == prog.symbols['hp_poll'])
        vp.tag = (vp.tag + 1) & 0x7fff or 1
        tag = vp.send(op, payload, tag=vp.tag | J['J_TAG_REPLY'])
        p = self.prof_poll(b, prog)
        o, st, rep = vp.wait(tag)
        if st != K['HP_OK']:
            raise T.Fail('op %x: status %d' % (op, st))
        return p

    def t_audit(self):
        """Not a check: instructions and estimated cycles per job for the
        shell's typical cases (jobs.md section 10, "Cost audit")"""
        row = T.Row('cost audit (informational)')
        self.rows.append(row)
        b, h, vp = self.setup(3)
        prog = self.image(JDEFS)[0]
        rng = __import__('random').Random(5)
        FR, BK = J['J_SURF_FRONT'], J['J_SURF_BACK']
        cov = self.slot(vp, J['J_CLS_DRIFT'], 300, 300, 0xC1, rng)[0]
        cov2 = self.slot(vp, J['J_CLS_DRIFT'], 300, 300, 0xC2, rng)[0]
        st320 = self.slot(vp, J['J_CLS_STRIP'], 320, 200, 0xC3, rng)[0]
        mq = self.slot(vp, J['J_CLS_THUMB'], 400, 16, 0xC4, rng)[0]
        al = [0, 0, 0]
        for i, v in enumerate(ALPHA):
            al[i // 4] |= v << (8 * (i % 4))
        PX, PW = xy(160, 0), xy(160, 240)
        offs = [(i * 320 // 12) for i in range(1, 13)]
        slide = [J['J_ANIM_SLIDE'], BK, FR, 0, xy(320, 240), 0, 33, 12] + [
            offs[k] | offs[k + 1] << 16 for k in range(0, 12, 2)]
        pan = [J['J_ANIM_PAN'], mq, 0, xy(200, 0), xy(200, 16), xy(100, 50), 3000, 2]
        cases = [
            ('BLIT drift pan, cover 300x300 -> 160x240', J['J_OP_BLIT'],
             [cov, xy(70, 30), PW, FR, PX, 0]),
            ('SHADE the shadow strip, 10x240', J['J_OP_SHADE'], [FR, PX, xy(10, 240)] + al),
            ('AVG drift half step, two framings', J['J_OP_AVG'],
             [FR, PX, PW, cov, xy(70, 30), cov, xy(71, 30)]),
            ('FADE drift raw (shade flag 10), toward cover', J['J_OP_FADE'],
             [FR, PX, PW, FR, PX, cov2, xy(70, 30), 128, J['J_FADE_SHADE_B'] | 10] + al),
            ('FADE drift, toward cover', J['J_OP_FADE'],
             [FR, PX, PW, FR, PX, cov2, xy(70, 30), 128]),
            # a window in phase with the frame (x = 64: rows 32-byte
            # aligned in both at the cover's 304 pitch, J_SLOT_PITCH)
            ('BLIT drift pan, in phase (x 64)', J['J_OP_BLIT'],
             [cov, xy(64, 30), PW, FR, PX, 0]),
            ('FADE drift, toward cover, in phase (x 64)', J['J_OP_FADE'],
             [FR, PX, PW, FR, PX, cov2, xy(64, 30), 128]),
            ('BLIT 160x200 from a 320-pitch slot', J['J_OP_BLIT'],
             [st320, xy(80, 0), xy(160, 200), FR, PX, 0]),
            ('FADE 160x200 toward a 320-pitch slot', J['J_OP_FADE'],
             [FR, PX, xy(160, 200), FR, PX, st320, xy(80, 0), 128]),
            ('SCROLL list 320x220 by one row (25)', J['J_OP_SCROLL'],
             [FR, xy(0, 20), xy(320, 220), (-25) & 0xFFFF]),
            ('SCROLL split list 160x220 by 25', J['J_OP_SCROLL'],
             [FR, xy(0, 20), xy(160, 220), 25]),
            ('FILL a list row 320x25', J['J_OP_FILL'], [FR, xy(0, 45), xy(320, 25), 0x3186]),
            ('FILL the full frame', J['J_OP_FILL'], [BK, 0, xy(320, 240), 0xFFFF]),
            ('FILL 150x25 at x 5 (unaligned)', J['J_OP_FILL'], [FR, xy(5, 45), xy(150, 25), 0]),
            ('BLIT FRONT -> BACK full frame', J['J_OP_BLIT'], [FR, 0, xy(320, 240), BK, 0, 0]),
            ('BLIT marquee strip 200x16 at x 100', J['J_OP_BLIT'],
             [mq, xy(37, 0), xy(200, 16), FR, xy(100, 50), 0]),
            ('BLIT keyed 160x200', J['J_OP_BLIT'],
             [st320, 0, xy(160, 200), FR, 0, J['J_BLIT_KEY'] | 0xFFFF << 16]),
            ('PRESENT full frame', J['J_OP_PRESENT'], [FR, 0, xy(320, 240), 0]),
            ('PRESENT the panel 160x240', J['J_OP_PRESENT'], [FR, PX, PW, 0]),
            ('FENCE', J['J_OP_FENCE'], [7]),
            ('ALLOC (a hit)', J['J_OP_ALLOC'], [J['J_CLS_DRIFT'], xy(300, 300), 1, 0xC1, 0]),
            ('STATS', J['J_OP_STATS'], []),
            ('ANIM SLIDE (the job)', J['J_OP_ANIM'], slide),
        ]
        out = []
        for name, op, pay in cases:
            if op == J['J_OP_PRESENT']:
                self.settle_dma(b)
            p = self.audit_job(b, vp, prog, op, pay)
            out.append((name, p))
        self.settle_dma(b)
        out.append(('ANIM SLIDE, one 320x240 frame', self.prof_poll(b, prog, 'jobs_tick')))
        out.append(('ANIM_STOP', self.audit_job(b, vp, prog, J['J_OP_ANIM_STOP'], [0])))
        self.settle_dma(b)
        # the retained slide (rev 0.4): the list body out of the front frame
        # itself, the new page in BACK.  The frame profiled is the second
        # (the first runs inside the ANIM's own pass): it moves the old page
        # on by the offsets' difference, 27 (odd: C pixel by pixel) or 26
        # (even: C words)
        for name, d, step in (('push', 0, 27), ('pop', 1, 27), ('push', 0, 26), ('pop', 1, 26)):
            o2 = [min(320, 26 + step * i) for i in range(12)]
            rs = [J['J_ANIM_SLIDE'], FR, BK, xy(0, 20), xy(320, 220), d, 33, 12] + [
                o2[k] | o2[k + 1] << 16 for k in range(0, 12, 2)]
            self.audit_job(b, vp, prog, J['J_OP_ANIM'], rs)
            out.append(('ANIM SLIDE %s 320x220 from FRONT, by %d' % (name, step),
                        self.prof_poll(b, prog, 'jobs_tick')))
            self.jcall(vp, J['J_OP_ANIM_STOP'], [0])
            self.settle_dma(b)
        out.append(('BLIT BACK -> FRONT 320x220 (a landing)',
                    self.audit_job(b, vp, prog, J['J_OP_BLIT'], [BK, xy(0, 20), xy(320, 220),
                                                                 FR, xy(0, 20), 0])))
        for dy in (-8, -17, 23):
            out.append(('SCROLL list 320x220 by %d (a glide step)' % dy,
                        self.audit_job(b, vp, prog, J['J_OP_SCROLL'],
                                       [FR, xy(0, 20), xy(320, 220), dy & 0xFFFF])))
        out.append(('BLIT BACK -> FRONT 320x17 (new rows)',
                    self.audit_job(b, vp, prog, J['J_OP_BLIT'], [BK, xy(0, 223), xy(320, 17),
                                                                 FR, xy(0, 223), 0])))
        out.append(('BLIT BACK -> FRONT 8x220 (scrollbar)',
                    self.audit_job(b, vp, prog, J['J_OP_BLIT'], [BK, xy(312, 20), xy(8, 220),
                                                                 FR, xy(312, 20), 0])))
        self.settle_dma(b)
        out.append(('PRESENT list 320x220 WAIT (setup)',
                    self.audit_job(b, vp, prog, J['J_OP_PRESENT'], [FR, xy(0, 20), xy(320, 220),
                                                                    0])))
        self.settle_dma(b)
        p = self.audit_job(b, vp, prog, J['J_OP_ANIM'], pan)
        out.append(('ANIM PAN 200x16 (the job)', p))
        out.append(('ANIM PAN, one 200x16 frame', self.prof_poll(b, prog, 'jobs_tick')))
        self.jcall(vp, J['J_OP_ANIM_STOP'], [0])
        # the marquee (J_PAN_STEP): the list's text cell, 290 x 18 at x 5,
        # from a strip of the cell at rest, a 400 px lap and the head
        # again (980 px, rounded to 992: rows 32-byte aligned); step 7
        # (x 30 + 14 against x 5: out of phase)
        self.settle_dma(b)
        mq2 = self.slot(vp, J['J_CLS_THUMB'], 992, 18, 0xC5, rng)[0]
        step = [J['J_ANIM_PAN'], mq2, xy(290, 0), xy(690, 0), xy(290, 18), xy(5, 50), 50,
                J['J_PAN_STEP'] | 1, 2 | 1000 << 16, 0, 1000 + 6 * 50]
        p = self.audit_job(b, vp, prog, J['J_OP_ANIM'], step)
        out.append(('ANIM PAN stepped 290x18 (the job)', p))
        out.append(('ANIM PAN stepped, one 290x18 frame (x 5)',
                    self.prof_poll(b, prog, 'jobs_tick')))
        self.jcall(vp, J['J_OP_ANIM_STOP'], [0])
        # COMMIT last: its F16w reads the whole slot
        self.settle_dma(b)
        out.append(('FREE', self.audit_job(b, vp, prog, J['J_OP_FREE'], [mq])))
        out.append(('COMMIT 300x300 with F16w', self.audit_commit(b, vp, prog, rng, cov2)))
        out.append(('RESET', self.audit_job(b, vp, prog, J['J_OP_RESET'], [])))
        print('\n%-46s %9s %6s %6s %9s %8s %8s  %s' % (
            'job', 'instr', 'vec', 'C %', 'cycles', '27 MHz', '150 MHz', 'top functions'))
        for name, p in out:
            n, cyc = p.total(0), p.total(2)
            kern = sum(v[0] for k, v in p.fn.items() if k.startswith('jv_'))
            top = sorted(p.fn.items(), key=lambda kv: -kv[1][2])[:3]
            print('%-46s %9d %6d %5.0f%% %9.0f %6.2fms %6.3fms  %s' % (
                name[:46], n, p.total(1), 100.0 * (n - kern) / max(1, n), cyc,
                cyc / 27e3, cyc / 150e3,
                ', '.join('%s %.0f%%' % (k, 100 * v[2] / max(1, cyc)) for k, v in top)))
        row.check(True, '')

    def audit_commit(self, b, vp, prog, rng, slot):
        """a 300 x 300 cover's COMMIT, its F16w checked: slot freed and
        its pool filled again"""
        pic = [rng.getrandbits(16) for _ in range(300 * 300)]
        self.jcall(vp, J['J_OP_FREE'], [slot])
        op, st, p = self.jcall(vp, J['J_OP_ALLOC'], [J['J_CLS_DRIFT'], xy(300, 300), 1, 0xC9, 0])
        if st != K['HP_OK']:
            raise T.Fail('ALLOC for COMMIT: %d' % st)
        wr_slot(vp, p[1], p[2], pic, 300, 300)
        return self.audit_job(b, vp, prog, J['J_OP_COMMIT'], [p[0]] + f16w(pic))

    def run(self):
        t0 = time.time()
        tests = (self.t_block, self.t_draw, self.t_slot, self.t_park, self.t_park_unflushed,
                 self.t_park_sticky,
                 self.t_window,
                 self.t_scroll, self.t_mix, self.t_blit, self.t_retained, self.t_marquee,
                 self.t_phase, self.t_gate, self.t_variants, self.t_viz, self.t_ovl,
                 self.t_ovl_merged, self.t_staged, self.t_staged_merged)
        if self.args.audit:
            tests = (self.t_audit,)
        elif self.args.only:
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
            print('  %-28s %s' % (k, self.metrics[k]))
        print('%d rows, %d cases, %d failed (%.0f s)' % (len(self.rows), cases, fails,
                                                         time.time() - t0))
        return 1 if fails else 0


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--seed', type=int, default=2722)
    ap.add_argument('--vmcs', default=T.DEFAULT_VMCS)
    ap.add_argument('--png', default=os.path.join(T.ROOT, 'build', 'bcmfw', 'test-jobs'))
    ap.add_argument('--only', default='')
    ap.add_argument('--audit', action='store_true',
                    help='only the cost audit: instructions and cycles per job')
    a = ap.parse_args(argv)
    if not H.B.have_gcc():
        print('skip: vc4-elf-gcc not found')
        return 0
    return Suite(a).run()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
