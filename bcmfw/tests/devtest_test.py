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
"""The BCM device test (HAVE_BCMFW_DEVTEST) against vc2sim.

The ARM-side stage logic, firmware/target/arm/ipod/video/
bcm-devtest-core.c, is compiled for the host as it is and driven through
its hardware layer (struct bcmdt_hal) by hello_test's models: vc2sim
running our images, the LCD panel, and the port of lcd-video.c (whose
stock wake path, lcd_awake(), stands in for "reload Apple's vmcs"; the
bench boots the default hello image there).

Rows: the full run on each panel ID, through stage 2 and through stage 3
(the DMA image: frames, a 2-D rectangle, the forced timeout and its CPU
re-send); the probe's mailbox and its silence on the panel and GPIO; the
entry at 0 (reset-branch images); and the failure paths: an image that
never reports ready, one that faults, one that hangs inside a command, a
failed self-check, a dead host port, a refused take, and a DMA engine
that never completes (dead request line).  Each failure must end with the
power cut and the stock wake path run, and never block.

Stage 4, the timing bench (BCMFW_BENCH=1, built here with 64 iterations
instead of the device's 4096; the stage code doesn't depend on it): a pass
through stages 0-4 and through 0, 1, 4 only, with the log read back by
bcmfw/bench/readlog.py; stage 4 still runs when stage 2 fails (it needs
stage 1 only) and is skipped when stage 1 fails or isn't run; a bench that
hangs mid-kernel (stall limit, step word logged, cut, restore), one that
faults mid-kernel, and a failed on-chip check.  Stage 4b (the 150 MHz
build): refused, with nothing touched, without a stage 4 pass flag that
names the embedded bench image; a pass with it.

The HAVE_BCMFW boot path: the pass flags' format and every way one is
void; bcmdt_boot_swap() with the CPU and DMA images, and with images that
never get ready, fault, or hang in the first frame (power cut, nothing
left for the caller but the stock restore).  #62 level 1's ladder
(bcmdt_boot_ladder): our DMA image, then our CPU-push image, then Apple's
vmcs, through images that never get ready, fault, hang in the first
frame, or whose bootstrap handshake never answers; each bounded.
p14-resident: the player's video image (bcmvideo.bin, as mkvideo.py
builds it) as the ladder's first rung on panels 1 and 3, then driven as
the display: Rockbox's updates, its idle sleep only while the host's gate
is set (counters at 0x1D4-0x1DC, 0x1FC), the stock sleep and a wake that
uploads it again; a video image that never gets ready falls to our DMA
image; the HAL's idle words match bcmfw/config.h.

Probe v2 (stage 1 reads the results block, bringup.md "Probe v2"; vc2sim's
exception model is on, as in hello_test): checks 9 and 12 fault and are
resumed while stage 1 passes, and readlog.py decodes the vector, the
saved-pc offset and r5, also with HYP exc_pc_offset 6 and a non-default
misaligned vector; a fault resumed inside a required check fails stage 1
with its record logged; a fault outside any check (record U and the
legacy words) fails it with the power cut and the stock restore.

PW5's PP half (bcmdt_hib_cycle, brief-map-power.md section 9) on the
shipped video image, with a model of what GPO32 bits 14 and 15 do to the
chip: Apple's pin order, both pins high only for its 5 ms, no port access
while bit 15 is high, the 0x1F0 handshake and the image's reply with
every pattern kept; then a chip that cuts, comes back scrambled, never
answers or loses its port: each ends in the standard cut (bit 15, then
bit 14 low) with no port access after it; and the refusals (pins not as
Rockbox leaves them, a send that fails, a park that never comes) leave
the pins alone.

Plus: the embedded images match a fresh build (bcmfw/mkdevtest.py --check),
and so does the player's video image (bcmfw/mkvideo.py --check).

Usage: bcmfw/tests/devtest_test.py [-v]
Skips (exit 0) without vc4-elf-gcc.
"""
import ctypes as ct
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
import hello_test as T     # noqa: E402  (vc2sim, models, lcd-video.c port)
import readlog as RL       # noqa: E402  (the stage 4 log reader)
import gen_bench as G      # noqa: E402  (the store-locating records)
import dma_test as D       # noqa: E402  (the DMA engine on the same bench)
B, C = T.B, T.C
S, M = T.S, T.M

TGT = os.path.join(ROOT, 'firmware', 'target', 'arm', 'ipod', 'video')
OUT = os.path.join(ROOT, 'build', 'bcmfw', 'devtest')
DOCS = os.path.join(ROOT, 'docs', 'research', 'bcm2722')
W, H = T.W, T.H
RECT = (200, 170, 100, 50)
RECT3 = (10, 20, 100, 50)
IMAGES = (('probe', ['BCMFW_PROBE=1']), ('hello', []),
          ('hello_rb', ['BCMFW_RESET_BRANCH=1']),
          ('hello_dma', ['BCMFW_LCD_DMA=1']),
          ('hello_dma_rb', ['BCMFW_LCD_DMA=1', 'BCMFW_RESET_BRANCH=1']),
          ('hello_dma_clk', ['BCMFW_LCD_DMA=1', 'BCMFW_CLOCK_CMD=1']),
          ('drift', ['BCMFW_DRIFT=1']),
          ('hello_dma_idle', ['BCMFW_LCD_DMA=1', 'BCMFW_IDLE_SLEEP=1',
                              'BCMFW_IDLE_DOORBELL=1']),
          ('hello_dma_sby', ['BCMFW_LCD_DMA=1', 'BCMFW_IDLE_SLEEP=1',
                             'BCMFW_IDLE_DOORBELL=1', 'BCMFW_IDLE_GATED=1',
                             'BCMFW_LCD_STANDBY=1']),
          ('hello_3d', ['BCMFW_R3D=1', 'BCMFW_CLOCK_CMD=1']))
BITERS = 64                 # bench iterations here (the device build: 4096)
BENCH_IMAGES = (('bench', ['BCMFW_BENCH=1', 'BCMFW_BENCH_ITERS=%d' % BITERS]),
                ('bench_pll', ['BCMFW_BENCH=1', 'BCMFW_BENCH_PLL=1',
                               'BCMFW_BENCH_ITERS=16']))
STAGE4B = 5
STAGE5 = 6
STAGE4C = 7                 # dev9: the 150 MHz clock with the panel
STAGE5B = 8                 # dev9: the drift offload A/B
STAGE5I = 9                 # idle sleep (p14-bcm-idle)
I_N, I_HOLD_MS, I_FACE_MS, I_WAKES = 4, 300, 100, 1    # 5i here (device: 60, 60 s, 10 s, 2)
STAGE5S = 10                # the sleep spikes (Phase 18-0)
FLAG5I = 58                 # pass-flag code "5i"
S_HOLD_MS, S_LOOK_MS, S_GAP_MS, S_REPS = 30, 10, 10, 2  # 5s here (device: 20 s, 15 s, 0.5 s, 3)
S_BIG = 0x30000             # 5s: the boot upload (three engine pieces)
STAGE3D = 11                # the 3D render test
D_SCENES = 5                # bcm-devtest.h BCMDT_3D_SCENES
D_RUN, D_MS = 2, 1          # 3d here: scenes 0-1, one live frame each (device: 5, 10 s)
E2_PIECE = 0x10000
FLAG4B, FLAG4C = 41, 42     # pass-flag codes: "4b", "4c"
PHASE5B_MS, SETTLE5B_MS = 700, 50   # stage 5b here (the device: 120 s, 20 s)
STORM, WAKES = 8, 2         # stage 5 here (the device build: 240, 3)
WAKES_OV = WAKES            # dev8: wakes with LCD_POWERON first
UP_N = 3                    # dev8: the upload lab's methods
CMD5 = (~5 & 0xffff) << 16 | 5             # BCM_CMD(LCD_UPDATERECT)
CMD_DRIFT = (~0x42 & 0xffff) << 16 | 0x42   # BCM_CMD(0x42): the drift op
DRIFT_X = 160               # 5b: the panel
DRIFT_PARAM = 0x106000      # 5b: the drift image's parameter block
EVERYDAY = (('list area', 0, 20, 320, 220), ('two rows', 0, 20, 320, 56),
            ('progress', 30, 200, 260, 8), ('clock', 260, 2, 52, 16))

U32 = ct.c_uint32
RD = ct.CFUNCTYPE(ct.c_int, U32, ct.POINTER(U32))
WR = ct.CFUNCTYPE(ct.c_int, U32, U32)
WRB = ct.CFUNCTYPE(ct.c_int, U32, ct.POINTER(ct.c_uint16), U32)
VOID = ct.CFUNCTYPE(None)
BOOT = ct.CFUNCTYPE(ct.c_int, ct.POINTER(ct.c_uint8), U32)
INT = ct.CFUNCTYPE(ct.c_int)
BOOL = ct.CFUNCTYPE(None, ct.c_bool)
NOW = ct.CFUNCTYPE(U32)
MS = ct.CFUNCTYPE(None, U32)
LOG = ct.CFUNCTYPE(None, ct.c_char_p)
FBUP = ct.CFUNCTYPE(ct.c_int, U32, U32, U32, ct.c_int)
FBP = ct.CFUNCTYPE(ct.c_void_p)
SCR = ct.CFUNCTYPE(ct.c_void_p, U32)
BOOTHOW = ct.CFUNCTYPE(ct.c_int, ct.POINTER(ct.c_uint8), U32, ct.c_int,
                       ct.POINTER(U32))
PALGN = ct.CFUNCTYPE(ct.c_int, U32, ct.POINTER(ct.c_uint16), U32, ct.c_uint)


class Image(ct.Structure):
    _fields_ = [('data', ct.POINTER(ct.c_uint8)), ('size', U32), ('fnv', U32),
                ('name', ct.c_char_p)]


HAL_IMAGES = ('probe', 'hello', 'hello_rb', 'hello_dma', 'hello_dma_rb', 'bench',
              'bench_pll', 'hello_dma_clk', 'drift', 'hello_dma_idle',
              'hello_dma_sby', 'hello_3d')


class Hal(ct.Structure):
    _fields_ = [('rd', RD), ('wr', WR), ('wr_block', WRB), ('doorbell', VOID),
                ('boot', BOOT), ('power_cut', VOID), ('take', INT),
                ('restore', BOOL), ('light', BOOL), ('now_us', NOW),
                ('yield_', VOID), ('sleep_ms', MS), ('hold', MS), ('log', LOG),
                ('probe', ct.POINTER(Image)), ('hello', ct.POINTER(Image)),
                ('hello_rb', ct.POINTER(Image)), ('hello_dma', ct.POINTER(Image)),
                ('hello_dma_rb', ct.POINTER(Image)), ('upload_fb', INT),
                ('bench', ct.POINTER(Image)), ('bench_pll', ct.POINTER(Image)),
                ('fb_upload', FBUP), ('fb', FBP), ('boost', BOOL),
                ('hello_dma_clk', ct.POINTER(Image)), ('mark', LOG),
                ('drift', ct.POINTER(Image)), ('scratch', SCR),
                ('hello_dma_idle', ct.POINTER(Image)),
                ('hello_dma_sby', ct.POINTER(Image)), ('boot_how', BOOTHOW),
                ('hello_3d', ct.POINTER(Image)),
                # bench row ALGN's port checks: left NULL here (no port
                # model of the forms), so stage 4 logs "ALGNP none"
                ('port_algn', PALGN)]


class Result(ct.Structure):
    _fields_ = [('passed', ct.c_int), ('panel_id', ct.c_int)] + \
        [(n, U32) for n in ('gpio_lev', 'info', 'selfcheck', 'selfcheck_all',
                            'from0', 'probe_ticks', 'fault_pc', 'fault_sp',
                            'apple_us', 'ready_us', 'first_us', 'frame_us',
                            'rect_us', 'sleep_us', 'wake_us',
                            'dma_first_us', 'dma_frame_us', 'dma_rect_us',
                            'forced_us', 'dma_bcm_us', 'dma_bcm_rect_us',
                            'forced_bcm_us', 'dma_done', 'dma_tmo', 'dma_cs',
                            'dma_int', 'dma_cb', 'dma_polls')] + \
        [('why', ct.c_char * 64), ('img2', ct.c_void_p), ('img3', ct.c_void_p),
         ('passed_mask', ct.c_uint), ('img4', ct.c_void_p)] + \
        [(n, U32) for n in ('b_step', 'b_us', 'b_flags', 'b_vpass', 'b_vall',
                            'b_spass', 'b_sall', 'b_ppass', 'b_pall', 'b_nk',
                            'b_kernels_ok', 'b_clock_khz', 'b_clock2_khz',
                            'b_start_khz', 'p_faulted', 'p_block', 'p_pass',
                            'p_u_status')] + \
        [('img5', ct.c_void_p)] + \
        [(n, U32) for n in ('e_host_us', 'e_rects', 'e_rect_max_us', 'e_full_us',
                            'e_wakes', 'e_wake_max_us', 'e_stop_us', 'e_fallback_us',
                            'e_draw_us')] + \
        [('e_lab_us', U32 * (2 * UP_N))] + \
        [(n, U32) for n in ('e_lab_bad', 'e_row_us', 'e_ov_wake_max_us')] + \
        [('img4b', ct.c_void_p), ('img4c', ct.c_void_p)] + \
        [(n, U32) for n in ('c_up_us', 'c_down_us', 'c_base_us', 'c_fast_us',
                            'c_slow_us')] + \
        [('m_frames', U32 * 2), ('m_late', U32 * 2), ('m_pp_us', U32 * 2),
         ('m_bcm_us', U32 * 2), ('m_wait_us', U32 * 2)] + \
        [('img5i', ct.c_void_p), ('i_nop_us', U32 * 2), ('i_rect_max_us', U32 * 2)] + \
        [(n, U32) for n in ('i_ticks', 'i_bells', 'i_sleeps', 'i_bad', 'i_late',
                            'i_fb_bad', 's_cold_us', 's_cold_pon_us',
                            's_reset_us', 's_reset_pon_us')] + \
        [('s_wake_us', U32 * 2), ('s_pon_us', U32 * 2)] + \
        [(n, U32) for n in ('s_refused', 's_fb_bad', 's_slept', 's_ticks',
                            's_sleeps', 's_bad')] + \
        [('s_up_us', U32 * 2), ('s_ready_us', U32 * 2), ('s_first_us', U32 * 2),
         ('s_up_bad', U32 * 2)] + \
        [(n, U32) for n in ('s_up_bytes', 's_pieces')] + \
        [('d_tris', U32 * D_SCENES)] + \
        [(n, (U32 * D_SCENES) * 2) for n in ('d_frames', 'd_us', 'd_render', 'd_max',
                                             'd_late', 'd_drawn')] + \
        [(n, U32) for n in ('d_crc_bad', 'd_fast', 'd_te_edges', 'd_te_us', 'd_sync',
                            'd_sync_frames', 'd_sync_us', 'd_sync_late')]



# PW5: bcmdt_hib_cycle's HAL and result (bcm-devtest.h)
SEND = ct.CFUNCTYPE(ct.c_int, ct.c_void_p)
GPO = ct.CFUNCTYPE(U32, U32, U32)


class HibHal(ct.Structure):
    _fields_ = [('rd', RD), ('wr', WR), ('send', SEND), ('gpo', GPO),
                ('gpo_enable', NOW), ('now_us', NOW), ('udelay', MS),
                ('sleep_ms', MS), ('yield_', VOID), ('ctx', ct.c_void_p)]


class HibResult(ct.Structure):
    _fields_ = [('end', ct.c_int)] + \
        [(n, U32) for n in ('gpo0', 'en0', 'park_us')] + [('both_us', U32 * 2)] + \
        [(n, U32) for n in ('hold_us', 'word_after', 'answer_us', 'wake_us',
                            'gpo_end')] + [('pins', ct.c_bool), ('cut', ct.c_bool)]


HIB_OK, HIB_REFUSED, HIB_E_SEND, HIB_E_PARK, HIB_E_DEAD, HIB_E_LOST, HIB_E_WAKE = \
    0, -1, -2, -3, -4, -5, -6
RUN_PIN, RETAIN_PIN = 0x4000, 0x8000


class HibRig:
    """PW5's PP half (bcmdt_hib_cycle) on the shipped video image in
    vc2sim, with a model of what GPO32 bits 14 and 15 do to the chip:
      retain  bit 15 high, then bit 14 low: the core stops, memory kept;
              bit 14 high again: it runs on (Apple's reading, section 3)
      cut     bit 14 low is a power cut whatever bit 15 says (no
              retention): the port reads all ones afterwards
      lost    retained, but SRAM comes back scrambled
      stuck   retained, and the core never runs again
      noport  retained, then every port read fails
    It logs every pin change and every port access with the pins then."""

    def __init__(self, lib, video, entry, model='retain', gpo=0x4003,
                 en=0xC003, session=False):
        import video_test as V
        self.V, self.lib, self.model = V, lib, model
        self.rng = random.Random(2722)
        b = V.VideoBench(video, 1, entry, None)
        h = T.Host(b, random.Random(7))
        h.fb = [(i * 7) & 0xffff for i in range(W * H)]
        h.lcd_awake()
        h.settle()
        h.tick_task = False
        h.idle_ok = False
        vp = V.Player(b)
        vp.attach()
        b.bus.regs[0x10001C28] = 0x0001         # the core on the crystal
        if vp.open(320, 240)[0] != V.K['HP_OK']:
            raise T.Fail('OPEN for SDRAM')
        if not session:
            vp.call(V.K['HP_OP_CLOSE'], [vp.session], timeout_us=100_000)
        self.b, self.h, self.vp = b, h, vp
        self.gpo_v, self.en = gpo, en
        self.frozen = self.dead = False
        self.pins, self.port = [], []           # (us, clear, set, after); (us, rw, gpo)
        self.tag = None
        self.send_fails = False
        K = V.K
        _, st, p = vp.call(K['HP_OP_HIB'], [K['HP_HIB_MAGIC'], K['HP_HIB_UNLOCK']])
        if st != K['HP_OK'] or not p:
            raise T.Fail('HIB UNLOCK %d' % st)
        self.code = p[0]
        self.hal = HibHal(rd=RD(self._rd), wr=WR(self._wr), send=SEND(self._send),
                          gpo=GPO(self._gpo), gpo_enable=NOW(lambda: self.en),
                          now_us=NOW(lambda: self.b.now() & 0xffffffff),
                          udelay=MS(self._step), sleep_ms=MS(lambda ms: self._step(ms * 1000)),
                          yield_=VOID(lambda: self._step(300)), ctx=None)

    def _step(self, us):
        b = self.b
        end = b.now() + us
        while b.now() < end:
            if self.frozen or self.dead or not b.running:
                b.timer.advance(min(1000, end - b.now()))
            else:
                b.run(300)

    def _rd(self, a, v):
        self.port.append((self.b.now(), 'R', self.gpo_v))
        if self.model == 'noport' and self.pins:
            return -1
        v[0] = 0xffffffff if self.dead else self.b.r32(a)
        return 0

    def _wr(self, a, v):
        self.port.append((self.b.now(), 'W', self.gpo_v))
        if not self.dead:
            self.b.w32(a, v)
        return 0

    def _send(self, ctx):
        if self.send_fails:
            return -1
        K = self.V.K
        self.tag = self.vp.send(K['HP_OP_HIB'], [K['HP_HIB_MAGIC'], K['HP_HIB_PARK'],
                                                 self.code, 10_000])
        return 0 if self.tag else -1

    def _gpo(self, clear, set_):
        old = self.gpo_v
        new = (old & ~(clear & 0xC000)) | (set_ & 0xC000)
        self.gpo_v = new
        if clear or set_:
            self.pins.append((self.b.now(), clear, set_, new))
        if old & RUN_PIN and not new & RUN_PIN:
            if old & RETAIN_PIN and self.model != 'cut':
                self.frozen = True
                if self.model == 'lost':
                    self.b.bus.b[0:0x100000] = bytes(self.rng.getrandbits(8)
                                                     for _ in range(256)) * 0x1000
            else:
                self.b.power_cut(self.rng)
                self.dead = True
        if not old & RUN_PIN and new & RUN_PIN and self.frozen and self.model != 'stuck':
            self.frozen = False
        return new

    def cycle(self, hold_ms):
        r = HibResult()
        self.ret = self.lib.bcmdt_hib_cycle(ct.byref(self.hal), hold_ms, ct.byref(r))
        return r

    def reply(self):
        return self.vp.wait(self.tag, timeout_us=2_000_000)

def build_lib():
    os.makedirs(OUT, exist_ok=True)
    lib = os.path.join(OUT, 'libbcmdt.so')
    src = os.path.join(TGT, 'bcm-devtest-core.c')
    subprocess.check_call(['cc', '-shared', '-fPIC', '-O1', '-std=gnu99', '-Wall',
                           '-Wextra', '-Werror', '-I' + TGT,
                           '-DBCMDT_STORM=%d' % STORM, '-DBCMDT_WAKES=%d' % WAKES,
                           '-DBCMDT_WAKES_OV=%d' % WAKES_OV,
                           '-DBCMDT_5B_PHASE_MS=%d' % PHASE5B_MS,
                           '-DBCMDT_5B_SETTLE_MS=%d' % SETTLE5B_MS,
                           '-DBCMDT_5B_PAIRS=1',
                           '-DBCMDT_5I_N=%d' % I_N, '-DBCMDT_5I_HOLD_MS=%d' % I_HOLD_MS,
                           '-DBCMDT_5I_FACE_MS=%d' % I_FACE_MS,
                           '-DBCMDT_5I_WAKES=%d' % I_WAKES,
                           '-DBCMDT_5S_HOLD_MS=%d' % S_HOLD_MS,
                           '-DBCMDT_5S_LOOK_MS=%d' % S_LOOK_MS,
                           '-DBCMDT_5S_GAP_MS=%d' % S_GAP_MS,
                           '-DBCMDT_5S_REPS=%d' % S_REPS,
                           '-DBCMDT_5S_BIG=%d' % S_BIG,
                           '-DBCMDT_3D_RUN=%d' % D_RUN, '-DBCMDT_3D_MS=%d' % D_MS,
                           '-DHAVE_BCMFW_L1',     # #62: the ladder
                           '-o', lib, src])
    return ct.CDLL(lib)


def fnv(data):
    v = 2166136261
    for b in data:
        v = ((v ^ b) * 16777619) & 0xffffffff
    return v


def patch(img, addr, text):
    """Assemble text (one or more ';'-separated instructions) at addr."""
    img = bytearray(img)
    for t in text.split(';'):
        ws = C.assemble_min(t.strip().replace('{pc}', '0x%x' % addr), addr)
        for w in ws:
            img[addr:addr + 2] = struct.pack('<H', w)
            addr += 2
    return bytes(img)


class TeLine:
    """A read rule for GPIO's level register: the panel ID (GPIO 50/51)
    and a tearing-effect pulse on GPIO 49 by the simulated clock."""

    def __init__(self, bus, pid, period, width=100):
        self.bus, self.period, self.width = bus, period, width
        self.id_bits = ((pid >> 1) & 1) << 18 | (pid & 1) << 19

    def read(self, last, since):
        t = self.bus.timer.clo if self.bus.timer else 0
        return self.id_bits | ((1 << 17) if t % self.period < self.width else 0)


class Run:
    """One bcmdt_run() against the simulator: the HAL callbacks."""

    def __init__(self, lib, images, pid, reset_pc=0x200, stock=None, rng=None,
                 port_dies_after=None, refuse_take=False, dreq=True, dreq_off_at=None,
                 ppdma=None, lab=True, boot_fails=0, scratch=True, bell=False,
                 spin=False, e2_bad=False, boot_how=True, te_period=None):
        self.lib = lib
        self.boot_fails = boot_fails    # #62: the next n bootstraps wedge
        self.e2_bad = e2_bad            # 5s: engine 2 drops one word
        self.has_boot_how = boot_how    # False: the HAL has no boot_how
        self.boot_hows = []             # 5s: (method, bytes, pieces) per boot
        self.rng = rng or random.Random(60)
        stock = stock or images['hello']
        self.b = D.DmaBench(stock, pid, 0x200, reset_pc)
        self.b.panel.dreq_enabled = dreq
        if te_period:
            # stage 3d: the panel's tearing-effect line on GPIO 49 (level
            # register bit 17), a 100 us pulse every te_period us of
            # simulated time, beside the panel ID bits
            self.b.bus.rule(0x10002824, 0x10002824, TeLine(self.b.bus, pid, te_period))
        # stage 5i: the host doorbell (source 23, [M]) as the harness
        # chooses (bell: BCM_CONTROL raises it), and a sleep that wakes
        # at once for it; reads of the command word let the core run a
        # little, so the stage's tight poll sees time pass
        self.bell = self.b.bus.attach(M.HostDoorbell(raises=bell))
        self.b.sim.mem.device(M.IntCtl).srcs.append(self.bell.sources)
        self.b.sim.on_sleep = self._on_sleep
        self.sleepers = {images[n] for n in ('hello_dma_idle', 'video',
                                             'hello_dma_sby') if n in images}
        self.spin_reads = spin
        self.dreq_off_at = dreq_off_at     # the request line dies at this hold
        self.h = T.Host(self.b, random.Random(self.rng.getrandbits(32)))
        self.h.fb = [0x1234] * (W * H)        # "today's screen"
        self.h.lcd_awake()
        self.h.settle()
        self.events = []
        self.lines = []
        self.snaps = []
        self.booted = []
        self.port_calls = 0
        self.port_dies_after = port_dies_after
        self.refuse_take = refuse_take
        self.ppdma = ppdma          # dev8 lab: None, 'drop', 'noop' or 'hang'
        self.lab = lab              # False: the HAL has no lab calls
        self.boosts = []
        self.marks = []             # dev9: power-log phase marks (5b)
        self.wr_blocks = 0          # host block writes (uploads)
        self.scratch_ok = scratch   # stage 5b: the HAL gives cover memory
        self._scr = None
        self.scratch_calls = []
        self.panels = []            # the panel's drift area after each push
        self.push_pending = False
        self._fbbuf = None
        # the bench reports ready, then works: the host model must not
        # fast-forward over a "done" command word while one runs
        self.benches = {images[n] for n in ('bench', 'bench_pll') if n in images}
        self._keep = []
        self.hal = self._make_hal(images)

    # ---- the HAL
    def _port(self):
        self.port_calls += 1
        return self.port_dies_after is not None and \
            self.port_calls > self.port_dies_after

    def _rd(self, a, out):
        if self._port():
            return -1
        if a == T.CMD_WORD and self.spin_reads:
            self.b.run(40)
            self.h.service()
        out[0] = self.b.r32(a)
        if a == T.CMD_WORD and out[0] == T.DONE and self.push_pending:
            self.push_pending = False
            fb = self.b.panel.fb
            self.panels.append(tuple(fb[y * W + x] for y in range(H)
                                     for x in range(DRIFT_X, W)))
        return 0

    def _on_sleep(self, nxt):
        sim = self.b.sim
        if self.bell.pending:
            npc = S.take_irq(sim, nxt)
            if npc is not None:
                return npc
        return S._sleep(sim, nxt)

    def _wr(self, a, v):
        if self._port():
            return -1
        if a == T.CMD_WORD:
            self.events.append(('cmd', v))
            # bench row ALGN's checks are misaligned on purpose, its DMA
            # checks included (D32M: a 32-byte unit off 32, which strict
            # DMA refuses): strict alignment off for that one command, as
            # bench_test.t_algn does, back on at the next command
            if v == (~0x45 & 0xFFFF) << 16 | 0x45:
                self.algn_strict = S.set_strict_align('')
            elif getattr(self, 'algn_strict', None) is not None:
                S.set_strict_align(self.algn_strict)
                self.algn_strict = None
            # 5b: a push (not PREP's planes) ends with a frame on the panel
            self.push_pending = v == CMD5 or (
                v == CMD_DRIFT and self.b.r32(DRIFT_PARAM) & 0xffff != 2)
        self.b.w32(a, v)
        return 0

    def _wr_block(self, a, src, n):
        """dt_wr_block (bcm-port-block.h bcm_port_block): a mid-word
        address refused; the whole words under one address; an odd last
        halfword merged into its word by read-modify-write (the port loses
        a lone one, quirk #35)"""
        if self._port():
            return -1
        if a & 3:
            self.half_writes = getattr(self, 'half_writes', 0) + 1
            return -1
        self.wr_blocks += 1
        data = ct.string_at(src, 2 * n)
        even = 2 * (n & ~1)
        if even:
            self.b.write_bytes(a, data[:even])
        if n & 1:
            w = self.b.r32(a + even)
            self.b.write_bytes(a + even, (int.from_bytes(data[even:], 'little') |
                                          (w & 0xFFFF0000)).to_bytes(4, 'little'))
        return 0

    def _boot(self, p, n):
        b, h = self.b, self.h
        data = ct.string_at(p, n)
        self.events.append(('boot', data, len(self.b.bus.log)))
        self.booted.append(data)
        h.idle_ok = data not in self.benches and data not in self.sleepers
        b.power_on(self.rng)
        h.sleep(T.HZ // 20)
        if self.boot_fails > 0:
            # level 1's bounded handshake: the port never answers, the
            # wait expires (20 ms), the HAL's boot reports it
            self.boot_fails -= 1
            h.sleep(T.HZ // 50)
            return -1
        k = ((n + 3) >> 1) & ~1
        b.write_bytes(0, (data + b'\0' * 4)[:2 * k])
        b.w32(T.CMD_WORD, 0)
        b.w32(0x10000C00, 0xC0000000)
        b.w32(0x10000C00, 0)
        b.w32(0x10000400, 0xA5A50002)
        return 0

    def _boot_how(self, p, n, how, out):
        """5s: the bootstrap, the upload (engine 2 in 64 KB pieces), every
        word read back, the start; the times are the session 37 rates
        (CPU 350 ns a halfword, engine 2 11.5 MB/s), not simulated"""
        b, h = self.b, self.h
        data = ct.string_at(p, n)
        pieces = (n + E2_PIECE - 1) // E2_PIECE if how else 0
        self.boot_hows.append((how, n, pieces))
        self.booted.append(data)
        h.idle_ok = not any(data.startswith(x) for x in self.sleepers)
        b.power_on(self.rng)
        h.sleep(T.HZ // 20)
        moved = bytearray(data)
        if how and self.e2_bad:
            moved[0x10010:0x10014] = b'\0\0\0\0'    # the second piece's 5th word
        b.write_bytes(0, bytes(moved))
        out[0] = n * 350 // 2000 if not how else n * 1000000 // 11500000
        out[1], out[2], out[3] = 0, 0xffffffff, pieces
        mem = bytes(b.bus.b[:n])            # the read-back, word by word
        if mem != data:
            for a in range(0, n, 4):
                if mem[a:a + 4] != data[a:a + 4]:
                    if out[1] == 0:
                        out[2] = a
                    out[1] += 1
        b.w32(T.CMD_WORD, 0)
        b.w32(0x10000C00, 0xC0000000)
        b.w32(0x10000C00, 0)
        b.w32(0x10000400, 0xA5A50002)
        return 0

    def _power_cut(self):
        self.events.append(('cut', len(self.b.bus.log)))
        self.b.power_cut(self.rng)
        self.h.idle_ok = True

    def _take(self):
        h = self.h
        if self.refuse_take or not h.display_on:
            return -1
        self.events.append(('take',))
        h.display_on = False
        h.wait(lambda: h.state in (T.LCD_INITIAL, T.LCD_IDLE), 'idle')
        h.tick_task = False
        return 0

    def _restore(self, running):
        h = self.h
        self.events.append(('restore', bool(running), self.b.powered))
        if running:
            h.state, h.display_on, h.tick_task = T.LCD_IDLE, True, True
        else:
            h.lcd_awake()               # bench stand-in for Apple's vmcs
            h.settle()

    def _yield(self):
        b = self.b
        if b.idle or not b.running:
            b.timer.advance(1000)       # a stopped core: let 1 ms pass
            self.h.service()
        else:
            self.h.yield_()

    def _sleep(self, ms):
        end = self.b.now() + ms * 1000
        while self.b.now() < end:
            self._yield()

    def _hold(self, ms):
        self.snaps.append(list(self.b.panel.fb))
        if len(self.snaps) == self.dreq_off_at:
            self.b.panel.dreq_enabled = False
        self._sleep(ms)

    def _upload_fb(self):
        if self._port():
            return -1
        self.events.append(('upload_fb',))
        self.b.write_bytes(T.FB, struct.pack('<%dH' % (W * H), *self.h.fb))
        return 0

    def _fb_upload(self, a, off, n, how):
        if self._port():
            return -1
        self.events.append(('fb_upload', a, off, n, how))
        data = struct.pack('<%dH' % (W * H), *self.h.fb)[off:off + n]
        if how == 2 and self.ppdma == 'hang':
            return 1
        if how == 2 and self.ppdma == 'drop':
            data = bytes(len(data))             # the engine never reached the port
        if how == 2 and self.ppdma == 'noop':
            return 0        # nothing moves: the CPU methods' frame stays (S8)
        self.b.write_bytes(a, data)
        return 0

    def _fb(self):
        self._fbbuf = (ct.c_uint16 * (W * H))(*self.h.fb)
        return ct.addressof(self._fbbuf)

    def _scratch(self, n):
        self.scratch_calls.append(n)
        if not n:
            self._scr = None
            return None
        if not self.scratch_ok:
            return None
        self._scr = (ct.c_uint8 * n)()
        return ct.addressof(self._scr)

    def _boost(self, on):
        self.boosts.append(bool(on))

    def _log(self, s):
        line = s.decode()
        self.lines.append(line)
        self.events.append(('log', line))

    def _make_hal(self, images):
        def img(name):
            data = images[name]
            buf = (ct.c_uint8 * len(data)).from_buffer_copy(data)
            im = Image(ct.cast(buf, ct.POINTER(ct.c_uint8)), len(data), fnv(data),
                       name.encode())
            self._keep += [buf, im]
            return ct.pointer(im)
        cbs = dict(rd=RD(self._rd), wr=WR(self._wr), wr_block=WRB(self._wr_block),
                   doorbell=VOID(self.bell.ring), boot=BOOT(self._boot),
                   power_cut=VOID(self._power_cut), take=INT(self._take),
                   restore=BOOL(self._restore),
                   light=BOOL(lambda on: self.events.append(('light', on))),
                   now_us=NOW(lambda: self.b.now() & 0xffffffff),
                   yield_=VOID(self._yield), sleep_ms=MS(self._sleep),
                   hold=MS(self._hold), log=LOG(self._log),
                   upload_fb=INT(self._upload_fb),
                   mark=LOG(lambda t: self.marks.append(t.decode())),
                   scratch=SCR(self._scratch))
        if self.has_boot_how:
            cbs.update(boot_how=BOOTHOW(self._boot_how))
        if self.lab:
            cbs.update(fb_upload=FBUP(self._fb_upload), fb=FBP(self._fb),
                       boost=BOOL(self._boost))
        self._keep.append(cbs)
        self.ptrs = {n: img(n) for n in images}
        # a rung that is no HAL image (the resident video image) is only
        # passed to the ladder
        return Hal(**{n: p for n, p in self.ptrs.items() if n in HAL_IMAGES},
                   **cbs)

    def addr(self, name):
        return ct.cast(self.ptrs[name], ct.c_void_p).value

    def run(self, last):
        self.res = Result()
        t0 = time.time()
        self.ret = self.lib.bcmdt_run(ct.byref(self.hal), last, ct.byref(self.res))
        self.wall = time.time() - t0
        return self.res

    def run_stages(self, stages, flag4=None):
        self.res = Result()
        t0 = time.time()
        self.ret = self.lib.bcmdt_run_stages(ct.byref(self.hal), stages, flag4,
                                             ct.byref(self.res))
        self.wall = time.time() - t0
        return self.res

    def swap(self, name):
        self.res = Result()
        t0 = time.time()
        self.ret = self.lib.bcmdt_boot_swap(ct.byref(self.hal), self.ptrs[name],
                                            ct.byref(self.res))
        self.wall = time.time() - t0
        return self.res

    def ladder(self, names):
        """#62 level 1: bcmdt_boot_ladder over names (None: a missing rung)"""
        self.res = Result()
        arr = (ct.c_void_p * len(names))(*[
            ct.cast(self.ptrs[n], ct.c_void_p).value if n else None for n in names])
        self.ret = self.lib.bcmdt_boot_ladder(ct.byref(self.hal), arr, len(names),
                                              ct.byref(self.res))
        return self.res


def expected_frames(lib):
    row = (ct.c_uint16 * W)()
    pat = []
    for y in range(H):
        lib.bcmdt_pattern_row(y, row)
        pat += list(row)

    def box(base, rect):
        out = list(base)
        rr = (ct.c_uint16 * rect[2])()
        for y in range(rect[3]):
            lib.bcmdt_rect_row(y, rr)
            i = (rect[1] + y) * W + rect[0]
            out[i:i + rect[2]] = list(rr)
        return out
    fine = []
    for y in range(H):
        lib.bcmdt_fine_row(y, row)
        fine += list(row)
    return pat, box(pat, RECT), box(pat, RECT3), fine


class Suite:
    def __init__(self, verbose):
        self.verbose = verbose
        self.rows = []
        self.lib = build_lib()
        self.progs = {}
        self.images = {}
        for name, defs in IMAGES + BENCH_IMAGES:
            prog, _ = B.build(os.path.join(OUT, name), defs, listing=False)
            self.progs[name] = prog
            self.images[name] = prog.load_image()
        self.pattern, self.boxed, self.boxed3, self.fine = expected_frames(self.lib)
        self.lib.bcmdt_flag_match.restype = ct.c_void_p
        self.ktags, self.vtags = RL.tags()
        # dev5: informational checks left out; b_vpass/b_vall hold checks
        # 0-31 (32-63: header words 27/28)
        self.vall = RL.required_mask() & 0xFFFFFFFF

    def row(self, name):
        r = T.Row(name)
        self.rows.append(r)
        return r

    # common checks for every run: it ended, the BCM is back on the stock
    # image through the stock path, and no boot happened without a log
    # line just before it
    def ended_clean(self, r, run, booted_ours):
        ev = run.events
        r.check(run.h.display_on and run.h.tick_task,
                'display not handed back (display_on %s tick %s)' % (
                    run.h.display_on, run.h.tick_task))
        if booted_ours:
            rs = [e for e in ev if e[0] == 'restore']
            r.check(rs and rs[-1][1] is False and rs[-1][2] is False,
                    'last restore not the stock wake path after a power cut: %s' % rs)
            # (0x000-0x1FF holds the vectors and the live mailbox)
            r.check(run.b.bus.b[0x200:len(run.b.image)] == run.b.image[0x200:],
                    'BCM SRAM does not hold the stock image after restore')
        for i, e in enumerate(ev):
            if e[0] == 'boot':
                prev = [x for x in ev[:i] if x[0] != 'light']
                r.check(prev and prev[-1][0] == 'log' and
                        prev[-1][1].split(None, 1)[1].startswith('boot'),
                        'boot without a flushed log line just before it')
        r.check(run.lines and run.lines[-1].split(None, 1)[1].startswith('=== end'),
                'log does not end with the summary line')
        # say() writes into 112 bytes: a line of 111 characters was cut off
        r.check(all(len(x) < 111 for x in run.lines), 'a log line was truncated: %s' %
                next((x for x in run.lines if len(x) >= 111), ''))

    def t_full(self):
        for pid in range(4):
            r = self.row('stages 0-2 pass, panel %d: pattern, rect, sleep/wake' % pid)
            run = Run(self.lib, self.images, pid)
            res = run.run(2)
            r.check(res.passed == 2 and run.ret == 2, 'passed %d (%s)' % (
                res.passed, res.why.decode()))
            r.check(res.panel_id == pid, 'panel id %d' % res.panel_id)
            r.check(res.selfcheck == res.selfcheck_all == 0x1FF,
                    'self-check %03x of %03x' % (res.selfcheck, res.selfcheck_all))
            r.check(len(run.snaps) == 3, '%d holds' % len(run.snaps))
            if len(run.snaps) == 3:
                r.check(run.snaps[0] == self.pattern, 'photo 1 is not the pattern')
                r.check(run.snaps[1] == self.boxed, 'photo 2 is not pattern + box')
                r.check(run.snaps[2] == self.boxed, 'photo 3 (wake) is not pattern + box')
            # dev4: one frame with Apple's bus timing, one with ours, logged
            T_ = T.timing_words(pid, B.config_value('BCMFW_LCD_BUS_KHZ'))
            A_ = T.TIMING_APPLE[pid]
            r.check(any("bus timing: Apple's %08x/%08x frame" % (A_[0], A_[2]) in x
                        for x in run.lines) and
                    any('bus timing: ours (%d kHz) %08x/%08x frame' % (
                        B.config_value('BCMFW_LCD_BUS_KHZ'), T_[0], T_[2]) in x
                        for x in run.lines), 'bus timing lines')
            names = [d for d in run.booted]
            r.check(names == [self.images['probe'], self.images['hello']],
                    'booted %d images, not probe then hello' % len(names))
            cuts = [e for e in run.events if e[0] in ('cut', 'restore')]
            # per stage: cut Apple's, (our image) cut ours, stock restore
            r.check([e[0] for e in cuts] == ['cut', 'cut', 'restore'] * 2,
                    'cut/restore order %s' % [e[0] for e in cuts])
            self.ended_clean(r, run, True)
            if pid == 0:
                T.png_write(os.path.join(DOCS, 'device-session-1-photo1.png'), self.pattern)
                T.png_write(os.path.join(DOCS, 'device-session-1-photo2.png'), self.boxed)
                for k in ('apple_us', 'ready_us', 'first_us', 'frame_us', 'rect_us',
                          'sleep_us', 'wake_us'):
                    self.metric('%s_p0' % k, getattr(res, k))
                self.metric('log_lines', len(run.lines))
                if self.verbose:
                    print('\n'.join('    ' + x for x in run.lines))
            self.metric('wall_s_p%d' % pid, round(run.wall, 1))

    def t_full3(self):
        for pid in range(4):
            r = self.row('stages 0-3 pass, panel %d: DMA frames, 2-D rect, forced timeout'
                         % pid)
            run = Run(self.lib, self.images, pid)
            res = run.run(3)
            r.check(res.passed == 3 and run.ret == 3, 'passed %d (%s)' % (
                res.passed, res.why.decode()))
            # photo 6: the CPU re-send, then the image's mark (dev4): the
            # top BCMFW_DMA_MARK rows black
            marked = [0] * (W * 8) + self.pattern[W * 8:]
            # dev5: spin()'s loop at eight alignments, every image
            r.check(sum('spin loop ticks at +0..+14:' in x for x in run.lines) >= 2 and
                    sum('spin loop ticks at +16..+30:' in x for x in run.lines) >= 2,
                    'spin alignment lines missing')
            # photos 5a-5c: the bus-timing sweep's frames, the same picture
            # as photo 5 at each clock's words; 5d-5g (dev6): the fine
            # pattern at our own words, then at each sweep clock's
            want = [self.pattern, self.boxed, self.boxed, self.pattern, self.boxed3,
                    self.boxed3, self.boxed3, self.boxed3] + [self.fine] * 4 + [marked]
            r.check(len(run.snaps) == 13, '%d holds' % len(run.snaps))
            for k, (got, exp) in enumerate(zip(run.snaps, want)):
                r.check(got == exp, 'photo %d differs from the expected picture' % (k + 1))
            r.check(run.booted == [self.images[n] for n in ('probe', 'hello', 'hello_dma')],
                    'booted %d images, not probe, hello, hello_dma' % len(run.booted))
            r.check(res.dma_done == 10 and res.dma_tmo == 1,
                    'DMA done %d timeouts %d' % (res.dma_done, res.dma_tmo))
            # the sweep's lines: each clock's words, as hello_test computes them
            for k in range(3):
                khz = B.config_value('BCMFW_SWEEP_KHZ_%d' % k)
                tw = T.timing_words(pid, khz)
                r.check(any('bus sweep %d kHz %08x/%08x: frame' % (khz, tw[0], tw[2]) in x
                            for x in run.lines) and
                        any('bus sweep fine %d kHz %08x/%08x: frame' % (khz, tw[0], tw[2])
                            in x for x in run.lines) and
                        any('PHOTO 5%s' % 'abc'[k] in x for x in run.lines) and
                        any('PHOTO 5%s' % 'efg'[k] in x for x in run.lines),
                        'sweep %d kHz line or photo prompt missing' % khz)
            khz = B.config_value('BCMFW_LCD_BUS_KHZ')
            tw = T.timing_words(pid, khz)
            r.check(any('bus sweep fine %d kHz %08x/%08x: frame' % (khz, tw[0], tw[2]) in x
                        for x in run.lines) and
                    any('PHOTO 5d (fine pattern at our own %d MHz' % (khz // 1000) in x
                        for x in run.lines), 'fine reference line or photo prompt missing')
            r.check(any('bus sweep: 7 frames; our own words' in x for x in run.lines),
                    'sweep end line missing')
            # the forced push: the full 100 ms wait, then the CPU frame
            r.check(res.forced_bcm_us >= 100000 and res.forced_us > res.dma_frame_us,
                    'forced timeout %d us (BCM), %d us host vs %d' % (
                        res.forced_bcm_us, res.forced_us, res.dma_frame_us))
            r.check(0 < res.dma_bcm_us < 20000 and 0 < res.dma_bcm_rect_us < 5000,
                    'DMA frame %d us, rect %d us on the BCM counter' % (
                        res.dma_bcm_us, res.dma_bcm_rect_us))
            r.check(res.img2 == run.addr('hello') and res.img3 == run.addr('hello_dma'),
                    'passed images not hello / hello_dma')
            r.check(not run.b.dma.errors and run.b.panel.fifo_stray == 0,
                    'DMA errors %s, stray FIFO words %d' % (run.b.dma.errors[:2],
                                                           run.b.panel.fifo_stray))
            cuts = [e[0] for e in run.events if e[0] in ('cut', 'restore')]
            r.check(cuts == ['cut', 'cut', 'restore'] * 3, 'cut/restore order %s' % cuts)
            self.ended_clean(r, run, True)
            if pid == 0:
                T.png_write(os.path.join(DOCS, 'device-session-1-photo5.png'),
                            self.boxed3)
                T.png_write(os.path.join(DOCS, 'device-session-1-photo5d.png'),
                            self.fine)
                for k in ('dma_first_us', 'dma_frame_us', 'dma_rect_us', 'forced_us',
                          'dma_bcm_us', 'dma_bcm_rect_us', 'forced_bcm_us'):
                    self.metric('%s_p0' % k, getattr(res, k))
                if self.verbose:
                    print('\n'.join('    ' + x for x in run.lines))

    def t_dma_dead(self):
        r = self.row('stage 3, DMA never completes (dead request line): fails, CPU frame')
        run = Run(self.lib, self.images, 1, dreq=False)
        res = run.run(3)
        r.check(res.passed == 2 and 'DMA completion bit 8 never set' in res.why.decode(),
                'passed %d why %s' % (res.passed, res.why.decode()))
        r.check(res.img2 == run.addr('hello') and not res.img3, 'stage 3 image recorded')
        # the first DMA frame timed out and the image re-sent it by CPU
        r.check(len(run.snaps) == 3, '%d holds' % len(run.snaps))
        r.check(run.b.panel.fb == self.pattern or run.b.bus.b[0x200:0x300] ==
                self.images['hello'][0x200:0x300], 'panel not the pattern before restore')
        r.check(res.dma_tmo == 1 and res.dma_done == 0, 'done %d timeouts %d' % (
            res.dma_done, res.dma_tmo))
        r.check(any('ch1 cs' in x for x in run.lines), 'DMA registers not logged')
        self.ended_clean(r, run, True)

    def t_sweep_tmo(self):
        r = self.row('stage 3, DMA dies during the bus sweep: sweep ends, forced '
                     'timeout skipped, stage passes')
        run = Run(self.lib, self.images, 3, dreq_off_at=6)     # after photo 5a
        res = run.run(3)
        r.check(res.passed == 3 and run.ret == 3, 'passed %d (%s)' % (
            res.passed, res.why.decode()))
        r.check(res.dma_done == 4 and res.dma_tmo == 1, 'done %d timeouts %d' % (
            res.dma_done, res.dma_tmo))
        r.check(len(run.snaps) == 7, '%d holds' % len(run.snaps))
        r.check(any('DMA TIMED OUT' in x for x in run.lines) and
                any('bus sweep: 2 frames, ended by a DMA timeout' in x for x in run.lines) and
                any('forced timeout skipped' in x for x in run.lines) and
                not any('PHOTO 6' in x for x in run.lines),
                'sweep timeout lines\n%s' % '\n'.join(run.lines[-12:]))
        self.ended_clean(r, run, True)

    def t_hang3(self):
        r = self.row('hello_dma hangs in its first LCD_UPDATE: timeout, cut, restore')
        imgs = dict(self.images)
        p = self.progs['hello_dma']
        a = p.symbols['lcd_update']
        imgs['hello_dma'] = patch(imgs['hello_dma'], a, 'sleep; b 0x%x' % a)
        run = Run(self.lib, imgs, 2, stock=self.images['hello'])
        res = run.run(3)
        r.check(res.passed == 2 and 'stage 3 command: timeout' in res.why.decode(),
                'passed %d why %s' % (res.passed, res.why.decode()))
        self.ended_clean(r, run, True)

    def storm_frame(self):
        """The panel after stage 5's storm: each rectangle the pattern
        scrolled by 2 i rows, as bcm-devtest-core.c's stage5() writes"""
        rows = {}
        row = (ct.c_uint16 * W)()

        def prow(y):
            if y not in rows:
                self.lib.bcmdt_pattern_row(y, row)
                rows[y] = list(row)
            return rows[y]
        out = list(self.pattern)
        for i in range(STORM):
            kind = i & 3
            _, x, y, w, h = EVERYDAY[kind]
            y0 = y + 28 * ((i >> 2) % 7) if kind == 1 else y
            for yy in range(y0, y0 + h):
                out[yy * W + x:yy * W + x + w] = prow((yy + 2 * i) % H)[x:x + w]
        return out

    def t_everyday(self):
        for pid in (1, 3):
            r = self.row('stage 5 (everyday), panel %d: storm, full frames, %d wakes, '
                         'stop, fallback' % (pid, WAKES))
            run = Run(self.lib, self.images, pid)
            res = run.run_stages(0x0F | 1 << STAGE5)
            r.check(res.passed_mask == 0x0F | 1 << STAGE5 and run.ret == 3,
                    'mask %x ret %d (%s)' % (res.passed_mask, run.ret, res.why.decode()))
            r.check(res.img5 == run.addr('hello_dma'), 'stage 5 image not hello_dma')
            r.check(res.e_rects == STORM and res.dma_tmo == 0,
                    'rects %d, DMA timeouts %d' % (res.e_rects, res.dma_tmo))
            r.check(res.e_wakes == WAKES and 0 < res.e_wake_max_us < 1000000,
                    'wakes %d, slowest %d us' % (res.e_wakes, res.e_wake_max_us))
            # (host writes take no simulated time: e_host_us can be 0 here)
            r.check(0 < res.e_full_us < 20000,
                    'full push %d us, upload %d us' % (res.e_full_us, res.e_host_us))
            r.check(res.e_stop_us < 500000 and res.e_fallback_us > 0,
                    'stop %d us, fallback %d us' % (res.e_stop_us, res.e_fallback_us))
            # stage 3's 13 holds, then: the storm's last frame, the hold
            # before the wakes, one per wake
            nw = WAKES + WAKES_OV
            r.check(len(run.snaps) == 13 + 2 + nw, '%d holds' % len(run.snaps))
            if len(run.snaps) == 13 + 2 + nw:
                r.check(run.snaps[13] == self.storm_frame(),
                        'the storm\'s last frame is not the scrolled rectangles')
                r.check(run.snaps[14] == self.pattern, 'full frames: not the pattern')
                for k in range(nw):
                    r.check(run.snaps[15 + k] == self.pattern,
                            'wake %d: not the pattern' % (k + 1))
            dma = self.images['hello_dma']
            r.check(run.booted == [self.images[n] for n in ('probe', 'hello')] +
                    [dma] * (2 + nw),
                    'booted %d images, not probe, hello, hello_dma x %d' % (
                        len(run.booted), 2 + nw))
            # dev8: the upload lab (every method's readback right, boost
            # balanced) and the power-on-first wakes
            lab = [x for x in run.lines if ' lab ' in x and 'full frame' in x]
            r.check(len(lab) == 2 * UP_N and all('readback ok' in x for x in lab) and
                    res.e_lab_bad == 0 and run.boosts == [False, True, False] and
                    any('two-pixel rows' in x for x in run.lines),
                    'lab lines %s, bad %x, boosts %s' % (lab[:2], res.e_lab_bad,
                                                        run.boosts))
            r.check(0 < res.e_ov_wake_max_us < 1000000 and
                    sum('power-on first' in x for x in run.lines) == WAKES_OV,
                    'power-on-first wakes: slowest %d us' % res.e_ov_wake_max_us)
            r.check(any('stop: fault word seen after' in x for x in run.lines) and
                    any("fallback to Apple's vmcs took" in x for x in run.lines) and
                    sum(x.split(None, 1)[1].startswith('storm ') and 'upload' in x
                        for x in run.lines) == 4,
                    'stop, fallback or storm lines missing')
            r.check(not run.b.dma.errors and run.b.panel.fifo_stray == 0,
                    'DMA errors %s, stray FIFO words %d' % (run.b.dma.errors[:2],
                                                           run.b.panel.fifo_stray))
            # stage 5's power cuts: one per sleep (stock order) and the stop
            cuts = [e[0] for e in run.events if e[0] in ('cut', 'restore')]
            r.check(cuts == ['cut', 'cut', 'restore'] * 3 +
                    ['cut'] * (1 + WAKES + WAKES_OV) + ['cut', 'restore'],
                    'cut/restore order %s' % cuts)
            self.ended_clean(r, run, True)
            ev = RL.parse_everyday(run.lines)
            r.check(sum(x.startswith('storm ') for x in ev) >= 4 and
                    sum(x.startswith('wake ') for x in ev) == WAKES + WAKES_OV and
                    sum(x.startswith('lab ') for x in ev) == 2 * UP_N + 1 and
                    any(x.startswith('restore: done; fallback') for x in ev),
                    'readlog: everyday lines %s' % ev[:6])
            # the flag names the image, as stage 5's
            buf = ct.create_string_buffer(64)
            self.lib.bcmdt_flag_format(buf, 64, 5, run.ptrs['hello_dma'])
            why = ct.c_char_p()
            arr = (ct.c_void_p * 2)(ct.cast(run.ptrs['hello_dma'], ct.c_void_p),
                                    ct.cast(run.ptrs['hello_dma_rb'], ct.c_void_p))
            got = self.lib.bcmdt_flag_match(buf.value, 5, arr, 2, ct.byref(why))
            r.check(got == run.addr('hello_dma') and buf.value.startswith(b'5 hello_dma '),
                    'stage 5 flag %r does not match (%s)' % (buf.value, why.value))
            if pid == 1:
                for k in ('e_host_us', 'e_rect_max_us', 'e_full_us', 'e_wake_max_us',
                          'e_stop_us', 'e_fallback_us'):
                    self.metric('%s_p1' % k, getattr(res, k))
                if self.verbose:
                    print('\n'.join('    ' + x for x in run.lines))

    def t_everyday_lab(self):
        """dev8's upload lab: a PP DMA that never reaches the port is logged
        BAD by its readback, one that never ends is logged as not finished;
        neither fails stage 5, and the clock boost is always given back.
        A HAL without the lab's calls skips it."""
        for mode in ('drop', 'noop', 'hang', None):
            r = self.row('stage 5 upload lab: %s' % (
                {'drop': 'PP DMA writes zeros (readback BAD, stage passes)',
                 'noop': 'PP DMA moves nothing over the CPU\'s frame (the '
                         'pre-filled pattern reads back BAD; S8, D1)',
                 'hang': 'PP DMA never ends (logged, stage passes)',
                 None: 'HAL without the lab: skipped'}[mode]))
            run = Run(self.lib, self.images, 3, ppdma=mode, lab=mode is not None)
            res = run.run_stages(0x0F | 1 << STAGE5)
            r.check(res.passed_mask == 0x0F | 1 << STAGE5,
                    'mask %x (%s)' % (res.passed_mask, res.why.decode()))
            lab = [x for x in run.lines if ' lab ' in x]
            if mode is None:
                r.check(not lab and not run.boosts, 'lab ran: %s' % lab[:1])
                continue
            r.check(res.e_lab_bad == (1 << 2 | 1 << 5), 'bad bits %x' % res.e_lab_bad)
            word = 'did not finish' if mode == 'hang' else 'BAD'
            r.check(sum('PP DMA' in x and word in x for x in lab) == 2 and
                    sum('readback ok' in x for x in lab) == 4,
                    'lab lines %s' % lab)
            r.check(run.boosts and run.boosts[-1] is False, 'boost left on %s' % run.boosts)
            self.ended_clean(r, run, True)

    def t_strikes(self):
        """The boot path's strike rule (dev8): a run-time fallback is one
        strike a boot; "test fallback" and the device test taking the BCM
        exercise the same path and count nothing; three strikes stop the
        swap."""
        r = self.row('boot path strikes: test fallback none, a real one +1, '
                     'three block')
        nxt, blk = self.lib.bcmdt_strike_next, self.lib.bcmdt_strike_blocks
        blk.restype = ct.c_bool
        for on_disk, real, asked, want in ((0, 0, 1, -1), (1, 0, 3, -1),
                                           (0, 1, 0, 1), (1, 2, 0, 2),
                                           (2, 1, 1, 3), (-1, 1, 0, 1),
                                           (9, 1, 0, 9)):
            got = nxt(on_disk, real, asked)
            r.check(got == want, 'on disk %d, real %d, asked %d: %d, not %d' % (
                on_disk, real, asked, got, want))
        r.check([blk(n) for n in range(5)] == [False, False, False, True, True],
                'blocks at %s' % [blk(n) for n in range(5)])

    def t_release_gates(self):
        """p19-bcm-release: a build without the device test takes the
        resident rung only for an image verified.txt lists (else the DMA
        image, then Apple's vmcs), and clears the attempt marker and the
        strikes once a build: only when the build ID on disk differs."""
        r = self.row('release gates: the resident rung verified only, the '
                     're-arm once a build')
        gate = self.lib.bcmdt_res_gate
        gate.restype = ct.c_char_p
        gate.argtypes = [ct.c_bool] * 5
        for args, want in (((1, 1, 0, 0, 0), None),     # developer: unverified ok
                           ((1, 1, 0, 1, 1), None),     # release, verified
                           ((1, 1, 0, 1, 0), b'not in verified.txt'),
                           ((0, 1, 0, 1, 1), b'setting off'),
                           ((1, 0, 0, 1, 1), b'no image offered'),
                           ((1, 1, 1, 1, 1), b'skipped: it failed')):
            got = gate(*args)
            ok = got is None if want is None else (got or b'').startswith(want)
            r.check(ok, 'gate%s: %r, not %r' % (args, got, want))
        due = self.lib.bcmdt_rearm_due
        due.restype = ct.c_bool
        due.argtypes = [ct.c_char_p, ct.c_char_p]
        for stored, build, want in ((None, b'r1-251025', True),
                                    (b'r1-251025', b'r1-251025', False),
                                    (b'r1-251025\n', b'r1-251025', False),
                                    (b'r0-251024', b'r1-251025', True),
                                    (b'r1-2510', b'r1-251025', True),
                                    (b'r1-251025x', b'r1-251025', True),
                                    (b'r1-251025', b'', False),
                                    (None, None, False)):
            got = due(stored, build)
            r.check(got == want, 'rearm(%r, %r): %s, not %s' % (stored, build, got, want))

    def t_everyday_needs3(self):
        r = self.row('stage 5 without stage 3 in the run: skipped, nothing booted for it')
        run = Run(self.lib, self.images, 2)
        res = run.run_stages(0x03 | 1 << STAGE5)
        r.check(not res.passed_mask & (1 << STAGE5) and
                any('stage 5 skipped: needs stage 3' in x for x in run.lines),
                'mask %x' % res.passed_mask)
        r.check(run.booted == [self.images['probe']], 'booted %d images' % len(run.booted))
        self.ended_clean(r, run, True)

    def t_everyday_hang(self):
        r = self.row('stage 5, image hangs in an UPDATERECT mid-storm: timeout, cut, '
                     'restore')
        imgs = dict(self.images)
        p = self.progs['hello_dma']
        a = p.symbols['lcd_update_rect']
        # stage 3 boots the good copy; stage 5 the hanging one
        hung = patch(imgs['hello_dma'], a, 'sleep; b 0x%x' % a)
        run = Run(self.lib, imgs, 1)
        # boot the hanging copy only for stage 5 (its first boot)
        orig = run._boot
        seen = []

        def boot(pp, n):
            seen.append(n)
            data = ct.string_at(pp, n)
            if len(seen) == 4:      # probe, hello, hello_dma (stage 3), stage 5
                buf = (ct.c_uint8 * len(hung)).from_buffer_copy(hung)
                run._keep.append(buf)
                return orig(ct.cast(buf, ct.POINTER(ct.c_uint8)), len(hung))
            return orig(pp, n) if data else -1
        run.hal.boot = BOOT(boot)
        run._keep.append(run.hal.boot)
        res = run.run_stages(0x0F | 1 << STAGE5)
        r.check(res.passed_mask == 0x0F and 'stage 5 command: timeout' in res.why.decode(),
                'mask %x why %s' % (res.passed_mask, res.why.decode()))
        r.check(not res.img5, 'a stage 5 image was recorded')
        self.ended_clean(r, run, True)

    def t_flags(self):
        r = self.row('pass flags: format, match, and every way one is void')
        lib = self.lib
        run = Run(self.lib, self.images, 0)
        p = run.ptrs
        buf = ct.create_string_buffer(64)
        n = lib.bcmdt_flag_format(buf, 64, 2, p['hello'])
        flag = buf.value
        img = self.images['hello']
        r.check(flag == b'2 hello %08x %08x\n' % (len(img), fnv(img)) and n == len(flag),
                'flag text %r' % flag)
        why = ct.c_char_p()
        arr2 = (ct.POINTER(Image) * 2)(p['hello'], p['hello_rb'])
        arr3 = (ct.POINTER(Image) * 2)(p['hello_dma'], p['hello_dma_rb'])

        def match(text, stage=2, arr=arr2):
            got = lib.bcmdt_flag_match(text, stage, arr, 2, ct.byref(why))
            return got, why.value.decode()
        got, w = match(flag)
        r.check(got == run.addr('hello') and w == 'ok', 'good flag: %s %s' % (got, w))
        got, w = match(flag.rstrip(b'\n'))
        r.check(got == run.addr('hello'), 'flag without newline: %s' % w)
        lib.bcmdt_flag_format(buf, 64, 3, p['hello_dma_rb'])
        got, w = match(buf.value, 3, arr3)
        r.check(got == run.addr('hello_dma_rb'), 'stage 3 flag: %s' % w)
        bad = {
            'other stage': (flag, 3, arr3),
            'stage 3 flag as stage 2': (buf.value, 2, arr2),
            'fnv changed': (flag[:-3] + (b'0' if flag[-3:-2] != b'0' else b'1') +
                            flag[-2:], 2, arr2),
            'size changed': (b'2 hello %08x %08x\n' % (len(img) + 4, fnv(img)), 2, arr2),
            'unknown name': (b'2 hullo %08x %08x\n' % (len(img), fnv(img)), 2, arr2),
            'CPU name for DMA': (flag.replace(b'2 ', b'3 ', 1), 3, arr3),
            'garbage': (b'2 hello xyz', 2, arr2),
            'empty': (b'', 2, arr2),
            'trailing junk': (flag.rstrip(b'\n') + b' 1\n', 2, arr2),
            'upper-case hex': (flag.upper().replace(b'HELLO', b'hello'), 2, arr2)
            if any(c in b'abcdef' for c in flag.split()[2] + flag.split()[3])
            else (b'x', 2, arr2),
        }
        for name, (text, st, arr) in bad.items():
            got, w = match(text, st, arr)
            r.check(not got, '%s: matched (%s)' % (name, w))
        # the embedded data changed but the flag and the table entry did not
        data = bytearray(self.images['hello'])
        data[0x300] ^= 1
        buf2 = (ct.c_uint8 * len(data)).from_buffer_copy(bytes(data))
        im = Image(ct.cast(buf2, ct.POINTER(ct.c_uint8)), len(img), fnv(img), b'hello')
        arr = (ct.POINTER(Image) * 1)(ct.pointer(im))
        got = lib.bcmdt_flag_match(flag, 2, arr, 1, ct.byref(why))
        r.check(not got and why.value.startswith(b'void'),
                'changed data matched (%s)' % why.value)

    def swap_run(self, name, imgs=None, pid=1):
        run = Run(self.lib, imgs or self.images, pid, stock=self.images['hello'])
        run.h.fb = T.random_frame(random.Random(pid))
        assert run._take() == 0         # the device's caller takes it first
        res = run.swap(name)
        return run, res

    def t_swap_ok(self):
        for name in ('hello', 'hello_dma'):
            r = self.row('boot path: swap to %s, the host frame shown, confirmed' % name)
            run, res = self.swap_run(name)
            r.check(run.ret == 0, 'swap returned %d (%s)' % (run.ret, res.why.decode()))
            r.check(run.booted == [self.images[name]], 'booted something else')
            r.check(run.b.panel.fb == run.h.fb, 'panel does not show the host frame')
            r.check(run.b.bus.b[0x200:len(self.images[name])] ==
                    self.images[name][0x200:], 'BCM SRAM does not hold %s' % name)
            kinds = [e[0] for e in run.events if e[0] in ('take', 'cut', 'boot', 'restore')]
            r.check(kinds == ['take', 'cut', 'boot'], 'events %s' % kinds)
            r.check(run.events[-2:] != [] and ('light', True) in run.events,
                    'backlight not back on')
            if name == 'hello_dma':
                r.check(res.dma_done == 1 and res.dma_tmo == 0,
                        'DMA done %d timeouts %d' % (res.dma_done, res.dma_tmo))
            self.metric('swap_%s_first_us' % name, res.first_us)
            self.metric('swap_%s_ready_us' % name, res.ready_us)

    def t_swap_bad(self):
        cases = (
            ('never ready', 'hello', lambda p: 'sleep; b 0x%x' % p.symbols['_start'],
             '_start', 'ready: timeout'),
            ('faults at start', 'hello', lambda p: 'b 0x%x' % p.symbols['bcmfw_fault'],
             '_start', 'ready: fault'),
            ('hangs in its first frame', 'hello_dma',
             lambda p: 'sleep; b 0x%x' % p.symbols['lcd_update'], 'lcd_update',
             'first frame: timeout'),
        )
        for what, name, text, sym, expect in cases:
            r = self.row('boot path: image %s: error, power cut, stock restore' % what)
            imgs = dict(self.images)
            p = self.progs[name]
            imgs[name] = patch(imgs[name], p.symbols[sym], text(p))
            run, res = self.swap_run(name, imgs)
            r.check(run.ret < 0 and expect in res.why.decode(), 'ret %d why %s' % (
                run.ret, res.why.decode()))
            r.check(not run.b.powered, 'BCM left powered for the caller')
            r.check(not [e for e in run.events if e[0] == 'restore'],
                    'the core restored by itself (the caller does)')
            # what the device's caller does next: the stock wake path
            run._restore(False)
            r.check(run.h.display_on and run.b.bus.b[0x200:len(self.images['hello'])]
                    == self.images['hello'][0x200:], 'stock restore after the swap failed')
            self.metric('swap_fail_%s_ms' % what.split()[0], round(run.b.now() / 1000.0))

    def t_ladder(self):
        """#62 level 1: our DMA image, then our CPU-push image, then the
        caller's Apple vmcs.  Each failure (never ready, the fault word, a
        hang in the first frame, a bootstrap handshake that never answers)
        cuts the power and the next rung starts cold; every wait bounded."""
        def bad(name, sym, text):
            p = self.progs[name]
            return patch(self.images[name], p.symbols[sym], text(p))
        never = lambda name: bad(name, '_start', lambda p: 'sleep; b 0x%x' % p.symbols['_start'])
        fault = lambda name: bad(name, '_start', lambda p: 'b 0x%x' % p.symbols['bcmfw_fault'])
        hang = lambda name: bad(name, 'lcd_update',
                                lambda p: 'sleep; b 0x%x' % p.symbols['lcd_update'])
        cases = (
            # what, dma image, cpu image (None: no rung), boot_fails, rung won
            ('DMA good', None, 'ok', 0, 0),
            ('DMA never ready', never, 'ok', 0, 1),
            ('DMA faults', fault, 'ok', 0, 1),
            ('DMA hangs in its first frame', hang, 'ok', 0, 1),
            ('DMA handshake never answers', None, 'ok', 1, 1),
            ('DMA faults, no CPU rung', fault, None, 0, -1),
            ('DMA faults, CPU hangs in its first frame', fault, hang, 0, -1),
            ('both handshakes never answer', None, 'ok', 2, -1),
        )
        for what, dma, cpu, fails, won in cases:
            r = self.row('ladder: %s: rung %s' % (what, {0: 'DMA', 1: 'CPU', -1: 'Apple'}[won]))
            imgs = dict(self.images)
            if dma:
                imgs['hello_dma'] = dma('hello_dma')
            if cpu and cpu != 'ok':
                imgs['hello'] = cpu('hello')
            run = Run(self.lib, imgs, 1, stock=self.images['hello'], boot_fails=fails)
            run.h.fb = T.random_frame(random.Random(7))
            assert run._take() == 0
            t0 = run.b.now()
            res = run.ladder(['hello_dma', 'hello' if cpu else None])
            ms = (run.b.now() - t0) / 1000.0
            r.check(run.ret == won, 'rung %d, wanted %d (%s)' % (run.ret, won, res.why.decode()))
            tried = 1 + (1 if cpu and won != 0 else 0)
            r.check(len(run.booted) == tried, 'booted %d images, wanted %d' % (
                len(run.booted), tried))
            if won >= 0:
                name = ('hello_dma', 'hello')[won]
                r.check(run.booted[-1] == imgs[name], 'the last boot was not %s' % name)
                r.check(run.b.panel.fb == run.h.fb, 'panel does not show the host frame')
                r.check(run.b.powered, 'BCM off after a confirmed rung')
                r.check(res.panel_id == 1, 'panel id %d' % res.panel_id)
            else:
                r.check(not run.b.powered, 'BCM left powered for the caller')
                r.check(not [e for e in run.events if e[0] == 'restore'],
                        'the core restored by itself (the caller does)')
                run._restore(False)
                r.check(run.h.display_on, 'stock restore after every rung failed')
            # every failure is bounded: the slowest case is two first-frame
            # timeouts (T_POWERON) plus two power cycles
            r.check(ms < 4000, 'ladder took %.0f ms' % ms)
            self.metric('ladder_%s_ms' % '_'.join(what.split()[:3]).lower(), round(ms))

    def video(self):
        """The player's image as mkvideo.py builds it (and its symbols)"""
        if 'video' not in self.progs:
            sys.path.insert(0, FW)
            import mkvideo
            prog, _ = B.build(os.path.join(OUT, 'video'), mkvideo.DEFS,
                              listing=False)
            data = prog.load_image()
            self.progs['video'] = prog
            self.images_video = data + b'\0' * (-len(data) % 4)
        return self.images_video

    def t_resident(self):
        """p14-resident: the video image as the boot path's first rung, then
        the display driver: Rockbox's own updates, sleep and wake (the wake
        reloads it), its idle sleep only while the host's gate is set; a
        video image that fails falls to our DMA image."""
        video = self.video()
        sym = self.progs['video'].symbols
        for pid in (1, 3):
            r = self.row('resident, panel %d: the video image confirmed, drives '
                         'the LCD, sleeps, wakes' % pid)
            imgs = dict(self.images, video=video)
            run = Run(self.lib, imgs, pid, stock=self.images['hello'])
            run.h.fb = T.random_frame(random.Random(pid))
            assert run._take() == 0
            t0 = run.b.now()
            res = run.ladder(['video', 'hello_dma', None])
            ms = (run.b.now() - t0) / 1000.0
            r.check(run.ret == 0, 'rung %d (%s)' % (run.ret, res.why.decode()))
            r.check(run.booted == [video], 'booted %s' % [len(x) for x in run.booted])
            r.check(run.b.panel.fb == run.h.fb, 'panel does not show the host frame')
            r.check(res.info >> 16 == 0x5042 and res.info & 0x400 and
                    res.panel_id == pid, 'info %08x panel %d' % (res.info, res.panel_id))
            self.metric('resident_p%d_swap_ms' % pid, round(ms))
            self.metric('resident_p%d_ready_us' % pid, res.ready_us)
            self.metric('resident_p%d_first_us' % pid, res.first_us)
            # the caller's dt_restore(true): the LCD driver from here on
            run._restore(True)
            h = run.h
            for k in range(3):
                h.fb = T.random_frame(random.Random(100 + k))
                h.lcd_update()
                h.settle()
            r.check(run.b.panel.fb == h.fb, 'the driver\'s update did not reach '
                    'the panel')
            # its idle gate: never asleep until the host sets it
            r.check(run.b.r32(0x1DC) == 0, 'slept %d times ungated' %
                    run.b.r32(0x1DC))
            run.b.w32(0x16C, 0x534C5031)
            run._sleep(30)
            sl, ti, bad = run.b.r32(0x1DC), run.b.r32(0x1D4), run.b.r32(0x1FC)
            r.check(sl > 0 and ti >= 20 and bad == 0,
                    'gated on: sleeps %d ticks %d bad %d' % (sl, ti, bad))
            h.fb = T.random_frame(random.Random(7))
            h.lcd_update()
            h.settle()
            r.check(run.b.panel.fb == h.fb, 'update lost while it idle-sleeps')
            run.b.w32(0x16C, 0)
            run._sleep(5)
            sl = run.b.r32(0x1DC)
            run._sleep(20)
            r.check(run.b.r32(0x1DC) == sl, 'slept after the gate was cleared')
            self.metric('resident_p%d_idle_ticks_30ms' % pid, ti)
            # sleep and wake: the stock power-down, then the wake path
            # uploads the resident image again (flash_vmcs_* point at it)
            run.b.image = video
            h.lcd_sleep()
            r.check(not run.b.powered, 'BCM powered after lcd_sleep')
            h.fb = T.random_frame(random.Random(8))
            t0 = run.b.now()
            h.lcd_awake()
            h.settle()
            self.metric('resident_p%d_wake_ms' % pid, round((run.b.now() - t0) / 1000.0))
            r.check(run.b.panel.fb == h.fb, 'wake: panel does not show the frame')
            # up to the AAC tables' load copy (M4): start-up parks those
            # and clears .bss over where they rode, so SRAM keeps only the
            # fixed part of the load image
            fixed = sym.get('__xdata_load', len(video))
            r.check(run.b.bus.b[0x200:fixed] == video[0x200:fixed],
                    'wake: BCM SRAM does not hold the video image')
            r.check(run.b.r32(0x16C) == 0, 'the gate survived the upload')
        r = self.row('resident: the video image never ready: our DMA image wins')
        imgs = dict(self.images, video=patch(video, sym['_start'],
                                             'sleep; b 0x%x' % sym['_start']))
        run = Run(self.lib, imgs, 1, stock=self.images['hello'])
        run.h.fb = T.random_frame(random.Random(9))
        assert run._take() == 0
        res = run.ladder(['video', 'hello_dma', None])
        r.check(run.ret == 1, 'rung %d (%s)' % (run.ret, res.why.decode()))
        r.check(run.booted == [imgs['video'], self.images['hello_dma']],
                'booted %s' % [len(x) for x in run.booted])
        r.check(run.b.panel.fb == run.h.fb and run.b.powered,
                'DMA rung: panel or power wrong')
        r = self.row('resident: the HAL\'s idle words match bcmfw/config.h')
        sys.path.insert(0, FW)
        import mkvideo
        want = {n: B.config_value('BCMFW_' + n, mkvideo.DEFS) for n in
                ('MB_IDLE_EN', 'IDLE_EN_MAGIC')}
        cfg = open(os.path.join(FW, 'config.h')).read()
        src = open(os.path.join(TGT, 'bcmfw-boot.c')).read()
        got = {}
        for n in ('IDLE_EN', 'IDLE_EN_MAGIC', 'IDLE_TICKS', 'IDLE_BELLS',
                  'IDLE_SLEEPS', 'IDLE_BAD'):
            for l in src.splitlines():
                f = l.split()
                if len(f) >= 3 and f[0] == '#define' and f[1] == 'BCMA_' + n:
                    got[n] = int(f[2], 0)
        r.check(got.get('IDLE_EN') == want['MB_IDLE_EN'] and
                got.get('IDLE_EN_MAGIC') == want['IDLE_EN_MAGIC'],
                'gate %s, config %s' % (got, want))
        # the clock build's counters: the second block of #defines in config.h
        blk = cfg[cfg.index('#if BCMFW_VIDEO || BCMFW_BENCH_PLL'):]
        blk = blk[:blk.index('#else')]
        cw = {}
        for l in blk.splitlines():
            f = l.split()
            if len(f) >= 3 and f[0] == '#define':
                cw[f[1].replace('BCMFW_MB_', '')] = int(f[2], 0)
        r.check(all(got.get(n) == cw.get(n) for n in
                    ('IDLE_TICKS', 'IDLE_BELLS', 'IDLE_SLEEPS', 'IDLE_BAD')),
                'counters %s, config %s' % (got, cw))
        # and the video image's own: its tick handler counts there
        r.check(self.progs['video'].symbols.get('bcmfw_idle_tick') is not None,
                'the video image has no idle tick handler')

    def t_stage0(self):
        r = self.row('stage 0 only: read-only, no power cut, no upload')
        run = Run(self.lib, self.images, 2)
        res = run.run(0)
        r.check(res.passed == 0 and res.panel_id == 2, 'passed %d id %d' % (
            res.passed, res.panel_id))
        kinds = [e[0] for e in run.events]
        r.check('boot' not in kinds and 'cut' not in kinds, 'stage 0 cut or uploaded')
        r.check([e for e in run.events if e[0] == 'restore'] == [('restore', True, True)],
                'stage 0 restore %s' % [e for e in run.events if e[0] == 'restore'])
        r.check([v for k, *v in run.events if k == 'cmd'] == [[T.LCD_UPDATE]],
                'stage 0 sent more than one LCD_UPDATE')
        self.ended_clean(r, run, False)

    def t_probe_silent(self):
        r = self.row('stage 1: probe mailbox; no panel, GPIO or clock writes')
        run = Run(self.lib, self.images, 1)
        res = run.run(1)
        r.check(res.passed == 1, 'passed %d (%s)' % (res.passed, res.why.decode()))
        r.check(res.from0 == 0 and res.info >> 16 == 0x5042 and res.info & 0x200,
                'from0 %d info %08x' % (res.from0, res.info))
        # the probe's own device writes: between its boot and its power-down
        # the panel model saw nothing, and the core wrote only the
        # interrupt controller
        b = run.b
        boot = next(e for e in run.events if e[0] == 'boot')
        cut = [e for e in run.events if e[0] == 'cut'][1]
        probe_dev = [a for a in b.bus.log[boot[2]:cut[1]] if a.rw == 'W' and
                     a.seq not in b.host_seqs]
        blocks = sorted({a.addr & ~0x3ff for a in probe_dev})
        r.check(blocks in ([0x10001000], []), 'probe core wrote %s' % [
            '%08x' % x for x in blocks])
        self.ended_clean(r, run, True)

    def t_entry0(self):
        r = self.row('core starts at 0: probe says so, stages 2-3 use the _rb images')
        run = Run(self.lib, self.images, 0, reset_pc=0,
                  stock=self.images['hello_rb'])
        res = run.run(3)
        r.check(res.passed == 3 and res.from0 == 1, 'passed %d from0 %d (%s)' % (
            res.passed, res.from0, res.why.decode()))
        r.check(run.booted[1:] == [self.images['hello_rb'], self.images['hello_dma_rb']],
                'stage 2/3 images not hello_rb / hello_dma_rb')
        r.check(res.img3 == run.addr('hello_dma_rb'), 'stage 3 image not hello_dma_rb')
        self.ended_clean(r, run, True)

    def bad_probe(self, name, text_at_start, expect, last=2):
        r = self.row(name)
        imgs = dict(self.images)
        start = self.progs['probe'].symbols['_start']
        imgs['probe'] = patch(imgs['probe'], start, text_at_start(self.progs['probe']))
        run = Run(self.lib, imgs, 0)
        res = run.run(last)
        r.check(res.passed == 0, 'passed %d' % res.passed)
        r.check(expect in res.why.decode(), 'why "%s", want "%s"' % (
            res.why.decode(), expect))
        r.check(len(run.booted) == 1, 'stage 2 ran after a failed stage 1')
        sl = [v for k, *v in run.events if k == 'cmd']
        return r, run, res, sl

    def t_never_ready(self):
        r, run, res, cmds = self.bad_probe(
            'never-ready image: timeout, power cut, stock restore',
            lambda p: 'sleep; b {pc}'.replace('{pc}', '0x%x' % p.symbols['_start']),
            'ready: timeout')
        # no commands to a dead image: the cut comes straight after
        ev = [e[0] for e in run.events]
        i = ev.index('boot')
        r.check('cmd' not in ev[i:ev.index('restore', i)],
                'commands sent to an image that never started')
        self.metric('never_ready_ms', round(run.b.now() / 1000.0))
        self.ended_clean(r, run, True)

    def t_fault(self):
        r, run, res, cmds = self.bad_probe(
            'faulting image: fault word, pc/sp logged, power cut, stock restore',
            lambda p: 'b 0x%x' % p.symbols['bcmfw_fault'], 'ready: fault')
        r.check(any('fault words' in x for x in run.lines), 'fault words not logged')
        self.ended_clean(r, run, True)

    def t_selfcheck(self):
        # make check 3 (eor) fail: its expected value no longer matches
        r = self.row('failed self-check stops before stage 2')
        imgs = dict(self.images)
        img = bytearray(imgs['probe'])
        k = img.find(struct.pack('<I', 0xF0F0F0F0))
        r.check(k > 0, 'eor constant not found')
        img[k:k + 4] = struct.pack('<I', 0xF0F0F0F1)
        imgs['probe'] = bytes(img)
        run = Run(self.lib, imgs, 3)
        res = run.run(2)
        r.check(res.passed == 0 and res.selfcheck == 0x1F7,
                'passed %d self-check %03x' % (res.passed, res.selfcheck))
        r.check(len(run.booted) == 1, 'stage 2 ran')
        self.ended_clean(r, run, True)

    # ---- probe v2: the results block in stage 1
    def p2_run(self, imgs=None, pid=1):
        run = Run(self.lib, imgs or self.images, pid)
        res = run.run(1)
        blks = RL.parse_p2(run.lines)
        out = io.StringIO()
        for b in blks:
            RL.report_p2(b, out)
        return run, res, blks, out.getvalue()

    def t_probe_v2(self):
        S = T.S
        saved = dict(S.HYP)
        try:
            for off, vec in ((None, None), (6, 17)):
                if off is None:
                    r = self.row('probe v2: checks 9, 12 fault and resume, stage 1 '
                                 'passes, readlog decodes')
                    off, vec = S.HYP['exc_pc_offset'], S.HYP['exc_vec_misaligned']
                else:
                    r = self.row('probe v2 with HYP pc offset %d, misaligned vector %d: '
                                 'readlog reports them' % (off, vec))
                    S.HYP['exc_pc_offset'], S.HYP['exc_vec_misaligned'] = off, vec
                run, res, blks, text = self.p2_run()
                r.check(res.passed == 1 and run.ret == 1, 'passed %d (%s)' % (
                    res.passed, res.why.decode()))
                r.check(res.selfcheck == res.selfcheck_all == 0x1FF and
                        res.p_faulted == 0x1200 and res.p_block == 0xCB000 and
                        res.p_pass & 0x1FFF == 0xDFF and res.p_u_status == 0xC5FF0000,
                        'self-check %03x/%03x faulted %04x block %x pass %x U %08x' % (
                            res.selfcheck, res.selfcheck_all, res.p_faulted,
                            res.p_block, res.p_pass, res.p_u_status))
                r.check(len(blks) == 1 and blks[0]['none'] is None,
                        '%d probe v2 blocks in the log' % len(blks))
                if not blks or blks[0]['none'] is not None:
                    continue
                b = blks[0]
                r.check(sorted(b['recs']) == [9, 10, 11, 12] and
                        all(x['regs'] for x in b['recs'].values()),
                        'records logged %s' % sorted(b['recs']))
                r.check(b['req'] == b['reqall'] == 0x1FF and b['infoall'] == 0x1E00 and
                        b['info'] == 0xC00 and b['flt'] == 0x1200 and
                        b['ticks'] == res.probe_ticks > 0 and
                        b['clo'][1] - b['clo'][0] == b['ticks'],
                        'summary %s' % {k: b[k] for k in ('req', 'info', 'flt', 'ticks')})
                prog = self.progs['probe']
                for i in (9, 12):
                    x = b['recs'].get(i, {})
                    # silicon (sessions 2-3): saved pc = the first instruction
                    # not run, 2 (check 9) or 4 (check 12) after the load
                    r.check(x.get('status') == 0xC5000003 | i << 16 | vec << 8 and
                            x.get('d') == prog.symbols['c%d_insn' % i] and
                            x.get('pc') == x.get('d', 0) + (6 if i == 9 else 18) + off,
                            'check %d record %s' % (i, x))
                r.check(b['recs'][9]['regs'][5] == 0x7B, 'check 9 r5')
                r.check('misaligned vector: %d (checks 9 and 12)' % vec in text,
                        'readlog vector verdict:\n' + text)
                r.check('check 9 +%d bytes, check 12 +%d bytes; 4 of check 12\'s adds '
                        'ran' % (6 + off, 18 + off) in text,
                        'readlog offset verdict:\n' + text)
                r.check('check 9 r5 7b: decremented by 4 before the fault' in text and
                        'aligned ld (--rs) as modelled' in text and
                        'ldb/stb (--rs) as modelled' in text,
                        'readlog pre-decrement verdict:\n' + text)
                r.check(not any(' P2U' in x for x in run.lines), 'record U logged')
                self.ended_clean(r, run, True)
                if self.verbose:
                    print(text)
                self.metric('p2_log_lines_off%d' % off, sum(
                    1 for x in run.lines if x.split(None, 1)[1].startswith('P2')))
        finally:
            S.HYP.clear()
            S.HYP.update(saved)

    def t_probe_v2_required(self):
        r = self.row('probe v2: a fault resumed in required check 0: stage 1 fails, '
                     'record logged')
        p = self.progs['probe']
        imgs = dict(self.images)
        imgs['probe'] = patch(imgs['probe'], p.symbols['c0'],
                              'mov r0, #0; bl 0x%x; mov r1, #0x%x; ld r0, (r1)' % (
                                  p.symbols['probe_begin'], p.symbols['probe_buf'] + 1))
        run, res, blks, text = self.p2_run(imgs, pid=2)
        r.check(res.passed == 0 and 'instruction self-check' in res.why.decode(),
                'passed %d why %s' % (res.passed, res.why.decode()))
        r.check(res.selfcheck == 0x1FE and res.p_faulted == 0x1201,
                'self-check %03x faulted %04x' % (res.selfcheck, res.p_faulted))
        x = blks[0]['recs'].get(0) if blks and blks[0]['none'] is None else None
        r.check(x and x['status'] & 0xFF00FF == 3 and x['regs'] and
                x['regs'][1] == p.symbols['probe_buf'] + 1,
                'check 0 record %s' % x)
        r.check('abs' in text and 'fault' in text, 'readlog does not show check 0')
        cuts = [e[0] for e in run.events if e[0] in ('cut', 'restore')]
        r.check(cuts == ['cut', 'cut', 'restore'], 'cut/restore order %s' % cuts)
        r.check(len(run.booted) == 1, 'stage 2 ran')
        self.ended_clean(r, run, True)

    def t_probe_v2_unexpected(self):
        r = self.row('probe v2: a fault outside any check: record U, legacy words, '
                     'cut, restore')
        p = self.progs['probe']
        imgs = dict(self.images)
        imgs['probe'] = patch(imgs['probe'], p.symbols['c_done'],
                              'mov r2, #0x%x; mov r0, #-1; st r0, (r2); mov r1, #0x%x; '
                              'ld r0, (r1)' % (p.symbols['bcmfw_probe_cur'],
                                               p.symbols['probe_buf'] + 1))
        run, res, blks, text = self.p2_run(imgs, pid=0)
        vec = T.S.HYP['exc_vec_misaligned']
        r.check(res.passed == 0 and 'ready: fault' in res.why.decode(),
                'passed %d why %s' % (res.passed, res.why.decode()))
        k = next((n for n, x in enumerate(run.lines) if 'fault words' in x), None)
        r.check(k is not None and any(' P2U ' in x for x in run.lines[k:]),
                'no fault words, or no P2U line after them')
        u = blks[0]['recs'].get('U') if blks and blks[0]['none'] is None else None
        # r1 is not checked: the exception is imprecise (vc2sim, as the
        # silicon), so the instructions after the load have run
        r.check(u and u['status'] == 0xC5FF0003 | vec << 8 and u['regs'] and
                res.fault_pc == u['pc'] and res.fault_sp == u['sp'],
                'record U %s, legacy pc %08x sp %08x' % (u, res.fault_pc, res.fault_sp))
        r.check(res.p_u_status & 0xFF == 3, 'p_u_status %08x' % res.p_u_status)
        r.check('unexpected fault' in text, 'readlog does not show record U')
        r.check(any('cutting power (image not answering)' in x for x in run.lines),
                'not a direct power cut')
        cuts = [e[0] for e in run.events if e[0] in ('cut', 'restore')]
        r.check(cuts == ['cut', 'cut', 'restore'], 'cut/restore order %s' % cuts)
        self.ended_clean(r, run, True)

    def t_hang_in_command(self):
        r = self.row('hello hangs in its first LCD_UPDATE: timeout, cut, restore')
        imgs = dict(self.images)
        p = self.progs['hello']
        a = p.symbols['lcd_update']
        imgs['hello'] = patch(imgs['hello'], a, 'sleep; b 0x%x' % a)
        run = Run(self.lib, imgs, 0, stock=self.images['hello'])
        res = run.run(2)
        r.check(res.passed == 1 and 'stage 2 command: timeout' in res.why.decode(),
                'passed %d why %s' % (res.passed, res.why.decode()))
        ev = run.events
        i = max(k for k, e in enumerate(ev) if e[0] == 'boot')
        after = [e for e in ev[i:] if e[0] in ('cmd', 'cut')]
        r.check(after and after[-1][0] == 'cut' and
                sum(1 for e in after if e[0] == 'cmd') == 1,
                'after the hang: %s' % after)
        self.ended_clean(r, run, True)

    # ---- hello v2: where an image stops, clock gates (vc2mmio.ClockBlock)
    def hello_words(self, run):
        out = io.StringIO()
        RL.report_hello(RL.parse_hello(run.lines), out)
        return out.getvalue()

    def stall_gate(self, run, n):
        """The LCD interface stops answering at its n-th access after our
        hello image boots (a core stalled on the bus), until the power cut."""
        seen, cut = [0], [False]
        cut0 = run.b.power_cut

        def gate():
            if cut[0] or len(run.booted) < 2:
                return True
            seen[0] += 1
            return seen[0] < n

        def power_cut(rng):
            cut[0] = len(run.booted) >= 2
            cut0(rng)
        run.b.panel.gate = gate
        run.b.power_cut = power_cut

    def t_stall_mid_sequence(self):
        r = self.row('hello stalls on the panel bus mid-sequence: step names the record')
        run = Run(self.lib, self.images, 3)
        self.stall_gate(run, 60)
        res = run.run(2)
        text = self.hello_words(run)
        r.check(res.passed == 1 and 'stage 2 command: timeout' in res.why.decode(),
                'passed %d why %s' % (res.passed, res.why.decode()))
        k = next((n for n, x in enumerate(run.lines) if 'image words: step' in x), None)
        r.check(k is not None and ' script record ' in run.lines[k] and
                ' script record ' in text,
                'step line %s\n%s' % (run.lines[k] if k is not None else None, text))
        r.check(any('image words: limit 00000000' in x for x in run.lines),
                'limit word logged')
        r.check(any('cutting power (image not answering)' in x for x in run.lines),
                'not a direct power cut')
        self.ended_clean(r, run, True)

    def t_clock_gates(self):
        r = self.row('clock gates: counter never runs (spin delays), started by 0x4C, '
                     'by 0x58: logged and decoded')
        for timer_on, want in ((None, 'still STOPPED after 0x4C and 0x58'),
                               ('4c', 'counter started by 0x4C/0x54'),
                               ('58', 'counter started by 0x58')):
            run = Run(self.lib, self.images, 3)
            run.b.gate_clocks(timer_on)
            res = run.run(2)
            text = self.hello_words(run)
            r.check(res.passed == 2, '%s: passed %d why %s' % (
                timer_on, res.passed, res.why.decode()))
            n = sum(1 for x in run.lines if want in x)
            r.check(n == 2, '%s: %d "%s" lines (probe, hello)' % (timer_on, n, want))
            r.check(any('found 4c 00000000 54 00000000 58 00000000' in x
                        for x in run.lines), '%s: registers as found' % timer_on)
            r.check(('ticks over spin(1000)' in text) and
                    (('M iterations/s' in text) == (timer_on is not None)),
                    '%s: readlog\n%s' % (timer_on, text))
            self.ended_clean(r, run, True)
        r = self.row('clock gates: LCD interface clock off: stage 2 times out at '
                     'power-on step 0, cut, restore')
        # Apple's vmcs (the stand-in) has the clock; our image does not:
        # its very first interface access never completes
        run = Run(self.lib, self.images, 3)
        self.stall_gate(run, 1)
        res = run.run(2)
        text = self.hello_words(run)
        r.check(res.passed == 1 and 'stage 2 command: timeout' in res.why.decode(),
                'passed %d why %s' % (res.passed, res.why.decode()))
        r.check('stopped at step 50531000: power-on step 0' in text, text)
        self.ended_clean(r, run, True)

    def t_delay_limit(self):
        r = self.row('the counter stops mid-delay: bounded, recorded, spin delays, '
                     'stage 2 passes')
        run = Run(self.lib, self.images, 3)
        state = {'t': None}
        run.b.gate_clocks('reset')

        def timer_runs():
            # stops 50 ms into our hello image's first command
            if len(run.booted) < 2 or run.b.r32(T.CMD_WORD) != 0xFFFF:
                return True
            if state['t'] is None:
                state['t'] = run.b.timer.clo
            return run.b.timer.clo - state['t'] < 50000
        run.b.timer.gate = timer_runs
        res = run.run(2)
        text = self.hello_words(run)
        r.check(res.passed == 2, 'passed %d why %s' % (res.passed, res.why.decode()))
        r.check('a counter delay ran out on spins' in text, text)
        self.ended_clean(r, run, True)

    def t_port_dies(self):
        r = self.row('host port stops answering mid-stage: no block, cut, restore')
        probe = Run(self.lib, self.images, 0)
        probe.run(1)
        # die a little after the probe's boot
        k = probe.port_calls - 20
        run = Run(self.lib, self.images, 0, port_dies_after=k)
        res = run.run(1)
        r.check(res.passed == 0 and 'port' in res.why.decode(), 'passed %d why %s' % (
            res.passed, res.why.decode()))
        rs = [e for e in run.events if e[0] == 'restore']
        r.check(rs and rs[-1][2] is False, 'restore before the power cut')

    def t_stage0_port(self):
        r = self.row('port dead in stage 0: power cut, stock wake (no hand-back)')
        run = Run(self.lib, self.images, 0, port_dies_after=1)
        res = run.run(2)
        r.check(res.passed == -1 and 'port' in res.why.decode(), 'passed %d why %s' % (
            res.passed, res.why.decode()))
        rs = [e for e in run.events if e[0] in ('cut', 'restore', 'boot')]
        r.check([e[0] for e in rs] == ['cut', 'restore'] and rs[1][1] is False,
                'events %s' % [e[:2] for e in rs])
        r.check(run.h.display_on and run.h.tick_task, 'display not handed back')

    def t_take_refused(self):
        r = self.row('display busy/asleep: take refused, nothing touched')
        run = Run(self.lib, self.images, 0, refuse_take=True)
        res = run.run(2)
        kinds = {e[0] for e in run.events}
        r.check(run.ret == -2 and not kinds & {'boot', 'cut', 'cmd', 'restore'},
                'ret %d events %s' % (run.ret, kinds))

    # ---- stage 4 and 4b: the timing bench
    def bench_events(self, run):
        """The events from the bench's boot to the cut that ends it."""
        ev = run.events
        i = max(k for k, e in enumerate(ev) if e[0] == 'boot' and e[1] in run.benches)
        j = next(k for k in range(i, len(ev)) if ev[k][0] == 'cut')
        return i, j

    def check_bench_log(self, r, run, stage, tables, nk=69):
        runs = [x for x in RL.parse(run.lines) if x['stage'] == stage]
        r.check(len(runs) == 1, '%d stage %s runs in the log' % (len(runs), stage))
        if not runs:
            return
        x = runs[0]
        r.check(x['hdr'] and x['hdr'][0] == 0x50424E31 and x['hdr'][1] == 2 and
                x['hdr'][2] == nk, 'log header %s' % (x['hdr'] or [])[:3])
        r.check(sorted(x['tables']) == tables, 'log tables %s' % sorted(x['tables']))
        for t in tables:
            tab = x['tables'].get(t, [])
            r.check([e['tag'] for e in tab] == self.ktags,
                    'table %d tags differ from bench_tab.c' % t)
            r.check(all(0 < e['min'] <= e['max'] and e['insns'] == e['per_iter'] *
                        x['hdr'][3] for e in tab), 'table %d counts or times' % t)
        # (64 iterations run in about 6 ms simulated: within one 10 ms poll)
        r.check(x['steps'] and x['steps'][-1] == 0x50520004,
                'steps logged: %s' % ['%08x' % st for st in x['steps'][-3:]])
        # the vector check records: one B4V/B4VL pair per check, tags in order
        vr = x['vrecs']
        r.check([vr.get(k, {}).get('tag') for k in range(len(self.vtags))] ==
                self.vtags and all('late' in v for v in vr.values()) and
                x['vlate'] is not None,
                'B4V records %s' % [vr.get(k, {}).get('tag') for k in sorted(vr)])
        out = io.StringIO()
        RL.report(x, self.ktags, self.vtags, out)
        r.check('clock estimate' in out.getvalue(), 'readlog report has no clock')
        # the store-locating records (dev5): every source lands where the
        # silicon layout puts it (the default store model), the load-style
        # d08 control at +256
        r.check([s_['row'] for s_ in x['slocs']] ==
                [G.sloc_code(*rec) for rec in G.SLOC_RECS] and
                '%d of %d records where the silicon layout puts them' % (
                    len(G.SLOC_RECS), len(G.SLOC_RECS)) in out.getvalue(),
                'B4S records %s\n%s' % (x['slocs'], out.getvalue()[-600:]))
        return x

    def t_bench(self):
        for pid, stages, label in ((0, None, '0-4'), (3, 0x13, '0, 1, 4')):
            r = self.row('stage 4 timing bench, stages %s, panel %d: pass, log' % (
                label, pid))
            run = Run(self.lib, self.images, pid)
            res = run.run(4) if stages is None else run.run_stages(stages)
            want_mask = 0x1F if stages is None else 0x13
            r.check(run.ret == 4 and res.passed == 4 and res.passed_mask == want_mask,
                    'ret %d passed %d mask %x (%s)' % (run.ret, res.passed,
                                                       res.passed_mask, res.why.decode()))
            want = ['probe', 'hello', 'hello_dma', 'bench'] if stages is None \
                else ['probe', 'bench']
            r.check(run.booted == [self.images[n] for n in want],
                    'booted %d images, not %s' % (len(run.booted), want))
            r.check(res.img4 == run.addr('bench'), 'stage 4 image not recorded')
            r.check(res.b_vpass == res.b_vall == self.vall and res.b_spass == res.b_sall
                    == 0x1FF and res.b_ppass == res.b_pall == 0x1FF,
                    'checks %x/%x %x/%x %x/%x' % (res.b_vpass, res.b_vall, res.b_spass,
                                                  res.b_sall, res.b_ppass, res.b_pall))
            r.check(res.b_nk == 69 and res.b_kernels_ok == 69,
                    'kernels %d ok %d' % (res.b_nk, res.b_kernels_ok))
            # vc2sim: 100 instructions per microsecond, time moving every 32
            # instructions (ALU1 at 64 iterations is 2112: about 84 MHz)
            r.check(75000 < res.b_clock_khz <= 110000, 'clock %d kHz' % res.b_clock_khz)
            cuts = [e[0] for e in run.events if e[0] in ('cut', 'restore')]
            r.check(cuts == ['cut', 'cut', 'restore'] * (len(want)),
                    'cut/restore order %s' % cuts)
            # the bench: no panel, GPIO-output, clock or DMA writes (only the
            # interrupt controller), and no commands until it is done
            i, j = self.bench_events(run)
            b = run.b
            # (bench row ALGN's DMA checks, after the bench, use channel
            # 7 at 0x10000b80 and nothing else in the DMA block)
            dev = [a for a in b.bus.log[run.events[i][2]:run.events[j][1]]
                   if a.rw == 'W' and a.seq not in b.host_seqs and
                   not 0x10000B80 <= a.addr < 0x10000C00]
            blocks = sorted({a.addr & ~0x3ff for a in dev})
            r.check(blocks in ([0x10001000], []), 'bench core wrote %s' % [
                '%08x' % x for x in blocks])
            ev = run.events[i:j]
            done = next((k for k, e in enumerate(ev) if e[0] == 'log' and
                         'bench step 50520004' in e[1]), None)
            r.check(done is not None and not [e for e in ev[:done] if e[0] == 'cmd'],
                    'a command sent while the bench ran')
            r.check(any(e[0] == 'cmd' and e[1] == T.LCD_SLEEP for e in ev[done or 0:]),
                    'not powered down in the stock order after done')
            x = self.check_bench_log(r, run, '4', [1])
            if x:
                # bench row ALGN after the bench: every check recorded and
                # read as silicon read it (sitting 2), the DMA checks too
                # (vc2mmio's model is attached), no port hook here
                import algnlog as AL
                av = AL.verdicts(x.get('algn'))
                r.check(sorted(t for t in av if t in AL.CHECKS) == sorted(AL.CHECKS) and
                        all(av[t][2] for t in AL.CHECKS),
                        'ALGN verdicts %s' % {t: v for t, v in av.items() if not v[2]})
                r.check(any(e[0] == 'log' and 'ALGNP none' in e[1] for e in run.events),
                        'no "ALGNP none" line')
            if x:
                out = io.StringIO()
                RL.report(x, self.ktags, self.vtags, out)
                # dev6: the rounding checks read back as the model's
                r.check('vector unit: every check passes' in out.getvalue() and
                        'vmulhrs #2048 rounds ties away from zero' in out.getvalue() and
                        'vmulhr #2048 rounds ties away from zero' in out.getvalue() and
                        'vmulms #16 into 8 bits clamps' in out.getvalue() and
                        all(v['flags'] & 3 == 3 for v in x['vrecs'].values()),
                        'readlog vector verdict:\n' + out.getvalue())
            self.ended_clean(r, run, True)
            if pid == 0:
                self.metric('bench_host_ms_p0', round(res.b_us / 1000.0))
                self.metric('bench_clock_khz_p0', res.b_clock_khz)
                self.metric('bench_log_lines_p0', len(run.lines))
                if self.verbose:
                    print('\n'.join('    ' + x for x in run.lines))

    def t_bench_after_fail2(self):
        r = self.row('stage 2 hangs: stage 3 skipped, stage 4 still runs (needs 1 only)')
        imgs = dict(self.images)
        a = self.progs['hello'].symbols['lcd_update']
        imgs['hello'] = patch(imgs['hello'], a, 'sleep; b 0x%x' % a)
        run = Run(self.lib, imgs, 1, stock=self.images['hello'])
        res = run.run(4)
        r.check(res.passed == 1 and res.passed_mask == 0x13 and run.ret == 1,
                'passed %d mask %x' % (res.passed, res.passed_mask))
        r.check(any('stage 3 skipped: needs stage 2' in x for x in run.lines),
                'no skip line for stage 3')
        r.check(run.booted == [imgs['probe'], imgs['hello'], imgs['bench']],
                'booted %d images' % len(run.booted))
        r.check(res.img4 == run.addr('bench') and not res.img2, 'flag images')
        self.ended_clean(r, run, True)

    def bench_patched(self, kernel, text):
        imgs = dict(self.images)
        p = self.progs['bench']
        sym = 'bench_k_' + self.ktags[kernel].lower()
        imgs['bench'] = patch(imgs['bench'], p.symbols[sym], text(p))
        return imgs

    def t_bench_hang(self):
        k = 23
        r = self.row('bench hangs mid-run (kernel %d, %s): stall limit, step logged, '
                     'cut, restore' % (k, self.ktags[k]))
        imgs = self.bench_patched(k, lambda p: 'sleep; b 0x%x' % p.symbols[
            'bench_k_' + self.ktags[k].lower()])
        run = Run(self.lib, imgs, 2)
        t0 = run.b.now()
        res = run.run_stages(0x13)
        took = (run.b.now() - t0) / 1e6
        r.check(res.passed_mask == 0x03 and 'bench stalled: timeout' in res.why.decode(),
                'mask %x why %s' % (res.passed_mask, res.why.decode()))
        st = 0x50523000 + k
        r.check(res.b_step == st and any(('bench stopped at step %08x (timing kernel %d)'
                                          % (st, k)) in x for x in run.lines),
                'step %08x not named in the log' % res.b_step)
        x = [y for y in RL.parse(run.lines) if y['stage'] == '4']
        r.check(x and RL.step_text(x[0]['steps'][-1], self.ktags, self.vtags) ==
                'timing kernel %d (%s)' % (k, self.ktags[k]), 'readlog step name')
        i, j = self.bench_events(run)
        r.check(not [e for e in run.events[i:j] if e[0] == 'cmd'],
                'commands sent to a hung bench')
        r.check(any('cutting power (image not answering)' in x for x in run.lines),
                'not a direct power cut')
        r.check(not res.img4, 'stage 4 image recorded')
        # 3 s stall limit: the whole run well under the 20 s overall limit
        r.check(3 < took < 12, 'run took %.1f s (simulated)' % took)
        self.metric('bench_hang_s', round(took, 1))
        self.ended_clean(r, run, True)

    def t_bench_fault(self):
        k = 40
        r = self.row('bench faults mid-run (kernel %d, %s): fault word, cut, restore'
                     % (k, self.ktags[k]))
        imgs = self.bench_patched(k, lambda p: 'b 0x%x' % p.symbols['bcmfw_fault'])
        run = Run(self.lib, imgs, 0)
        res = run.run_stages(0x13)
        r.check(res.passed_mask == 0x03 and 'bench: fault' in res.why.decode(),
                'mask %x why %s' % (res.passed_mask, res.why.decode()))
        r.check(any('fault words' in x for x in run.lines), 'fault words not logged')
        r.check(any(' P2 req 1ff/1ff ' in x for x in run.lines),
                'probe v2 block not logged after the fault')
        r.check(any('bench step %08x' % (0x50523000 + k) in x for x in run.lines),
                'faulting kernel step not logged')
        self.ended_clean(r, run, True)

    def t_bench_check(self):
        r = self.row('a failed on-chip check: stage 4 fails, logged, no flag image')
        img = bytearray(self.images['bench'])
        p = self.progs['bench']
        sym = next(v for n, v in p.symbols.items() if n.endswith('want_vmax'))
        w = struct.unpack_from('<I', img, sym)[0]
        struct.pack_into('<I', img, sym, w ^ 0x00010000)
        imgs = dict(self.images, bench=bytes(img))
        run = Run(self.lib, imgs, 1)
        res = run.run_stages(0x13)
        k = self.vtags.index('VMAX')
        r.check(res.passed_mask == 0x03 and 'bench checks: status' in res.why.decode(),
                'mask %x why %s' % (res.passed_mask, res.why.decode()))
        r.check(res.b_vpass == self.vall & ~(1 << k) and not res.img4,
                'vector pass %05x img4 %s' % (res.b_vpass, res.img4))
        r.check(any(('first mismatch %08x' % (k << 16)) in x for x in run.lines),
                'first mismatch not logged')
        x = self.check_bench_log(r, run, '4', [1])
        out = io.StringIO()
        if x:
            RL.report(x, self.ktags, self.vtags, out)
        r.check('check %d (VMAX) word 0' % k in out.getvalue(),
                'readlog does not name the failed check')
        self.ended_clean(r, run, True)

    def t_bench_vstore_lost(self):
        r = self.row('a vector store lost after an ALU op (CSUM, VMAX patched): '
                     'B4V records, readlog names the cause')
        p = self.progs['bench']
        imgs = dict(self.images)
        img = imgs['bench']
        # each check's vst16 of its row replaced by nops (the 48-bit store
        # is six bytes): the store is lost, the r5 read-back still right
        import vc2dis as VD
        for tag in ('csum', 'vmax'):
            a = p.symbols['bench_vc_' + tag]
            ins = None
            while ins is None and a < p.symbols['bench_vc_' + tag] + 64:
                d = VD.decode(lambda x: img[x] | img[x + 1] << 8, a)
                if d is None:
                    break
                if d.text.startswith('vst16'):
                    ins = a
                a += 2 * len(d.ws)
            r.check(ins is not None, '%s: no vst16 found' % tag)
            if ins is None:
                return
            img = patch(img, ins, 'nop; nop; nop')
        imgs['bench'] = img
        run = Run(self.lib, imgs, 1)
        res = run.run_stages(0x13)
        kc, kv = self.vtags.index('CSUM'), self.vtags.index('VMAX')
        r.check(res.passed_mask == 0x03 and res.b_vpass == self.vall & ~(
            1 << kc | 1 << kv), 'mask %x vector pass %x' % (res.passed_mask, res.b_vpass))
        runs = [y for y in RL.parse(run.lines) if y['stage'] == '4']
        x = runs[0] if runs else None
        r.check(x is not None and x['vrecs'].get(kc, {}).get('flags', 0) >> 16 == 8 and
                x['vrecs'][kc]['got'][0] == 16 * 0xF4,     # #52 = -12 (signed)
                'CSUM record %s' % (x and x['vrecs'].get(kc)))
        out = io.StringIO()
        if x:
            RL.report(x, self.ktags, self.vtags, out)
        text = out.getvalue()
        r.check('the vst after an ALU op is lost' in text and
                'encodings wrong' not in text and 'posted' not in text,
                'readlog vector verdict:\n' + text)
        self.ended_clean(r, run, True)

    def t_bench_needs1(self):
        r = self.row('stage 4 refused when stage 1 fails or was not run')
        imgs = dict(self.images)
        start = self.progs['probe'].symbols['_start']
        imgs['probe'] = patch(imgs['probe'], start, 'sleep; b 0x%x' % start)
        run = Run(self.lib, imgs, 0)
        res = run.run(4)
        r.check(res.passed == 0 and res.passed_mask == 1, 'passed %d mask %x' % (
            res.passed, res.passed_mask))
        r.check(run.booted == [imgs['probe']], 'booted %d images' % len(run.booted))
        r.check(any('stage 4 skipped: needs stage 1' in x for x in run.lines),
                'no skip line for stage 4')
        self.ended_clean(r, run, True)
        # stages 0 and 4 only: nothing booted, Apple's vmcs handed back as is
        run = Run(self.lib, self.images, 0)
        res = run.run_stages(0x11)
        r.check(res.passed == 0 and res.passed_mask == 1 and not run.booted,
                'stages 0, 4: passed %d mask %x booted %d' % (
                    res.passed, res.passed_mask, len(run.booted)))
        r.check([e for e in run.events if e[0] in ('cut', 'restore')] ==
                [('restore', True, True)], 'not handed back running')
        self.ended_clean(r, run, False)

    def flag(self, stage, name):
        buf = ct.create_string_buffer(64)
        run = Run(self.lib, self.images, 0)
        self.lib.bcmdt_flag_format(buf, 64, stage, run.ptrs[name])
        return buf.value

    def t_4b_refused(self):
        r = self.row('stage 4b refused without a stage 4 flag for this bench image')
        good = self.flag(4, 'bench')
        bad = {
            'no flag': None,
            'empty flag': b'',
            'stage 2 flag': self.flag(2, 'hello'),
            'flag for the PLL image': self.flag(4, 'bench_pll'),
            'stale image (fnv)': good[:-3] + (b'0' if good[-3:-2] != b'0' else b'1') +
            good[-2:],
        }
        for what, text in bad.items():
            run = Run(self.lib, self.images, 0)
            res = run.run_stages(1 | 1 << STAGE4B, text)
            kinds = {e[0] for e in run.events} - {'log'}
            r.check(run.ret == -3 and not kinds and
                    res.why.decode().startswith('stage 4b refused'),
                    '%s: ret %d events %s why %s' % (what, run.ret, kinds,
                                                    res.why.decode()))

    def t_4b(self):
        r = self.row('stage 4b with the flag: 150 MHz build, both tables, restore')
        run = Run(self.lib, self.images, 1)
        res = run.run_stages(1 | 1 << STAGE4B, self.flag(4, 'bench'))
        r.check(res.passed_mask == (1 | 1 << STAGE4B) and run.ret == 0,
                'mask %x ret %d (%s)' % (res.passed_mask, run.ret, res.why.decode()))
        r.check(run.booted == [self.images['bench_pll']], 'booted something else')
        r.check(res.b_flags & 0xF == 0x7, 'flags %x' % res.b_flags)
        # vc2sim's clock doesn't change: the start-up clock reads 150 MHz
        r.check(120000 < res.b_start_khz < 180000 and 50000 < res.b_clock2_khz < 110000,
                'start-up %d kHz, ALU1 at 150 MHz %d kHz' % (res.b_start_khz,
                                                            res.b_clock2_khz))
        r.check(not res.img4, 'stage 4b recorded a stage 4 image')
        cuts = [e[0] for e in run.events if e[0] in ('cut', 'restore')]
        r.check(cuts == ['cut', 'cut', 'restore'], 'cut/restore order %s' % cuts)
        self.check_bench_log(r, run, '4b', [1, 2])
        self.ended_clean(r, run, True)

    def t_4b_flag(self):
        r = self.row('stage 4b records its image for the 4b flag ("4b ...")')
        run = Run(self.lib, self.images, 1)
        res = run.run_stages(1 | 1 << STAGE4B, self.flag(4, 'bench'))
        r.check(res.img4b == run.addr('bench_pll'), '4b image not recorded')
        f = self.flag(FLAG4B, 'bench_pll')
        r.check(f.startswith(b'4b bench_pll '), 'flag text %r' % f)
        r.check(self.lib.bcmdt_flag_match(f, FLAG4B, ct.byref(
            (ct.c_void_p * 1)(run.addr('bench_pll'))), 1, ct.byref(ct.c_char_p())) ==
            run.addr('bench_pll'), '4b flag does not match its own image')
        r.check(not self.lib.bcmdt_flag_match(f, 4, ct.byref(
            (ct.c_void_p * 1)(run.addr('bench_pll'))), 1, ct.byref(ct.c_char_p())),
            'a 4b flag passes for stage 4')

    def t_4c_refused(self):
        r = self.row('stage 4c refused without a 4b flag for this 150 MHz bench')
        for what, text in {'no flag': None,
                           'stage 4 flag': self.flag(4, 'bench'),
                           '4b flag, other image': self.flag(FLAG4B, 'bench')}.items():
            run = Run(self.lib, self.images, 0)
            res = run.run_stages(3 | 1 << STAGE4C, text)
            kinds = {e[0] for e in run.events} - {'log'}
            r.check(run.ret == -3 and not kinds and
                    res.why.decode().startswith('stage 4c refused'),
                    '%s: ret %d events %s why %s' % (what, run.ret, kinds,
                                                    res.why.decode()))

    def t_4c(self):
        r = self.row('stage 4c: up, frames on the PLL, down, sleep comes down')
        for pid in (1, 3):
            run = Run(self.lib, self.images, pid)
            res = run.run_stages(3 | 1 << STAGE4C, self.flag(FLAG4B, 'bench_pll'))
            r.check(res.passed_mask == (3 | 1 << STAGE4C), 'panel %d: mask %x (%s)' % (
                pid, res.passed_mask, res.why.decode()))
            r.check(res.img4c == run.addr('hello_dma_clk'), 'panel %d: no 4c image' % pid)
            r.check(run.booted == [self.images['probe'], self.images['hello_dma_clk']],
                    'panel %d: booted %d images' % (pid, len(run.booted)))
            r.check(len(run.snaps) == 3 and run.snaps[0] == self.pattern and
                    run.snaps[1] == self.boxed3 and run.snaps[2] == self.pattern,
                    'panel %d: photos 4c-1..3 wrong (%d holds)' % (pid, len(run.snaps)))
            r.check(any('clock up: state 434b0001' in x for x in run.lines) and
                    any('clock down: state 434b0000' in x for x in run.lines) and
                    any('after LCD_SLEEP: clock state 434b0000' in x for x in run.lines),
                    'panel %d: clock state lines' % pid)
            # Apple's 150 MHz words on the PLL, ours on the crystal
            A_ = T.TIMING_APPLE[pid]
            r.check(any("4c PLL: frame" in x and "%08x (Apple's" % A_[2] in x
                        for x in run.lines), 'panel %d: PLL frame not on Apple\'s words' % pid)
            r.check(any('4c crystal again: frame' in x and '(ours)' in x
                        for x in run.lines), 'panel %d: crystal frame not on ours' % pid)
            self.ended_clean(r, run, True)
        # 4b and 4c in one run: 4b's pass lets 4c in without a 4b flag
        run = Run(self.lib, self.images, 1)
        res = run.run_stages(3 | 1 << STAGE4B | 1 << STAGE4C, self.flag(4, 'bench'))
        r.check(res.passed_mask == (3 | 1 << STAGE4B | 1 << STAGE4C),
                '4b+4c: mask %x (%s)' % (res.passed_mask, res.why.decode()))

    def drift_ref(self, n, halves=-1):
        """The first n panels of a 5b phase as the shell draws them (s8-int
        91b06b7784): the start cover's window, then cycle 0's fade
        (xfade_rect's steps) into cover 1, then its pans: each pixel
        whole, then half-way to the next (AVG2), the half steps stopping
        after 'halves' of them (-1: never; the guard)."""
        side, pw, ph, steps, pmax = 300, 160, 240, 8, 44
        dirs = ((-1, 0), (1, 1), (0, -1), (1, 0), (-1, -1), (0, 1), (1, -1), (-1, 1))
        pans = min((7000000 - 1 - 400000) // 150000, pmax)

        def start(rng, d):
            return rng // 2 - pmax // 2 if d > 0 else rng // 2 + pmax // 2 if d < 0 \
                else rng // 2

        def win(c, p):
            d = dirs[c % 8]
            return start(side - pw, d[0]) + d[0] * p, start(side - ph, d[1]) + d[1] * p

        def cover(k):
            f = self.lib.bcmdt_drift_px
            f.restype = ct.c_uint16
            return [f(k, u, v) for v in range(side) for u in range(side)]

        def window(cv, xy):
            x, y = xy
            return [cv[(y + r) * side + x + c] for r in range(ph) for c in range(pw)]

        def weight(k):
            return 256 if k >= steps else (256 + (steps - k + 1) // 2) // (steps - k + 1)

        def xfade(dst, src, wt):
            if wt >= 256:
                return list(src)
            w32 = (wt + 4) >> 3
            iw = 32 - w32
            if w32 == 0:
                w32, iw = 1, 31
            out = []
            for d, s_ in zip(dst, src):
                if d == s_:
                    out.append(d)
                    continue
                d = (d | (d << 16)) & 0x07e0f81f
                s_ = (s_ | (s_ << 16)) & 0x07e0f81f
                r = ((d * iw + s_ * w32 + 0x02008010) >> 5) & 0x07e0f81f
                out.append((r | (r >> 16)) & 0xffff)
            return out

        c0, c1 = cover(0), cover(1)
        cur = window(c0, win(7, pans))
        out = [cur]
        target = window(c1, win(0, 0))
        for k in range(1, steps + 1):
            cur = xfade(cur, target, weight(k))
            out.append(cur)
        def avg2(a, b):
            return [(x & y) + (((x ^ y) & 0xf7de) >> 1) for x, y in zip(a, b)]

        done = 0
        for p in range(0, pans + 1):
            if len(out) >= n:
                break
            if p:
                out.append(window(c1, win(0, p)))
            if halves < 0 or done < halves:
                out.append(avg2(window(c1, win(0, p)), window(c1, win(0, p + 1))))
                done += 1
        return [tuple(f) for f in out[:n]]

    def t_5b(self):
        r = self.row('stage 5b: the drift on the PP (A) and on the BCM (B), bit for bit')
        run = Run(self.lib, self.images, 1)
        res = run.run_stages(1 | 1 << STAGE5B, None)
        r.check(run.ret == -3 and res.why.decode().startswith('stage 5b refused'),
                'no stage 5 flag: ret %d (%s)' % (run.ret, res.why.decode()))
        run = Run(self.lib, self.images, 1)
        res = run.run_stages(1 | 1 << STAGE5B, self.flag(5, 'hello_dma'))
        r.check(res.passed_mask == (1 | 1 << STAGE5B), 'mask %x (%s)' % (
            res.passed_mask, res.why.decode()))
        r.check(run.marks == ['s5_A1', 's5_B1', 's5_end'], 'marks %s' % run.marks)
        fa, fb = res.m_frames[0], res.m_frames[1]
        r.check(fa >= 10 and fb >= 10, 'frames A %d B %d (want the fade, a half step and '
                'a pan)' % (fa, fb))
        r.check(run.booted == [self.images['drift']], 'booted something else')
        r.check(run.scratch_calls == [2 * 2 * 300 * 300, 0],
                'scratch %s' % run.scratch_calls)
        r.check(False in run.boosts and True not in run.boosts, 'boost %s' % run.boosts)
        # every push: the phase's start cover, then its frames
        a_shown, b_shown = run.panels[:1 + fa], run.panels[1 + fa:]
        r.check(len(b_shown) == 1 + fb, 'pushes %d for %d + %d frames' % (
            len(run.panels), fa, fb))
        ref = self.drift_ref(max(len(a_shown), len(b_shown)))
        bad = [i for i, (got, want) in enumerate(zip(a_shown, ref)) if got != want]
        r.check(not bad, 'mode A differs from the shell\'s drift at frames %s' % bad[:5])
        bad = [i for i, (got, want) in enumerate(zip(b_shown, ref)) if got != want]
        r.check(not bad, 'mode B differs from the shell\'s drift at frames %s' % bad[:5])
        # uploads: the pattern; A's panel (240 rows) for its start and each
        # frame; B's two covers (300 rows each), nothing a frame.  (The
        # PP's busy time is 0 here: the simulator's clock is the BCM's.)
        want = H + H * (1 + fa) + 2 * 300
        r.check(run.wr_blocks == want, 'uploads %d rows, want %d' % (run.wr_blocks, want))
        r.check(res.m_bcm_us[0] > 0 and res.m_bcm_us[1] > 0 and res.m_wait_us[1] > 0,
                'BCM busy A %d B %d us, B waited %d' % (
                    res.m_bcm_us[0], res.m_bcm_us[1], res.m_wait_us[1]))
        r.check(any('B: the BCM fades and pans' in ln for ln in run.lines),
                'no mode B phase line')
        r.check(any('s5_B1:' in ln and 'half steps' in ln and 'would not trip' in ln
                    for ln in run.lines), 'no B half-step line')
        rep = RL.drift_verdict(run.lines) or []
        r.check(len(rep) >= 2 and rep[1].startswith('5b B: %d frames' % fb),
                'readlog: %s' % rep)
        self.metric('5b A BCM us/frame', res.m_bcm_us[0] // max(fa, 1))
        self.metric('5b B BCM us/frame', res.m_bcm_us[1] // max(fb, 1))
        self.ended_clean(r, run, True)

    def t_5b_guard(self):
        r = self.row('stage 5b: the half steps\' guard turns A\'s off; B logs it would trip')
        self.lib.bcmdt_set_half_guard(0, 1)     # every half step slow, one is enough
        try:
            run = Run(self.lib, self.images, 1)
            res = run.run_stages(1 | 1 << STAGE5B, self.flag(5, 'hello_dma'))
        finally:
            self.lib.bcmdt_set_half_guard(25000, 4)
        fa, fb = res.m_frames[0], res.m_frames[1]
        r.check(res.passed_mask == (1 | 1 << STAGE5B), 'mask %x (%s)' % (
            res.passed_mask, res.why.decode()))
        r.check(any('half steps off' in ln for ln in run.lines), 'A: no "half steps off"')
        r.check(any('would trip the shell\'s guard' in ln for ln in run.lines),
                'B: no "would trip" line')
        a_shown, b_shown = run.panels[:1 + fa], run.panels[1 + fa:]
        ref_a = self.drift_ref(len(a_shown), halves=1)
        ref_b = self.drift_ref(len(b_shown))
        r.check(a_shown == ref_a, 'A after the guard differs from the shell at %s' % [
            i for i, (g, w) in enumerate(zip(a_shown, ref_a)) if g != w][:5])
        r.check(b_shown == ref_b, 'B (half steps kept) differs at %s' % [
            i for i, (g, w) in enumerate(zip(b_shown, ref_b)) if g != w][:5])
        self.ended_clean(r, run, True)

    def t_5b_stub(self):
        r = self.row('stage 5b without cover memory: B is the dev9 stub on hello_dma')
        run = Run(self.lib, self.images, 1, scratch=False)
        res = run.run_stages(1 | 1 << STAGE5B, self.flag(5, 'hello_dma'))
        r.check(res.passed_mask == (1 | 1 << STAGE5B), 'mask %x (%s)' % (
            res.passed_mask, res.why.decode()))
        r.check(run.marks == ['s5_A1', 's5_B1', 's5_end'], 'marks %s' % run.marks)
        r.check(res.m_frames[0] >= 3 and res.m_frames[1] >= 3,
                'frames A %d B %d' % (res.m_frames[0], res.m_frames[1]))
        # A uploads its 160-pixel rows every frame, B (the stub) nothing:
        # the pattern's own upload, then 240 per mode A frame
        r.check(run.wr_blocks == H + H * res.m_frames[0],
                'uploads %d rows for %d A frames' % (run.wr_blocks, res.m_frames[0]))
        r.check(run.booted == [self.images['hello_dma']], 'booted something else')
        r.check(any('no scratch memory' in ln for ln in run.lines), 'no stub line')
        self.ended_clean(r, run, True)

    def t_5i(self):
        r = self.row('stage 5i refused without a stage 5 flag; nothing booted')
        run = Run(self.lib, self.images, 1)
        res = run.run_stages(1 | 1 << STAGE5I, None)
        r.check(run.ret == -3 and res.why.decode().startswith('stage 5i refused'),
                'no stage 5 flag: ret %d (%s)' % (run.ret, res.why.decode()))
        r.check(not run.booted and not any(e[0] == 'take' for e in run.events),
                'something was touched')
        for bell in (False, True):
            r = self.row('stage 5i, doorbell %s: busy vs idle, unlit-face hold, wake'
                         % ('raises source 23' if bell else 'never fires'))
            run = Run(self.lib, self.images, 1, bell=bell, spin=True)
            res = run.run_stages(1 | 1 << STAGE5I, self.flag(5, 'hello_dma'))
            r.check(res.passed_mask == (1 | 1 << STAGE5I), 'mask %x (%s)' % (
                res.passed_mask, res.why.decode()))
            r.check(run.booted == [self.images['hello_dma']] +
                    [self.images['hello_dma_idle']] * (1 + I_WAKES),
                    'booted %s' % [len(x) for x in run.booted])
            r.check(res.img5i == run.addr('hello_dma_idle'), 'no flag image')
            r.check(res.i_bad == 0, '%d sleeps came back with r0-r5 changed' % res.i_bad)
            # one a millisecond over the hold (its updates make it a little
            # longer than I_HOLD_MS; the stage itself checks against the
            # time it measured)
            r.check(I_HOLD_MS * 9 // 10 <= res.i_ticks <= I_HOLD_MS * 13 // 10,
                    'ticks %d over a %d ms hold' % (res.i_ticks, I_HOLD_MS))
            if bell:
                r.check(res.i_rect_max_us[1] < res.i_rect_max_us[0] + 500,
                        'woken by the doorbell, the idle rect still waits for a '
                        'tick: %d vs %d us' % (res.i_rect_max_us[1],
                                               res.i_rect_max_us[0]))
            sent = 2 * I_N + 1 + I_HOLD_MS // I_FACE_MS
            r.check((res.i_bells >= sent) if bell else res.i_bells == 0,
                    'doorbells %d (%d commands)' % (res.i_bells, sent))
            r.check(res.i_sleeps > res.i_ticks, 'sleeps %d ticks %d' % (
                res.i_sleeps, res.i_ticks))
            r.check(res.i_late == 0, '%d late face updates' % res.i_late)
            r.check(run.marks == ['s5i_hold', 's5i_end'], 'marks %s' % run.marks)
            lines = [x.split(None, 1)[1] for x in run.lines]
            v = [x for x in lines if x.startswith('5i doorbell:')]
            r.check(v and (('every command' in v[0]) if bell else ('never seen' in v[0])),
                    'doorbell verdict %s' % v)
            r.check(any(x == 'stage 5i PASS' for x in lines), 'no PASS line')
            r.check(res.i_fb_bad == 0 and any(
                x.startswith('5i hold: framebuffer 0 of %d words' % (W * H // 2))
                for x in lines), 'SRAM hold: %d words changed' % res.i_fb_bad)
            self.ended_clean(r, run, True)
            self.metric('5i_ticks_%s' % ('bell' if bell else 'tick'), res.i_ticks)
            self.metric('5i_rect_max_us_%s' % ('bell' if bell else 'tick'),
                        '%d busy / %d idle' % (res.i_rect_max_us[0], res.i_rect_max_us[1]))

    def t_5i_regs(self):
        r = self.row('stage 5i: a tick handler that clobbers r2 fails it, counted')
        imgs = dict(self.images)
        a = self.progs['hello_dma_idle'].symbols['bcmfw_idle_tick']
        # its third instruction, st r1, (sp+4) (2 bytes), becomes mov r2, #1:
        # r2 is changed and r1 comes back from an unwritten stack word
        imgs['hello_dma_idle'] = patch(self.images['hello_dma_idle'], a + 4,
                                       'mov r2, #1')
        run = Run(self.lib, imgs, 1, spin=True)
        res = run.run_stages(1 | 1 << STAGE5I, self.flag(5, 'hello_dma'))
        r.check(not res.passed_mask & (1 << STAGE5I), 'passed with a clobbering handler')
        r.check(res.i_bad > 0, 'bad count %d' % res.i_bad)
        r.check(not res.img5i, 'flag image recorded')
        self.ended_clean(r, run, True)

    def t_5s(self):
        r = self.row('stage 5s refused without a stage 5 flag; nothing booted')
        run = Run(self.lib, self.images, 3)
        res = run.run_stages(1 | 1 << STAGE5S, None)
        r.check(run.ret == -3 and res.why.decode().startswith('stage 5s refused'),
                'no stage 5 flag: ret %d (%s)' % (run.ret, res.why.decode()))
        r.check(not run.booted and not any(e[0] == 'take' for e in run.events),
                'something was touched')
        f5 = self.flag(5, 'hello_dma')
        for pid, flags, sleep in ((3, f5, False), (1, f5, False),
                                  (3, f5 + self.flag(FLAG5I, 'hello_dma_idle'), True)):
            r = self.row('stage 5s, panel %d, core sleep %s: standby light/deep, '
                         'meter phases, engine-2 boot' % (pid, 'on' if sleep else 'off'))
            run = Run(self.lib, self.images, pid, spin=True)
            res = run.run_stages(1 | 1 << STAGE5S, flags)
            r.check(res.passed_mask == (1 | 1 << STAGE5S), 'mask %x (%s)' % (
                res.passed_mask, res.why.decode()))
            big = self.images['hello_dma_sby']
            r.check(run.booted[:1] == [big] and len(run.booted) == 5 and
                    all(x[:len(big)] == big and len(x) == S_BIG for x in run.booted[1:]),
                    'booted %s' % [len(x) for x in run.booted])
            r.check(run.boot_hows == [(0, S_BIG, 0), (1, S_BIG, 3)] * 2,
                    'boot uploads %s' % run.boot_hows)
            r.check(res.s_slept == sleep, 'slept %d' % res.s_slept)
            if sleep:
                r.check(res.s_ticks > 0 and res.s_sleeps > 0 and res.s_bad == 0,
                        'idle counters %d/%d/%d' % (res.s_ticks, res.s_sleeps, res.s_bad))
            # panel 1 has no light standby: refused, deep only
            want_ref = 0 if pid == 3 else 1 << 1
            r.check(res.s_refused == want_ref, 'refused %x' % res.s_refused)
            # a wake from standby skips the reset pulse and its wait
            for d in ((0, 1) if pid == 3 else (1,)):
                r.check(0 < res.s_pon_us[d] and
                        res.s_pon_us[d] + 100000 < res.s_cold_pon_us,
                        'depth %d: panel %d us vs cold %d us' % (
                            d + 1, res.s_pon_us[d], res.s_cold_pon_us))
            r.check(abs(res.s_reset_pon_us - res.s_cold_pon_us) < 20000,
                    'reset power-on %d vs cold %d us' % (res.s_reset_pon_us,
                                                         res.s_cold_pon_us))
            r.check(res.s_fb_bad == 0, 'framebuffer changed %d' % res.s_fb_bad)
            r.check(res.s_up_bad[0] == 0 and res.s_up_bad[1] == 0 and
                    res.s_pieces == 3 and res.s_up_bytes == S_BIG,
                    'uploads wrong %d/%d, %d pieces' % (res.s_up_bad[0],
                                                        res.s_up_bad[1], res.s_pieces))
            r.check(run.marks == ['s5s_on', 's5s_light', 's5s_deep', 's5s_reset',
                                  's5s_cut', 's5s_end'], 'marks %s' % run.marks)
            # the panel: the pattern after every wake (the last frame of the
            # stage is the engine-2 boot's pattern too)
            r.check(run.b.panel.fb == self.pattern or any(
                snap == self.pattern for snap in run.snaps), 'pattern never shown')
            looks = [x for x in run.snaps if x == self.pattern]
            r.check(len(looks) == (4 if pid == 3 else 3),
                    '%d of %d holds showed the pattern' % (len(looks), len(run.snaps)))
            lines = [x.split(None, 1)[1] for x in run.lines]
            r.check(any(x == 'stage 5s PASS' for x in lines), 'no PASS line')
            self.ended_clean(r, run, True)
            self.metric('5s_pon_us_p%d%s' % (pid, '_sleep' if sleep else ''),
                        'light %d deep %d reset %d cold %d' % (
                            res.s_pon_us[0], res.s_pon_us[1], res.s_reset_pon_us,
                            res.s_cold_pon_us))

        r = self.row('stage 5s: a 5i flag for another build: core sleep stays off')
        bad5i = self.flag(FLAG5I, 'hello_dma_idle')
        bad5i = bad5i[:-9] + b'%08x\n' % (int(bad5i[-9:-1], 16) ^ 1)
        run = Run(self.lib, self.images, 3, spin=True)
        res = run.run_stages(1 | 1 << STAGE5S, f5 + bad5i)
        r.check(res.passed_mask == (1 | 1 << STAGE5S) and res.s_slept == 0,
                'mask %x slept %d (%s)' % (res.passed_mask, res.s_slept, res.why.decode()))
        lines = [x.split(None, 1)[1] for x in run.lines]
        r.check(any(x.startswith('stage 5s: 5i flag: ') for x in lines),
                'the flag verdict not logged')
        self.ended_clean(r, run, True)

        r = self.row('stage 5s: an engine-2 upload that drops a word fails it')
        run = Run(self.lib, self.images, 3, spin=True, e2_bad=True)
        res = run.run_stages(1 | 1 << STAGE5S, f5)
        r.check(not res.passed_mask & (1 << STAGE5S), 'passed with a dropped word')
        r.check(res.s_up_bad[0] == 0 and res.s_up_bad[1] == 2,
                'wrong %d/%d (want 0/2)' % (res.s_up_bad[0], res.s_up_bad[1]))
        lines = [x.split(None, 1)[1] for x in run.lines]
        r.check(any('first at 00010010' in x for x in lines), 'first bad address not logged')
        self.ended_clean(r, run, True)

        r = self.row('stage 5s: a HAL without the engine-2 boot skips step 3, fails')
        run = Run(self.lib, self.images, 3, spin=True, boot_how=False)
        res = run.run_stages(1 | 1 << STAGE5S, f5)
        r.check(not res.passed_mask & (1 << STAGE5S), 'passed without step 3')
        r.check(not run.boot_hows and len(run.booted) == 1, 'booted %d' % len(run.booted))
        self.ended_clean(r, run, True)

    def t_3d(self):
        r = self.row('stage 3d refused without a stage 5 flag; nothing booted')
        run = Run(self.lib, self.images, 3)
        res = run.run_stages(1 | 1 << STAGE3D, None)
        r.check(run.ret == -3 and res.why.decode().startswith('stage 3d refused'),
                'no stage 5 flag: ret %d (%s)' % (run.ret, res.why.decode()))
        r.check(not run.booted and not any(e[0] == 'take' for e in run.events),
                'something was touched')
        r = self.row('stage 3d refused in a build without the 3D image')
        imgs = {k: v for k, v in self.images.items() if k != 'hello_3d'}
        run = Run(self.lib, imgs, 3)
        res = run.run_stages(1 | 1 << STAGE3D, self.flag(5, 'hello_dma'))
        r.check(run.ret == -3 and 'no 3D image' in res.why.decode(),
                'ret %d (%s)' % (run.ret, res.why.decode()))
        f5 = self.flag(5, 'hello_dma')
        fast = f5 + self.flag(FLAG4B, 'bench_pll') + self.flag(FLAG4C, 'hello_dma_clk')
        for pid, flags, pll in ((3, f5, False), (1, fast, True)):
            r = self.row('stage 3d, panel %d, %s: scenes 0-%d render, check frames match '
                         'the simulator' % (pid, '27 and 150 MHz' if pll else '27 MHz',
                                            D_RUN - 1))
            run = Run(self.lib, self.images, pid)
            res = run.run_stages(1 | 1 << STAGE3D, flags)
            r.check(res.passed_mask == (1 | 1 << STAGE3D), 'mask %x (%s)' % (
                res.passed_mask, res.why.decode()))
            r.check(run.booted == [self.images['hello_3d']],
                    'booted %d images' % len(run.booted))
            r.check(res.d_crc_bad == 0, '%d check frames differ' % res.d_crc_bad)
            r.check(res.d_fast == (1 if pll else 0), 'd_fast %d' % res.d_fast)
            for c in range(2 if pll else 1):
                for sc in range(D_RUN):
                    r.check(res.d_frames[c][sc] >= 1 and res.d_render[c][sc] > 0,
                            'clock %d scene %d: %d frames' % (c, sc, res.d_frames[c][sc]))
            if not pll:
                r.check(all(res.d_frames[1][sc] == 0 for sc in range(D_SCENES)),
                        'PLL runs without the flags')
            r.check(res.d_tris[0] == 250, 'scene 0: %d triangles' % res.d_tris[0])
            lines = [x.split(None, 1)[1] for x in run.lines]
            r.check(sum(' ok, ' in x and x.endswith(' ok') for x in lines
                        if x.startswith('3d ') and 'crc' in x) == D_RUN * (2 if pll else 1),
                    'checksum lines')
            r.check(any('clock up: state 434b0001' in x for x in lines) == pll,
                    'clock lines (pll %s)' % pll)
            r.check(any(x.startswith('stage 3d PASS') for x in lines), 'no PASS line')
            self.ended_clean(r, run, True)
            r.check(res.d_te_us == 0 and res.d_sync == 0 and
                    any(x.startswith('TE: no period') for x in lines),
                    'TE without a TE line: %d us, sync %d' % (res.d_te_us, res.d_sync))

        r = self.row('stage 3d with a 60 Hz TE line: the probe finds 16.7 ms, the walk '
                     'runs on every second edge')
        run = Run(self.lib, self.images, 3, te_period=16667)
        res = run.run_stages(1 | 1 << STAGE3D, f5)
        r.check(res.passed_mask == (1 | 1 << STAGE3D), 'mask %x (%s)' % (
            res.passed_mask, res.why.decode()))
        r.check(abs(res.d_te_us - 16667) < 100 and res.d_te_edges >= 20,
                'TE period %d us, %d edges' % (res.d_te_us, res.d_te_edges))
        r.check(res.d_sync == 2 and res.d_sync_frames >= 1,
                'sync %d, %d frames' % (res.d_sync, res.d_sync_frames))
        lines = [x.split(None, 1)[1] for x in run.lines]
        r.check(any(x.startswith('TE: period 166') and '(60.0 Hz)' in x for x in lines),
                'TE period line: %s' % [x for x in lines if x.startswith('TE')])
        r.check(any('on the TE line' in x for x in lines), 'no TE-synced run')
        self.ended_clean(r, run, True)

    def t_hib(self):
        """PW5's PP half (bcmdt_hib_cycle) against the video image: Apple's
        pin order and waits, no port access while bit 15 is high, the
        handshake, and the standard cut on every way it can fail"""
        video = self.video()
        entry = B.config_value('BCMFW_ENTRY', ['BCMFW_VIDEO=1'])
        hold = 50

        def pin_checks(r, rig, res, awake):
            moves = [(c, s_) for _, c, s_, _ in rig.pins]
            want = [(0, RETAIN_PIN), (RUN_PIN, 0), (0, RUN_PIN), (RETAIN_PIN, 0)]
            if not awake:
                want += [(RETAIN_PIN, 0), (RUN_PIN, 0)]
            r.check(moves == want, 'pin order %s' % moves)
            t = [x[0] for x in rig.pins]
            r.check(len(t) >= 4 and 5000 <= t[1] - t[0] <= 5400 and
                    5000 <= t[3] - t[2] <= 5400,
                    'both pins high only for Apple\'s 5 ms: %s us' % (
                        [t[1] - t[0], t[3] - t[2]] if len(t) >= 4 else t))
            r.check(len(t) >= 3 and t[2] - t[1] >= hold * 1000, 'held %d us' % (
                t[2] - t[1] if len(t) >= 3 else 0))
            during = [p for p in rig.port if p[2] & RETAIN_PIN]
            r.check(not during, 'port accesses while bit 15 was high: %d' % len(during))
            r.check(all(x[3] & 0x3 == 0x3 for x in rig.pins),
                    'the other GPO32 bits kept')
            r.check(res.pins, 'pins flag')

        r = self.row('PW5 hibernate pins: retained, the image answers, all kept')
        rig = HibRig(self.lib, video, entry)
        res = rig.cycle(hold)
        r.check(rig.ret == HIB_OK and res.end == HIB_OK, 'end %d' % rig.ret)
        r.check(res.word_after == 0xA5A50000, 'word after %08x' % res.word_after)
        r.check(res.gpo_end == RUN_PIN and not res.cut, 'pins at the end %04x, cut %s' % (
            res.gpo_end, res.cut))
        pin_checks(r, rig, res, True)
        r.check(res.hold_us >= hold * 1000 and res.wake_us >= 5000 and
                res.answer_us > 0 and res.park_us > 0,
                'times: park %d hold %d wake %d answer %d' % (
                    res.park_us, res.hold_us, res.wake_us, res.answer_us))
        K = rig.V.K
        op, st, p = rig.reply()
        r.check(st == K['HP_OK'] and len(p) == K['HP_HIB_REPLY_WORDS'] and
                p[K['HP_HIB_R_EXIT']] == 0 and p[K['HP_HIB_R_SRAM_BAD']] == 0 and
                p[K['HP_HIB_R_SDRAM_BAD']] == 0 and p[K['HP_HIB_R_MISMATCH']] == 0 and
                p[K['HP_HIB_R_FRAME_PRE']] == p[K['HP_HIB_R_FRAME_POST']],
                'the image\'s reply %d %s' % (st, p[:12]))
        r.check(p and p[K['HP_HIB_R_HELD_US']] >= hold * 1000,
                'the image saw the hold on its counter: %s' % (p and p[K['HP_HIB_R_HELD_US']]))
        self.metric('hib_wake_us_sim', res.wake_us)
        self.metric('hib_answer_us_sim', res.answer_us)
        self.metric('hib_park_us_sim', res.park_us)

        for model, end, word in (('cut', HIB_E_DEAD, 0xffffffff),
                                 ('lost', HIB_E_LOST, None),
                                 ('stuck', HIB_E_WAKE, 0xA5A50000),
                                 ('noport', HIB_E_DEAD, 0)):
            r = self.row('PW5 hibernate pins, chip model "%s": the standard cut' % model)
            rig = HibRig(self.lib, video, entry, model)
            res = rig.cycle(hold)
            r.check(rig.ret == end and res.end == end, 'end %d, want %d' % (rig.ret, end))
            if word is not None:
                r.check(res.word_after == word, 'word after %08x' % res.word_after)
            r.check(res.cut and res.gpo_end == 0 and rig.gpo_v & 0xC000 == 0,
                    'cut %s, pins %04x' % (res.cut, rig.gpo_v))
            pin_checks(r, rig, res, False)
            cut_at = rig.pins[-1][0]
            r.check(not [p for p in rig.port if p[0] > cut_at],
                    'no port access after the cut')

        r = self.row('PW5 hibernate pins: refusals and a park that never comes')
        for gpo, en, why in ((0x4003, 0x4003, 'bit 15 not an output'),
                             (0x0003, 0xC003, 'bit 14 low'),
                             (0xC003, 0xC003, 'bit 15 already high')):
            rig = HibRig(self.lib, video, entry, gpo=gpo, en=en)
            res = rig.cycle(hold)
            r.check(rig.ret == HIB_REFUSED and not rig.pins and rig.tag is None and
                    not rig.port, '%s: end %d, pins %s, sent %s' % (
                        why, rig.ret, rig.pins, rig.tag))
        rig = HibRig(self.lib, video, entry)
        rig.send_fails = True
        res = rig.cycle(hold)
        r.check(rig.ret == HIB_E_SEND and not rig.pins, 'not sent: %d' % rig.ret)
        # a session open: the image refuses, the word never parks
        rig = HibRig(self.lib, video, entry, session=True)
        t0 = rig.b.now()
        res = rig.cycle(hold)
        r.check(rig.ret == HIB_E_PARK and not rig.pins and res.gpo_end == RUN_PIN and
                rig.b.now() - t0 >= 300_000, 'no park: end %d, pins %s, after %d us' % (
                    rig.ret, rig.pins, rig.b.now() - t0))
        K = rig.V.K
        op, st, p = rig.reply()
        r.check(st == K['HP_E_BUSY'], 'the image\'s refusal %d' % st)

    def t_embedded(self):
        r = self.row('embedded images match a fresh build (mkdevtest --check)')
        rc = subprocess.call([sys.executable, '-I', os.path.join(FW, 'mkdevtest.py'),
                              '--check'], stdout=subprocess.DEVNULL)
        r.check(rc == 0, 'bcm-devtest-images.c is stale')
        r = self.row('the video image matches a fresh build (mkvideo --check)')
        rc = subprocess.call([sys.executable, '-I', os.path.join(FW, 'mkvideo.py'),
                              '--check'], stdout=subprocess.DEVNULL)
        r.check(rc == 0, 'bcmfw/video/bcmvideo.bin or bcmvideo_image.h is stale')

    def t_readlog6(self):
        r = self.row('readlog: stage 6 lines (p14-player)')
        lines = ['     0 === BCM test, stage 6 (play a clip) ===\n',
                 '  8200 stage 6 PASS: 239 pictures shown of 239 decoded, 0 skipped, '
                 '0 GOPs dropped, 1 late posts\n',
                 '  8200 V6 33 -2000 11000 150 1\n',
                 '  8210 V6 1533 9000 12000 150 46\n',
                 '  8220 V6 2533 -3000 30000 27 76\n',
                 '  8300 === end: stage 6 played to the end ===\n']
        items, recs = RL.parse_6(lines)
        r.check(len(items) == 3 and len(recs) == 3, 'parsed %d lines, %d records'
                % (len(items), len(recs)))
        d = RL.stage6_detail(recs)
        r.check(d and d[0].endswith('0:2 1:9 2:3') and '30000 us at 2533 ms' in d[1]
                and d[2] == 'clock: 27/150 MHz', 'detail %s' % d)

    def t_readlog6_clips(self):
        r = self.row('readlog: stage 6, two clips and pacing (s9-int)')
        v = ['  %d V6 %d %d 9000 150 %d\n' % (9000 + k, k * 1000 // 24,
                                              (k & 1) * 8000 + (k == 50) * 30000, k + 1)
             for k in range(72)]
        lines = (['     0 === BCM test, stage 6 (play a clip) ===\n',
                  '     5 stage 6 clip 1: synthetic (test pattern, counter)\n',
                  '  8200 stage 6 PASS: 239 pictures shown of 239 decoded, 0 skipped, '
                  '0 GOPs dropped, 1 late posts\n',
                  '  8200 V6 33 -2000 11000 150 1\n',
                  '  8300 stage 6 clip 2: real footage (pans and cuts)\n',
                  '  9000 stage 6 PASS: 72 pictures shown of 72 decoded, 0 skipped, '
                  '0 GOPs dropped, 0 late posts\n'] + v +
                 ['  9900 === end: stage 6 played to the end ===\n'])
        clips = RL.parse_6_clips(lines)
        r.check(len(clips) == 2 and len(clips[0][2]) == 1 and len(clips[1][2]) == 72
                and clips[1][0].startswith('stage 6 clip 2'),
                'clips %s' % [(c[0], len(c[1]), len(c[2])) for c in clips])
        n, jmax, jmean, nh, hits = RL.stage6_pacing(clips[1][2])
        # vtrace.c's own figures for the same run (tests/video/test_vpcore.c)
        r.check((n, jmax, nh, hits) == (48, 22000, 1, [50 * 1000 // 24]),
                'pacing %s' % ((n, jmax, jmean, nh, hits),))
        one = RL.parse_6_clips(lines[:1] + lines[2:4])
        r.check(len(one) == 1 and one[0][0] == 'stage 6', 'a one-clip log: %s' % one)

    metrics = {}

    def metric(self, k, v):
        self.metrics[k] = v

    def run(self, only=None):
        t0 = time.time()
        for t in (self.t_embedded, self.t_readlog6, self.t_readlog6_clips,
                  self.t_stage0, self.t_full, self.t_full3,
                  self.t_probe_silent, self.t_entry0, self.t_never_ready, self.t_fault,
                  self.t_selfcheck, self.t_probe_v2, self.t_probe_v2_required,
                  self.t_probe_v2_unexpected, self.t_hang_in_command, self.t_hang3,
                  self.t_stall_mid_sequence, self.t_clock_gates, self.t_delay_limit,
                  self.t_dma_dead, self.t_sweep_tmo, self.t_everyday,
                  self.t_everyday_lab, self.t_strikes, self.t_release_gates, self.t_everyday_needs3, self.t_everyday_hang, self.t_port_dies, self.t_stage0_port,
                  self.t_take_refused, self.t_flags, self.t_swap_ok, self.t_swap_bad,
                  self.t_ladder, self.t_resident, self.t_hib,
                  self.t_bench, self.t_bench_after_fail2, self.t_bench_hang,
                  self.t_bench_fault, self.t_bench_check, self.t_bench_vstore_lost,
                  self.t_bench_needs1,
                  self.t_4b_refused, self.t_4b, self.t_4b_flag,
                  self.t_4c_refused, self.t_4c, self.t_5b, self.t_5b_guard,
                  self.t_5b_stub, self.t_5i, self.t_5i_regs, self.t_5s, self.t_3d):
            if only and t.__name__ not in only:
                continue
            n0 = len(self.rows)
            t1 = time.time()
            try:
                t()
            except T.Fail as e:
                self.rows[-1].check(False, 'bench: %s' % e) if len(self.rows) > n0 \
                    else self.row(t.__name__).check(False, str(e))
            for row in self.rows[n0:]:
                print('%-72s %5d %5d %5.1fs' % (row.name[:72], row.cases,
                                                row.cases - len(row.fails),
                                                time.time() - t1))
                for f in row.fails[:8]:
                    print('    FAIL: %s' % f)
        for k in sorted(self.metrics):
            print('  %-24s %s' % (k, self.metrics[k]))
        cases = sum(r.cases for r in self.rows)
        fails = sum(len(r.fails) for r in self.rows)
        print('%d rows, %d cases, %d failed (%.0f s)' % (len(self.rows), cases, fails,
                                                         time.time() - t0))
        return 1 if fails else 0


def main(argv):
    if not B.have_gcc():
        print('skip: vc4-elf-gcc not found at %s (set VC4_GCC)' % C.GCC)
        return 0
    # --only t_a,t_b: just those tests (a quick look; the gate runs all)
    only = argv[argv.index('--only') + 1].split(',') if '--only' in argv else None
    return Suite('-v' in argv).run(only)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
