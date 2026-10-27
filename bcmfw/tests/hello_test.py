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
"""bcmfw in the simulator: our VideoCore image against a model of
Rockbox's host driver and of the LCD panel.

The host side is a port of firmware/target/arm/ipod/video/lcd-video.c's
logic (bcm_init, lcd_update_rect, lcd_tick, bcm_command, lcd_sleep,
lcd_awake) driving vc2sim through the modelled bus (vc2mmio): it
uploads bcmfw.bin, starts it with 0x10000C00 / 0x10000400 = 0xA5A50002,
writes frames to 0xE0000, issues commands and polls the status word.
The panel is vc2mmio.LcdPanel: it records the power-on traffic and
collects the pixels, which are written to PNG, read back and compared
pixel-exact with the host framebuffer.

Power-on and power-off sequences are compared with
  - the facts in docs/research/bcm2722/hardware.md (as the self-test
    encodes them: vc2boot_tests.PANELS, TIMING, the reset pulse), and
  - when Apple's vmcs.bin 1.3 is available (reference only, read at
    test time, never shipped): its own power handler run in vc2sim
    against the same panel model, event for event, with our delays at
    least as long as its.

Usage:
  bcmfw/tests/hello_test.py [-v] [--png DIR] [--vmcs PATH] [--seed N]

Skips cleanly (exit 0, message) when vc4-elf-gcc is not installed.
"""
import collections
import hashlib
import os
import random
import struct
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
ROOT = os.path.dirname(FW)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bcmre'))
sys.path.insert(0, FW)
import vc2sim as S      # noqa: E402
import vc2mmio as M     # noqa: E402
import vc2cc as C       # noqa: E402
import vc2forms as F    # noqa: E402
import build as B       # noqa: E402
import vc2sim_tests     # noqa: E402,F401  (imports vc2boot_tests first)
import vc2boot_tests as VB  # noqa: E402

W, H = 320, 240
FB = 0xE0000
SRAM = 0x140000
CMD_WORD = 0x1F8
DONE = 0x50420001
BUSY = 0xFFFF
FAULT = 0xFA170000
MB_FAULT_PC, MB_FAULT_SP, MB_INFO, MB_FRAMES = 0x1E0, 0x1E4, 0x1E8, 0x1EC
MB_RECT_XY, MB_RECT_WH = 0x1F0, 0x1F4
# hello v2 (config.h): step word, bounded-wait record, clock enable record
MB_STEP, MB_LIMIT, MB_LIMIT_VAL, MB_CLK = 0x1C0, 0x180, 0x184, 0x188
INFO_SPIN, INFO_CLK = 0x2000, 0x4000
STEP_IDLE, STEP_DONE, STEP_POWER, STEP_SCRIPT, STEP_FRAME = (
    0x50530006, 0x5053000F, 0x50531000, 0x50532000, 0x50535000)
CLK_RAN0, CLK_RAN_A, CLK_RAN_B, CLK_WROTE_A, CLK_WROTE_B = 1, 2, 4, 0x10, 0x20
# Apple's start-up writes for the counter's clock (vmcs.bin 1.3 @14562,
# @53b4), in order; hello v2 makes the first four, then the fifth only if
# the counter still does not count
CLK_WRITES = [(0x10001C4C, 0xA5A50501), (0x10001C54, 0xA5A50000),
              (0x10001C4C, 0xA5A5051B), (0x10001C54, 0xA5A50000),
              (0x10001C58, 0xA5A50101)]
SLICE = 2000                 # instructions per host "yield"
VMCS_13_SHA256 = 'cde4b5a9c9a020b6e23de1be0492d9a853b7d974c4060462d5f57a80e47f55be'
DEFAULT_VMCS = os.path.expanduser(
    '~/Git/re/bcm2722/extract/1.3/vc/VideoCore/Boot/vmcs.bin')


def bcm_cmd(x):
    return ((~x & 0xffff) << 16) | x


LCD_UPDATE, SELFTEST, LCD_UPDATERECT, LCD_SLEEP = (
    bcm_cmd(0), bcm_cmd(1), bcm_cmd(5), bcm_cmd(8))


class Fail(Exception):
    pass


# --------------------------------------------------------------------------
# The BCM side: vc2sim with the modelled bus, timer, interrupt controller
# and panel.

class Bench:
    def __init__(self, image, panel_id, entry, reset_pc=None, us_per_read=4,
                 exceptions=True):
        self.image = image
        self.entry = entry
        self.reset_pc = entry if reset_pc is None else reset_pc
        sim = S.Sim()
        # faults enter VC II exceptions (vc2sim HYP exc_*), as on the chip:
        # the image's own handler reports them
        sim.exceptions = exceptions
        sim.mem = S.Bus(sim, S.BOOT_MEM_SIZE, poll_limit=1 << 30)
        for r in S.BCM_RULES:
            sim.mem.rule(r)
        sim.mem.rule(S.panel_rule(panel_id))
        self.timer = sim.mem.attach(M.SysTimer(us_per_read))
        sim.mem.attach(M.IntCtl(self.timer.sources))
        self.panel = sim.mem.attach(M.LcdPanel(M.LcdPanel.KINDS[panel_id]))
        for a in (0x1000280c, 0x10002814, 0x1000281c):
            sim.mem.on_write[a] = self.panel.gpio_write
        sim.mem.on_write[0x10000400] = self._run_control
        # strict code coherence (vc2sim.StaleCode): a flush request by the
        # host or the core makes SRAM as it is the instruction side's view
        sim.mem.on_write[0x10000C00] = \
            lambda bus, a, v: v & 0x40000000 and sim.cache_flushed()
        sim.on_sleep = lambda nxt: S._sleep(sim, nxt)
        sim.irq_ctx = {}
        self.sim, self.bus = sim, sim.mem
        self.powered = self.running = False
        self.idle = False           # stopped in sleep with nothing to wake it
        self.pc = None
        self.host_seqs = set()
        self.starts = 0
        self.clock = None
        self.stalled = None         # the M.Stall that stopped the core

    def gate_clocks(self, timer_on, lcd_on='reset'):
        """Model the clock gates (vc2mmio.ClockBlock): the counter and the
        LCD interface run only once the named register is written."""
        self.clock = self.bus.attach(M.ClockBlock(timer_on, lcd_on))
        self.timer.gate = self.clock.timer_runs
        self.panel.gate = self.clock.lcd_runs

    # -- host access (the PP side of the host port).  The port's write
    # address ignores bits 1:0 (quirks.md #34 [silicon, inferred from CR1's
    # 301 x 7]): a write asked at 2 mod 4 lands 2 bytes low here too, so a
    # host path that writes mid-word fails in the sim as on the chip
    def w32(self, a, v):
        a &= ~3
        if a >= 0x10000000:
            self.bus.dev_write(a, 4, v)
            self.host_seqs.add(self.bus.seq)
        else:
            self.bus.w32(a, v)

    def r32(self, a):
        if a >= 0x10000000:
            v = self.bus.dev_read(a, 4)
            self.host_seqs.add(self.bus.seq)
            return v
        return self.bus.r32(a)

    def write_bytes(self, a, data):
        """One block through the port: from the word's start (the address's
        bits 1:0 dropped), whole words only.  A lone trailing halfword is
        lost [silicon, sitting 2 ALGNP PLH1/3/5: an odd last halfword keeps
        the old contents once a new address follows], and so is an odd
        byte; each such block is counted in lone_tails.  The PP's writers
        send whole words (dt_wr_block merges an odd last halfword into its
        word, bcm-port-block.h), so a count here is a writer's bug."""
        a &= ~3                     # the port: a block from the word's start
        n = len(data) & ~3
        if n != len(data):
            Bench.lone_tails += 1   # every bench of the run (the suites check)
        self.bus.b[a:a + n] = data[:n]

    lone_tails = 0

    def _run_control(self, bus, addr, v):
        if v == 0xA5A50002 and self.powered:
            self.running, self.idle = True, False
            self.pc = self.reset_pc
            self.sim.r[:] = [0] * 32
            self.starts += 1
            # bench row ALGN's checks are misaligned on purpose: strict
            # alignment leaves them alone (vc2sim.algn_exempt)
            self.sim.align_exempt = []
            S.algn_exempt(self.sim, bytes(self.bus.b[0:0x40000]))

    def power_on(self, rng):
        self.powered = True
        # SRAM content is undefined after power-up
        self.bus.b[0:SRAM] = bytes(rng.getrandbits(8) for _ in range(256)) * (SRAM // 256)
        self.sim.code.clear()
        self.sim.code_view = None

    def power_cut(self, rng):
        self.powered = self.running = False
        self.stalled = None
        if self.clock is not None:
            self.clock.reset()
        self.panel.power_cut()
        self.bus.regs = {}
        self.bus.b[0:SRAM] = bytes(rng.getrandbits(8) for _ in range(256)) * (SRAM // 256)
        self.sim.code.clear()
        self.sim.code_view = None

    def now(self):
        return self.timer.clo

    def run(self, n=SLICE):
        """Run the core for about n instructions; time passes either way."""
        if not self.running or self.idle:
            self.timer.advance(max(1, n // 100))
            return
        try:
            self.pc = S.run_machine(self.sim, self.pc, n, stop=True)
        except S.Idle:
            self.idle = True
        except M.Stall as e:
            # a bus access that never completes: the core hangs, time goes on
            self.idle, self.stalled = True, e
            self.pc = self.sim.r[31]
        except S.SimError as e:
            raise Fail('core stopped: %s' % e)

    def step_until(self, pred, limit):
        """Single-step until pred(); returns instructions executed."""
        sim, r = self.sim, self.sim.r
        pc, n = self.pc, 0
        while not pred():
            h = sim.code.get(pc) or sim.fetch(pc)
            r[31] = pc
            npc = h[0]()
            pc = (pc + h[1]) if npc is None else npc & S.MASK
            n += 1
            if n > limit:
                raise Fail('no progress after %d instructions' % limit)
        self.pc = pc
        sim.steps += n
        return n

    def vpu_device_writes(self):
        return [a for a in self.bus.log if a.rw == 'W' and a.seq not in self.host_seqs]


# --------------------------------------------------------------------------
# The host side: lcd-video.c's logic, step for step (HZ = 100).

HZ = 100
TICK_US = 1_000_000 // HZ
LCD_IDLE, LCD_INITIAL, LCD_NEED_UPDATE, LCD_UPDATING = range(4)
BCM_UPDATE_TIMEOUT = HZ // 20
BCM_LCDINIT_TIMEOUT = HZ // 2


class Host:
    def __init__(self, bench, rng):
        self.b, self.rng = bench, rng
        self.fb = [0] * (W * H)
        self.state = LCD_INITIAL
        self.blocked = False
        self.display_on = False
        self.waking = False
        self.update_timeout = 0
        self.tick_task = False
        self.next_tick = 0
        self.rekicks = 0
        self.seen = set()           # status values the host read
        self.ticks = 0

    def tick(self):
        return self.b.now() // TICK_US

    def yield_(self):
        b = self.b
        # nothing for the core to do: let time pass without simulating
        # its polling loop instruction by instruction
        if b.running and b.r32(CMD_WORD) in (DONE,) and self.idle_ok:
            b.timer.advance(max(1, self.next_tick * TICK_US - b.now()))
        else:
            b.run()
        self.service()

    idle_ok = True

    def service(self):
        while self.next_tick * TICK_US <= self.b.now():
            self.next_tick += 1
            self.ticks += 1
            if self.tick_task:
                self.lcd_tick()

    def wait(self, pred, what, limit_us=5_000_000):
        t0 = self.b.now()
        while not pred():
            if self.b.now() - t0 > limit_us:
                raise Fail('host timed out waiting for %s (status %08x)' % (
                    what, self.b.r32(CMD_WORD)))
            self.yield_()

    def sleep(self, ticks):
        end = self.b.now() + ticks * TICK_US
        while self.b.now() < end:
            self.yield_()

    def read_status(self):
        v = self.b.r32(CMD_WORD)
        self.seen.add(v)
        return v

    def kick(self):
        # bcm_write32(BCMA_COMMAND, BCMCMD_LCD_UPDATE); BCM_CONTROL = 0x31
        # (the doorbell is not modelled: the image polls)
        self.b.w32(CMD_WORD, LCD_UPDATE)

    # lcd_tick (tick task, non-bootloader build)
    def lcd_tick(self):
        if self.blocked or self.state < LCD_NEED_UPDATE:
            return
        data = self.read_status()
        busy = data in (LCD_UPDATE, 0xFFFF)
        timed_out = self.tick() > self.update_timeout
        if (self.state == LCD_NEED_UPDATE and not busy) or (timed_out and busy):
            if timed_out and busy:
                self.rekicks += 1
            self.kick()
            self.update_timeout = self.tick() + BCM_UPDATE_TIMEOUT
            self.state = LCD_UPDATING
            if self.waking:
                self.waking = False
        elif self.state == LCD_UPDATING and not busy:
            self.state = LCD_IDLE
            if self.waking:
                self.waking = False

    def lcd_unblock_and_update(self):
        data = self.read_status()
        busy = data in (LCD_UPDATE, 0xFFFF)
        if not busy or self.state == LCD_INITIAL or self.tick() > self.update_timeout:
            self.kick()
            self.update_timeout = self.tick() + BCM_UPDATE_TIMEOUT
            self.state = LCD_UPDATING
            if self.waking:
                self.waking = False
        else:
            self.state = LCD_NEED_UPDATE
        self.blocked = False

    def write_fb(self, x, y, n):
        """lcd_write_data(): n pixels of the host frame from (x, y), from
        an even x to an even end (whole words: the port drops a write
        address's bits 1:0, so the driver writes from the word's start, as
        lcd_update_rect rounds x and width even)."""
        end = x + n
        x &= ~1
        n = (end - x + 1) & ~1
        i = y * W + x
        data = struct.pack('<%dH' % n, *self.fb[i:i + n])
        self.b.write_bytes(FB + 2 * i, data)

    def lcd_update_rect(self, x, y, width, height):
        if not self.display_on:
            return
        if x + width >= W:
            width = W - x
        if y + height >= H:
            height = H - y
        if width <= 0 or height <= 0:
            return
        width = (width + (x & 1) + 1) & ~1
        x &= ~1
        self.blocked = True
        if width == W:
            self.write_fb(x, y, width * height)
        else:
            for k in range(height):
                self.write_fb(x, y + k, width)
        self.lcd_unblock_and_update()

    def lcd_update(self):
        self.lcd_update_rect(0, 0, W, H)

    def bcm_command(self, cmd):
        self.b.w32(CMD_WORD, cmd)
        self.wait(lambda: self.read_status() not in (cmd, 0xFFFF), 'command %08x' % cmd)

    def bcm_init(self):
        b = self.b
        b.power_on(self.rng)
        self.sleep(HZ // 20)
        n = ((len(b.image) + 3) >> 1) & ~1          # halfwords, even count
        b.write_bytes(0, (b.image + b'\0' * 4)[:2 * n])
        b.w32(CMD_WORD, 0)
        b.w32(0x10000C00, 0xC0000000)
        k = 0
        while not b.r32(0x10000C00) & 1:
            k += 1
            if k > 100:
                raise Fail('cache flush bit never set')
        b.w32(0x10000C00, 0)
        b.w32(0x10000400, 0xA5A50002)
        self.wait(lambda: b.r32(CMD_WORD) != 0, 'the image to report ready')

    def lcd_awake(self):
        if self.display_on:
            return
        sleepwait = self.update_timeout + HZ // 20 - self.tick()
        if 0 < sleepwait < HZ // 20:
            self.sleep(sleepwait)
        self.bcm_init()
        self.state = LCD_INITIAL
        self.display_on = True
        self.lcd_update()
        self.update_timeout = self.tick() + BCM_LCDINIT_TIMEOUT
        self.waking = True
        self.tick_task = True
        self.wait(lambda: not self.waking, 'the first update (lcd_awake)')

    def lcd_sleep(self):
        if not self.display_on:
            return
        self.display_on = False
        self.wait(lambda: self.state in (LCD_INITIAL, LCD_IDLE), 'idle before sleep')
        self.tick_task = False
        b = self.b
        b.w32(0x10001400, b.r32(0x10001400) & ~0xF0)
        self.bcm_command(LCD_SLEEP)
        self.bcm_command(bcm_cmd(0xC))
        b.power_cut(self.rng)
        self.update_timeout = self.tick()

    def settle(self):
        """Wait until the driver has no update in flight."""
        self.wait(lambda: self.state == LCD_IDLE, 'the update to finish')


# --------------------------------------------------------------------------
# PNG (RGB, 8-bit) written and read back with the standard library.

def rgb565_to_rgb(v):
    r, g, b = v >> 11 & 31, v >> 5 & 63, v & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def png_write(path, pix):
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        for x in range(W):
            v = pix[y * W + x]
            raw += bytes(rgb565_to_rgb(0 if v is None else v))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))


def png_read(path):
    """Our own PNG files only (RGB8, filter 0)."""
    d = open(path, 'rb').read()
    pos, idat = 8, b''
    while pos < len(d):
        n, t = struct.unpack('>I4s', d[pos:pos + 8])
        if t == b'IDAT':
            idat += d[pos + 8:pos + 8 + n]
        pos += 12 + n
    raw = zlib.decompress(idat)
    out = []
    for y in range(H):
        row = raw[y * (3 * W + 1) + 1:(y + 1) * (3 * W + 1)]
        out += [tuple(row[3 * x:3 * x + 3]) for x in range(W)]
    return out


# --------------------------------------------------------------------------
# Apple's reference sequences (vmcs.bin 1.3 run in vc2sim, reference only)

LCD_POWER = 0xf672          # vmcs.bin 1.3 "display_control ... power=N"
LCD_MODEL = 0x12a02         # "display_control ... model=N"


def reference_sequences(vmcs, log):
    """{(panel, model): (on events, off events)} with time stamps."""
    if not vmcs or not os.path.exists(vmcs):
        return None
    if hashlib.sha256(open(vmcs, 'rb').read()).hexdigest() != VMCS_13_SHA256:
        return None
    sim = S.make_boot_sim(vmcs, S.BCM_RULES, S.BCM_ALIASES, machine=True, us_per_read=4)
    panel = sim.mem.attach(M.LcdPanel('dcs'))
    for a in (0x1000280c, 0x10002814, 0x1000281c):
        sim.mem.on_write[a] = panel.gpio_write
    S.boot(sim, max_steps=VB.BOOT_STEPS)
    snap = VB._snapshot(sim)
    out = {}
    for pid in range(4):
        for model in ((1, 2) if pid in (1, 3) else (1,)):
            VB._restore(sim, snap)
            panel.__init__(M.LcdPanel.KINDS[pid])
            panel.bus = sim.mem
            if model == 2:
                sim.call(LCD_MODEL, 2, max_steps=50_000_000)
            sim.mem.rule(S.panel_rule(pid))
            del panel.events[:]
            sim.call(LCD_POWER, 2, max_steps=50_000_000)
            on = list(panel.events)
            del panel.events[:]
            sim.call(LCD_POWER, 0, max_steps=50_000_000)
            out[(pid, model)] = (on, list(panel.events))
    log('reference: vmcs.bin 1.3 power handler, %d panel variants' % len(out))
    return out


def strip(evs):
    return [e[:-1] for e in evs]


# LCD bus timing: Apple's per-panel nanosecond tables (vmcs.bin 1.3
# descriptors +168/+176/+184, four halfwords each) and the words Apple
# computes from them for 150 MHz (hardware.md, "Bus timing").  bcmfw's
# panels.c computes its words from the same tables; this is an
# independent copy of the formula (Apple's @c87c).
TIMING_NS = {0: ((10, 45, 90, 25), (10, 35, 10, 55), (10, 35, 10, 55)),
             1: ((10, 170, 5, 285), (0, 50, 5, 145), (0, 65, 5, 30)),
             2: ((10, 250, 10, 240), (10, 40, 10, 50), (10, 40, 10, 50))}
TIMING_NS[3] = TIMING_NS[1]
TIMING_APPLE = {0: (0x410D0206, 0x41010705, 0x41010705),
                1: (0x41002A19, 0x40001407, 0x40000309),
                2: (0x41012325, 0x41010605, 0x41010605)}
TIMING_APPLE[3] = TIMING_APPLE[1]
MB_TIMING_SEL, MB_TIMING0, MB_TIMING1, MB_TIMING_KHZ = 0x170, 0x174, 0x178, 0x17C
TIMING_APPLE_MAGIC = 0x4150504C
TIMING_SWEEP_MAGIC = 0x53575000     # config.h: "SWP" << 8 | k (dev5)


def timing_word(ns, khz):
    def cyc(n):
        return max((n * khz + 999999) // 1000000 - 1, 0)

    def over(n):
        return (cyc(n) + 1) * 1000000 // khz - n
    n0, n1, n2, n3 = ns
    d = cyc(max(n3 - over(n2) - over(n0), 0))
    return (0x40000000 | (cyc(n0) & 63) << 24 | (cyc(n2) & 63) << 16 |
            (d & 127) << 8 | (cyc(n1) & 127))


def timing_words(pid, khz):
    """The three words an image built for khz uses (0: Apple's)."""
    if not khz:
        return TIMING_APPLE[pid]
    return tuple(timing_word(t, khz) for t in TIMING_NS[pid])


def timing_fields(w):
    return (w >> 24 & 63, w & 127, w >> 16 & 63, w >> 8 & 127)   # a, b, c, d


def delays_ok(ours, ref, tol=10):
    """Every gap of 0.5 ms or more in ref is at least as long in ours."""
    bad = []
    for i in range(1, min(len(ours), len(ref))):
        g = ref[i][-1] - ref[i - 1][-1]
        if g >= 500 and ours[i][-1] - ours[i - 1][-1] < g - tol:
            bad.append((i, ref[i][:-1], g, ours[i][-1] - ours[i - 1][-1]))
    return bad


# --------------------------------------------------------------------------
# Test rows

class Row:
    def __init__(self, name):
        self.name, self.cases, self.fails, self.instr = name, 0, [], 0

    def check(self, ok, what):
        self.cases += 1
        if not ok:
            self.fails.append(what)
        return ok


def random_frame(rng):
    return [rng.getrandbits(16) for _ in range(W * H)]


class Suite:
    def __init__(self, args):
        self.args = args
        self.rng = random.Random(args.seed)
        self.rows = []
        self.builds = {}
        self.ref = None
        self.png_dir = args.png
        os.makedirs(self.png_dir, exist_ok=True)
        self.metrics = {}

    def log(self, msg):
        if self.args.verbose:
            print('  ' + msg)

    def image(self, defines=()):
        key = tuple(sorted(defines))
        # a mutant kernel (mutant.py, vc2cc.SUBST) is its own image: its
        # own cache entry and directory, never the tracked build's
        subst = tuple(sorted(C.SUBST.items()))
        if (key, subst) not in self.builds:
            name = '-'.join(d.replace('BCMFW_', '').replace('=', '').lower()
                            for d in key) or 'default'
            out = os.path.join(ROOT, 'build', 'bcmfw', 'test-' + name)
            if subst:
                import shutil
                out += '-mutant-%d' % os.getpid()
            try:
                prog, rep = B.build(out, list(key), listing=False)
            finally:
                if subst:
                    shutil.rmtree(out, ignore_errors=True)
            self.builds[(key, subst)] = (prog, rep, prog.load_image())
        return self.builds[(key, subst)]

    def bench(self, pid, defines=(), reset_pc=None):
        prog, rep, img = self.image(defines)
        entry = B.config_value('BCMFW_ENTRY', defines)
        b = Bench(img, pid, entry, reset_pc)
        h = Host(b, random.Random(self.rng.getrandbits(32)))
        return b, h, prog

    def frame_check(self, row, b, h, tag, png=False):
        p = b.panel
        same = p.fb == h.fb
        if not same:
            diff = [i for i in range(W * H) if p.fb[i] != h.fb[i]]
            i = diff[0]
            row.check(False, '%s: %d pixels differ, first (%d,%d) panel %s host %04x' % (
                tag, len(diff), i % W, i // W, p.fb[i], h.fb[i]))
        else:
            row.check(True, tag)
        if png:
            path = os.path.join(self.png_dir, tag + '.png')
            png_write(path, p.fb)
            back = png_read(path)
            row.check(back == [rgb565_to_rgb(v) for v in h.fb],
                      '%s: PNG read back differs from the host frame' % tag)
        row.check(p.high_bits == 0 and p.stray == 0 and p.outside == 0,
                  '%s: panel bus misuse: high bits %d, stray data %d, pixels before '
                  'reset %d' % (tag, p.high_bits, p.stray, p.outside))
        st = b.r32(CMD_WORD)
        row.check(st == DONE, '%s: status %08x after the update' % (tag, st))

    def compare_on(self, row, pid, model, evs, tag):
        """Power-on traffic up to the first RAM-write command."""
        ramwr = M.LcdPanel.RAMWR[M.LcdPanel.KINDS[pid]]
        k = next((i for i, e in enumerate(evs) if e[0] == 'cmd' and e[1] == ramwr), None)
        row.check(k is not None, '%s: no RAM-write command' % tag)
        if k is None:
            return
        on = evs[:k + 1]
        seq = strip(on)
        # hardware.md facts (the self-test's encoding of them)
        resets = [e[1] for e in seq if e[0] == 'reset']
        row.check(resets[:4] == [0, 1, 0, 1], '%s: reset pulse %s' % (tag, resets[:4]))
        # our timing words are Apple's recomputed for BCMFW_LCD_BUS_KHZ:
        # check them, then compare the rest against Apple with Apple's
        ours = timing_words(pid, B.config_value('BCMFW_LCD_BUS_KHZ'))
        tw = [e for e in seq if e[0] == 'ctl' and e[1] in (0x10, 0x14)]
        row.check([v for _, _, v in tw[:2]] == list(ours[:2]),
                  '%s: bus timing %s' % (tag, ['%08x' % v for _, _, v in tw[:2]]))
        to_apple = dict(zip(ours, TIMING_APPLE[pid]))
        seq = [('ctl', e[1], to_apple.get(e[2], e[2]))
               if e[0] == 'ctl' and e[1] in (0x10, 0x14) else e for e in seq]
        row.check([v for _, _, v in tw[:2]] != list(VB.TIMING[pid]) or ours[:2] ==
                  TIMING_APPLE[pid][:2], '%s: Apple\'s words in a build for other ones' % tag)
        cmds = [e[1] for e in seq if e[0] == 'cmd' and e[1] != 0]
        row.check(VB._subseq(VB.PANELS[pid], cmds) and cmds[-1] == VB.PANELS[pid][-1],
                  '%s: command order vs hardware.md' % tag)
        syncs = [i for i, e in enumerate(seq) if e == ('cmd', 0)]
        row.check(len(syncs) == 6, '%s: %d bus-sync commands' % (tag, len(syncs)))
        if self.ref is not None:
            ref_on, _ = self.ref[(pid, model)]
            same = seq == strip(ref_on)
            if not same:
                rs = strip(ref_on)
                i = next((i for i in range(min(len(rs), len(seq))) if rs[i] != seq[i]),
                         min(len(rs), len(seq)))
                row.check(False, '%s: differs from vmcs.bin at event %d: ours %s, ref %s' % (
                    tag, i, seq[i:i + 3], rs[i:i + 3]))
            else:
                row.check(True, tag)
                bad = delays_ok(on, ref_on)
                row.check(not bad, '%s: shorter delays than vmcs.bin: %s' % (tag, bad[:3]))

    # ---- rows ---------------------------------------------------------

    def t_timing(self):
        row = Row('bus timing: Apple\'s formula, our clock, the host\'s switch')
        self.rows.append(row)
        khz = B.config_value('BCMFW_LCD_BUS_KHZ')
        for pid in range(4):
            row.check(timing_words(pid, 150000) == TIMING_APPLE[pid],
                      'panel %d: formula at 150 MHz %s' % (
                          pid, ['%08x' % w for w in timing_words(pid, 150000)]))
            # every field at least Apple's ns at the assumed clock, none
            # truncated by its mask (a wrap would make it faster)
            for ns, w in zip(TIMING_NS[pid], timing_words(pid, khz)):
                a, bb, c, d = timing_fields(w)
                ok = all((f + 1) * 1000000 // khz >= n
                         for f, n in ((a, ns[0]), (bb, ns[1]), (c, ns[2])))
                ok = ok and (a + c + d + 3) * 1000000 // khz >= ns[0] + ns[2] + ns[3] - 1
                row.check(ok and w == timing_word(ns, khz),
                          'panel %d: %08x for %s at %d kHz' % (pid, w, ns, khz))
                aa, ab, ac, ad = timing_fields(TIMING_APPLE[pid][TIMING_NS[pid].index(ns)])
                row.check(a <= aa and bb <= ab and c <= ac and d <= ad,
                          'panel %d: %08x has a field above Apple\'s' % (pid, w))
            self.metrics['timing_p%d' % pid] = ' '.join(
                '%08x' % w for w in timing_words(pid, khz))
        for defs, want in ((('BCMFW_LCD_BUS_KHZ=150000',), TIMING_APPLE[3]),
                           (('BCMFW_LCD_BUS_KHZ=0',), TIMING_APPLE[3]),
                           ((), timing_words(3, khz))):
            b, h, _ = self.bench(3, defs)
            h.fb = random_frame(self.rng)
            try:
                h.lcd_awake()
                h.settle()
            except Fail as e:
                row.check(False, '%s: %s' % (defs, e))
                continue
            tw = [e[2] for e in b.panel.events if e[0] == 'ctl' and e[1] in (0x10, 0x14)]
            row.check(tw == [want[0], want[1], want[0], want[2]],
                      '%s: timing writes %s' % (defs, ['%08x' % v for v in tw]))
            row.check((b.r32(MB_TIMING0), b.r32(MB_TIMING1)) == (want[0], want[2]) and
                      b.r32(MB_TIMING_KHZ) == B.config_value('BCMFW_LCD_BUS_KHZ', defs),
                      '%s: mailbox %08x %08x %d' % (defs, b.r32(MB_TIMING0),
                                                    b.r32(MB_TIMING1), b.r32(MB_TIMING_KHZ)))
            self.frame_check(row, b, h, 'timing_%s' % (defs[0].split('=')[1] if defs else 'ours'))
            if defs:
                continue
            # the host's switch: Apple's words for the next frame, then each
            # sweep clock's (dev5: Apple's ns recomputed, config.h
            # BCMFW_SWEEP_KHZ_k), then ours again
            h.tick_task = False
            sweep = [(TIMING_SWEEP_MAGIC | k, timing_words(3, B.config_value(
                'BCMFW_SWEEP_KHZ_%d' % k))) for k in range(3)]
            for sel, w in [(TIMING_APPLE_MAGIC, TIMING_APPLE[3])] + sweep + [(0, want)]:
                n0 = len(b.panel.events)
                b.w32(MB_TIMING_SEL, sel)
                h.bcm_command(LCD_UPDATE)
                tw = [e[2] for e in b.panel.events[n0:]
                      if e[0] == 'ctl' and e[1] in (0x10, 0x14)]
                k = sel ^ TIMING_SWEEP_MAGIC
                khz_want = (0 if sel == TIMING_APPLE_MAGIC else
                            B.config_value('BCMFW_SWEEP_KHZ_%d' % k) if k < 3 else khz)
                row.check(tw == [w[0], w[2]] and b.r32(MB_TIMING1) == w[2] and
                          b.r32(MB_TIMING_KHZ) == khz_want,
                          'switch %08x: writes %s, kHz %d' % (
                              sel, ['%08x' % v for v in tw], b.r32(MB_TIMING_KHZ)))
                self.frame_check(row, b, h, 'timing_switch_%x' % sel)

    def t_boot_and_full(self):
        for pid in range(4):
            for model in ((1, 2) if pid in (1, 3) else (1,)):
                defs = ('BCMFW_PANEL_MODEL=2',) if model == 2 else ()
                row = Row('boot + first LCD_UPDATE, panel %d%s' % (
                    pid, ' model 2' if model == 2 else ''))
                self.rows.append(row)
                b, h, prog = self.bench(pid, defs)
                h.fb = random_frame(self.rng)
                t0, s0 = b.now(), b.sim.steps
                try:
                    h.lcd_awake()
                    h.settle()
                except Fail as e:
                    row.check(False, str(e))
                    continue
                row.instr = b.sim.steps - s0
                info = b.r32(MB_INFO)
                row.check(info >> 16 == 0x5042 and (info & 3) == pid and
                          (info & 0x300) == 0x300,
                          'info word %08x' % info)
                row.check(b.r32(MB_FRAMES) == 1, 'frame counter %d' % b.r32(MB_FRAMES))
                # nothing outside what bring-up needs: no clocks, SDRAM, DMA, cache
                vw = b.vpu_device_writes()
                blocks = sorted({a.addr & ~0x3ff for a in vw})
                row.check(set(blocks) <= {0x10001000, 0x10001800 & ~0x3ff, 0x10002800, 0x10004000},
                          'core wrote outside interrupts/GPIO/LCD: %s' % ['%08x' % x for x in blocks])
                row.check(h.rekicks == 0, '%d re-kicks (host timeout)' % h.rekicks)
                row.check(LCD_UPDATE in h.seen or BUSY in h.seen,
                          'host never saw the update in progress')
                init_ms = (b.now() - t0) / 1000.0
                self.metrics['awake_ms_p%d_m%d' % (pid, model)] = init_ms
                tag = 'full_p%d%s' % (pid, '_m2' if model == 2 else '')
                self.compare_on(row, pid, model, b.panel.events, tag)
                self.frame_check(row, b, h, tag, png=True)
                self.log('%s: %d instructions, %.0f ms simulated' % (row.name, row.instr, init_ms))

    def t_clock_gate(self):
        """hello v2 against vc2mmio.ClockBlock: the counter and the LCD
        interface each gated by one hypothesis about the clock block."""
        row = Row('clock gate: counter opened by 0x4C or 0x58, or never; image adapts')
        self.rows.append(row)
        want = {'reset': (CLK_RAN0, 0, True), '4c': (CLK_WROTE_A | CLK_RAN_A, 4, True),
                '58': (CLK_WROTE_A | CLK_WROTE_B | CLK_RAN_B, 5, True),
                None: (CLK_WROTE_A | CLK_WROTE_B, 5, False)}
        for timer_on, (bits, nwrites, runs) in want.items():
            tag = 'timer %s' % timer_on
            b, h, prog = self.bench(3)
            b.gate_clocks(timer_on)
            h.fb = random_frame(self.rng)
            try:
                h.lcd_awake()
                h.settle()
            except Fail as e:
                row.check(False, '%s: %s' % (tag, e))
                continue
            info, clk = b.r32(MB_INFO), b.r32(MB_CLK)
            row.check(bool(info & 0x200) == runs and bool(info & INFO_SPIN) != runs and
                      bool(info & INFO_CLK) == (timer_on != 'reset'),
                      '%s: info %08x' % (tag, info))
            row.check(clk & 0xFF == bits and (clk >> 8 & 0xF) == 0xC and
                      ((clk >> 16) > 0) == runs,
                      '%s: clock record %08x' % (tag, clk))
            cw = [(a.addr, a.value) for a in b.vpu_device_writes()
                  if a.addr & ~0x3FF in (0x10001C00, 0x10001400, 0x10000400)]
            row.check(cw == CLK_WRITES[:nwrites],
                      '%s: clock-block writes %s' % (tag, ['%08x=%08x' % w for w in cw]))
            row.check(b.r32(MB_STEP) == STEP_DONE and b.r32(MB_LIMIT) == 0,
                      '%s: step %08x limit %08x' % (tag, b.r32(MB_STEP), b.r32(MB_LIMIT)))
            self.frame_check(row, b, h, 'gate_%s' % timer_on)

        row = Row('clock gate: LCD clock off stalls the core; the step word names where')
        self.rows.append(row)
        for timer_on, lcd_on, ok in (('reset', None, False), ('reset', '4c', False),
                                     ('4c', '4c', True), (None, '58', True)):
            tag = 'timer %s lcd %s' % (timer_on, lcd_on)
            b, h, prog = self.bench(3)
            b.gate_clocks(timer_on, lcd_on)
            try:
                h.bcm_init()
                b.w32(CMD_WORD, LCD_UPDATE)
                h.wait(lambda: b.r32(CMD_WORD) not in (LCD_UPDATE, BUSY),
                       'LCD_UPDATE', limit_us=3_000_000)
                done = True
            except Fail:
                done = False
            step = b.r32(MB_STEP)
            if ok:
                row.check(done and b.stalled is None and step == STEP_DONE and
                          b.r32(CMD_WORD) == DONE,
                          '%s: done %s step %08x stall %s' % (tag, done, step, b.stalled))
            else:
                # the first LCD access of the power-on: interface on, timing
                row.check(not done and b.stalled is not None and step == STEP_POWER and
                          b.r32(CMD_WORD) == BUSY,
                          '%s: done %s step %08x status %08x stall %s' % (
                              tag, done, step, b.r32(CMD_WORD), b.stalled))

    def t_rects(self):
        rects = [(0, 0, 1, 1), (319, 239, 1, 1), (317, 5, 3, 7), (13, 17, 51, 33),
                 (0, 100, 320, 3), (300, 200, 40, 60), (1, 0, 318, 240), (159, 119, 2, 2)]
        for pid in range(4):
            row = Row('lcd_update_rect (host path) at edges, odd sizes, panel %d' % pid)
            self.rows.append(row)
            b, h, _ = self.bench(pid)
            h.fb = random_frame(self.rng)
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
                    self.frame_check(row, b, h, 'rect_p%d_%d_%d_%d_%d' % (pid, x, y, w, hh),
                                     png=(pid == 0 and (x, y) == (13, 17)))
            except Fail as e:
                row.check(False, str(e))

    def t_updaterect(self):
        rects = [(0, 0, 1, 1), (319, 239, 1, 1), (317, 5, 3, 7), (13, 17, 51, 33),
                 (0, 100, 320, 3), (300, 200, 40, 60), (5, 3, 0, 9), (400, 10, 5, 5)]
        for pid in range(4):
            row = Row('LCD_UPDATERECT (cmd 5, ours) windows, panel %d' % pid)
            self.rows.append(row)
            b, h, _ = self.bench(pid)
            h.fb = random_frame(self.rng)
            try:
                h.lcd_awake()
                h.settle()
                h.tick_task = False
                for (x, y, w, hh) in rects:
                    for yy in range(y, min(H, y + hh)):
                        for xx in range(x, min(W, x + w)):
                            h.fb[yy * W + xx] = self.rng.getrandbits(16)
                    for yy in range(y, min(H, y + hh)):
                        if x < W:
                            h.write_fb(x, yy, min(w, W - x))
                    b.w32(MB_RECT_XY, x | y << 16)
                    b.w32(MB_RECT_WH, w | hh << 16)
                    n0, s0 = b.panel.pixels_total, b.sim.steps
                    h.bcm_command(LCD_UPDATERECT)
                    row.instr += b.sim.steps - s0
                    cw = max(0, min(w, W - x)) if x < W and y < H else 0
                    ch = max(0, min(hh, H - y)) if x < W and y < H else 0
                    got = b.panel.pixels_total - n0
                    row.check(got == cw * ch, 'rect %s: %d pixels sent, want %d' % (
                        (x, y, w, hh), got, cw * ch))
                    self.frame_check(row, b, h, 'urect_p%d_%d_%d_%d_%d' % (pid, x, y, w, hh))
            except Fail as e:
                row.check(False, str(e))

    def t_back_to_back(self, defines=(), label=''):
        row = Row('back-to-back LCD_UPDATEs and a host re-kick%s' % label)
        self.rows.append(row)
        b, h, _ = self.bench(1, defines)
        h.fb = random_frame(self.rng)
        try:
            h.lcd_awake()
            h.settle()
            n = 12
            s0 = b.sim.steps
            for k in range(n):
                # lcd_update() with no wait in between: posted as NEED_UPDATE
                # while busy, then kicked by the tick when the BCM is free
                for i in self.rng.sample(range(W * H), 500):
                    h.fb[i] = self.rng.getrandbits(16)
                h.lcd_update()
                b.run(300)
            h.settle()
            row.instr = b.sim.steps - s0
            self.frame_check(row, b, h, 'b2b%s' % label.replace(' ', '_'))
            # a re-kick while busy (the host's 50 ms stall path)
            h.tick_task = False
            f0 = b.r32(MB_FRAMES)
            b.w32(CMD_WORD, LCD_UPDATE)
            b.step_until(lambda: b.r32(CMD_WORD) == BUSY, 100_000)
            b.run(3000)
            b.w32(CMD_WORD, LCD_UPDATE)            # host rewrites while busy
            h.tick_task = True
            h.state = LCD_UPDATING
            h.update_timeout = h.tick() + BCM_UPDATE_TIMEOUT
            h.settle()
            row.check(b.r32(MB_FRAMES) == f0 + 2,
                      're-kick: frames %d -> %d, want +2' % (f0, b.r32(MB_FRAMES)))
            self.frame_check(row, b, h, 'rekick%s' % label.replace(' ', '_'))
            row.check(not b.idle, 'core went idle (sleep with no wake-up)')
        except Fail as e:
            row.check(False, str(e))
        return b

    def t_sleep_awake(self):
        for pid in range(4):
            row = Row('LCD_SLEEP, 0xC, power cut, re-upload, re-init: panel %d' % pid)
            self.rows.append(row)
            b, h, _ = self.bench(pid)
            h.fb = random_frame(self.rng)
            try:
                h.lcd_awake()
                h.settle()
                n0 = len(b.panel.events)
                h.lcd_sleep()
                off = b.panel.events[n0:]
                row.check(off and off[-1][:2] == ('reset', 0), 'panel %d: reset not left low' % pid)
                row.check(not b.running, 'core still running after the power cut')
                if self.ref is not None:
                    _, ref_off = self.ref[(pid, 1)]
                    row.check(strip(off) == strip(ref_off),
                              'panel %d: power-off differs from vmcs.bin (%d vs %d events)' % (
                                  pid, len(off), len(ref_off)))
                    bad = delays_ok(off, ref_off)
                    row.check(not bad, 'panel %d: power-off delays %s' % (pid, bad[:3]))
                # wake: the 50 ms off-time, upload again, power-on again
                h.fb = random_frame(self.rng)
                n1 = len(b.panel.events)
                h.lcd_awake()
                h.settle()
                row.check(b.starts == 2, 'image started %d times' % b.starts)
                self.compare_on(row, pid, 1, b.panel.events[n1:], 'awake_p%d' % pid)
                self.frame_check(row, b, h, 'awake_p%d' % pid, png=(pid == 0))
            except Fail as e:
                row.check(False, str(e))

    def t_other_commands(self):
        row = Row('SELFTEST, TV, 0xC, unknown and malformed commands: acknowledged')
        self.rows.append(row)
        b, h, _ = self.bench(3)
        h.fb = random_frame(self.rng)
        try:
            h.lcd_awake()
            h.settle()
            h.tick_task = False
            ev0 = len(b.panel.events)
            for c in (SELFTEST, bcm_cmd(2), bcm_cmd(3), bcm_cmd(4), bcm_cmd(14),
                      bcm_cmd(0xC), bcm_cmd(0x7F), 0x12345678, 0xFFFFFFFF):
                s0 = b.sim.steps
                h.bcm_command(c)
                st = b.r32(CMD_WORD)
                row.check(st == DONE, 'command %08x: status %08x' % (c, st))
                row.check(b.sim.steps - s0 < 50_000, 'command %08x took %d instructions' % (
                    c, b.sim.steps - s0))
            row.check(len(b.panel.events) == ev0, 'panel traffic on a no-op command')
            # after them the panel still updates
            h.tick_task = True
            h.fb = random_frame(self.rng)
            h.lcd_update()
            h.settle()
            self.frame_check(row, b, h, 'after_noops')
        except Fail as e:
            row.check(False, str(e))

    def t_standby(self):
        """BCMFW_LCD_STANDBY: dark with reset high, wakes without the reset
        pulse; LCD_SLEEP from either depth ends in the stock power-down"""
        defs = ('BCMFW_LCD_STANDBY=1',)    # the CPU push (this bench has no DMA)
        for pid in (3, 0):
            for depth in (1, 2):
                row = Row('LCD_STANDBY %s, wake, LCD_SLEEP from %s: panel %d' % (
                    ('light', 'deep')[depth - 1], ('light', 'deep')[depth - 1], pid))
                self.rows.append(row)
                b, h, _ = self.bench(pid, defs)
                try:
                    h.fb = random_frame(self.rng)
                    h.lcd_awake()
                    h.settle()
                    h.tick_task = False
                    for cycle in range(2):
                        b.w32(0x1C8, 0x53425900 | depth)
                        n0 = len(b.panel.events)
                        h.bcm_command(bcm_cmd(0x43))
                        dark = b.panel.events[n0:]
                        row.check(b.r32(0x1CC) == depth and not b.r32(MB_INFO) & 0x100,
                                  'state %08x info %08x' % (b.r32(0x1CC), b.r32(MB_INFO)))
                        row.check(not any(e[0] == 'reset' for e in dark),
                                  'reset line moved going dark')
                        if cycle == 0:
                            n1 = len(b.panel.events)
                            h.tick_task = True
                            h.fb = random_frame(self.rng)
                            h.lcd_update()
                            h.settle()
                            h.tick_task = False
                            wake = b.panel.events[n1:]
                            row.check(not any(e[0] == 'reset' for e in wake),
                                      'reset pulse on a standby wake')
                            row.check(b.r32(0x1D0) >> 31 == 1, 'power-on not from standby')
                            self.frame_check(row, b, h, 'standby_p%d_%d' % (pid, depth))
                    # LCD_SLEEP from standby: the off script (again, after light)
                    n2 = len(b.panel.events)
                    h.bcm_command(bcm_cmd(8))
                    off = b.panel.events[n2:]
                    row.check(off and off[-1][:2] == ('reset', 0), 'reset not left low')
                    ran_off = any(e[:2] == (('cmd', 0x100) if pid == 3 else ('cmd', 0x10))
                                  for e in off)
                    row.check(ran_off == (depth == 1),
                              'off script %s after %s standby' % (
                                  'ran' if ran_off else 'missing',
                                  ('light', 'deep')[depth - 1]))
                except Fail as e:
                    row.check(False, str(e))

    def t_idle_sleep(self):
        b = self.t_back_to_back(('BCMFW_IDLE_SLEEP=1',), ' (idle-sleep build)')
        row = self.rows[-1]
        n49 = sum(1 for v, _ in b.sim.irqs if v == 49)
        row.check(n49 > 10, 'idle tick (vector 49) taken %d times' % n49)
        good, bad = b.sim.irq_returns
        row.check(bad == 0, 'interrupt returns: %d restored, %d clobbered' % (good, bad))
        self.metrics['idle_tick_irqs'] = n49
        self.metrics['idle_irq_returns_intact'] = '%d of %d' % (good, good + bad)
        row.check(b.r32(MB_INFO) & 0x400, 'info word lacks the idle-sleep bit')

    def t_reset_branch(self):
        row = Row('core started at 0 (BCMFW_RESET_BRANCH build)')
        self.rows.append(row)
        b, h, prog = self.bench(0, ('BCMFW_RESET_BRANCH=1',), reset_pc=0)
        h.fb = random_frame(self.rng)
        try:
            h.lcd_awake()
            h.settle()
            self.frame_check(row, b, h, 'reset_at_0')
        except Fail as e:
            row.check(False, str(e))

    def t_pixel_pace(self):
        row = Row('paced pixel path (BCMFW_PIXEL_PACE=2 build), full + rect')
        self.rows.append(row)
        defs = ('BCMFW_PIXEL_PACE=2',)
        b, h, prog = self.bench(2, defs)
        h.fb = random_frame(self.rng)
        try:
            h.lcd_awake()
            h.settle()
            self.frame_check(row, b, h, 'paced_full')
            h.tick_task = False
            for yy in range(30, 41):
                for xx in range(7, 30):
                    h.fb[yy * W + xx] = self.rng.getrandbits(16)
                h.write_fb(7, yy, 23)
            b.w32(MB_RECT_XY, 7 | 30 << 16)
            b.w32(MB_RECT_WH, 23 | 11 << 16)
            h.bcm_command(LCD_UPDATERECT)
            self.frame_check(row, b, h, 'paced_rect')
            b.w32(CMD_WORD, LCD_UPDATE)
            b.step_until(lambda: b.r32(CMD_WORD) == BUSY, 100_000)
            self.metrics['frame_instr_paced2_p2'] = b.step_until(
                lambda: b.r32(CMD_WORD) == DONE, 50_000_000)
        except Fail as e:
            row.check(False, str(e))

    def t_fault(self):
        row = Row('fault stub reports to the host')
        self.rows.append(row)
        b, h, prog = self.bench(0)
        try:
            h.bcm_init()
            # an exception frame as the hardware would push it: sr, pc
            sp = 0xC0000
            b.sim.r[25] = sp - 8
            b.bus.w32(sp - 8, 0)
            b.bus.w32(sp - 4, 0x1234)
            b.pc = prog.symbols['bcmfw_fault']
            b.run(1000)
            row.check(b.r32(CMD_WORD) == FAULT, 'status %08x' % b.r32(CMD_WORD))
            row.check(b.r32(MB_FAULT_PC) == 0x1234 and b.r32(MB_FAULT_SP) == sp - 8,
                      'fault pc %08x sp %08x' % (b.r32(MB_FAULT_PC), b.r32(MB_FAULT_SP)))
            row.check(b.idle, 'fault stub did not stop')
            vec = [b.bus.r32(4 * k) for k in range(64)]
            stubs = prog.symbols['bcmfw_vstubs']
            row.check(vec == [stubs + 8 * k for k in range(64)],
                      'vector k is not its own stub (bcmfw_vstubs + 8k)')
        except Fail as e:
            row.check(False, str(e))
        # a real exception (vc2sim's model) through each of a few vectors
        for v in (1, 3, 6, 40, 63):
            b, h, prog = self.bench(0)
            try:
                h.bcm_init()
                h.wait(lambda: b.r32(CMD_WORD) == DONE, 'ready')
                pc0, sp0 = b.pc, b.sim.r[25]
                b.pc = S.take_exception(b.sim, v, pc0, 'test')
                b.run(2000)
                row.check(b.r32(CMD_WORD) == FAULT and b.idle,
                          'vector %d: status %08x' % (v, b.r32(CMD_WORD)))
                row.check(b.r32(MB_FAULT_PC) == pc0 + S.HYP['exc_pc_offset'] and
                          b.r32(MB_FAULT_SP) == sp0 - 8,
                          'vector %d: fault pc %08x sp %08x' % (
                              v, b.r32(MB_FAULT_PC), b.r32(MB_FAULT_SP)))
            except Fail as e:
                row.check(False, 'vector %d: %s' % (v, e))

    # ---- probe v2 (BCMFW_PROBE=1): the results block (bringup.md)
    PROBE = ('BCMFW_PROBE=1',)
    PRB_RES, PRB_MAGIC = 0xCB000, 0x50524232
    MB_STEP, MB_FROM0, MB_PASS, MB_ALL, MB_TICKS = 0x1C0, 0x1C4, 0x1C8, 0x1CC, 0x1D0
    MB_FAULTED, MB_RESULTS = 0x1D8, 0x1DC
    STEP_READY = 0x50520003

    def probe_ready(self, defines=PROBE):
        b, h, prog = self.bench(0, defines)
        h.bcm_init()
        h.wait(lambda: b.r32(self.MB_STEP) == self.STEP_READY, 'the probe to finish')
        return b, h, prog

    def prec(self, b, i):
        """Record i of the results block (-1: record U), 12 words."""
        a = self.PRB_RES + 32 + 48 * (i + 1)
        return [b.r32(a + 4 * k) for k in range(12)]

    def probe_block_check(self, row, b, prog, tag):
        """The block after a full self-check under the current HYP."""
        hyp = S.HYP
        mis, off = hyp['exc_vec_misaligned'], hyp['exc_pc_offset']
        hdr = [b.r32(self.PRB_RES + 4 * k) for k in range(8)]
        row.check(b.r32(self.MB_PASS) == b.r32(self.MB_ALL) == 0x1FF,
                  '%s: PASS %03x ALL %03x' % (tag, b.r32(self.MB_PASS), b.r32(self.MB_ALL)))
        row.check(b.r32(self.MB_FAULTED) == 0x1200 and hdr[5] == 0x1200,
                  '%s: faulted %04x / %04x' % (tag, b.r32(self.MB_FAULTED), hdr[5]))
        row.check(b.r32(self.MB_RESULTS) == self.PRB_RES, '%s: results address' % tag)
        row.check(hdr[:5] == [self.PRB_MAGIC, 13 | 12 << 8 | 2 << 16, 0x1FF, 0x1E00,
                              0xDFF] and hdr[7] - hdr[6] == b.r32(self.MB_TICKS) > 0,
                  '%s: header %s' % (tag, ' '.join('%08x' % w for w in hdr)))
        row.check(b.r32(CMD_WORD) == DONE, '%s: status %08x' % (tag, b.r32(CMD_WORD)))
        u = self.prec(b, -1)
        row.check(u == [0xC5FF0000] + [0] * 11, '%s: record U written' % tag)
        for i in range(13):
            r = self.prec(b, i)
            st = r[0]
            row.check(st >> 24 == 0xC5 and (st >> 16) & 0xFF == i,
                      '%s: check %d status %08x' % (tag, i, st))
            if i in (9, 12):
                insn = prog.symbols['c%d_insn' % i]
                # silicon (device sessions 2-3): the load completes, then
                # vector 1 after 2 (writeback form) / 4 more instructions;
                # check 9 is followed by 16-bit nops, check 12 by 32-bit adds
                delta = (2 + 2 * hyp['exc_defer_insns_wb'] if i == 9 else
                         2 + 4 * min(hyp['exc_defer_insns'], 4))
                row.check(st & 0xFFFF == 3 | mis << 8 and r[10] == insn and
                          r[2] == insn + delta + off,
                          '%s: check %d status %08x pc %08x insn %08x' % (
                              tag, i, st, r[2], insn))
                row.check(r[3] == r[3] & ~3 and r[3] < 0xD0000 and r[3] > 0xCC000,
                          '%s: check %d frame at %08x' % (tag, i, r[3]))
            else:
                row.check(st & 0xFFFF == 1, '%s: check %d status %08x' % (tag, i, st))
        r9, r10, r11, r12 = (self.prec(b, i) for i in (9, 10, 11, 12))
        row.check(r9[4] == b.bus.r32(0x78) and r9[9] == 0x7B,
                  '%s: check 9 r0 %x (word at 0x78: %x) r5 %x' % (
                      tag, r9[4], b.bus.r32(0x78), r9[9]))
        buf = prog.symbols['probe_buf']
        row.check(r10[10] == 0x5EED1234 and r10[11] == buf + 4,
                  '%s: check 10 detail %08x %08x' % (tag, r10[10], r10[11]))
        row.check(r11[10] == 0xC35A and r11[11] == 0, '%s: check 11 detail %08x %08x' % (
            tag, r11[10], r11[11]))
        row.check(r12[4] == b.bus.r32(buf & ~3) and r12[5] == buf + 1 and
                  r12[6:10] == [0x111, 0x222, 0x333, 0x444][:min(hyp['exc_defer_insns'], 4)]
                  + [0] * (4 - min(hyp['exc_defer_insns'], 4)),
                  '%s: check 12 r0 %08x r1 %08x r2-r5 %s' % (tag, r12[4], r12[5],
                                                             r12[6:10]))
        excs = [(v, pc) for v, pc, _ in b.sim.excs]
        row.check(excs == [(mis, prog.symbols['c9_insn']), (mis, prog.symbols['c12_insn'])],
                  '%s: exceptions taken %s' % (tag, excs))
        if tag == 'default':
            # device sessions 2 and 3's P2 records, word for word, for the
            # parts that do not depend on the image layout (sp, sr) and,
            # for the device's own build (probe fnv 4d981526), all of them
            for i, dev in ((9, (0xFB0, 0xCFFEC, 0x8, 0xFAA, 0, 0x338, 0xFAA, 0xCB228,
                                0x1840, 0xA5C31234, 0x7B)),
                           (12, (0x1094, 0xCFFEC, 0x8, 0x1082, 0, 0x0BADF00D, 0x1855,
                                 0x111, 0x222, 0x333, 0x444))):
                rec = self.prec(b, i)
                got = (rec[2], rec[3], rec[1], rec[10], rec[11]) + tuple(rec[4:10])
                same = got == dev if prog.symbols['c9_insn'] == 0xFAA else \
                    got[1:3] == dev[1:3]
                row.check(same, '%s: check %d record %s, device %s' % (
                    tag, i, ' '.join('%x' % w for w in got),
                    ' '.join('%x' % w for w in dev)))

    def t_probe(self):
        row = Row('probe v2: 0x1FF pass, 9 and 12 fault and resume, block (HYP)')
        self.rows.append(row)
        try:
            b, h, prog = self.probe_ready()
            self.probe_block_check(row, b, prog, 'default')
            row.instr = b.sim.steps
        except Fail as e:
            row.check(False, str(e))
        row = Row('probe v2: other pc offsets and vector numbers resume the same')
        self.rows.append(row)
        saved = dict(S.HYP)
        try:
            for off, vec in ((6, 1), (0, 5), (6, 17), (2, 63), (4, 2)):
                S.HYP['exc_pc_offset'], S.HYP['exc_vec_misaligned'] = off, vec
                try:
                    b, h, prog = self.probe_ready()
                    self.probe_block_check(row, b, prog, 'offset %d vector %d' % (off, vec))
                except Fail as e:
                    row.check(False, 'offset %d vector %d: %s' % (off, vec, e))
        finally:
            S.HYP.clear()
            S.HYP.update(saved)

    def t_probe_faults(self):
        row = Row('probe v2: fault outside a check: record U, legacy words, stop')
        self.rows.append(row)
        for v in (6, 3, 33):
            try:
                b, h, prog = self.probe_ready()
                pc0, sp0, regs = b.pc, b.sim.r[25], list(b.sim.r[:6])
                sr0 = b.sim.r[30]
                b.pc = S.take_exception(b.sim, v, pc0, 'test')
                b.run(4000)
                u = self.prec(b, -1)
                want = [0xC5FF0003 | v << 8, sr0, pc0, sp0 - 8] + regs
                row.check(u[:10] == want, 'vector %d: record U %s' % (
                    v, ' '.join('%08x' % w for w in u)))
                row.check(b.r32(MB_FAULT_PC) == pc0 and b.r32(MB_FAULT_SP) == sp0 - 8,
                          'vector %d: fault pc %08x sp %08x' % (
                              v, b.r32(MB_FAULT_PC), b.r32(MB_FAULT_SP)))
                row.check(b.r32(CMD_WORD) == FAULT and b.idle,
                          'vector %d: status %08x' % (v, b.r32(CMD_WORD)))
                row.check(b.r32(self.MB_PASS) == 0x1FF and
                          b.r32(self.MB_FAULTED) == 0x1200,
                          'vector %d: the checks\' words changed' % v)
            except Fail as e:
                row.check(False, 'vector %d: %s' % (v, e))
        # an exception through vector 0 (a jump to 0) after the start
        try:
            b, h, prog = self.probe_ready()
            pc0, sp = b.pc, b.sim.r[25] - 8
            b.bus.w32(sp, b.sim.r[30])
            b.bus.w32(sp + 4, pc0)
            b.sim.r[25] = sp
            r0 = b.sim.r[0]
            b.pc = 0
            b.run(4000)
            u = self.prec(b, -1)
            row.check(u[0] == 0xC5FF0003 and u[2] == pc0 and u[4] == r0 and
                      b.r32(self.MB_FROM0) == 0,
                      'vector 0 re-entry: record U %s from0 %d' % (
                          ' '.join('%08x' % w for w in u[:5]), b.r32(self.MB_FROM0)))
            row.check(b.r32(CMD_WORD) == FAULT and b.r32(MB_FAULT_PC) == pc0 and b.idle,
                      'vector 0 re-entry: status %08x' % b.r32(CMD_WORD))
        except Fail as e:
            row.check(False, 'vector 0 re-entry: %s' % e)

        row = Row('probe v2: fault inside the handler: straight to the legacy stop')
        self.rows.append(row)
        try:
            b, h, prog = self.probe_ready()
            b.pc = S.take_exception(b.sim, 6, b.pc, 'first')
            common = prog.symbols['bcmfw_exc_common']
            busy = prog.symbols['bcmfw_exc_busy']
            b.step_until(lambda: b.sim.r[31] > common and b.bus.r32(busy) == 1, 200)
            pc1, sp1 = b.pc, b.sim.r[25]
            b.pc = S.take_exception(b.sim, 3, pc1, 'nested')
            b.run(4000)
            row.check(b.r32(CMD_WORD) == FAULT and b.idle, 'status %08x' % b.r32(CMD_WORD))
            row.check(b.r32(MB_FAULT_PC) == pc1 and b.r32(MB_FAULT_SP) == sp1 - 8,
                      'fault pc %08x (handler at %08x) sp %08x' % (
                          b.r32(MB_FAULT_PC), pc1, b.r32(MB_FAULT_SP)))
            row.check(self.prec(b, -1)[0] == 0xC5FF0000, 'record U written: %08x' %
                      self.prec(b, -1)[0])
        except Fail as e:
            row.check(False, str(e))

    def t_frame_cost(self):
        row = Row('instructions per LCD_UPDATE / UPDATERECT (panel on)')
        self.rows.append(row)
        for pid in (0, 1, 2):
            b, h, _ = self.bench(pid)
            h.fb = random_frame(self.rng)
            try:
                h.lcd_awake()
                h.settle()
                h.tick_task = False
                n = []
                for cmd, xy, wh in ((LCD_UPDATE, 0, 0), (LCD_UPDATERECT, 10 | 20 << 16, 100 | 50 << 16)):
                    b.w32(MB_RECT_XY, xy)
                    b.w32(MB_RECT_WH, wh)
                    b.w32(CMD_WORD, cmd)
                    b.step_until(lambda: b.r32(CMD_WORD) == BUSY, 100_000)
                    n.append(b.step_until(lambda: b.r32(CMD_WORD) == DONE, 5_000_000))
                self.metrics['frame_instr_p%d' % pid] = n[0]
                self.metrics['rect100x50_instr_p%d' % pid] = n[1]
                row.check(n[0] < 4 * W * H, 'panel %d: %d instructions per frame' % (pid, n[0]))
                row.instr += sum(n)
            except Fail as e:
                row.check(False, str(e))

    def t_libc(self):
        row = Row('libc subset: memcpy, memset (vc2sim, random cases)')
        self.rows.append(row)
        sim = S.Sim()
        prog = C.build([os.path.join(FW, 'lib', 'string.c')], extra=['-I' + FW] + B.CFLAGS,
                       extern=sim.import_addr, no_float=True, conservative=True)
        C.load_into_sim(prog, sim)
        rng = self.rng
        buf = 0x800000
        for k in range(200):
            n = rng.randrange(0, 70)
            so, do = rng.randrange(0, 8), rng.randrange(0, 8)
            src = bytes(rng.getrandbits(8) for _ in range(n + 16))
            sim.mem.write(buf, src)
            sim.mem.write(buf + 0x1000, bytes(n + 16))
            if k & 1:
                r = sim.call('memcpy', buf + 0x1000 + do, buf + so, n, check_abi=True)
                want = bytearray(n + 16)
                want[do:do + n] = src[so:so + n]
            else:
                c = rng.getrandbits(8)
                r = sim.call('memset', buf + 0x1000 + do, c | 0x100 * rng.getrandbits(8), n,
                             check_abi=True)
                want = bytearray(n + 16)
                want[do:do + n] = bytes([c]) * n
            got = sim.mem.read(buf + 0x1000, n + 16)
            row.check(bytes(got) == bytes(want) and r == buf + 0x1000 + do,
                      'case %d (n %d, offsets %d/%d)' % (k, n, so, do))
        row.instr = sim.steps

    def t_float_ban(self):
        row = Row('integer-only: float rejected at compile and link')
        self.rows.append(row)
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'f.c')
            with open(p, 'w') as f:
                f.write('#include "config.h"\nint f(int x) { float y = x; return y * 2; }\n')
            try:
                C.build([p], extra=['-I' + FW], no_float=True)
                row.check(False, 'float accepted with config.h')
            except RuntimeError as e:
                row.check('poison' in str(e), 'unexpected error: %s' % str(e)[:200])
            p2 = os.path.join(d, 'g.c')
            with open(p2, 'w') as f:
                f.write('int g(int x) { double y = x; return y / 3; }\n')
            try:
                C.build([p2], no_float=True)
                row.check(False, 'double accepted by --no-float')
            except C.FloatBanned as e:
                row.check('__' in str(e), str(e))

    # Instruction kinds the image may execute although no Apple code runs
    # them in the vc2sim self-test: encodings that occur in Apple's static
    # code with their operation/condition run in another encoding, and
    # kinds verified on the device (tools/bcmre/vc2forms.py).
    ACCEPTED_KINDS = F.APPLE_STATIC_ONLY | F.DEVICE_VERIFIED

    def t_coverage(self, ours):
        row = Row('image uses only instruction kinds the self-test exercises')
        self.rows.append(row)
        corpus = os.path.dirname(os.path.dirname(self.args.vmcs))
        import collections
        import contextlib
        import io
        S.COVER = collections.Counter()
        with contextlib.redirect_stdout(io.StringIO()):
            rc = vc2sim_tests.main([corpus])
        apple = set(S.COVER)
        S.COVER = None
        row.check(rc == 0, 'vc2sim self-test failed')
        extra = sorted(set(ours) - apple)
        self.metrics['kinds_executed'] = len(ours)
        self.metrics['kinds_not_in_selftest'] = ' '.join(k.replace(' ', ':') for k in extra)
        row.check(set(extra) <= self.ACCEPTED_KINDS,
                  'unproven kinds: %s' % ', '.join(sorted(set(extra) - self.ACCEPTED_KINDS)))

    def run(self):
        t0 = time.time()
        cover = self.args.vmcs and os.path.exists(self.args.vmcs) and not self.args.only
        self.ref = reference_sequences(self.args.vmcs, self.log)
        if cover:
            S.COVER = collections.Counter()     # our image only, from here
        if self.ref is None:
            print('note: vmcs.bin 1.3 not found: power sequences checked against '
                  'hardware.md facts only')
        for t in (self.t_timing, self.t_boot_and_full, self.t_clock_gate, self.t_rects, self.t_updaterect,
                  self.t_back_to_back, self.t_sleep_awake, self.t_other_commands,
                  self.t_idle_sleep, self.t_standby, self.t_reset_branch, self.t_pixel_pace,
                  self.t_fault,
                  self.t_frame_cost, self.t_libc, self.t_float_ban,
                  # after the coverage snapshot: the probe runs the forms
                  # it tests (forms_test checks them statically)
                  self.t_probe, self.t_probe_faults):
            if self.args.only and self.args.only not in t.__name__:
                continue
            n0 = len(self.rows)
            t()
            if t == self.t_float_ban and cover:
                ours = collections.Counter(S.COVER)
                self.t_coverage(ours)
            for row in self.rows[n0:]:
                print('%-72s %5d %5d %10d' % (row.name[:72], row.cases,
                                              row.cases - len(row.fails), row.instr))
                for f in row.fails[:8]:
                    print('    FAIL: %s' % f)
        if Bench.lone_tails:
            print('FAIL: the host port model lost %d lone trailing halfwords: a '
                  'writer sent an odd count' % Bench.lone_tails)
        cases = sum(r.cases for r in self.rows)
        fails = sum(len(r.fails) for r in self.rows) + (Bench.lone_tails > 0)
        prog, rep, img = self.image()
        print()
        print(rep['lines'][0])
        for k in sorted(self.metrics):
            v = self.metrics[k]
            print('  %-24s %s' % (k, ('%.1f' % v) if isinstance(v, float) else v))
        print('%d rows, %d cases, %d failed (seed %d, %.0f s); PNGs in %s' % (
            len(self.rows), cases, fails, self.args.seed, time.time() - t0, self.png_dir))
        return 1 if fails else 0


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--seed', type=int, default=2722)
    ap.add_argument('--vmcs', default=DEFAULT_VMCS)
    ap.add_argument('--png', default=os.path.join(ROOT, 'build', 'bcmfw', 'test'))
    ap.add_argument('--only', default='')
    a = ap.parse_args(argv)
    if not B.have_gcc():
        print('skip: vc4-elf-gcc not found at %s (set VC4_GCC)' % C.GCC)
        return 0
    return Suite(a).run()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
