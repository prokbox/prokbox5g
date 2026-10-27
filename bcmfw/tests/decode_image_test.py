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

"""J_OP_DECODE in the display-only image (S3b; docs/research/bcm2722/
album-render.md), in vc2sim: the image booted through lcd-video.c's port
as jobs_image_test.py boots it, and the PP driver modelled here streaming
a corpus JPEG (tests/jpeg/make_corpus.sh, build/jpeg/corpus) through the
input ring (a J_CLS_SCRATCH slot, jobs.h J_DEC_RING, J_DEC_IN_RD/WR) as
the BCM consumes it.  Every output slot's FNV-1a (of the bytes read back
over the port) must equal tests/jpeg/golden.txt, and COMMIT with the
read-back's F16w must agree:
  - a file shorter than the ring (streamed once, read again in place);
  - files longer than the ring (a new pass per jdec pass, the ring
    wrapping), with one output and with two (the NP box and a thumbnail);
  - the panel (jdec streams its outputs, jpeg-spike.md section 15): the
    300 x 300 fill from 600 and 3000 px covers, alone and with the art
    set in one job, and the cropped fills of a 16:9 and a 2:3 cover, in
    the image's own work area, into DRIFT slots (pitch 304, not the
    window's 300) and THUMB slots; every window equal to the Mac's jdec
    (the CPU twin's code: bcmfw/jpeg/jdtest.c, JDTEST_DUMP), cropped;
  - a progressive file (J_E_DEC_UNSUPPORTED), the PP's abort
    (J_E_DEC_CORRUPT), a ring that is not a scratch slot (J_E_SLOT), an
    output too big for the work area (a 16:1 cover's fill, HP_E_NOMEM);
  - S3c, the decode's yield: with an animation running and the 1 MHz
    counter driven at 27 MHz's pace, a DECODE still shows the animation's
    frames, serves an LCD_UPDATERECT the PP writes meanwhile and keeps the
    heartbeat stepping, while the ring's next job waits for it; its output
    still equals golden;
  - the 8x8 IDCT's start-up check (J_VPU_MAC, jpeg-spike.md section 13):
    as built it passes and DECODE's 4-, 8- and 16-point blocks go to
    jv_idct4n; built
    with BCMFW_JOBS_MAC_FAIL=1 it fails, J_VPU_MAC stays clear and the
    same decodes take the C IDCT; golden either way.

Usage: bcmfw/tests/decode_image_test.py [-v]
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hostproto_test as H     # noqa: E402
import hello_test as T         # noqa: E402
import jobs_image_test as JI   # noqa: E402

K, J, xy = H.K, JI.J, JI.xy
CORPUS = os.path.join(T.ROOT, 'build', 'jpeg', 'corpus')
GOLDEN = os.path.join(T.ROOT, 'tests', 'jpeg', 'golden.txt')
RING_W = RING_H = 128                   # 32 KB: the scratch class's budget
PIECE = 8192                            # the PP's engine-2 pieces


def corpus_problem():
    """None if build/jpeg/corpus holds every file of tests/jpeg/
    corpus.sha256 with its sha256, else what is wrong: a corpus being
    built (make_corpus.sh writes the files one by one) or half built must
    fail the bench by name, not decode a short file or skip rows"""
    import hashlib
    if not os.path.isdir(CORPUS):
        return 'corpus missing: %s (tests/jpeg/make_corpus.sh)' % CORPUS
    bad = []
    for ln in open(os.path.join(T.ROOT, 'tests', 'jpeg', 'corpus.sha256')):
        want, name = ln.split()
        try:
            got = hashlib.sha256(open(os.path.join(CORPUS, name), 'rb').read()).hexdigest()
        except FileNotFoundError:
            got = 'missing'
        if got != want:
            bad.append('%s %s' % (name, 'missing' if got == 'missing' else 'differs'))
    if bad:
        return 'corpus missing or incomplete (%d of tests/jpeg/corpus.sha256: %s): ' \
            'tests/jpeg/make_corpus.sh' % (len(bad), ', '.join(bad[:4]))
    return None


def golden():
    out = {}
    for ln in open(GOLDEN):
        if ln.startswith('#'):
            continue
        f = ln.split()
        out[(f[0], int(f[1]), int(f[2]))] = f[3:]
    return out


def enc(v):
    return H.enc(v & 0xFFFF)


class Stream:
    """The PP side of the ring: follows the BCM's pass, writes the file in
    pieces of whole 64-byte units (the last partial), never more than a
    ring ahead of what the BCM has consumed"""

    def __init__(self, vp, base, ring_bytes, data):
        self.vp, self.base, self.ring, self.data = vp, base, ring_bytes, data
        self.jb = vp.rd(J['HP_JOBS'])
        self.pass_ = self.rd_pass()     # the last pass served (before the job)
        self.cur = None
        self.written = 0
        self.passes = 0
        self.bytes = 0
        self.abort_at = None

    def rd_word(self):
        w = self.vp.rd(self.jb + J['J_DEC_IN_RD'])
        return (w & 0xFFFF) if H.valid(w) else None

    def rd_pass(self):
        w = self.rd_word()
        return None if w is None else w >> 12 & 0xF

    def publish(self):
        units = (self.written + 63) // 64
        self.vp.wr(self.jb + J['J_DEC_IN_WR'], enc(self.cur << 12 | units & 0xFFF))

    def service(self):
        w = self.rd_word()
        if w is None:
            return
        p, u = w >> 12 & 0xF, w & 0xFFF
        if p != self.cur and p != self.pass_:
            self.cur, self.pass_, self.written = p, p, 0     # back to byte 0
            self.passes += 1
            self.publish()
        if p != self.cur:
            return
        wu = (self.written + 63) // 64
        done = wu - ((wu - u) & 0xFFF)                       # absolute units
        while self.written < len(self.data):
            if self.abort_at is not None and self.written >= self.abort_at:
                self.vp.wr(self.jb + J['J_DEC_IN_WR'], enc(J['J_DEC_ABORT']))
                return
            room = self.ring - (self.written - done * 64)
            at = self.written % self.ring
            n = min(PIECE, room, self.ring - at, len(self.data) - self.written)
            if n < 64 and self.written + n < len(self.data):
                return                                      # wait for room
            if n <= 0:
                return
            chunk = self.data[self.written:self.written + n] + b'\0' * (-n % 4)
            words = [int.from_bytes(chunk[i:i + 4], 'little') for i in range(0, len(chunk), 4)]
            self.vp.wr_block(self.base + at, words)
            self.written += n
            self.bytes += n
            self.publish()


class Suite(JI.Suite):
    def ring_slot(self, vp):
        op, st, p = self.jcall(vp, J['J_OP_ALLOC'], [J['J_CLS_SCRATCH'], xy(RING_W, RING_H),
                                                     J['J_FMT_RGB565'], 0x52494E47, 0])
        if st != K['HP_OK']:
            raise T.Fail('ring ALLOC: %d' % st)
        op, st2, _ = self.jcall(vp, J['J_OP_COMMIT'], [p[0], 0, 0, J['J_COMMIT_NOCHECK']])
        if st2 != K['HP_OK']:
            raise T.Fail('ring COMMIT: %d' % st2)
        return p[0], p[1]

    def out_slot(self, vp, cls, w, h, key):
        op, st, p = self.jcall(vp, J['J_OP_ALLOC'], [cls, xy(w, h), J['J_FMT_RGB565'], key, 0])
        if st != K['HP_OK']:
            raise T.Fail('output ALLOC %dx%d: %d' % (w, h, st))
        return p[0], p[1]

    def decode(self, b, vp, src, base, data, outs, abort_at=None, ring=True):
        """One DECODE, the file streamed through the ring; returns (status,
        reply payload, stream, instructions)"""
        s = Stream(vp, base, RING_W * RING_H * 2, data)
        s.abort_at = abort_at
        pay = [src | (J['J_DEC_RING'] if ring else 0), len(data), len(outs)]
        for o, slot in outs:
            dw, dh, cx, cy, cw, ch = o if len(o) == 6 else (o[0], o[1], 0, 0, o[0], o[1])
            pay += [xy(dw, dh), xy(cx, cy), xy(cw, ch), slot, xy(0, 0)]
        vp.tag = (vp.tag + 1) & 0x7fff or 1
        tag = vp.send(J['J_OP_DECODE'], pay, tag=vp.tag | J['J_TAG_REPLY'])
        if tag is None:
            raise T.Fail('command ring full')
        steps0 = b.sim.steps
        for _ in range(20000):
            b.run(100_000)
            s.service()
            vp.drain()
            if tag in vp.replies:
                op, st, p = vp.replies.pop(tag)
                return st, p, s, b.sim.steps - steps0
        raise T.Fail('no DECODE reply after %d instructions' % (b.sim.steps - steps0))

    def t_overlay(self):
        """DECODE beside the visualizer's overlay (J_OP_OVERLAY; proposed,
        sim only): the decode work area (0xC400-0x46000) and the overlay
        region (0x13B000-0x140000) are apart (build.py checks the layout),
        and jobs run one at a time from the ring, so SPECTRUM and BARS never
        run during a DECODE.  With the overlay loaded, a DECODE gives the
        golden pixels, leaves the region's bytes and J_OPS as they were,
        and a SPECTRUM after it still equals jviz.c on the Mac"""
        import ctypes
        import viz_test as V
        row = T.Row('DECODE beside the overlay: golden, region and ops intact')
        self.rows.append(row)
        if not os.path.isdir(CORPUS):
            row.check(False, 'corpus missing: ln -s the jpeg build into build/jpeg')
            return
        try:
            b, h, vp = self.setup(3)
            jb = vp.rd(J['HP_JOBS'])
            words = self.overlay()[2]
            base = H.B.config_value('BCMFW_JOBS_OVL', JI.JDEFS)
            st = self.ovl_load(vp, words)
            ops = vp.rd(jb + J['J_OPS'])
            row.check(st == K['HP_OK'] and ops >> (J['J_OP_SPECTRUM'] - 0x20) & 1,
                      'load: status %d, J_OPS %08x' % (st, ops))
            work = H.B.config_value('BCMFW_JOBS_DEC_WORK', JI.JDEFS)
            wend = H.B.config_value('BCMFW_JOBS_DO_DRIFT', JI.JDEFS)
            row.check(wend <= base, 'decode work area 0x%x-0x%x reaches the region 0x%x' % (
                work, wend, base))
            before = [b.r32(base + 4 * i) for i in range(len(words))]
            rslot, rbase = self.ring_slot(vp)
            sl, addr = self.out_slot(vp, J['J_CLS_THUMB'], 142, 142, 0x0E1)
            data = open(os.path.join(CORPUS, 't600_exif.jpg'), 'rb').read()
            st, p, s, n = self.decode(b, vp, rslot, rbase, data, [((142, 142), sl)])
            row.check(st == K['HP_OK'], 'DECODE: status %d' % st)
            if st == K['HP_OK']:
                self.check_out(row, b, vp, 't600_exif beside the overlay', sl, addr, 142, 142,
                               golden()[('t600_exif.jpg', 142, 142)][3])
            row.check([b.r32(base + 4 * i) for i in range(len(words))] == before,
                      'the region changed during a DECODE')
            row.check(vp.rd(jb + J['J_OPS']) == ops, 'J_OPS changed: %08x' % vp.rd(jb + J['J_OPS']))
            # a SPECTRUM after it, through the overlay
            lib = V.mac_lib()
            op, st, p = self.jcall(vp, J['J_OP_ALLOC'], [J['J_CLS_STRIP'], xy(32, 16),
                                                        J['J_FMT_RGB565'], 0x5EC, 0])
            pcm = V.split([int(20000 * __import__('math').sin(0.37 * i)) for i in range(512)])
            JI.wr_slot(vp, p[1], 64, [v & 0xFFFF for v in pcm], 32, 16)
            self.jcall(vp, J['J_OP_COMMIT'], [p[0], 0, 0, J['J_COMMIT_NOCHECK']])
            edges = [0, 4, 16, 64, 256]
            op, st, rp = self.jcall(vp, J['J_OP_SPECTRUM'], [p[0], 512 | 4 << 16,
                                                             edges[0] | edges[1] << 16,
                                                             edges[2] | edges[3] << 16, edges[4]])
            pw = (ctypes.c_uint32 * 256)()
            lib.jviz_power_ref(pw, (ctypes.c_int16 * 512)(*pcm), 512)
            l2 = (ctypes.c_uint16 * 32)()
            lib.jviz_bands(l2, pw, 256, (ctypes.c_uint16 * 5)(*edges), 4)
            got = [(rp[k // 2] >> (16 * (k & 1))) & 0xFFFF for k in range(4)] \
                if st == K['HP_OK'] else st
            row.check(got == list(l2[:4]), 'SPECTRUM after the DECODE: %s, want %s' % (
                got, list(l2[:4])))
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def t_yield(self):
        """S3c (album-render.md section 8): the main loop's service inside a
        running DECODE.  The ring holds ANIM (a looping pan), DECODE and
        ANIM_STOP, so every frame ANIM_STOP counts was drawn inside the
        decode; the counter runs at 27 MHz's pace (MHZ instructions a us)"""
        row = T.Row('S3c: frames, LCD pushes and the heartbeat during a DECODE')
        self.rows.append(row)
        if not os.path.isdir(CORPUS):
            row.check(False, 'corpus missing (tests/jpeg/make_corpus.sh)')
            return
        MHZ, SLICE = 27, 27_000
        g = golden()
        try:
            b, h, vp = self.setup(3)
            rslot, rbase = self.ring_slot(vp)
            # the marquee: a 320 x 16 strip panned across the panel, looping
            pic = [(i * 2654435761 >> 16) & 0xFFFF for i in range(320 * 16)]
            op, st, p = self.jcall(vp, J['J_OP_ALLOC'], [J['J_CLS_STRIP'], xy(320, 16),
                                                         J['J_FMT_RGB565'], 0x5C, 0])
            JI.wr_slot(vp, p[1], p[2], pic, 320, 16)
            strip = p[0]
            self.jcall(vp, J['J_OP_COMMIT'], [strip] + JI.f16w(pic))
            for name in ('t600_exif.jpg', 't300_420_q85.jpg'):
                data = open(os.path.join(CORPUS, name), 'rb').read()
                out, oaddr = self.out_slot(vp, J['J_CLS_THUMB'], 142, 142, 0x5D)
                s = Stream(vp, rbase, RING_W * RING_H * 2, data)
                pan = [J['J_ANIM_PAN'], strip, 0, xy(160, 0), xy(160, 16), xy(80, 200), 500, 1]
                dec = [rslot | J['J_DEC_RING'], len(data), 1, xy(142, 142), xy(0, 0),
                       xy(142, 142), out, xy(0, 0)]
                tags = []
                for op_, pay in ((J['J_OP_ANIM'], pan), (J['J_OP_DECODE'], dec),
                                 (J['J_OP_ANIM_STOP'], [0])):
                    vp.tag = (vp.tag + 1) & 0x7fff or 1
                    tags.append(vp.send(op_, pay, tag=vp.tag | J['J_TAG_REPLY']))
                jb = vp.rd(J['HP_JOBS'])
                f0, beats, pushed, cur = b.r32(T.MB_FRAMES), set(), None, set()
                n0, t0 = b.sim.steps, b.now()
                for _ in range(40000):
                    b.run(SLICE)
                    b.timer.advance(SLICE // MHZ)
                    s.service()
                    vp.drain()
                    beats.add(b.r32(K['HP_STATE']) >> 8 & 0xFF)
                    if tags[1] not in vp.replies:
                        cur.add(vp.rd(jb + J['J_CUR_OP']) & 0xFFFF)
                    if pushed is None and s.bytes > 0 and tags[1] not in vp.replies:
                        # the PP pushes a rect while the decode runs
                        b.w32(T.MB_RECT_XY, xy(10, 10))
                        b.w32(T.MB_RECT_WH, xy(40, 20))
                        b.w32(T.CMD_WORD, T.LCD_UPDATERECT)
                        pushed = False
                    if pushed is False and b.r32(T.MB_FRAMES) > f0:
                        pushed = tags[1] not in vp.replies
                    if tags[2] in vp.replies:
                        break
                else:
                    raise T.Fail('%s: no ANIM_STOP reply' % name)
                us = (b.now() - t0)
                o_, st, p = vp.replies.pop(tags[1])
                o_, st2, p2 = vp.replies.pop(tags[2])
                frames = p2[0] if p2 else 0
                want = us / 33333.0
                row.check(st == K['HP_OK'], '%s: DECODE status %d' % (name, st))
                row.check(st2 == K['HP_OK'] and frames >= max(2, int(want * 0.8)) and
                          frames <= want + 2,
                          '%s: %d frames in %.0f ms, want about %.1f' % (
                              name, frames, us / 1000.0, want))
                row.check(pushed is True, '%s: the LCD_UPDATERECT waited for the decode' % name)
                row.check(len(beats) >= 3, '%s: the heartbeat took %d values' % (
                    name, len(beats)))
                row.check(J['J_OP_DECODE'] in cur, '%s: J_CUR_OP never showed DECODE (%s)' % (
                    name, sorted(cur)))
                row.check((vp.rd(jb + J['J_CUR_OP']) & 0xFFFF) == 0, '%s: J_CUR_OP not 0 '
                          'after the jobs' % name)
                self.metrics['S3c %s' % name] = '%d frames in %.0f ms at 27 MHz (%.1f M ' \
                    'instructions)' % (frames, us / 1000.0, (b.sim.steps - n0) / 1e6)
                if st == K['HP_OK']:
                    self.check_out(row, b, vp, '%s during an animation' % name, out, oaddr,
                                   142, 142, g[(name, 142, 142)][3])
                self.jcall(vp, J['J_OP_FREE'], [out])
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def check_out(self, row, b, vp, what, slot, addr, w, h, want, pitch=None):
        """The output: the bytes read back over the port (row by row at the
        slot's pitch, pixels), their FNV-1a against the golden value; then
        COMMIT with their F16w, which the BCM's own sum of the slot must
        equal"""
        nb = w * h * 2
        if pitch is None:
            pitch = (w + 15) & ~15      # jobs.h J_SLOT_PITCH: every class
        if pitch == w:
            raw = b''.join(b.r32(addr + i).to_bytes(4, 'little') for i in range(0, nb, 4))[:nb]
        else:
            raw = b''.join(b''.join(b.r32(addr + 2 * pitch * y + i).to_bytes(4, 'little')
                                    for i in range(0, w * 2, 4))[:w * 2] for y in range(h))
        got = '%08x' % JI_fnv(raw)
        row.check(got == want, '%s: read back %s, golden %s' % (what, got, want))
        sums = JI.f16w([raw[i] | raw[i + 1] << 8 for i in range(0, nb, 2)])
        op, st, p = self.jcall(vp, J['J_OP_COMMIT'], [slot] + sums)
        row.check(st == K['HP_OK'] and p[:2] == sums, '%s: COMMIT with the read-back\'s '
                  'F16w: status %d' % (what, st))

    def t_decode(self):
        row = T.Row('DECODE: corpus JPEGs through the input ring, FNV-1a = golden')
        self.rows.append(row)
        if not os.path.isdir(CORPUS):
            row.check(False, 'corpus missing: ln -s the jpeg build into build/jpeg '
                      '(tests/jpeg/make_corpus.sh)')
            return
        g = golden()
        try:
            b, h, vp = self.setup(3)
            jb = vp.rd(J['HP_JOBS'])
            ops = vp.rd(jb + J['J_OPS'])
            row.check(ops >> (J['J_OP_DECODE'] - 0x20) & 1, 'DECODE not in J_OPS')
            rslot, rbase = self.ring_slot(vp)
            cases = [('t300_420_q85.jpg', [(142, 142)]),       # 5.9 KB: in place
                     ('t600_exif.jpg', [(142, 142)]),          # 33 KB: wraps, 2 passes
                     ('t600_444_q85.jpg', [(142, 142)]),       # 4:4:4, 2 passes
                     ('w640x360_420_q85.jpg', [(142, 80)])]    # the NP box's fit
            key = 0x100
            for name, sizes in cases:
                data = open(os.path.join(CORPUS, name), 'rb').read()
                outs = []
                for (w, hh) in sizes:
                    key += 1
                    cls = J['J_CLS_THUMB'] if w * hh * 2 <= 65536 else J['J_CLS_DRIFT']
                    outs.append(((w, hh), self.out_slot(vp, cls, w, hh, key)))
                st, p, s, n = self.decode(b, vp, rslot, rbase, data, [(sz, sl) for sz, (sl, _) in outs])
                # jdec rev 2: the probe, then one pass for every output
                want_passes = 1 if len(data) <= RING_W * RING_H * 2 else 2
                row.check(st == K['HP_OK'], '%s: status %d' % (name, st))
                row.check(s.passes == want_passes, '%s: %d passes streamed, want %d' % (
                    name, s.passes, want_passes))
                sw = int(name[1:4]) if name[0] == 't' else 640
                row.check(st != K['HP_OK'] or p and p[0] & 0xFFFF == sw, '%s: reply %s' % (name, p))
                self.metrics['%s instructions' % name] = '%.1f M (%d passes, %d B streamed)' % (
                    n / 1e6, s.passes, s.bytes)
                if st == K['HP_OK']:
                    for (w, hh), (sl, addr) in outs:
                        want = g[(name, w, hh)]
                        self.check_out(row, b, vp, '%s %dx%d' % (name, w, hh), sl, addr, w, hh,
                                       want[3])
                for _, (sl, _) in outs:
                    self.jcall(vp, J['J_OP_FREE'], [sl])

            # two outputs in one job (the NP box and a thumbnail): inside the
            # work area since jdec streams; the Mac's jdec of the same set
            o2 = [self.out_slot(vp, J['J_CLS_THUMB'], 142, 142, 0x1F0),
                  self.out_slot(vp, J['J_CLS_THUMB'], 55, 55, 0x1F1)]
            data = open(os.path.join(CORPUS, 't600_444_q85.jpg'), 'rb').read()
            st, p, s, n = self.decode(b, vp, rslot, rbase, data,
                                      [((142, 142), o2[0][0]), ((55, 55), o2[1][0])])
            row.check(st == K['HP_OK'], 'two outputs in one job: status %d' % st)
            if st == K['HP_OK']:
                want = mac_windows('t600_444_q85.jpg', [(142, 142, 0, 0, 142, 142),
                                                        (55, 55, 0, 0, 55, 55)])
                for k, (w, hh) in enumerate(((142, 142), (55, 55))):
                    self.check_out(row, b, vp, 't600_444 %dx%d, two in one job' % (w, hh),
                                   o2[k][0], o2[k][1], w, hh, want[k])
            for sl, _ in o2:
                self.jcall(vp, J['J_OP_FREE'], [sl])

            # the failures
            o = self.out_slot(vp, J['J_CLS_THUMB'], 142, 142, 0x200)
            data = open(os.path.join(CORPUS, 't600_prog.jpg'), 'rb').read()
            st, p, s, n = self.decode(b, vp, rslot, rbase, data, [((142, 142), o[0])])
            row.check(st == J['J_E_DEC_UNSUPPORTED'], 'progressive: status %d' % st)
            data = open(os.path.join(CORPUS, 't600_444_q85.jpg'), 'rb').read()
            st, p, s, n = self.decode(b, vp, rslot, rbase, data, [((142, 142), o[0])],
                                      abort_at=16384)
            row.check(st == J['J_E_DEC_CORRUPT'], 'the PP\'s abort: status %d' % st)
            st, p, s, n = self.decode(b, vp, o[0], o[1], data, [((142, 142), rslot)])
            row.check(st == J['J_E_SLOT'], 'a ring that is not a scratch slot: status %d' % st)
            # a 16:1 cover's fill, 4,800 x 300 cropped to the panel: jdec_work()
            # 794,240 B, past the work area
            big = self.out_slot(vp, J['J_CLS_DRIFT'], 300, 300, 0x201)
            st, p, s, n = self.decode(b, vp, rslot, rbase, data,
                                      [((4800, 300, 2250, 0, 300, 300), big[0])])
            row.check(st == K['HP_E_NOMEM'], '4800x300 (past the work area): status %d' % st)
            self.jcall(vp, J['J_OP_FREE'], [big[0]])
            # after all that, a good decode still works (the passes carry on)
            data = open(os.path.join(CORPUS, 't600_exif.jpg'), 'rb').read()
            st, p, s, n = self.decode(b, vp, rslot, rbase, data, [((142, 142), o[0])])
            row.check(st == K['HP_OK'], 'a decode after the failures: status %d' % st)
            if st == K['HP_OK']:
                self.check_out(row, b, vp, 't600_exif again', o[0], o[1], 142, 142,
                               g[('t600_exif.jpg', 142, 142)][3])
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def t_panel(self):
        """The panel's fill on the BCM (jpeg-spike.md section 15): before
        jdec streamed its outputs, a 300 x 300 needed 378,160 B of work
        and the PP never sent it"""
        row = T.Row('DECODE: the panel\'s fill and the art set in the work area')
        self.rows.append(row)
        if not os.path.isdir(CORPUS):
            row.check(False, 'corpus missing (tests/jpeg/make_corpus.sh)')
            return
        # (file, outputs: dw, dh, cx, cy, cw, ch); the fills as artcache.c
        # decode_run() sizes them (the short side fills, rounded up,
        # centred), the box fitted
        cases = [
            ('t600_420_q85.jpg', [(300, 300, 0, 0, 300, 300)]),
            ('t3000_420_q85.jpg', [(300, 300, 0, 0, 300, 300)]),
            ('t3000_420_q85.jpg', [(300, 300, 0, 0, 300, 300), (142, 142, 0, 0, 142, 142),
                                   (55, 55, 0, 0, 55, 55)]),
            ('w640x360_420_q85.jpg', [(534, 300, 117, 0, 300, 300), (142, 80, 0, 0, 142, 80),
                                      (98, 55, 21, 0, 55, 55)]),
            ('h333x500_420_q85.jpg', [(300, 451, 0, 75, 300, 300)]),
        ]
        work = H.B.config_value('BCMFW_JOBS_DO_DRIFT', JI.JDEFS) - \
            H.B.config_value('BCMFW_JOBS_DEC_WORK', JI.JDEFS)
        try:
            b, h, vp = self.setup(3)
            rslot, rbase = self.ring_slot(vp)
            key = 0x300
            for name, outs in cases:
                data = open(os.path.join(CORPUS, name), 'rb').read()
                need = jdec_work(outs)
                what = '%s %s' % (name, ','.join('%dx%d' % (o[0], o[1]) for o in outs))
                row.check(need <= work, '%s: jdec_work %d past the work area %d' % (
                    what, need, work))
                slots = []
                for o in outs:
                    key += 1
                    cls = J['J_CLS_DRIFT'] if o[4] == 300 else J['J_CLS_THUMB']
                    sl, addr = self.out_slot(vp, cls, o[4], o[5], key)
                    pitch = (o[4] + 15) & ~15       # J_SLOT_PITCH, every class
                    slots.append((sl, addr, pitch))
                st, p, s, n = self.decode(b, vp, rslot, rbase, data,
                                          [(o, sl) for o, (sl, _, _) in zip(outs, slots)])
                row.check(st == K['HP_OK'], '%s: status %d' % (what, st))
                self.metrics['panel %s' % what] = '%.1f M instructions, work %d of %d B' % (
                    n / 1e6, need, work)
                if st == K['HP_OK']:
                    want = mac_windows(name, outs)
                    for o, (sl, addr, pitch), wv in zip(outs, slots, want):
                        self.check_out(row, b, vp, '%s %dx%d window %d,%d (pitch %d)' % (
                            name, o[0], o[1], o[2], o[3], pitch), sl, addr, o[4], o[5], wv,
                            pitch=pitch)
                for sl, _, _ in slots:
                    self.jcall(vp, J['J_OP_FREE'], [sl])
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def t_gate(self):
        """The IDCT's start-up check (jdec_vpu.c idct_check, J_VPU_MAC): as
        built it passes and a DECODE whose 8x8 blocks it serves (t300 at
        142: the luma at 4 points, the chroma at 8; t142 at its own size: the
        chroma at 16) calls jv_idct4n; built to fail
        (BCMFW_JOBS_MAC_FAIL), J_VPU_MAC stays clear and the same decodes
        never call it (the C IDCT); the outputs equal golden either way"""
        row = T.Row('IDCT self-check: J_VPU_MAC, vector or C, golden either way')
        self.rows.append(row)
        if not os.path.isdir(CORPUS):
            row.check(False, 'corpus missing (tests/jpeg/make_corpus.sh)')
            return
        g = golden()
        cases = [('t300_420_q85.jpg', 142, 142), ('h333x500_420_q95.jpg', 95, 142),
                 ('t142_420_q85.jpg', 142, 142)]
        for fail in (False, True):
            defs = JI.JDEFS + (('BCMFW_JOBS_MAC_FAIL=1',) if fail else ())
            what = 'check forced to fail' if fail else 'check passes'
            try:
                b, h, vp = self.setup(3, defines=defs)
                jb = vp.rd(J['HP_JOBS'])
                want = J['J_VPU_CHECKED'] | J['J_VPU_SHIFT'] | J['J_VPU_YUV'] | \
                    (0 if fail else J['J_VPU_MAC'])
                got = vp.rd(jb + J['J_VPU'])
                row.check(got == want, '%s: J_VPU %x, want %x' % (what, got, want))
                calls = count_calls(b, self.image(defs)[0].symbols['jv_idct4n'])
                rslot, rbase = self.ring_slot(vp)
                for name, w, hh in cases:
                    if (name, w, hh) not in g:
                        row.check(False, '%s %dx%d: not in golden.txt' % (name, w, hh))
                        continue
                    data = open(os.path.join(CORPUS, name), 'rb').read()
                    sl, addr = self.out_slot(vp, J['J_CLS_THUMB'], w, hh, 0x300 + fail)
                    calls[0] = 0
                    st, p, s, n = self.decode(b, vp, rslot, rbase, data, [((w, hh), sl)])
                    row.check(st == K['HP_OK'], '%s: %s: status %d' % (what, name, st))
                    row.check((calls[0] > 0) != fail, '%s: %s: jv_idct4n called %d times' % (
                        what, name, calls[0]))
                    if st == K['HP_OK']:
                        self.check_out(row, b, vp, '%s: %s %dx%d' % (what, name, w, hh), sl,
                                       addr, w, hh, g[(name, w, hh)][3])
                    self.metrics['%s %s' % (name, 'C IDCT' if fail else 'vector IDCT')] = \
                        '%.2f M instructions' % (n / 1e6)
                    self.jcall(vp, J['J_OP_FREE'], [sl])
            except (T.Fail, H.Timeout) as e:
                row.check(False, '%s: %s' % (what, e))

    # MERGED IMAGE, next re-verify batch (album-render.md section 5.3)
    MERGED_OVL = JI.Suite.VIDEO_SHIPPED + ('BCMFW_JOBS=1', 'BCMFW_JOBS_MERGED_OVL=1')

    def t_merged_overlay(self):
        """DECODE in the merged image, as an overlay (video/dec_ovl.c,
        J_OP_OVERLAY into 0x12B000): before a load DECODE is unknown; once
        loaded J_OPS offers it; with no work slot lent it fails HP_E_NOMEM;
        with a J_CLS_DRIFT slot keyed J_DEC_WORK_KEY lent and the file's
        length in J_DEC_LEN, the ring's decodes give the golden pixels (the
        NP box, and the 300 x 300 panel into the other DRIFT slot); an
        epoch (RESET) drops the overlay's DECODE again"""
        row = T.Row('merged image: DECODE as an overlay, a lent work slot')
        self.rows.append(row)
        if not os.path.isdir(CORPUS):
            row.check(False, 'corpus missing: ln -s the jpeg build into build/jpeg')
            return
        defs = self.MERGED_OVL
        bit = 1 << (J['J_OP_DECODE'] - 0x20)
        g = golden()
        try:
            b, h, vp = self.setup(3, defines=defs)
            jb = vp.rd(J['HP_JOBS'])
            row.check(not vp.rd(jb + J['J_OPS']) & bit, 'DECODE offered before a load')
            rslot, rbase = self.ring_slot(vp)
            data = open(os.path.join(CORPUS, 't600_exif.jpg'), 'rb').read()
            box, baddr = self.out_slot(vp, J['J_CLS_THUMB'], 142, 142, 0x0E2)
            st, p, s, n = self.decode_len(b, vp, rslot, rbase, data, [((142, 142), box)])
            row.check(st == K['HP_E_UNKNOWN'], 'DECODE before the load: status %d' % st)
            # the load
            res = self.image(defs)[0]
            out = os.path.join(T.ROOT, 'build', 'bcmfw', 'test-dec-ovl')
            oprog, orep = H.B.build_overlay(res, out, list(defs), listing=False)
            img = open(os.path.join(out, 'dec-ovl.bin'), 'rb').read()
            words = [int.from_bytes(img[i:i + 4], 'little') for i in range(0, len(img), 4)]
            self.metrics['merged DECODE overlay bytes, spare'] = '%d, %d' % (
                orep['load'], orep['spare'])
            vp.wr_block(H.B.config_value('BCMFW_JOBS_OVL', defs), words)
            sums = JI.f16w([hw for w in words for hw in (w & 0xFFFF, w >> 16)])
            op, st, _ = self.jcall(vp, J['J_OP_OVERLAY'], [len(words)] + sums)
            ops = vp.rd(jb + J['J_OPS'])
            row.check(st == K['HP_OK'] and ops & bit, 'load: status %d, J_OPS %08x' % (st, ops))
            caps = vp.rd(jb + J['J_CAPS'])
            row.check(caps >> 16 == J['J_CAPS_MAGIC'] >> 16 and caps & J['J_CAP_DEC_LENT'],
                      'J_CAPS %08x: no J_CAP_DEC_LENT' % caps)
            # nothing lent: NOMEM, never a write elsewhere
            stream = lambda: self.decode_len(b, vp, rslot, rbase, data, [((142, 142), box)])
            st, p, s, n = stream()
            row.check(st == K['HP_E_NOMEM'], 'no work slot: status %d' % st)
            work, waddr = self.out_slot(vp, J['J_CLS_DRIFT'], 300, 300, J['J_DEC_WORK_KEY'])
            st, p, s, n = stream()
            row.check(st == K['HP_OK'], 'the NP box: status %d' % st)
            if st == K['HP_OK']:
                self.metrics['merged overlay t600_exif 142'] = '%.1f M instructions, %d ' \
                    'passes' % (n / 1e6, s.passes)
                self.check_out(row, b, vp, 'merged overlay: t600_exif 142', box, baddr, 142,
                               142, g[('t600_exif.jpg', 142, 142)][3])
            pan, paddr = self.out_slot(vp, J['J_CLS_DRIFT'], 300, 300, 0x0E3)
            st, p, s, n = self.decode_len(b, vp, rslot, rbase, data, [((300, 300), pan)])
            row.check(st == K['HP_OK'], 'the panel: status %d' % st)
            if st == K['HP_OK']:
                self.check_out(row, b, vp, 'merged overlay: t600_exif 300 (the resident '
                               'panel)', pan, paddr, 300, 300,
                               g[('t600_exif.jpg', 300, 300)][3], pitch=J['J_DRIFT_PITCH'])
            self.jcall(vp, J['J_OP_FREE'], [work])
            op, st, _ = self.jcall(vp, J['J_OP_RESET'], [])
            row.check(not vp.rd(jb + J['J_OPS']) & bit, 'DECODE still offered after RESET')
            row.check(not vp.rd(jb + J['J_CAPS']) & J['J_CAP_DEC_LENT'],
                      'J_CAP_DEC_LENT kept after RESET')
        except (T.Fail, H.Timeout) as e:
            row.check(False, str(e))

    def decode_len(self, b, vp, rslot, rbase, data, outs):
        """The merged overlay's ring DECODE: J_DEC_LEN the file's length,
        the record's len clipped to the ring (jref's bound)"""
        jb = vp.rd(J['HP_JOBS'])
        vp.wr(jb + J['J_DEC_LEN'], len(data))
        s = Stream(vp, rbase, RING_W * RING_H * 2, data)
        pay = [rslot, min(len(data), RING_W * RING_H * 2), len(outs)]
        for o, slot in outs:
            pay += [xy(o[0], o[1]), xy(0, 0), xy(o[0], o[1]), slot, xy(0, 0)]
        vp.tag = (vp.tag + 1) & 0x7fff or 1
        tag = vp.send(J['J_OP_DECODE'], pay, tag=vp.tag | J['J_TAG_REPLY'])
        steps0 = b.sim.steps
        for _ in range(20000):
            b.run(100_000)
            s.service()
            vp.drain()
            if tag in vp.replies:
                op, st, p = vp.replies.pop(tag)
                return st, p, s, b.sim.steps - steps0
        raise T.Fail('no DECODE reply')

    def run(self):
        t0 = time.time()
        bad = corpus_problem()
        if bad:
            print(bad)
            print('0 rows, 0 cases, 1 failed (%.0f s)' % (time.time() - t0))
            return 1
        self.t_decode()
        self.t_panel()
        self.t_overlay()
        self.t_yield()
        self.t_gate()
        self.t_merged_overlay()
        for row in self.rows:
            print('%-72s %5d %5d' % (row.name[:72], row.cases, row.cases - len(row.fails)))
            for f in row.fails[:12]:
                print('    FAIL: %s' % f)
        for k in sorted(self.metrics):
            print('  %-36s %s' % (k, self.metrics[k]))
        cases = sum(r.cases for r in self.rows)
        fails = sum(len(r.fails) for r in self.rows)
        print('%d rows, %d cases, %d failed (%.0f s)' % (len(self.rows), cases, fails,
                                                         time.time() - t0))
        return 1 if fails else 0


def count_calls(b, addr):
    """[n]: how often the core reaches addr from now on (the simulator's
    compiled handler of the instruction there, wrapped)"""
    sim, cnt = b.sim, [0]
    fn, size, ins = sim.code.get(addr) or sim.fetch(addr)

    def w():
        cnt[0] += 1
        return fn()
    sim.code[addr] = (w, size, ins)
    return cnt


def jdec_work(outs):
    """jdec.h jdec_work() for outputs (dw, dh, ...): the largest area's
    rows, every output's resizer"""
    import re
    jh = open(os.path.join(T.ROOT, 'bcmfw', 'jobs', 'jdec.h')).read()
    base = int(re.search(r'#define\s+JDEC_WORK_BASE\s+\((\d+)u \* 1024\)', jh).group(1)) * 1024
    rows = [int(x) for x in re.search(
        r'#define\s+JDEC_WORK_ROWS\(dw\)\s+\(\(uint32_t\)\(dw\) \* (\d+) \+ (\d+)\)', jh).groups()]
    out = [int(x) for x in re.search(
        r'#define\s+JDEC_WORK_OUT\(dw\)\s+\(\(uint32_t\)\(dw\) \* (\d+) \+ (\d+)\)', jh).groups()]
    p = 0
    for k, o in enumerate(outs):
        if o[0] * o[1] > outs[p][0] * outs[p][1]:
            p = k
    return base + outs[p][0] * rows[0] + rows[1] + sum(o[0] * out[0] + out[1] for o in outs)


_MAC = []


def mac_windows(name, outs):
    """Each output's window as the Mac's jdec makes it (the CPU twin's
    code, decoded once with the same outputs): its FNV-1a, hex"""
    import subprocess
    import tempfile
    import jpeg_test as JT
    if not _MAC:
        _MAC.append(JT.mac_build())
    with tempfile.TemporaryDirectory() as d:
        dump = os.path.join(d, 'px')
        args = [str(v) for o in outs for v in o[:2]]
        r = subprocess.run([_MAC[0], os.path.join(CORPUS, name)] + args, capture_output=True,
                           env=dict(os.environ, JDTEST_DUMP=dump))
        if r.returncode:
            raise T.Fail('Mac jdec of %s: %s' % (name, r.stdout))
        px = open(dump, 'rb').read()
    want, at = [], 0
    for dw, dh, cx, cy, cw, ch in outs:
        rows = b''.join(px[at + 2 * (dw * (cy + y) + cx):][:2 * cw] for y in range(ch))
        want.append('%08x' % JI_fnv(rows))
        at += 2 * dw * dh
    return want


def JI_fnv(raw):
    v = 0x811C9DC5
    for x in raw:
        v = ((v ^ x) * 0x01000193) & 0xFFFFFFFF
    return v


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--seed', type=int, default=2722)
    ap.add_argument('--vmcs', default=T.DEFAULT_VMCS)
    ap.add_argument('--png', default=os.path.join(T.ROOT, 'build', 'bcmfw', 'test-decode'))
    a = ap.parse_args(argv)
    if not H.B.have_gcc():
        print('skip: vc4-elf-gcc not found')
        return 0
    return Suite(a).run()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
