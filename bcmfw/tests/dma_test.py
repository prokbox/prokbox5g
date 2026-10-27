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
"""bcmfw's DMA frame push (BCMFW_LCD_DMA=1) in the simulator.

Same host model and panel as hello_test.py, plus vc2mmio.DmaEngine: the
engine reads control blocks and SRAM and feeds the LCD interface's FIFO
(+0x0C), paced by the interface's request line while an automatic pixel
run is armed.  Checks: pixel-exact full frames and rectangles for every
panel ID, the power-on traffic unchanged, frames read only from the
framebuffer in internal SRAM (nothing in SDRAM), the CPU fallback for
odd rectangles and after a DMA timeout, and instructions per frame
against the CPU push.

Usage:
  bcmfw/tests/dma_test.py [-v] [--png DIR] [--vmcs PATH] [--seed N] [--only NAME]

Skips cleanly (exit 0, message) when vc4-elf-gcc is not installed.
"""
import collections
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hello_test as T   # noqa: E402

S, M, B = T.S, T.M, T.B
W, H = T.W, T.H
DMA = ('BCMFW_LCD_DMA=1',)
INFO_DMA, INFO_DMA_OFF = 0x0800, 0x1000
FB_LO, FB_HI = T.FB, T.FB + 2 * W * H
LCD_CH_CS = 0x10000800 + 0x80 * 1
MB_DMA_CS, MB_DMA_INT = 0x1B0, 0x1B4
MB_DMA_RUN0, MB_DMA_RUN1 = 0x198, 0x19C
MARK = 8                                # BCMFW_DMA_MARK rows after a timeout
# the core clock (config.h, dev9)
MB_CLOCK_STATE, MB_CLOCK_REQ = 0x140, 0x168
CLOCK_MAGIC, CLOCK_REQ_MAGIC = 0x434B0000, 0x434C4B00
CMD_CLOCK = 0x41


class DmaBench(T.Bench):
    def __init__(self, *a, paced_wpu=5, **k):
        super().__init__(*a, **k)
        self.dma = self.sim.mem.attach(M.DmaEngine(paced_wpu=paced_wpu))
        self.dma.attach_dreq(3, self.panel.dreq)

    def power_cut(self, rng):
        super().power_cut(rng)
        d = self.dma
        d.cs, d.status, d.subs = [0] * 8, [0] * 8, {}
        d.finished = [False] * 8


class Suite(T.Suite):
    def __init__(self, args):
        super().__init__(args)
        self.paced_wpu = 5

    def bench(self, pid, defines=DMA, reset_pc=None):
        prog, rep, img = self.image(defines)
        entry = B.config_value('BCMFW_ENTRY', defines)
        b = DmaBench(img, pid, entry, reset_pc, paced_wpu=self.paced_wpu)
        h = T.Host(b, T.random.Random(self.rng.getrandbits(32)))
        return b, h, prog

    def dma_check(self, row, b, tag):
        d, p = b.dma, b.panel
        row.check(not d.errors, '%s: DMA errors %s' % (tag, d.errors[:3]))
        row.check(not d.outside, '%s: DMA outside SRAM/peripherals: %s' % (tag, d.outside[:3]))
        pages = sorted(d.sources_touched)
        bad = [a for a in pages if not FB_LO <= a < FB_HI]
        row.check(not bad, '%s: DMA read pages outside the framebuffer: %s' % (
            tag, ['%x' % a for a in bad[:4]]))
        row.check(p.fifo_stray == 0, '%s: %d FIFO words with no run' % (tag, p.fifo_stray))
        row.check(p.run_left == 0, '%s: run left %d transfers short' % (tag, p.run_left))
        sd = [a for a in b.bus.log if a.addr >= 0xC0000000]
        row.check(not sd, '%s: SDRAM accessed (%d)' % (tag, len(sd)))

    # ---- rows ---------------------------------------------------------

    def t_dma_full(self):
        for pid in range(4):
            for model in ((1, 2) if pid in (1, 3) else (1,)):
                defs = DMA + (('BCMFW_PANEL_MODEL=2',) if model == 2 else ())
                row = T.Row('DMA: boot + first LCD_UPDATE, panel %d%s' % (
                    pid, ' model 2' if model == 2 else ''))
                self.rows.append(row)
                b, h, prog = self.bench(pid, defs)
                h.fb = T.random_frame(self.rng)
                s0 = b.sim.steps
                try:
                    h.lcd_awake()
                    h.settle()
                except T.Fail as e:
                    row.check(False, str(e))
                    continue
                row.instr = b.sim.steps - s0
                info = b.r32(T.MB_INFO)
                row.check(info & (INFO_DMA | INFO_DMA_OFF) == INFO_DMA and
                          (info & 3) == pid, 'info word %08x' % info)
                vw = b.vpu_device_writes()
                blocks = sorted({a.addr & ~0x3ff for a in vw})
                row.check(set(blocks) <= {0x10000800, 0x10001000, 0x10001800 & ~0x3ff,
                                          0x10002800, 0x10004000},
                          'core wrote outside DMA/interrupts/GPIO/LCD: %s' % [
                              '%08x' % x for x in blocks])
                row.check(b.dma.blocks == 1, '%d control blocks for one frame' % b.dma.blocks)
                runs = b.panel.runs
                row.check(runs == [(W * H, 0x1F, 0x329)],
                          'LCD runs %s, want one burst run of 76800' % runs)
                row.check(h.rekicks == 0, '%d re-kicks' % h.rekicks)
                tag = 'dma_full_p%d%s' % (pid, '_m2' if model == 2 else '')
                self.compare_on(row, pid, model, b.panel.events, tag)
                self.frame_check(row, b, h, tag, png=True)
                self.dma_check(row, b, tag)
                self.done_check(row, b, tag, W * H)

    def done_check(self, row, b, tag, n):
        """The image's report: Apple's LCD completion bit ended the wait
        (session 4's 0x2300 and state 2), and the run-length readback."""
        cs, st = b.r32(MB_DMA_CS), b.r32(MB_DMA_INT)
        row.check(st & 0x100 and st & 0x2000, '%s: status %08x lacks bits 8/13' % (tag, st))
        row.check(cs >> 29 == 2, '%s: channel state %d, want 2 (cs %08x)' % (tag, cs >> 29, cs))
        r0, r1 = b.r32(MB_DMA_RUN0), b.r32(MB_DMA_RUN1)
        # device session 5: the run length counts up to n; the control
        # reads 0x325 while the run goes, 0x323 once it is out
        row.check(r1 & 0xFFFFF == n and r0 & 0xFFFFF < n,
                  '%s: run readback %08x then %08x for %d transfers' % (tag, r0, r1, n))
        row.check(r0 >> 20 in (0x325, 0x323) and r1 >> 20 == 0x323,
                  '%s: control read %03x/%03x, want 325/323' % (tag, r0 >> 20, r1 >> 20))

    def t_dma_rects(self):
        rects = [(0, 0, 1, 1), (319, 239, 1, 1), (317, 5, 3, 7), (13, 17, 51, 33),
                 (0, 100, 320, 3), (300, 200, 40, 60), (1, 0, 318, 240), (159, 119, 2, 2)]
        for pid in range(4):
            row = T.Row('DMA: lcd_update_rect (host path), panel %d' % pid)
            self.rows.append(row)
            b, h, _ = self.bench(pid)
            h.fb = T.random_frame(self.rng)
            try:
                h.lcd_awake()
                h.settle()
                for (x, y, w, hh) in rects:
                    for yy in range(y, min(H, y + hh)):
                        for xx in range(x, min(W, x + w)):
                            h.fb[yy * W + xx] = self.rng.getrandbits(16)
                    s0 = b.sim.steps
                    h.lcd_update_rect(x, y, w, hh)
                    h.settle()
                    row.instr += b.sim.steps - s0
                    tag = 'dma_rect_p%d_%d_%d_%d_%d' % (pid, x, y, w, hh)
                    self.frame_check(row, b, h, tag, png=(pid == 2 and (x, y) == (13, 17)))
                # the host sends full frames here (lcd-video.c pushes the whole
                # screen per kick), all of them by DMA
                row.check(b.dma.blocks == 1 + len(rects), '%d blocks for %d frames' % (
                    b.dma.blocks, 1 + len(rects)))
                self.dma_check(row, b, 'dma_rects_p%d' % pid)
            except T.Fail as e:
                row.check(False, str(e))

    def t_dma_updaterect(self):
        # (x, y, w, h, by DMA?): odd x or width falls back to the CPU
        rects = [(0, 0, 2, 1, 1), (318, 239, 2, 1, 1), (316, 5, 4, 7, 1),
                 (12, 17, 52, 33, 1), (0, 100, 320, 3, 1), (300, 200, 40, 60, 1),
                 (13, 17, 51, 33, 0), (1, 1, 2, 2, 0), (5, 3, 0, 9, 0), (400, 10, 5, 5, 0)]
        for pid in range(4):
            row = T.Row('DMA: LCD_UPDATERECT 2-D rects, odd ones by CPU, panel %d' % pid)
            self.rows.append(row)
            b, h, _ = self.bench(pid)
            h.fb = T.random_frame(self.rng)
            try:
                h.lcd_awake()
                h.settle()
                h.tick_task = False
                for (x, y, w, hh, by_dma) in rects:
                    for yy in range(y, min(H, y + hh)):
                        for xx in range(x, min(W, x + w)):
                            h.fb[yy * W + xx] = self.rng.getrandbits(16)
                    for yy in range(y, min(H, y + hh)):
                        if x < W:
                            h.write_fb(x, yy, min(w, W - x))
                    b.w32(T.MB_RECT_XY, x | y << 16)
                    b.w32(T.MB_RECT_WH, w | hh << 16)
                    n0, k0, s0 = b.panel.pixels_total, b.dma.blocks, b.sim.steps
                    h.bcm_command(T.LCD_UPDATERECT)
                    row.instr += b.sim.steps - s0
                    cw = max(0, min(w, W - x)) if x < W and y < H else 0
                    ch = max(0, min(hh, H - y)) if x < W and y < H else 0
                    got = b.panel.pixels_total - n0
                    row.check(got == cw * ch, 'rect %s: %d pixels sent, want %d' % (
                        (x, y, w, hh), got, cw * ch))
                    row.check(b.dma.blocks - k0 == by_dma, 'rect %s: %d DMA blocks, want %d' % (
                        (x, y, w, hh), b.dma.blocks - k0, by_dma))
                    self.frame_check(row, b, h, 'dma_urect_p%d_%d_%d_%d_%d' % (pid, x, y, w, hh))
                    if by_dma:
                        st, r1 = b.r32(MB_DMA_INT), b.r32(MB_DMA_RUN1)
                        n = cw * ch      # session 5: the count ends at the run length
                        row.check(st & 0x100 and r1 & 0xFFFFF == n,
                                  'rect %s: status %08x, run count %x, want %x' % (
                                      (x, y, w, hh), st, r1, n))
                self.dma_check(row, b, 'dma_urect_p%d' % pid)
            except T.Fail as e:
                row.check(False, str(e))

    def t_dma_timeout(self):
        row = T.Row('DMA: dead request line -> timeout, CPU re-send, DMA stays off')
        self.rows.append(row)
        b, h, _ = self.bench(1)
        h.fb = T.random_frame(self.rng)
        try:
            h.lcd_awake()
            h.settle()
            b.panel.dreq_enabled = False
            h.fb = T.random_frame(self.rng)
            k0 = b.dma.blocks
            # one command, no host re-kick (lcd-video.c's after 50 ms would
            # redraw the frame by CPU before the mark could be seen)
            h.tick_task = False
            for y in range(H):
                h.write_fb(0, y, W)
            h.bcm_command(T.LCD_UPDATE)
            # the CPU re-send, then the mark: the top rows black
            want = h.fb
            h.fb = [0] * (W * MARK) + want[W * MARK:]
            self.frame_check(row, b, h, 'dma_timeout_cpu', png=True)
            h.fb = want
            row.check(b.r32(T.MB_LIMIT) >> 24 == 2, 'limit word %08x' % b.r32(T.MB_LIMIT))
            info = b.r32(T.MB_INFO)
            row.check(info & INFO_DMA_OFF, 'info word %08x lacks DMA-off' % info)
            row.check(b.bus.regs.get(LCD_CH_CS) == 0, 'LCD channel not stopped')
            row.check(not b.dma.busy(), 'a transfer is still active')
            row.check(b.dma.blocks == k0 + 1, '%d blocks tried, want 1' % (b.dma.blocks - k0))
            # later frames go straight to the CPU path
            b.panel.dreq_enabled = True
            h.tick_task = True
            h.fb = T.random_frame(self.rng)
            h.lcd_update()
            h.settle()
            self.frame_check(row, b, h, 'dma_after_timeout')
            row.check(b.dma.blocks == k0 + 1, 'DMA tried again after the timeout')
            row.check(b.panel.fifo_stray == 0, 'FIFO words outside a run')
        except T.Fail as e:
            row.check(False, str(e))

    def t_dma_sleep(self):
        row = T.Row('DMA: LCD_SLEEP, power cut, awake: DMA again after re-upload')
        self.rows.append(row)
        b, h, _ = self.bench(0)
        h.fb = T.random_frame(self.rng)
        try:
            h.lcd_awake()
            h.settle()
            h.lcd_sleep()
            h.fb = T.random_frame(self.rng)
            k0 = b.dma.blocks
            h.lcd_awake()
            h.settle()
            row.check(b.dma.blocks == k0 + 1, 'no DMA frame after awake')
            self.frame_check(row, b, h, 'dma_awake')
            self.dma_check(row, b, 'dma_awake')
        except T.Fail as e:
            row.check(False, str(e))

    def t_dma_clock(self):
        """Stage 4c's image (dev9): CMD_CLOCK switches the core to PLL A
        with Apple's sequence and back to the crystal with ours; the bus
        timing follows (Apple's 150 MHz words on the PLL, ours on the
        crystal); LCD_SLEEP on the PLL comes back to the crystal; the
        frames stay right throughout."""
        row = T.Row('DMA + CLOCK_CMD: 150 MHz and back, bus timing follows')
        self.rows.append(row)
        defs = DMA + ('BCMFW_CLOCK_CMD=1',)
        b, h, _ = self.bench(3, defs)
        b.gate_clocks('reset', 'reset')
        ours = T.timing_words(3, B.config_value('BCMFW_LCD_BUS_KHZ'))
        apple = T.TIMING_APPLE[3]

        def clk_writes(n0):
            return [(a.addr, a.value) for a in b.bus.log[n0:]
                    if a.rw == 'W' and a.seq not in b.host_seqs and
                    (a.addr == 0x10001400 or a.addr >> 8 == 0x10001C)]

        def switch(up):
            n0 = len(b.bus.log)
            b.w32(MB_CLOCK_REQ, CLOCK_REQ_MAGIC | up)
            h.bcm_command(T.bcm_cmd(CMD_CLOCK))
            return clk_writes(n0)

        try:
            h.fb = T.random_frame(self.rng)
            h.lcd_awake()
            h.settle()
            h.tick_task = False
            w = switch(1)
            sel = [v for a, v in w if a == 0x10001C28]
            row.check(sel == [0xA5A50001, 0xA5A50101],
                      'up: clock select writes %s' % ['%08x' % v for v in sel])
            row.check((0x10001C00, 0xA5A5006C) in w and (0x10001C14, 0xA5A50658) in w,
                      'up: PLL A not set to M 0x6C, N 0x258 | enable')
            st = b.r32(MB_CLOCK_STATE)
            row.check(st == CLOCK_MAGIC | 1, 'up: state %08x' % st)
            row.check(b.r32(T.MB_TIMING_KHZ) == 0 and
                      (b.r32(T.MB_TIMING0), b.r32(T.MB_TIMING1)) == (apple[0], apple[2]),
                      'up: timing %08x %08x kHz %d, want Apple\'s' % (
                          b.r32(T.MB_TIMING0), b.r32(T.MB_TIMING1), b.r32(T.MB_TIMING_KHZ)))
            h.bcm_command(T.LCD_UPDATE)
            self.frame_check(row, b, h, 'dma_clock_fast')
            w = switch(0)
            row.check([a for a, v in w][:3] == [0x10001C28, 0x10001C14, 0x10001C00] and
                      w[0][1] == 0xA5A50001 and w[1][1] == 0xA5A50000,
                      'down: writes %s' % ['%08x=%08x' % x for x in w])
            row.check(b.r32(MB_CLOCK_STATE) == CLOCK_MAGIC, 'down: state %08x' %
                      b.r32(MB_CLOCK_STATE))
            row.check(b.r32(T.MB_TIMING_KHZ) == B.config_value('BCMFW_LCD_BUS_KHZ') and
                      b.r32(T.MB_TIMING1) == ours[2],
                      'down: timing %08x kHz %d, want ours' % (
                          b.r32(T.MB_TIMING1), b.r32(T.MB_TIMING_KHZ)))
            h.bcm_command(T.LCD_UPDATE)
            self.frame_check(row, b, h, 'dma_clock_slow')
            # asleep on the crystal: LCD_SLEEP while fast switches down
            switch(1)
            n0 = len(b.bus.log)
            h.tick_task = True
            h.lcd_sleep()
            w = clk_writes(n0)
            row.check((0x10001C28, 0xA5A50001) in w, 'sleep on the PLL: no switch down')
            # a request without the magic does nothing
            b2, h2, _ = self.bench(3, defs)
            b2.gate_clocks('reset', 'reset')
            h2.fb = T.random_frame(self.rng)
            h2.lcd_awake()
            h2.settle()
            h2.tick_task = False
            b2.w32(MB_CLOCK_REQ, 1)
            h2.bcm_command(T.bcm_cmd(CMD_CLOCK))
            row.check(b2.r32(MB_CLOCK_STATE) == 0, 'no magic: switched anyway')
            self.dma_check(row, b, 'dma_clock')
        except T.Fail as e:
            row.check(False, str(e))
        # the UI images never switch: no clock code linked
        prog, _, _ = self.image(DMA)
        row.check('clock_fast' not in prog.symbols, 'hello_dma links clock_fast')

    def t_dma_b2b(self):
        b = self.t_back_to_back(DMA, ' (DMA build)')
        row = self.rows[-1]
        self.dma_check(row, b, 'dma_b2b')
        row.check(b.panel.runs and all(r[0] == W * H for r in b.panel.runs),
                  'runs of other lengths')

    def frame_cost(self, defines, pid, paced_wpu):
        """Instructions (and simulated us) per full frame and per 100x50
        rect at (10, 20), panel on."""
        self.paced_wpu = paced_wpu
        try:
            b, h, _ = self.bench(pid, defines)
        finally:
            self.paced_wpu = 5
        h.fb = T.random_frame(self.rng)
        h.lcd_awake()
        h.settle()
        h.tick_task = False
        out = []
        for cmd, xy, wh in ((T.LCD_UPDATE, 0, 0),
                            (T.LCD_UPDATERECT, 10 | 20 << 16, 100 | 50 << 16)):
            b.w32(T.MB_RECT_XY, xy)
            b.w32(T.MB_RECT_WH, wh)
            b.w32(T.CMD_WORD, cmd)
            b.step_until(lambda: b.r32(T.CMD_WORD) == T.BUSY, 100_000)
            t0 = b.now()
            n = b.step_until(lambda: b.r32(T.CMD_WORD) == T.DONE, 5_000_000)
            out.append((n, b.now() - t0))
            self.frame_check(T.Row('x'), b, h, 'cost')
        return out, b

    def t_dma_cost(self):
        row = T.Row('instructions per frame: DMA push vs CPU push (panel on)')
        self.rows.append(row)
        for pid in (0, 1, 2):
            try:
                cpu, _ = self.frame_cost((), pid, 5)
                dma, b = self.frame_cost(DMA, pid, 5)
                inst, _ = self.frame_cost(DMA, pid, 1 << 20)   # engine never the limit
                for name, v in (('cpu', cpu), ('dma', dma), ('dma_setup', inst)):
                    self.metrics['%s_frame_instr_p%d' % (name, pid)] = v[0][0]
                    self.metrics['%s_rect100x50_instr_p%d' % (name, pid)] = v[1][0]
                self.metrics['dma_frame_us_p%d' % pid] = dma[0][1]
                self.metrics['dma_rect100x50_us_p%d' % pid] = dma[1][1]
                row.check(dma[0][0] * 10 < cpu[0][0],
                          'panel %d: DMA frame %d vs CPU %d instructions' % (
                              pid, dma[0][0], cpu[0][0]))
                row.check(inst[0][0] < 2000, 'panel %d: DMA setup %d instructions' % (
                    pid, inst[0][0]))
                row.check(dma[0][1] >= W * H // 10, 'frame done before the modelled bus could '
                          'carry it (%d us)' % dma[0][1])
                row.instr += dma[0][0] + dma[1][0]
            except T.Fail as e:
                row.check(False, str(e))

    def t_dma_coverage(self):
        row = T.Row('DMA image: instruction kinds the self-test exercises')
        self.rows.append(row)
        corpus = os.path.dirname(os.path.dirname(self.args.vmcs))
        import contextlib
        import io
        ours = collections.Counter(S.COVER)
        S.COVER = collections.Counter()
        with contextlib.redirect_stdout(io.StringIO()):
            rc = T.vc2sim_tests.main([corpus])
        apple = set(S.COVER)
        S.COVER = None
        row.check(rc == 0, 'vc2sim self-test failed')
        extra = sorted(set(ours) - apple)
        self.metrics['dma_kinds_executed'] = len(ours)
        self.metrics['dma_kinds_not_in_selftest'] = ' '.join(k.replace(' ', ':') for k in extra)
        row.check(set(extra) <= self.ACCEPTED_KINDS,
                  'unproven kinds: %s' % ', '.join(sorted(set(extra) - self.ACCEPTED_KINDS)))

    def run(self):
        t0 = time.time()
        cover = self.args.vmcs and os.path.exists(self.args.vmcs) and not self.args.only
        self.ref = T.reference_sequences(self.args.vmcs, self.log)
        if cover:
            S.COVER = collections.Counter()
        if self.ref is None:
            print('note: vmcs.bin 1.3 not found: power sequences checked against '
                  'hardware.md facts only')
        tests = [self.t_dma_full, self.t_dma_rects, self.t_dma_updaterect,
                 self.t_dma_timeout, self.t_dma_sleep, self.t_dma_clock, self.t_dma_b2b]
        for t in tests + [self.t_dma_cost]:
            if self.args.only and self.args.only not in t.__name__:
                continue
            if t == self.t_dma_cost and cover:
                self.t_dma_coverage()        # before the CPU-build cost runs
                self._print(self.rows[-1:])
            n0 = len(self.rows)
            t()
            self._print(self.rows[n0:])
        cases = sum(r.cases for r in self.rows)
        fails = sum(len(r.fails) for r in self.rows)
        prog, rep, img = self.image(DMA)
        print()
        print(rep['lines'][0])
        for k in sorted(self.metrics):
            v = self.metrics[k]
            print('  %-28s %s' % (k, ('%.1f' % v) if isinstance(v, float) else v))
        print('%d rows, %d cases, %d failed (seed %d, %.0f s); PNGs in %s' % (
            len(self.rows), cases, fails, self.args.seed, time.time() - t0, self.png_dir))
        return 1 if fails else 0

    def _print(self, rows):
        for row in rows:
            print('%-72s %5d %5d %10d' % (row.name[:72], row.cases,
                                          row.cases - len(row.fails), row.instr))
            for f in row.fails[:8]:
                print('    FAIL: %s' % f)


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--seed', type=int, default=2722)
    ap.add_argument('--vmcs', default=T.DEFAULT_VMCS)
    ap.add_argument('--png', default=os.path.join(T.ROOT, 'build', 'bcmfw', 'test-dma'))
    ap.add_argument('--only', default='')
    a = ap.parse_args(argv)
    if not B.have_gcc():
        print('skip: vc4-elf-gcc not found at %s (set VC4_GCC)' % T.C.GCC)
        return 0
    return Suite(a).run()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
