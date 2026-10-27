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
"""Video end to end in the simulator: the BCMFW_VIDEO image, driven over
the host protocol by a model of the ProkBox player's driver, decodes
H.264 on the chip, converts to RGB565, and pushes frames to the panel.

The host side (class Player) does what the Rockbox driver will do
(docs/research/bcm2722/pipeline.md, "The ProkBox driver contract"):
  - splits the corpus .264 into NAL units and groups them into access
    units (the samples an MP4 demuxer hands over), one packet each;
  - attaches (HELLO), OPENs an H.264 session (own display, FIT);
  - writes packets into the SDRAM bitstream FIFO through the host port in
    bursts, 64-byte aligned, wrapping, and posts a FEED per packet;
    it keeps within the queue depth from OPEN and the free space from
    FIFO_RD (flow control without commands), never waiting without a
    deadline;
  - flags the last packet EOS, waits for EV_EOS, reads STATUS, CLOSEs.

The panel model captures each frame the image pushes (at the end of each
DMA run).  Every frame is compared, pixel for pixel, with ffmpeg's
decode of the same stream (the corpus .yuv) converted through the exact
reference (bcmfw/video/dispref.py: BT.601 integer conversion, 2:1 box
above 320x240, letterbox placement).

Profile: instructions per frame by stage (decode, convert, scale, push,
protocol glue), counted per address in vc2sim; vector instructions also
by REP rows for the cycle estimate.  The budget is 5.0 M cycles per frame
(150 MHz, 30 fps).

Robustness: fuzzed and truncated streams go through the same protocol
path; every packet must be consumed (FIFO_RD) within an instruction
bound, STATUS must answer, the heartbeat must move, no fault.

Usage:
  bcmfw/tests/video_test.py [-v] [--frames N] [--clips a,b] [--fuzz N]
                            [--json OUT] [--only NAME]

Needs vc4-elf-gcc (skips cleanly without) and the corpus
(tests/video/make_corpus.sh builds it).
"""
import collections
import json
import os
import random
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hello_test as T      # noqa: E402
import hostproto_test as HP  # noqa: E402
import dma_test as DT       # noqa: E402

S, M, B, C = T.S, T.M, T.B, T.C
sys.path.insert(0, os.path.join(T.FW, 'video'))
import dispref as R         # noqa: E402

K = HP.K
DEFS = ('BCMFW_VIDEO=1',)
CORPUS = os.path.join(T.ROOT, 'build', 'video', 'corpus')
SDRAM_BCM, SDRAM_HOST = 0xFE000000, 0xC0000000
SDRAM_SIZE = 0x400000
BUDGET = 5_000_000           # cycles per frame: 150 MHz / 30 fps
FRAME_US = 33367             # 29.97 fps PTS step

# Stages by function (self time).  Decoder functions are everything the
# h264 sources define (sim_test's stage table) plus its vector kernels.
CONVERT = {'disp_yuv_tile', 'disp_convert'}
SCALE = {'disp_box2_16', 'disp_box2_8', 'disp_scale2', 'box2_c'}
PUSH = {'lcd_blit', 'lcd_blit_wait', 'lcd_window', 'push_rect', 'push',
        'dma_lcd_start', 'dma_lcd_wait', 'dma_lcd_push', 'dma_lcd_stop', 'udelay',
        'spin', 'lcd_reg', 'lcd_index', 'lcd_data', 'dcs_range', 'bcmfw_push',
        'lcd_power_on', 'lcd_timing', 'run_script', 'gpio_reset', 'mdelay'}
GLUE = {'video_work', 'present', 'decode_error', 'video_queue', 'video_get_stats',
        'video_open', 'video_close', 'sdram_init', 'sdram_cmd', 'cache_flush',
        'hp_work', 'hp_event', 'post', 'reply', 'dispatch', 'cmd_feed', 'cmd_status',
        'cmd_open', 'cmd_close', 'cmd_hello', 'check_session', 'evt_free',
        'end_session', 'geometry', 'decode_packet'}
POLL = {'bcmfw_main', 'hp_poll', 'heartbeat', 'publish_state', 'idle_wait',
        'command', 'publish_info'}
STAGES = ('decode', 'convert', 'scale', 'push', 'glue', 'memops', 'poll')


def stage_of(name, h264_names):
    if name in CONVERT:
        return 'convert'
    if name in SCALE:
        return 'scale'
    if name in PUSH:
        return 'push'
    if name in GLUE:
        return 'glue'
    if name in POLL:
        return 'poll'
    if name in ('memcpy', 'memset'):
        return 'memops'
    if name in h264_names or name.startswith(('h264_', 'bs_', '__')):
        return 'decode'
    return 'glue'


def h264_function_names():
    """Every function the decoder's sources define (static ones too)."""
    names = set()
    src = os.path.join(T.FW, 'h264')
    for f in os.listdir(src):
        if f.endswith('.c'):
            text = open(os.path.join(src, f)).read()
            names.update(re.findall(r'^(?:static\s+)?(?:inline\s+)?[\w\s\*]+?\b(\w+)\s*\(',
                                    text, re.M))
    return names


# --------------------------------------------------------------------------
# The stream: NAL units grouped into access units

def nal_units(data):
    out, starts, i = [], [], 0
    while True:
        j = data.find(b'\x00\x00\x01', i)
        if j < 0:
            break
        starts.append(j + 3)
        i = j + 3
    for k, s in enumerate(starts):
        e = starts[k + 1] - 3 if k + 1 < len(starts) else len(data)
        nal = data[s:e].rstrip(b'\x00')
        if nal:
            out.append(nal)
    return out


def access_units(data):
    """Packets as a demuxer would hand them over: parameter sets and SEI
    go with the next picture; a slice with first_mb_in_slice = 0 (first
    RBSP bit 1) starts a new picture."""
    aus, cur, has_slice = [], [], False
    for nal in nal_units(data):
        t = nal[0] & 31
        first = t in (1, 5) and len(nal) > 1 and nal[1] & 0x80
        if has_slice and (first or t in (6, 7, 8, 9)):
            aus.append(cur)
            cur, has_slice = [], False
        cur.append(nal)
        has_slice = has_slice or t in (1, 5)
    if cur:
        aus.append(cur)
    return [b''.join(b'\x00\x00\x00\x01' + n for n in au) for au in aus]


# --------------------------------------------------------------------------
# The BCM side: hello_test's bench with DMA, SDRAM and a profile

class VideoBench(DT.DmaBench):
    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self.bus.alias(SDRAM_BCM, SDRAM_BCM + SDRAM_SIZE - 1, S.SDRAM_BACKING)
        self.pcount = collections.Counter()
        self.on_mark = None              # called with the counts at each present()
        self.mark_pc = None
        self.captures = []               # panel frames at each run end
        sim = self.sim
        orig_fetch = sim.fetch
        bench = self

        def fetch(pc):
            fn, size, ins = orig_fetch(pc)
            cnt = bench.pcount
            if pc == bench.mark_pc:
                def counted():
                    cnt[pc] += 1
                    if bench.on_mark:
                        bench.on_mark(cnt)
                    return fn()
            else:
                def counted():
                    cnt[pc] += 1
                    return fn()
            sim.code[pc] = (counted, size, ins)
            return sim.code[pc]
        sim.fetch = fetch
        p = self.panel
        orig_fifo = p._fifo

        def fifo(v):
            before = p.run_left
            orig_fifo(v)
            if before > 0 and p.run_left == 0:
                bench.captures.append((tuple(p.win), list(p.fb)))
        p._fifo = fifo

    def power_on(self, rng):
        super().power_on(rng)
        # SDRAM content is undefined too
        b = S.SDRAM_BACKING
        self.bus.b[b:b + SDRAM_SIZE] = rng.randbytes(SDRAM_SIZE)

    def host_sdram_write(self, off, data):
        """The PP writes SDRAM through the host port (0xC0000000 + off)."""
        b = S.SDRAM_BACKING
        self.bus.b[b + off:b + off + len(data)] = data


def vclk_timer(b):
    """P19 PW4: the counter at the core's clock (clock_band checks the
    core's speed against the crystal's), vclk_test.py's model"""
    import vclk_test
    return vclk_test.ClockTimer(b)


class Player(HP.Vp):
    """The ProkBox video driver model: HP.Vp's rings plus the FIFO."""

    def open(self, w, h, mode=None, flags=None):
        mode = K['HP_MODE_FIT'] if mode is None else mode
        flags = K['HP_OPEN_OWN_DISPLAY'] if flags is None else flags
        op, st, p = self.call(K['HP_OP_OPEN'], [K['HP_CODEC_H264'], w | h << 16,
                                                mode | flags, 0, 320 | 240 << 16],
                              timeout_us=200_000)
        if st == K['HP_OK']:
            self.session, fifo, self.fsize, self.depth = p[0], p[1], p[2], p[3]
            self.fifo_off = fifo - SDRAM_HOST
            self.wr_bytes = 0            # FIFO bytes written (64-aligned per packet)
            self.rd_units = 0
            self.in_flight = []          # 64-byte units per packet fed, oldest first
        return st, p

    def fifo_free(self):
        rd = self.index(K['HP_FIFO_RD'])
        if rd is not None:
            # packets fully consumed since last look
            done = (rd - self.rd_units) & 0xffff
            while done and self.in_flight:
                u = self.in_flight[0]
                if u > done:
                    break
                done -= u
                self.in_flight.pop(0)
                self.rd_units = (self.rd_units + u) & 0xffff
        used = sum(self.in_flight) * 64
        return self.fsize - used

    def feed(self, pkt, pts, flags=0):
        """Write one packet into the FIFO and post its FEED.  Returns False
        (and does nothing) when the FIFO, the queue or the ring is full."""
        n = len(pkt)
        units = (n + 63) // 64
        free = self.fifo_free()          # reads FIFO_RD, retires packets
        if len(self.in_flight) >= self.depth or free < units * 64:
            return False
        off = self.wr_bytes % self.fsize
        first = min(n, self.fsize - off)
        self.b.host_sdram_write(self.fifo_off + off, pkt[:first])
        if first < n:
            self.b.host_sdram_write(self.fifo_off, pkt[first:])
        # host-port cost: an address setup per contiguous piece, then one
        # 16-bit transfer per halfword
        self.xfers += 2 * (1 + (first < n)) + (n + 1) // 2
        tag = self.send(K['HP_OP_FEED'], [self.session, off, n, pts & 0xffffffff,
                                          pts >> 32, flags])
        if tag is None:
            return False
        self.wr_bytes += units * 64
        self.in_flight.append(units)
        return True


class Suite(T.Suite):
    def __init__(self, args):
        super().__init__(args)
        self.results = {}
        self.h264_names = h264_function_names()

    def bench(self, pid, defines=None, reset_pc=None):
        defines = DEFS if defines is None else defines
        prog, rep, img = self.image(defines)
        entry = B.config_value('BCMFW_ENTRY', defines)
        b = VideoBench(img, pid, entry, reset_pc)
        b.mark_pc = prog.symbols['present']
        h = T.Host(b, random.Random(self.rng.getrandbits(32)))
        return b, h, prog

    def setup(self, pid=1):
        b, h, prog = self.bench(pid)
        h.fb = [0] * (T.W * T.H)
        h.lcd_awake()
        h.settle()
        h.tick_task = False
        h.idle_ok = False
        vp = Player(b)
        vp.attach()
        return b, h, vp, prog

    # ---- one clip end to end --------------------------------------------

    def play(self, row, clip, max_frames=0, pid=1, mode=None):
        data = open(clip_path(clip), 'rb').read()
        ref = open(clip_path(clip)[:-4] + '.yuv', 'rb').read()
        aus = access_units(data)
        if max_frames:
            aus = aus[:max_frames]
        w, h = sps_size(clip)
        b, hst, vp, prog = self.setup(pid)
        op, st, p = vp.call(K['HP_OP_HELLO'], [K['HP_MAJOR'] << 8 | K['HP_MINOR']])
        row.check(st == K['HP_OK'] and p[1] & K['HP_CAP_H264_BP'],
                  '%s: HELLO caps %s' % (clip, p))
        st, p = vp.open(w, h, mode)
        row.check(st == K['HP_OK'], '%s: OPEN status %d' % (clip, st))
        if st != K['HP_OK']:
            return None
        b.captures.clear()
        self.start_profile(prog, b)
        t_open = b.now()
        steps0 = b.sim.steps
        x0 = vp.xfers
        k = 0
        deadline_steps = b.sim.steps + 40_000_000 * (len(aus) + 2)
        while k < len(aus):
            fl = K['HP_FEED_KEY'] if k == 0 else 0
            if k == len(aus) - 1:
                fl |= K['HP_FEED_EOS']
            if vp.feed(aus[k], k * FRAME_US, fl):
                k += 1
                continue
            b.run(20_000)
            vp.drain()
            if b.sim.steps > deadline_steps:
                raise HP.Timeout('%s: no progress feeding packet %d' % (clip, k))
        # EOS
        while not any(e[0] == K['HP_EV_EOS'] for e in vp.events):
            b.run(20_000)
            vp.drain()
            if b.sim.steps > deadline_steps:
                raise HP.Timeout('%s: no EV_EOS' % clip)
        feed_xfers = vp.xfers - x0
        _, st, sp = vp.call(K['HP_OP_STATUS'], [vp.session])
        steps = b.sim.steps - steps0
        t_play = b.now() - t_open
        _, cst, cp = vp.call(K['HP_OP_CLOSE'], [vp.session], timeout_us=100_000)
        row.check(cst == K['HP_OK'], '%s: CLOSE %d' % (clip, cst))
        errors = [e for e in vp.events if e[0] == K['HP_EV_ERROR']]
        row.check(not errors, '%s: EV_ERROR %s' % (clip, errors[:2]))
        # ---- compare every frame
        fsize = w * h * 3 // 2
        nref = len(ref) // fsize
        caps = b.captures
        black = [0] * (T.W * T.H)
        _, _, _, ow0, oh0 = R.display(ref[:fsize], w, h)
        if ow0 < T.W or oh0 < T.H:
            row.check(caps and caps[0][1] == black, '%s: first push is not the black '
                      'letterbox clear' % clip)
            frames = caps[1:]
        else:
            frames = caps          # it fills the panel: no clear (dev39)
        n = len(aus)
        row.check(len(frames) == n and sp[6] == n and sp[7] == n,
                  '%s: %d frames pushed, STATUS decoded %d shown %d, want %d' % (
                      clip, len(frames), sp[6], sp[7], n))
        bad = 0
        for i, (win, fb) in enumerate(frames[:min(n, nref)]):
            pix, ox, oy, ow, oh = R.display(ref[i * fsize:(i + 1) * fsize], w, h)
            want = list(black)
            for y in range(oh):
                want[(oy + y) * T.W + ox:(oy + y) * T.W + ox + ow] = pix[y * ow:(y + 1) * ow]
            if fb != want:
                bad += 1
                d = next(j for j in range(T.W * T.H) if fb[j] != want[j])
                row.check(False, '%s frame %d: panel differs first at (%d,%d): %s, want %04x' % (
                    clip, i, d % T.W, d // T.W, fb[d], want[d]))
            if i in (0, n - 1) and self.png_dir and not clip.endswith('.264'):
                T.png_write(os.path.join(self.png_dir, '%s_%02d.png' % (clip, i)), fb)
        row.check(bad == 0, '%s: %d of %d frames differ' % (clip, bad, n))
        row.check(sp[0] in (K['HP_ST_PLAYING'], K['HP_ST_OPEN']) and sp[8] == 0,
                  '%s: STATUS state %d errors %d' % (clip, sp[0], sp[8]))
        # words 13-15 (stage 6's per-frame log): the last FEED's and the
        # longest FEED's time on the chip, and the core clock
        row.check(len(sp) >= 16 and 0 < sp[13] <= sp[14] and sp[15] in (27, 150),
                  '%s: STATUS work %s, max %s, MHz %s' % (
                      clip, sp[13:14], sp[14:15], sp[15:16]))
        pts = sp[11] | sp[12] << 32
        row.check(pts == (n - 1) * FRAME_US, '%s: last PTS %d, want %d' % (
            clip, pts, (n - 1) * FRAME_US))
        self.dma_check(row, b, clip)
        prof = self.profile(prog, b, n)
        res = {'clip': clip, 'size': [w, h], 'frames': n, 'exact': n - bad,
               'steps': steps, 'sim_us': t_play, 'feed_xfers': feed_xfers,
               'feed_bytes': sum(len(a) for a in aus), 'profile': prof}
        self.results[clip] = res
        return res

    def dma_check(self, row, b, tag):
        d, p = b.dma, b.panel
        row.check(not d.errors, '%s: DMA errors %s' % (tag, d.errors[:3]))
        row.check(not d.outside, '%s: DMA read outside SRAM: %s' % (tag, d.outside[:3]))
        bad = [a for a in sorted(d.sources_touched)
               if not (0x105000 <= a < 0x12B000 or T.FB <= a < T.FB + 2 * T.W * T.H)]
        row.check(not bad, '%s: DMA read pages outside the video buffer: %s' % (
            tag, ['%x' % a for a in bad[:4]]))
        row.check(p.fifo_stray == 0 and p.run_left == 0, '%s: LCD run misuse' % tag)
        st = b.r32(T.CMD_WORD)
        row.check(st != T.FAULT, '%s: the image faulted (pc %08x)' % (tag, b.r32(T.MB_FAULT_PC)))

    def start_profile(self, prog, b):
        """Per frame by stage, from the counts at each present() entry:
        frame k's work lies between the entries k and k + 1 (its decode,
        then the previous frame's wait, its conversion and push)."""
        import bisect
        funcs = sorted((a, nm) for nm, a in prog.symbols.items()
                       if a < prog.ends['text'] and not nm.startswith('__') or nm in
                       ('__ctzsi2', '__clzsi2', '__mulsi3', '__divsi3', '__udivsi3'))
        addrs = [a for a, _ in funcs]
        pc_info = {}

        def info(pc):
            r = pc_info.get(pc)
            if r is None:
                k = bisect.bisect_right(addrs, pc) - 1
                st = stage_of(funcs[k][1] if k >= 0 else '?', self.h264_names)
                ins = b.sim.code[pc][2]
                extra = 0
                if getattr(ins.e, 'vclass', None):
                    m = re.search(r' rep(\d+)', ins.text)
                    extra = (int(m.group(1)) if m else 1) - 1
                r = pc_info[pc] = (st, extra)
            return r

        state = {'prev': collections.Counter(b.pcount)}
        per = self.per = []

        def on_mark(cnt):
            prev = state['prev']
            out, rows = collections.Counter(), collections.Counter()
            for pc, c in cnt.items():
                d = c - prev.get(pc, 0)
                if d:
                    st, extra = info(pc)
                    out[st] += d
                    rows[st] += d * extra
            per.append((out, rows))
            state['prev'] = collections.Counter(cnt)
        b.on_mark = on_mark
        self.end_mark = lambda: on_mark(b.pcount)

    def profile(self, prog, b, n):
        self.end_mark()
        b.on_mark = None
        per = self.per[1:]               # from the first present() on
        if not per:
            return {}
        out = {'frames_profiled': len(per)}
        for s in STAGES:
            v = [p[0][s] for p in per]
            out[s] = {'mean': sum(v) / len(v), 'max': max(v)}
        work = [sum(p[0][s] for s in STAGES if s != 'poll') for p in per]
        cyc = [work[i] + sum(per[i][1][s] for s in STAGES if s != 'poll')
               for i in range(len(per))]
        out['work'] = {'mean': sum(work) / len(work), 'max': max(work)}
        out['cycles_est'] = {'mean': sum(cyc) / len(cyc), 'max': max(cyc)}
        out['work_per_frame'] = work
        out['over_budget'] = sum(1 for c in cyc if c > BUDGET)
        for s in ('convert', 'scale'):
            r = [p[1][s] + p[0][s] for p in per]
            out[s + '_cycles_est'] = sum(r) / len(r)
        return out

    # ---- rows -------------------------------------------------------------

    def t_clips(self):
        clips = self.args.clips.split(',')
        for clip in clips:
            row = T.Row('end to end: %s, every frame pixel-exact on the panel' % clip)
            self.rows.append(row)
            try:
                mf = self.args.frames if clip == 'd_ipod_768k' else self.args.max_frames
                res = self.play(row, clip, mf)
                if res:
                    self.log('%s: %d frames, %d exact, %.1f M instructions in all' % (
                        clip, res['frames'], res['exact'], res['steps'] / 1e6))
            except (T.Fail, HP.Timeout) as e:
                row.check(False, str(e))

    def t_native(self):
        """NATIVE mode on the 640x480 clip: the centre 320x240, 1:1."""
        row = T.Row('NATIVE mode, 640x480: the centre 320x240 at 1:1')
        self.rows.append(row)
        try:
            clip, w, h = 'e_vga', 640, 480
            data = open(os.path.join(CORPUS, clip + '.264'), 'rb').read()
            ref = open(os.path.join(CORPUS, clip + '.yuv'), 'rb').read()
            aus = access_units(data)[:2]
            b, hst, vp, prog = self.setup(2)
            st, p = vp.open(w, h, K['HP_MODE_NATIVE'])
            row.check(st == K['HP_OK'], 'OPEN %d' % st)
            b.captures.clear()
            for k, au in enumerate(aus):
                while not vp.feed(au, k * FRAME_US, K['HP_FEED_EOS'] if k == len(aus) - 1 else 0):
                    b.run(20_000)
                    vp.drain()
            t0 = b.sim.steps
            while not any(e[0] == K['HP_EV_EOS'] for e in vp.events):
                b.run(20_000)
                vp.drain()
                if b.sim.steps - t0 > 60_000_000:
                    raise HP.Timeout('no EV_EOS')
            frames = b.captures     # 320x240 of it fills the panel: no clear (dev39)
            row.check(len(frames) == len(aus), '%d frames' % len(frames))
            fsize = w * h * 3 // 2
            for i, (win, fb) in enumerate(frames):
                yp, up, vq = R.split_yuv(ref[i * fsize:(i + 1) * fsize], w, h)
                cx, cy = 160, 120
                y2 = b''.join(yp[(cy + r) * w + cx:(cy + r) * w + cx + 320] for r in range(240))
                u2 = b''.join(up[(cy // 2 + r) * (w // 2) + cx // 2:
                                 (cy // 2 + r) * (w // 2) + cx // 2 + 160] for r in range(120))
                v2 = b''.join(vq[(cy // 2 + r) * (w // 2) + cx // 2:
                                 (cy // 2 + r) * (w // 2) + cx // 2 + 160] for r in range(120))
                want = R.convert(y2, u2, v2, 320, 240)
                if fb != want:
                    d = next(j for j in range(T.W * T.H) if fb[j] != want[j])
                    nd = sum(1 for j in range(T.W * T.H) if fb[j] != want[j])
                    row.check(False, 'frame %d: %d pixels differ, first (%d,%d) %s want %04x' % (
                        i, nd, d % T.W, d // T.W, fb[d], want[d]))
                else:
                    row.check(True, '')
            vp.call(K['HP_OP_CLOSE'], [vp.session], timeout_us=100_000)
        except (T.Fail, HP.Timeout) as e:
            row.check(False, str(e))

    def t_audio(self):
        """AUDIO (dev39, stage 8b): a PCM block through the core and back."""
        row = T.Row('AUDIO: a PCM block through the chip at half gain, every sample')
        self.rows.append(row)
        try:
            import struct
            b, hst, vp, prog = self.setup(1)
            st, p = vp.open(320, 240)
            row.check(st == K['HP_OK'], 'OPEN %d' % st)
            base, size = 0x340000, 0x10000
            op, st, p = vp.call(K['HP_OP_AUDIO'], [vp.session, K['HP_AUDIO_CONFIG'],
                                                   base, base + size, size])
            row.check(st == K['HP_OK'], 'CONFIG %d' % st)
            samples = [((k * 977) % 65536) - 32768 for k in range(2048)]
            data = struct.pack('<%dh' % len(samples), *samples)
            b.host_sdram_write(base + 0x1000, data)
            op, st, p = vp.call(K['HP_OP_AUDIO'], [vp.session, K['HP_AUDIO_PROCESS'],
                                                   0x1000, len(data), 0x8000],
                                timeout_us=200_000)
            row.check(st == K['HP_OK'] and len(p) == 1, 'PROCESS %d %s' % (st, p))
            sb = S.SDRAM_BACKING + base + size + 0x1000
            got = struct.unpack('<%dh' % len(samples), bytes(b.bus.b[sb:sb + len(data)]))
            want = [(v * 0x8000) >> 16 for v in samples]
            bad = sum(1 for x, y in zip(got, want) if x != y)
            row.check(bad == 0, '%d of %d samples wrong' % (bad, len(samples)))
            self.log('AUDIO: %d samples processed in %d us (simulated)' % (len(samples), p[0] if p else -1))
            op, st, p = vp.call(K['HP_OP_AUDIO'], [vp.session, K['HP_AUDIO_PROCESS'],
                                                   size - 2, 4, 0x10000])
            row.check(st == K['HP_E_INVAL'], 'past the ring refused: %d' % st)
            vp.call(K['HP_OP_CLOSE'], [vp.session], timeout_us=100_000)
        except (T.Fail, HP.Timeout) as e:
            row.check(False, str(e))

    def t_aac(self):
        """AAC (M4, stage 9): the bcs/bcc probe, then a clip's first access
        units decoded on the chip through HP_OP_AAC, the PCM byte for byte
        against the host build of the same sources (bcmfw/aac/test)."""
        import struct
        sys.path.insert(0, os.path.join(T.FW, 'aac', 'test'))
        from adts2au import read_au
        row = T.Row('AAC: probe, then %d access units decoded on the chip, PCM '
                    'byte-exact vs the host' % AAC_AUS)
        self.rows.append(row)
        try:
            ref = aac_reference(AAC_CLIP)
            if ref is None:
                row.check(False, 'no host reference for %s (bcmfw/aac/test/run.sh '
                          '--host failed)' % AAC_CLIP)
                return
            asc, aus = read_au(os.path.join(AAC_OUT, 'clips', AAC_CLIP + '.au'))
            b, hst, vp, prog = self.setup(1)
            # the probe first: registers only, no session
            op, st, p = vp.call(K['HP_OP_AAC'], [0, K['HP_AAC_PROBE']])
            want = aac_probe_want()
            row.check(st == K['HP_OK'] and p[1] == want and p[2] == len(AAC_PROBE),
                      'PROBE: status %d, %s; the model says %08x' % (st, p, want))
            row.check(p[0] == want, 'PROBE: vc2sim took %08x, want %08x' % (p[0], want))
            # nothing without a session
            op, st, p = vp.call(K['HP_OP_AAC'], [1, K['HP_AAC_OPEN'], AAC_ASC, len(asc)])
            row.check(st == K['HP_E_NOSESSION'], 'OPEN without a session: %d' % st)
            st, p = vp.open(320, 240)
            row.check(st == K['HP_OK'], 'video OPEN %d' % st)
            op, st, p = vp.call(K['HP_OP_AAC'], [vp.session, K['HP_AAC_DECODE'],
                                                 AAC_AU, 4, AAC_PCM])
            row.check(st == K['HP_E_STATE'], 'DECODE before OPEN: %d' % st)
            # an ASC outside the windows is refused
            op, st, p = vp.call(K['HP_OP_AAC'], [vp.session, K['HP_AAC_OPEN'],
                                                 0x100000, len(asc)])
            row.check(st == K['HP_E_INVAL'], 'OPEN on the DPB: %d' % st)
            b.host_sdram_write(AAC_ASC, asc)
            # flags 2: AAC_FLAG_TRACE, as stage 9 (vplayer.c) opens
            op, st, p = vp.call(K['HP_OP_AAC'], [vp.session, K['HP_AAC_OPEN'],
                                                 AAC_ASC, len(asc), 2], timeout_us=500_000)
            row.check(st == K['HP_OK'] and p[1] in (1, 2),
                      'AAC OPEN: status %d, reply %s' % (st, p))
            # dev42: the tables as placed, against the image file's bytes
            img = self.image(DEFS)[2]
            ok = len(p) >= 7 and p[4] + p[5] <= len(img)
            fw = 2166136261
            for i in range(p[4], p[4] + p[5], 4) if ok else ():
                fw = ((fw ^ int.from_bytes(img[i:i + 4], 'little')) * 16777619) \
                    & 0xffffffff
            row.check(ok and p[3] == fw and p[6] == fw,
                      'AAC OPEN: tables parked %08x, SDRAM %08x, file %08x' % (
                          p[3] if ok else 0, p[6] if ok else 0, fw))
            # dev42: byte and halfword stores in SRAM and SDRAM
            want = [K['HP_MEMPROBE_W%d' % i] for i in range(5)]
            op, st, p = vp.call(K['HP_OP_AAC'], [0, K['HP_AAC_MEMPROBE']])
            row.check(st == K['HP_OK'] and list(p[:10]) == want * 2,
                      'MEMPROBE: status %d, %s' % (st, ['%08x' % w for w in p]))
            # session 36c: stores then immediate loads, the decoder's copy
            # and clear, in SRAM and SDRAM: no mismatch in the model
            op, st, p = vp.call(K['HP_OP_AAC'], [0, K['HP_AAC_MEMSTRESS']],
                                timeout_us=20_000_000)
            row.check(st == K['HP_OK'] and len(p) >= 19 and not any(p[:18])
                      and p[18] == 1, 'MEMSTRESS: status %d, %s' % (st, list(p)))
            pcm_at = S.SDRAM_BACKING + AAC_PCM
            h_got = h_want = 2166136261
            exact, instr, us = 0, [], []
            for k, au in enumerate(aus[:AAC_AUS]):
                b.host_sdram_write(AAC_AU, au)
                s0 = b.sim.steps
                op, st, p = vp.call(K['HP_OP_AAC'], [vp.session, K['HP_AAC_DECODE'],
                                                     AAC_AU, len(au), AAC_PCM],
                                    timeout_us=2_000_000)
                n, rpcm = ref[k]
                got = bytes(b.bus.b[pcm_at:pcm_at + 4 * n]) if st == K['HP_OK'] else b''
                ok = st == K['HP_OK'] and p[0] == n and got == rpcm
                row.check(ok, 'AU %d: status %d, reply %s, %d frames want %d, PCM %s' % (
                    k, st, p, p[0] if p else -1, n, 'same' if got == rpcm else 'DIFFERS'))
                exact += ok
                h_got = aac_fnv(h_got, got)
                h_want = aac_fnv(h_want, rpcm)
                if n:
                    instr.append(b.sim.steps - s0)
                    us.append(p[1])
                # the parse's checkpoints (HP_AAC_STATE): recorded, and the
                # second ic_stream hashes as the first after its copy
                op, st, q = vp.call(K['HP_OP_AAC'], [0, K['HP_AAC_STATE'], 0])
                tags = dict((q[i], q[i + 1]) for i in range(1, len(q) - 1, 2))
                row.check(st == K['HP_OK'] and q[0] >= 10 and 1 in tags
                          and tags.get(3) == tags.get(4),
                          'AU %d: STATE status %d, %d pairs, ics %s/%s' % (
                              k, st, q[0] if q else -1, tags.get(3), tags.get(4)))
            row.check(h_got == h_want, 'running FNV-1a %08x, host %08x' % (h_got, h_want))
            # stage 9's byte-copy run: OPEN with AAC_FLAG_BYTE_COPY on a fresh
            # boot, as the device runs it (libfaad keeps state a re-OPEN does
            # not clear), against the host reference
            b2, hst2, vp2, prog2 = self.setup(1)
            st, p = vp2.open(320, 240)
            b2.host_sdram_write(AAC_ASC, asc)
            op, st, p = vp2.call(K['HP_OP_AAC'], [vp2.session, K['HP_AAC_OPEN'],
                                                   AAC_ASC, len(asc), 1], timeout_us=500_000)
            row.check(st == K['HP_OK'], 'AAC OPEN with flags 1: status %d' % st)
            pcm2 = S.SDRAM_BACKING + AAC_PCM
            for k, au in enumerate(aus[:3]):
                b2.host_sdram_write(AAC_AU, au)
                op, st, p = vp2.call(K['HP_OP_AAC'], [vp2.session, K['HP_AAC_DECODE'],
                                                       AAC_AU, len(au), AAC_PCM],
                                     timeout_us=4_000_000)
                n, rpcm = ref[k]
                got = bytes(b2.bus.b[pcm2:pcm2 + 4 * n]) if st == K['HP_OK'] else b''
                row.check(st == K['HP_OK'] and p[0] == n and got == rpcm,
                          'byte copies, AU %d: status %d, %d frames, PCM %s' % (
                              k, st, p[0] if p else -1,
                              'same' if got == rpcm else 'DIFFERS'))
            # a past-the-window PCM offset and RESET
            op, st, p = vp.call(K['HP_OP_AAC'], [vp.session, K['HP_AAC_DECODE'], AAC_AU,
                                                 4, 0x39F800])
            row.check(st == K['HP_E_INVAL'], 'PCM past the I/O window: %d' % st)
            op, st, p = vp.call(K['HP_OP_AAC'], [vp.session, K['HP_AAC_RESET']])
            row.check(st == K['HP_OK'], 'RESET %d' % st)
            vp.call(K['HP_OP_CLOSE'], [vp.session], timeout_us=100_000)
            self.results['aac'] = {'clip': AAC_CLIP, 'aus': AAC_AUS, 'exact': exact,
                                   'instr_per_au': instr, 'sim_us_per_au': us}
            if instr:
                self.log('AAC: %s, %d of %d AUs exact; %.0f k instructions per AU '
                         '(protocol and all), %.0f us simulated' % (
                             AAC_CLIP, exact, AAC_AUS, sum(instr) / len(instr) / 1e3,
                             sum(us) / len(us)))
        except (T.Fail, HP.Timeout) as e:
            row.check(False, str(e))

    def t_tone(self):
        """TONE (dev41, dev45: stage 8a-1): refused without its magic or with
        start bits outside Apple's 0x6209.  With no clocks on GPIO 3 the
        pins are never muxed and nothing plays.  With a frame clock modelled
        on GPIO 3 and the block reporting ready: GPIO 2/3 go to function 2
        first, GPIO 1 only after ready, the tone plays, and all three are
        inputs again after."""
        row = T.Row('TONE: magic and start bits checked; no clocks: pins never muxed; '
                    'clocks: 2/3 first, then 1, played, inputs after; PB-5 reads')
        self.rows.append(row)
        try:
            b, hst, vp, prog = self.setup(1)
            fsels = []
            b.bus.on_write[0x10002800] = lambda bus, a, v: fsels.append(v & 0xFC)
            op, st, p = vp.call(K['HP_OP_TONE'], [0, 100, 34, 0, 167393])
            row.check(st == K['HP_E_INVAL'], 'no magic: %d' % st)
            op, st, p = vp.call(K['HP_OP_TONE'], [K['HP_TONE_MAGIC'], 100, 34, 4, 167393])
            row.check(st == K['HP_E_INVAL'], 'layout 4 refused: %d' % st)
            op, st, p = vp.call(K['HP_OP_TONE'], [K['HP_TONE_MAGIC'], 100, 34, 0, 167393,
                                                  0x10000])
            row.check(st == K['HP_E_INVAL'], 'start bit outside 0x6209 refused: %d' % st)
            # no clocks: GPIO 3 never moves
            del fsels[:]
            op, st, p = vp.call(K['HP_OP_TONE'], [K['HP_TONE_MAGIC'], 100, 34, 2, 167393],
                                timeout_us=400_000)
            row.check(st == K['HP_OK'] and len(p) == 14, 'TONE status %d %s' % (st, p))
            if st == K['HP_OK'] and len(p) == 14:
                row.check(p[1] == 0 and p[2] == 0, 'no clocks: played %d ms' % p[2])
                row.check(p[7] == 0, 'no edges on GPIO 3: %d' % p[7])
            row.check(all(f == 0 for f in fsels), 'no clocks: GPIO 1-3 never muxed %s'
                      % ['%02x' % f for f in fsels])
            row.check((b.r32(0x10002800) & 0xFC) == 0, 'GPIO 1-3 inputs after')
            # a frame clock on GPIO 3 (a count by 8: bit 3 toggles every read) and
            # a ready block
            b.bus.rule('0x10002820+=0x8')
            b.bus.rule('0x10004800|=0x800000')
            # PB-5: the two audio clock words, read only
            b.bus.rule('0x10001C34=0x00000214')
            b.bus.rule('0x10001C38=0x000002C8')
            clkw = []
            for a in (0x10001C34, 0x10001C38):
                b.bus.on_write[a] = lambda bus, a, v: clkw.append(a)
            del fsels[:]
            op, st, p = vp.call(K['HP_OP_TONE'], [K['HP_TONE_MAGIC'], 100, 34, 2, 167393],
                                timeout_us=2_000_000)
            row.check(st == K['HP_OK'] and len(p) == 14, 'TONE (clocks) status %d %s' % (st, p))
            if st == K['HP_OK'] and len(p) == 14:
                row.check(p[7] > 0 and p[1] == 1 and p[2] == 100,
                          'clocks: edges %d, ready %d, played %d ms' % (p[7], p[1], p[2]))
                row.check(p[5] & 0xFC == 0xA8, 'GPIO 1-3 on function 2 while playing: %08x'
                          % p[5])
                row.check(p[4] & 0x6209 == 0x6209, 'Apple\'s start bits while playing: %08x'
                          % p[4])
                row.check(p[8:14] == [0x214, 0x2C8] * 3,
                          'PB-5: clock words before, while playing, after: %s'
                          % ['%x' % x for x in p[8:14]])
            row.check(not clkw, 'PB-5: the clock words never written')
            mux = [f for f in fsels if f]
            row.check(mux[:2] == [0xA0, 0xA8], 'clock pins first, then data: %s'
                      % ['%02x' % f for f in fsels])
            row.check((b.r32(0x10002800) & 0xFC) == 0, 'GPIO 1-3 inputs after (clocks)')
        except (T.Fail, HP.Timeout) as e:
            row.check(False, str(e))

    def t_overlay(self):
        """OVERLAY (dev35/dev39): a band of the host frame over the pictures,
        never overdrawn while they play, the last picture back when it goes."""
        row = T.Row('OVERLAY: the host band stays over 3 pictures, then the picture returns')
        self.rows.append(row)
        try:
            data = open(os.path.join(CORPUS, 'd_ipod_768k.264'), 'rb').read()
            ref = open(os.path.join(CORPUS, 'd_ipod_768k.yuv'), 'rb').read()
            aus = access_units(data)[:3]
            b, hst, vp, prog = self.setup(1)
            st, p = vp.open(320, 240)
            row.check(st == K['HP_OK'], 'OPEN %d' % st)
            y0, bh = 200, 24
            pat = [(x * 7 + r * 13 + 0x1234) & 0xFFFF for r in range(bh) for x in range(320)]
            for i in range(0, len(pat), 2):
                r, x = divmod(i, 320)
                b.w32(0xE0000 + 2 * ((y0 + r) * 320 + x), pat[i] | pat[i + 1] << 16)
            op, st, p = vp.call(K['HP_OP_OVERLAY'], [vp.session, y0 << 16, 320 | bh << 16],
                                timeout_us=100_000)
            row.check(st == K['HP_OK'], 'OVERLAY on: %d' % st)
            b.captures.clear()
            for k, au in enumerate(aus):
                while not vp.feed(au, k * FRAME_US, K['HP_FEED_EOS'] if k == len(aus) - 1 else 0):
                    b.run(20_000)
                    vp.drain()
            t0 = b.sim.steps
            while not any(e[0] == K['HP_EV_EOS'] for e in vp.events):
                b.run(20_000)
                vp.drain()
                if b.sim.steps - t0 > 60_000_000:
                    raise HP.Timeout('no EV_EOS')
            row.check(len(b.captures) >= 3, '%d panel runs' % len(b.captures))
            band = lambda fb: [fb[(y0 + r) * T.W + x] for r in range(bh) for x in range(320)]
            for i, (win, fb) in enumerate(b.captures):
                row.check(band(fb) == pat, 'run %d: the band was overdrawn' % i)
            fsize = 320 * 240 * 3 // 2
            last = len(aus) - 1
            yp, up, vq = R.split_yuv(ref[last * fsize:(last + 1) * fsize], 320, 240)
            want = R.convert(yp, up, vq, 320, 240)
            fb = b.captures[-1][1]
            row.check(fb[:y0 * T.W] == want[:y0 * T.W], 'the picture above the band')
            b.captures.clear()
            op, st, p = vp.call(K['HP_OP_OVERLAY'], [vp.session, 0, 0], timeout_us=100_000)
            row.check(st == K['HP_OK'], 'OVERLAY off: %d' % st)
            for _ in range(40):
                b.run(1_000_000)
            fb = b.captures[-1][1] if b.captures else None
            self.log('after OVERLAY off: %d panel runs, windows %s' % (
                len(b.captures), [w for w, _ in b.captures]))
            if fb is not None and fb != want:
                d = [j for j in range(T.W * T.H) if fb[j] != want[j]]
                row.check(False, 'after OVERLAY off: %d pixels differ, first (%d,%d) %04x want %04x' % (
                    len(d), d[0] % T.W, d[0] // T.W, fb[d[0]], want[d[0]]))
            else:
                row.check(fb is not None, 'the last picture back under the band')
            vp.call(K['HP_OP_CLOSE'], [vp.session], timeout_us=100_000)
        except (T.Fail, HP.Timeout) as e:
            row.check(False, str(e))

    def t_overlay_slide(self):
        """OVERLAY's slide (2026-10-15): the band comes up from its bottom
        edge and goes down and away by the image's clock, the picture put
        back under the rows it leaves (after the last picture: paused)."""
        row = T.Row('OVERLAY slide: up from the bottom, down and away, picture back')
        self.rows.append(row)
        try:
            data = open(os.path.join(CORPUS, 'd_ipod_768k.264'), 'rb').read()
            ref = open(os.path.join(CORPUS, 'd_ipod_768k.yuv'), 'rb').read()
            aus = access_units(data)[:2]
            b, hst, vp, prog = self.setup(1)
            st, p = vp.open(320, 240)
            row.check(st == K['HP_OK'], 'OPEN %d' % st)
            for k, au in enumerate(aus):
                while not vp.feed(au, k * FRAME_US, K['HP_FEED_EOS'] if k == len(aus) - 1 else 0):
                    b.run(20_000)
                    vp.drain()
            t0 = b.sim.steps
            while not any(e[0] == K['HP_EV_EOS'] for e in vp.events):
                b.run(20_000)
                vp.drain()
                if b.sim.steps - t0 > 60_000_000:
                    raise HP.Timeout('no EV_EOS')
            fsize = 320 * 240 * 3 // 2
            last = len(aus) - 1
            yp, up, vq = R.split_yuv(ref[last * fsize:(last + 1) * fsize], 320, 240)
            want = R.convert(yp, up, vq, 320, 240)
            y0, bh = 196, 44
            pat = [(x * 5 + r * 11 + 0x4321) & 0xFFFF for r in range(bh) for x in range(320)]
            for i in range(0, len(pat), 2):
                r, x = divmod(i, 320)
                b.w32(0xE0000 + 2 * ((y0 + r) * 320 + x), pat[i] | pat[i + 1] << 16)
            b.captures.clear()
            op, st, p = vp.call(K['HP_OP_OVERLAY'], [vp.session, y0 << 16, 320 | bh << 16,
                                                     K['HP_OVL_SLIDE_UP'] | 60],
                                timeout_us=100_000)
            row.check(st == K['HP_OK'], 'OVERLAY slide up: %d' % st)
            for _ in range(600):        # until the band lands (the clock's
                b.run(1_000_000)        # pace in the simulator is its own)
                if any(w[1] == y0 for w, _ in b.captures):
                    break
            ys = sorted({w[1] for w, _ in b.captures if w[1] >= y0})
            self.log('slide up: %d panel runs, band tops %s' % (
                len(b.captures), ys[:12]))
            row.check(len(ys) > 2, 'slide up: %d band positions' % len(ys))
            band = lambda fb: [fb[(y0 + r) * T.W + x] for r in range(bh) for x in range(320)]
            fb = b.captures[-1][1] if b.captures else None
            row.check(fb is not None and band(fb) == pat, 'slide up: the band ends up whole')
            b.captures.clear()
            op, st, p = vp.call(K['HP_OP_OVERLAY'], [vp.session, y0 << 16, 320 | bh << 16,
                                                     K['HP_OVL_SLIDE_DOWN'] | 60],
                                timeout_us=100_000)
            row.check(st == K['HP_OK'], 'OVERLAY slide down: %d' % st)
            for _ in range(600):        # until the whole picture goes out
                b.run(1_000_000)
                if b.captures and b.captures[-1][0] == (0, 0, 319, 239):
                    break
            n_runs = len(b.captures)
            fb = b.captures[-1][1] if b.captures else None
            self.log('slide down: %d panel runs' % n_runs)
            row.check(n_runs > 2, 'slide down: %d panel runs' % n_runs)
            if fb is not None and fb != want:
                d = [j for j in range(T.W * T.H) if fb[j] != want[j]]
                row.check(False, 'after the slide down: %d pixels differ, first (%d,%d) %04x want %04x' % (
                    len(d), d[0] % T.W, d[0] // T.W, fb[d[0]], want[d[0]]))
            else:
                row.check(fb is not None, 'the picture back after the slide down')
            vp.call(K['HP_OP_CLOSE'], [vp.session], timeout_us=100_000)
        except (T.Fail, HP.Timeout) as e:
            row.check(False, str(e))

    def fast_run(self, flags):
        """Two pictures of d_ipod_768k through OPEN flags, then CLOSE: the
        panel frames, and the clock/regulator/SDRAM-refresh writes and the
        bus-timing kHz seen at each point."""
        data = open(os.path.join(CORPUS, 'd_ipod_768k.264'), 'rb').read()
        aus = access_units(data)[:2]
        b, hst, vp, prog = self.setup(3)
        b.gate_clocks('reset', 'reset')
        vclk_timer(b)
        n0 = len(b.bus.log)
        st, self.last_open = vp.open(320, 240, None, flags)
        if st != K['HP_OK']:
            raise HP.Timeout('OPEN %d' % st)
        khz_open = b.r32(T.MB_TIMING_KHZ)
        b.captures.clear()
        for k, au in enumerate(aus):
            while not vp.feed(au, k * FRAME_US, K['HP_FEED_EOS'] if k == len(aus) - 1 else 0):
                b.run(20_000)
                vp.drain()
        t0 = b.sim.steps
        while not any(e[0] == K['HP_EV_EOS'] for e in vp.events):
            b.run(20_000)
            vp.drain()
            if b.sim.steps - t0 > 60_000_000:
                raise HP.Timeout('no EV_EOS')
        frames = [fb for _, fb in b.captures]
        n1 = len(b.bus.log)
        vp.call(K['HP_OP_CLOSE'], [vp.session], timeout_us=100_000)
        khz_close = b.r32(T.MB_TIMING_KHZ)
        w = [(a.seq, a.addr, a.value) for a in b.bus.log[n0:]
             if a.rw == 'W' and a.seq not in b.host_seqs and
             (a.addr in (0x10001400, 0x10001C28, 0x10003424, 0x10003428))]
        return frames, w, n1, khz_open, khz_close, b

    def t_fast_clock(self):
        """HP_OPEN_FAST_CLOCK (dev9): the session runs on PLL A, SDRAM is
        set up with the 150 MHz refresh after the switch, the bus timing
        is Apple's 150 MHz words; CLOSE lowers the refresh first, then
        the clock, then gives our words back.  The pictures are the same
        as without the flag."""
        row = T.Row('OPEN with FAST_CLOCK: 150 MHz session, refresh and timing follow')
        self.rows.append(row)
        try:
            own = K['HP_OPEN_OWN_DISPLAY']
            slow, ws, _, k0, _, _ = self.fast_run(own)
            fast, wf, n1, k1, k2, b = self.fast_run(own | K['HP_OPEN_FAST_CLOCK'])
            row.check(len(fast) == len(slow) >= 2 and fast == slow,
                      'frames differ with the flag (%d vs %d)' % (len(fast), len(slow)))
            row.check(not [x for x in ws if x[1] == 0x10001C28],
                      'no flag: the clock was switched')
            row.check(k0 == B.config_value('BCMFW_LCD_BUS_KHZ'), 'no flag: timing kHz %d' % k0)
            sel = [(s, v & 0xFFFF) for s, a, v in wf if a == 0x10001C28]
            ref = [(s, v >> 16) for s, a, v in wf if a == 0x10003424]
            cmd = [s for s, a, v in wf if a == 0x10003428]
            up = next((s for s, v in sel if v == 0x101), None)
            down = next((s for s, v in sel if v == 1 and up is not None and s > up), None)
            row.check(up is not None and cmd and up < cmd[0],
                      'the switch up before SDRAM\'s first command')
            row.check(any(v == 0x493 and up < s < (down or 1 << 62) for s, v in ref),
                      'no 150 MHz refresh while on the PLL: %s' % ref)
            slow_ref = [s for s, v in ref if v == 0xD2 and s > up]
            # (P19 PW4's clock_gov_set writes 27 MHz's again after it: the
            # same value)
            row.check(down is not None and any(s < down for s in slow_ref),
                      'CLOSE: refresh not lowered before the switch down (%s, %s)' % (
                          ref, sel))
            row.check(k1 == 0, 'session timing kHz %d, want 0 (Apple\'s 150 MHz words)' % k1)
            row.check(k2 == B.config_value('BCMFW_LCD_BUS_KHZ'),
                      'after CLOSE: timing kHz %d' % k2)
            row.check(b.bus.regs.get(0x10001C28, 1) & 0xFFFF == 1,
                      'after CLOSE: core word %08x, want the crystal' % b.bus.regs.get(0x10001C28, 1))
        except (T.Fail, HP.Timeout) as e:
            row.check(False, str(e))

    def t_frames_place(self):
        """dev11: a 320x240 stream decodes with its frames in SRAM (the
        arena after the context); HP_OPEN_SDRAM_DPB puts them in SDRAM,
        runs the memory test first (4 MB, no grid errors, the vector row
        both ways), and the pictures are the same."""
        row = T.Row('frames: SRAM at 320x240, SDRAM with the diagnostic flag')
        self.rows.append(row)
        try:
            own = K['HP_OPEN_OWN_DISPLAY']
            sram, _, _, _, _, b = self.fast_run(own)
            dpb = b.r32(0x1D0)
            row.check(0x20000 <= dpb < 0xCC000, 'frames at %08x, want SRAM' % dpb)
            sd, _, _, _, _, b = self.fast_run(own | K['HP_OPEN_SDRAM_DPB'])
            dpb = b.r32(0x1D0)
            row.check(dpb >= 0xFE000000, 'diagnostic: frames at %08x' % dpb)
            mt = self.last_open[5] if len(self.last_open) > 5 else 0
            row.check(mt == 0x4D400083, 'memory test word %08x' % mt)
            row.check(len(sram) == len(sd) >= 2 and sram == sd,
                      'pictures differ between SRAM and SDRAM frames')
        except (T.Fail, HP.Timeout) as e:
            row.check(False, str(e))

    def t_c_kernels(self):
        """dev14: OPEN's HP_OPEN_C_* bits run decoder families and the
        display conversion in scalar C; the pictures are the same as the
        vectors'.  The guard block reports no damage to the VP header and
        records the first picture's planes."""
        row = T.Row('kernels: scalar C per OPEN, same pictures; the guard')
        self.rows.append(row)
        try:
            own = K['HP_OPEN_OWN_DISPLAY']
            vec, _, _, _, _, b = self.fast_run(own)
            g = [b.r32(0xDF000 + 4 * k) for k in range(16)]
            row.check(g[0] == 0x47554152 and g[1] == 0 and g[15] == 0,
                      'guard %s' % ['%08x' % x for x in g[:3]])
            row.check(0x20000 <= g[8] < 0xCC000 and g[9] == g[8] + 300 * 256
                      and g[10] == g[8] + 300 * 320 and g[11] == 320,
                      'planes %s' % ['%08x' % x for x in g[8:12]])
            row.check(g[14] > 0, 'no checks ran')
            _, _, _, _, _, b = self.fast_run(own | K['HP_OPEN_SELFTEST'])
            st = [b.r32(0xDF040 + 4 * k) for k in range(5)]
            # dev21: the VRF dumps, kept as the reference for the device's
            vrf = bytes(b.bus.b[S.SDRAM_BACKING + 0x3D0000:
                                S.SDRAM_BACKING + 0x3D0000 + 11 * 2048])
            with open(os.path.join(self.args.png, 'vrf_sim.bin'), 'wb') as f:
                f.write(vrf)
            # the transform (test 0) leaves coefficient rows 1-3 in H16
            # rows 1-3, lanes 0-3 (selftest.c's first coefficients)
            seed = 2722
            def rnd():
                nonlocal seed
                seed = (seed * 1664525 + 1013904223) & 0xFFFFFFFF
                return seed >> 24
            for _ in range(1536):
                rnd()
            co = [(rnd() & 127) - 64 for _ in range(16)]
            rows = [[int.from_bytes(vrf[32 * y + 2 * i:32 * y + 2 * i + 2], 'little',
                                    signed=True) for i in range(4)] for y in (1, 2, 3)]
            row.check(b.r32(0xDF0BC) == 0x53430000,
                      'clean self-test %08x' % b.r32(0xDF0BC))
            row.check(rows == [co[4:8], co[8:12], co[12:16]],
                      'VRF dump rows 1-3 %s, coefficients %s' % (rows, co[4:]))
            alu = b.r32(0xDF090)
            row.check(alu == 0x414C0000, 'scalar test %08x (%d, %08x)' %
                      (alu, b.r32(0xDF094), b.r32(0xDF098)))

            def fnv(data):
                h = 0x811C9DC5
                for c in data:
                    h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
                return h
            yuv = open(os.path.join(CORPUS, 'd_ipod_768k.yuv'), 'rb').read()
            want = []
            for f in range(2):
                o = f * 115200
                want += [fnv(yuv[o:o + 76800]), fnv(yuv[o + 76800:o + 96000]),
                         fnv(yuv[o + 96000:o + 115200])]
            got = [b.r32(0xDF0A0 + 4 * k) for k in range(6)]
            _, _, _, _, _, b = self.fast_run(own | K['HP_OPEN_DUMP'])
            dumped = bytes(b.bus.b[S.SDRAM_BACKING + 0x3A0000:
                                   S.SDRAM_BACKING + 0x3A0000 + 115200])
            row.check(b.r32(0xDF09C) == 0x44000001 and dumped == yuv[:115200],
                      'dump flags %08x; picture %s' % (
                          b.r32(0xDF09C),
                          'right' if dumped == yuv[:115200] else
                          'differs from byte %d (%s vs %s)' % (
                              next(i for i in range(115200)
                                   if dumped[i] != yuv[i]),
                              dumped[next(i for i in range(115200) if dumped[i] != yuv[i]):][:8].hex(),
                              yuv[next(i for i in range(115200) if dumped[i] != yuv[i]):][:8].hex())))
            row.check(got == want, 'picture fingerprints %s, ffmpeg %s' %
                      (['%08x' % x for x in got], ['%08x' % x for x in want]))
            row.check(st[0] == 0x53540000 and st[4] >= 20 and
                      b.r32(0xDF004) == 0 and b.r32(0xDF054) == 0,
                      'self-test %s, guard %08x' % (['%08x' % x for x in st],
                                                    b.r32(0xDF004)))
            for bits in (K['HP_OPEN_C_MASK'], K['HP_OPEN_C_DISP'],
                         0x1 << 12, 0x2 << 12, 0x4 << 12, 0x8 << 12):
                pics, _, _, _, _, b = self.fast_run(own | bits)
                row.check(len(pics) == len(vec) >= 2 and pics == vec,
                          'C bits %05x: pictures differ' % bits)
                row.check(b.r32(0xDF030) == bits and b.r32(0xDF004) == 0,
                          'C bits %05x: guard %08x %08x' %
                          (bits, b.r32(0xDF030), b.r32(0xDF004)))
        except (T.Fail, HP.Timeout) as e:
            row.check(False, str(e))

    def t_sdram_dead(self):
        """dev10 (session 9's NOMEM): SDRAM that never answers.  OPEN with
        FAST_CLOCK makes four attempts (fields 8, 4, 8, 4), answers
        HP_E_NOMEM with the four SDRAM words, and leaves the core back
        on the crystal for an OPEN without the flag."""
        row = T.Row('OPEN, SDRAM dead: NOMEM with its words, back at 27 MHz')
        self.rows.append(row)
        try:
            b, h, vp, prog = self.setup(0)
            vclk_timer(b)
            b.bus.aliases = [a for a in b.bus.aliases if a[0] != SDRAM_BCM]
            b.bus.rule('0x10003420&=0x7fff')         # bit 15 never set
            b.bus.rule('0xfe000000=0x87654321')      # the bus shows B
            n0 = len(b.bus.log)
            st, p = vp.open(320, 240, None,
                            K['HP_OPEN_OWN_DISPLAY'] | K['HP_OPEN_FAST_CLOCK'])
            row.check(st == K['HP_E_NOMEM'] and len(p) == 5 and p[4] == 0,
                      'open %d %s' % (st, p))
            if len(p) == 5:
                row.check(p[0] >> 12 == 0x53441 and (p[0] >> 8 & 0xF) == 4 and
                          (p[0] >> 4 & 0xF) == 0 and p[0] & 2,
                          'word 0 %08x: want step 1, 4 attempts, none seen, PLL' % p[0])
                row.check(p[2] == 0x87654321 and not p[1] & 0x8000,
                          'words 1-3 %s' % ['%08x' % x for x in p[1:]])
            modes = [a.value for a in b.bus.log[n0:]
                     if a.rw == 'W' and a.addr == 0x10003428]
            row.check(modes == [0x3057C834, 0x3057C434] * 2,
                      'mode words %s' % ['%08x' % x for x in modes])
            row.check(b.bus.regs.get(0x10001C28, 1) & 0xFFFF == 1,
                      'after NOMEM: core word %08x, want the crystal' %
                      b.bus.regs.get(0x10001C28, 1))
        except (T.Fail, HP.Timeout) as e:
            row.check(False, str(e))

    def t_open_errors(self):
        row = T.Row('OPEN: FILL refused; LCD contract around a session; caps')
        self.rows.append(row)
        try:
            b, h, vp, prog = self.setup(0)
            row.check(vp.caps & K['HP_CAP_H264_BP'] and vp.caps & K['HP_CAP_LCD_DMA'],
                      'caps %x' % vp.caps)
            row.check(vp.fifo_base == SDRAM_HOST and vp.fifo_size == 0x100000,
                      'FIFO %x/%x' % (vp.fifo_base, vp.fifo_size))
            st, _ = vp.open(320, 240, K['HP_MODE_FILL'])
            row.check(st == K['HP_E_UNSUPPORTED'], 'FILL: %d' % st)
            st, p = vp.open(320, 240)
            row.check(st == K['HP_OK'] and len(p) == 6 and p[3] == 16 and
                      p[4] >> 8 == 0x534401 and p[5] == 0, 'open %d %s' % (st, p))
            # a session open: the host's LCD_UPDATE is dropped (own display)
            h.tick_task = True
            h.fb = T.random_frame(self.rng)
            h.lcd_update()
            h.settle()
            h.tick_task = False
            row.check(b.panel.fb != h.fb, 'LCD_UPDATE drew during an own-display session')
            _, st, cp = vp.call(K['HP_OP_CLOSE'], [vp.session], timeout_us=100_000)
            row.check(st == K['HP_OK'] and cp[2:] == [0, 0, 0], 'close %d %s' % (st, cp))
            # after CLOSE the UI repaints
            h.tick_task = True
            h.lcd_update()
            h.settle()
            h.tick_task = False
            self.frame_check(row, b, h, 'video_after_close')
        except (T.Fail, HP.Timeout) as e:
            row.check(False, str(e))

    def t_robust(self):
        """Fuzzed, truncated and garbage streams through the protocol:
        bounded progress, STATUS answers, heartbeat, no fault."""
        row = T.Row('robustness: fuzzed/truncated streams, bounded progress')
        self.rows.append(row)
        rng = random.Random(self.args.seed + 1)
        base = {}
        for clip in ('d_ipod_768k', 'g_crop', 'e_vga', 'f_slices'):
            base[clip] = access_units(open(os.path.join(CORPUS, clip + '.264'), 'rb').read())
        worst = 0
        stats = collections.Counter()
        try:
            for case in range(self.args.fuzz):
                clip = rng.choice(sorted(base))
                aus = [bytearray(a) for a in base[clip][:rng.randrange(2, 7)]]
                kind = rng.choice(('flip', 'flip', 'bytes', 'truncate', 'garbage', 'drop', 'zeros'))
                if kind == 'flip':
                    for _ in range(rng.randrange(1, 12)):
                        a = rng.choice(aus)
                        i = rng.randrange(len(a))
                        a[i] ^= 1 << rng.randrange(8)
                elif kind == 'bytes':
                    for _ in range(rng.randrange(1, 6)):
                        a = rng.choice(aus)
                        i = rng.randrange(len(a))
                        a[i:i + 4] = bytes(rng.getrandbits(8) for _ in range(4))
                elif kind == 'truncate':
                    k = rng.randrange(len(aus))
                    aus[k] = aus[k][:rng.randrange(1, len(aus[k]) + 1)]
                    aus = aus[:k + 1]
                elif kind == 'garbage':
                    aus.insert(rng.randrange(len(aus) + 1),
                               bytearray(rng.getrandbits(8) for _ in range(rng.randrange(1, 3000))))
                elif kind == 'drop':
                    del aus[rng.randrange(1, len(aus))]
                else:
                    a = rng.choice(aus)
                    i = rng.randrange(len(a))
                    a[i:i + 64] = bytes(64)
                stats[kind] += 1
                w, h = sps_size(clip)
                b, hst, vp, prog = self.setup(rng.choice((0, 1, 2, 3)))
                st, _ = vp.open(w, h)
                if st != K['HP_OK']:
                    row.check(False, 'case %d: OPEN %d' % (case, st))
                    continue
                s0 = b.sim.steps
                # every packet must be consumed: at most 25 M instructions each
                bound = 25_000_000 * (len(aus) + 1)
                for k, au in enumerate(aus):
                    fl = K['HP_FEED_EOS'] if k == len(aus) - 1 else 0
                    while not vp.feed(bytes(au) or b'\0', k * FRAME_US, fl):
                        b.run(20_000)
                        vp.drain()
                        if b.sim.steps - s0 > bound:
                            break
                beat0 = b.r32(K['HP_STATE']) >> 8 & 0xff
                ok = True
                while not any(e[0] == K['HP_EV_EOS'] for e in vp.events):
                    b.run(20_000)
                    vp.drain()
                    if b.sim.steps - s0 > bound:
                        ok = False
                        break
                used = b.sim.steps - s0
                # video_work raises EV_EOS before hp_work advances FIFO_RD
                # (the same pass): a slice can end between the two, so the
                # host gives the pass time to finish before it reads FIFO_RD
                # (P19 PW4's busy meter moved the slices' edges onto that gap)
                b.run(20_000)
                vp.drain()
                worst = max(worst, used / len(aus))
                row.check(ok, 'case %d (%s, %s): no EV_EOS within %d M instructions' % (
                    case, clip, kind, bound // 1_000_000))
                rd = vp.index(K['HP_FIFO_RD'])
                want = sum((len(a) or 1) + 63 for a in aus) // 64 if False else \
                    sum(((len(a) or 1) + 63) // 64 for a in aus)
                row.check(rd == want & 0xffff, 'case %d (%s): FIFO_RD %s, want %d' % (
                    case, kind, rd, want))
                _, st, sp = vp.call(K['HP_OP_STATUS'], [vp.session])
                row.check(st == K['HP_OK'], 'case %d: STATUS %d' % (case, st))
                stats['errors_reported'] += sp[8] > 0
                stats['fatal'] += sp[0] == K['HP_ST_ERROR']
                stats['frames_shown'] += sp[7]
                beat1 = b.r32(K['HP_STATE']) >> 8 & 0xff
                row.check(beat1 != beat0 or used < 1000, 'case %d: heartbeat stopped' % case)
                row.check(b.r32(T.CMD_WORD) != T.FAULT, 'case %d (%s): fault at %08x' % (
                    case, kind, b.r32(T.MB_FAULT_PC)))
                _, cst, _ = vp.call(K['HP_OP_CLOSE'], [vp.session], timeout_us=100_000)
                row.check(cst == K['HP_OK'], 'case %d: CLOSE %d' % (case, cst))
        except (T.Fail, HP.Timeout) as e:
            row.check(False, str(e))
        self.results['robustness'] = {'cases': self.args.fuzz, 'kinds': dict(stats),
                                      'worst_instr_per_packet': worst}

    def run(self):
        t0 = time.time()
        tests = (self.t_open_errors, self.t_sdram_dead, self.t_frames_place,
                 self.t_c_kernels,
                 self.t_fast_clock,
                 self.t_clips, self.t_native, self.t_overlay, self.t_overlay_slide, self.t_audio, self.t_tone, self.t_aac,
                 self.t_robust)
        for t in tests:
            if self.args.only and self.args.only not in t.__name__:
                continue
            n0 = len(self.rows)
            t()
            for row in self.rows[n0:]:
                print('%-72s %5d %5d' % (row.name[:72], row.cases,
                                         row.cases - len(row.fails)))
                for f in row.fails[:8]:
                    print('    FAIL: %s' % f)
        prog, rep, img = self.image(DEFS)
        print()
        print(rep['lines'][0])
        print(rep['lines'][1])
        self.report()
        if self.args.json:
            with open(self.args.json, 'w') as f:
                json.dump({'image': rep['sizes'], 'load': rep['load'], 'end': rep['end'],
                           'results': self.results}, f, indent=1)
        cases = sum(r.cases for r in self.rows)
        fails = sum(len(r.fails) for r in self.rows)
        print('%d rows, %d cases, %d failed (seed %d, %.0f s)' % (
            len(self.rows), cases, fails, self.args.seed, time.time() - t0))
        print('VIDEO END TO END %s' % ('PASSED' if not fails else 'FAILED'))
        return 1 if fails else 0

    def report(self):
        for clip, res in self.results.items():
            if clip == 'robustness':
                print('robustness: %d cases %s; worst %.1f M instructions per packet' % (
                    res['cases'], res['kinds'], res['worst_instr_per_packet'] / 1e6))
                continue
            if clip == 'aac':
                ins = res['instr_per_au']
                if ins:
                    print('aac: %s, %d of %d AUs byte-exact; %.0f k instructions per AU '
                          '(max %.0f k) with the protocol' % (
                              res['clip'], res['exact'], res['aus'],
                              sum(ins) / len(ins) / 1e3, max(ins) / 1e3))
                continue
            p = res['profile']
            if not p:
                continue
            print('%s (%dx%d, %d frames, %d pixel-exact): per frame, k instructions '
                  '(mean / max)' % (os.path.basename(clip), res['size'][0], res['size'][1], res['frames'],
                                    res['exact']))
            print('  ' + ', '.join('%s %.0f/%.0f' % (s, p[s]['mean'] / 1e3, p[s]['max'] / 1e3)
                                   for s in STAGES))
            print('  work (all but poll) %.2f M (max %.2f M) = %.0f%% of the 5.0 M budget; '
                  'cycle estimate %.2f M (max %.2f M); convert %.0f k, scale %.0f k cycles' % (
                      p['work']['mean'] / 1e6, p['work']['max'] / 1e6,
                      100.0 * p['work']['mean'] / BUDGET, p['cycles_est']['mean'] / 1e6,
                      p['cycles_est']['max'] / 1e6, p['convert_cycles_est'] / 1e3,
                      p['scale_cycles_est'] / 1e3))
            top = sorted(range(len(p['work_per_frame'])), key=lambda i: -p['work_per_frame'][i])[:3]
            print('  costliest frames (index from the 2nd present): %s; %d over 5.0 M cycles (estimate)' % (
                ', '.join('%d: %.2f M' % (i, p['work_per_frame'][i] / 1e6) for i in top),
                p['over_budget']))
            print('  host: %d bytes fed, %.0f host-port transfers per frame; %.1f ms '
                  'simulated per frame' % (res['feed_bytes'], res['feed_xfers'] / res['frames'],
                                           res['sim_us'] / 1e3 / res['frames']))


# AAC (t_aac): a clip of bcmfw/aac/test's set, its host reference
# (run.sh --host writes build/aac/NAME.s16), and where the test puts the
# ASC, the access unit and the PCM: video.h VIDEO_AAC_IO, as the device
# test does (apps/video/vplayer.c)
AAC_OUT = os.path.join(T.ROOT, 'build', 'aac')
AAC_CLIP = 'sine_st128'
AAC_AUS = 6
AAC_ASC, AAC_AU, AAC_PCM = 0x39C000, 0x39C800, 0x39D000


def aac_probe_cases():
    """(a, b) per case, from bcmfw/aac/aac_image.c's table (one source)."""
    text = open(os.path.join(T.FW, 'aac', 'aac_image.c')).read()
    body = text[text.index('probe_cases['):]
    body = body[:body.index('};')]
    return [(int(a, 0), int(b, 0)) for a, b in
            re.findall(r'\{\s*(0x[0-9A-Fa-f]+|\d+),\s*(0x[0-9A-Fa-f]+|\d+)\s*\}', body)]


AAC_PROBE = aac_probe_cases()


def aac_probe_want():
    """The taken bits by vc2sim's model and C's arithmetic: bcs when a < b
    (unsigned; cmp's C is a borrow), bcc otherwise.  Must equal
    aac_image.h's AAC_PROBE_WANT, which the image reports beside it."""
    w = 0
    for k, (a, b) in enumerate(AAC_PROBE):
        w |= (1 << k) if a < b else (1 << (16 + k))
    hdr = open(os.path.join(T.FW, 'aac', 'aac_image.h')).read()
    m = re.search(r'#define\s+AAC_PROBE_WANT\s+(0x[0-9A-Fa-f]+)', hdr)
    if not m or int(m.group(1), 0) != w:
        raise T.Fail('aac_image.h AAC_PROBE_WANT is not %08x' % w)
    return w


def aac_fnv(h, data):
    for c in data:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def aac_reference(clip):
    """[(frames, PCM bytes)] per AU from the host build, made if missing."""
    import struct
    import subprocess
    raw = os.path.join(AAC_OUT, clip + '.raw')
    if not os.path.exists(raw):
        subprocess.run([os.path.join(T.FW, 'aac', 'test', 'run.sh'), '--host'],
                       check=False, stdout=subprocess.DEVNULL)
    if not os.path.exists(raw):
        return None
    r = open(raw, 'rb').read()
    pcm = open(os.path.join(AAC_OUT, clip + '.s16'), 'rb').read()
    rec, out, p = 4 + 8192, [], 0
    for k in range(len(r) // rec):
        n = struct.unpack_from('<I', r, k * rec)[0]
        out.append((n, pcm[p:p + 4 * n]))
        p += 4 * n
    return out


def clip_path(clip):
    """A corpus name, or a path to a .264 with its .yuv next to it (e.g. an
    owner-supplied excerpt under build/, never committed)."""
    return clip if clip.endswith('.264') else os.path.join(CORPUS, clip + '.264')


def sps_size(clip):
    known = {'e_vga': (640, 480), 'g_crop': (176, 120)}
    if clip in known or not clip.endswith('.264'):
        return known.get(clip, (320, 240))
    import subprocess
    out = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                          '-show_entries', 'stream=width,height', '-of', 'csv=p=0', clip],
                         capture_output=True, text=True, check=True).stdout
    w, h = out.strip().split(',')[:2]
    return int(w), int(h)


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--seed', type=int, default=2722)
    ap.add_argument('--frames', type=int, default=0,
                    help='iPod clip: the first N access units only (0: all 60)')
    ap.add_argument('--clips', default='d_ipod_768k,a_ionly_qp32,e_vga,g_crop',
                    help='corpus names, or paths to .264 files with a .yuv beside')
    ap.add_argument('--max-frames', type=int, default=0,
                    help='other clips: the first N access units only')
    ap.add_argument('--fuzz', type=int, default=12)
    ap.add_argument('--json')
    ap.add_argument('--only', default='')
    ap.add_argument('--png', default=os.path.join(T.ROOT, 'build', 'bcmfw', 'test-video'))
    ap.add_argument('--vmcs', default=T.DEFAULT_VMCS)
    ap.add_argument('-D', dest='defines', action='append', default=[],
                    help='more image defines (BCMFW_JOBS=1: the merged image)')
    a = ap.parse_args(argv)
    if not B.have_gcc():
        print('skip: vc4-elf-gcc not found at %s (set VC4_GCC)' % C.GCC)
        return 0
    if not os.path.exists(os.path.join(CORPUS, 'd_ipod_768k.264')):
        print('video_test: corpus missing: run tests/video/make_corpus.sh')
        return 1
    global DEFS
    DEFS = DEFS + tuple(a.defines)
    return Suite(a).run()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
