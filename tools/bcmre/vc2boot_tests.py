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
"""Self-test rows for the modelled bus (vc2mmio) and the boot runner.

They boot Apple's vmcs.bin (reference only, never shipped) against the
peripheral model and check the hardware facts recorded in
docs/research/bcm2722/hardware.md: the register writes its start-up
makes, that interrupts are taken and returned from correctly, and the
LCD power-on sequence for each panel ID.  So the facts stay
reproducible, and a change to the simulator that breaks them shows up.
They also decide two ISA hypotheses no library function can: the push
order (an interrupt frame is saved in two pushes and restored by one
pop) and the word alignment of pc-relative loads (HYP pc48_mem_aligned).
Addresses of vmcs.bin functions are 1.3's; other versions skip.
"""
import hashlib

import vc2sim as S
import vc2mmio as M
from vc2sim_tests import Row, Skip

VMCS_13_SHA256 = 'cde4b5a9c9a020b6e23de1be0492d9a853b7d974c4060462d5f57a80e47f55be'
LCD_POWER = 0xf672          # vmcs.bin 1.3: "display_control ... power=N" handler
BOOT_STEPS = 400_000


def _sim(ctx):
    if not ctx.vmcs:
        raise Skip('vmcs.bin not found')
    if hashlib.sha256(open(ctx.vmcs, 'rb').read()).hexdigest() != VMCS_13_SHA256:
        raise Skip('vmcs.bin is not 1.3 (function addresses differ)')
    sim = S.make_boot_sim(ctx.vmcs, S.BCM_RULES, S.BCM_ALIASES, machine=True,
                          us_per_read=4)
    sim.irq_ctx = {}
    reason, pc = S.boot(sim, max_steps=BOOT_STEPS)
    return sim, reason


# Writes vmcs.bin's start-up makes, in this order (hardware.md "Start-up").
BOOT_WRITES = [
    (0x10000c00, 0x80000000, 'cache on'),
    (0x10000400, 0xa5a50000, 'reset/run control: bit 0 cleared'),
    (0x10000408, 0xa5a50002, 'second run-control word'),
    (0x10001400, 0xa5a5000f, 'regulator level 15 for 150 MHz'),
    (0x10001c00, 0xa5a5006c, 'PLL A divider M = 108'),
    (0x10001c14, 0xa5a50258, 'PLL A multiplier N = 600 (27 MHz * 600 / 108 = 150 MHz)'),
    (0x10001c14, 0xa5a50658, 'PLL A enable (bit 10) after the lock wait'),
    (0x10001c28, 0xa5a50101, 'core clock from PLL A'),
    (0x10003424, 0x04930000, 'SDRAM refresh 1171 cycles = 7.8 us at 150 MHz'),
    (0x10003410, 0x0105008c, 'SDRAM timing'),
    (0x10003414, 0x0105000c, 'SDRAM timing'),
    (0x10001808, None, 'system tick compare'),
    (0x10003430, 0x000080e8, 'SDRAM init'),
    (0x10003428, 0x3057c834, 'SDRAM mode word'),
    (0x10003420, 0x00000007, 'SDRAM command 7'),
    (0x10003430, 0x00000064, 'SDRAM run'),
    (0x10000c00, 0x80010000, 'cache on + SDRAM (bit 16)'),
]


def t_boot(ctx):
    sim, reason = _sim(ctx)
    log = sim.mem.log
    row = Row(ctx, 'vmcs.bin start-up writes (MMIO model)', 'vmcs',
              'hardware.md "Start-up"; push order, pc-relative alignment')
    row.instr = sim.steps
    row.check(reason.startswith('step limit') or reason.startswith('idle'),
              'boot stopped early: %s' % reason)
    writes = [a for a in log if a.rw == 'W']
    k = 0
    for addr, val, what in BOOT_WRITES:
        while k < len(writes) and not (writes[k].addr == addr and
                                       (val is None or writes[k].value == val)):
            k += 1
        row.check(k < len(writes), 'missing or out of order: %08x <- %s (%s)' % (
            addr, 'any' if val is None else '%08x' % val, what))
        k = min(k + 1, len(writes))
    # PLL lock wait: at least 1 ms between programming N and enabling.
    t = [a.us for a in writes if a.addr == 0x10001c14 and a.value in (0xa5a50258, 0xa5a50658)]
    row.check(len(t) == 2 and t[1] - t[0] >= 1000, 'PLL lock wait %s' % t)
    # System tick: 100 Hz, compare 0 = counter + 10000.
    ticks = [a for a in writes if a.addr == 0x10001808]
    reads = {a.seq: a.value for a in log if a.addr == 0x10001804}
    ok = False
    for w in ticks:
        before = [v for s, v in reads.items() if s < w.seq]
        if before and 9900 <= (w.value - before[-1]) & S.MASK <= 10000:
            ok = True
    row.check(ok, 'no 10 ms system-tick compare')
    n48 = sum(1 for v, _ in sim.irqs if v == 48)
    row.check(n48 >= 5, 'system tick taken %d times' % n48)
    # Every interrupted context resumes with all its registers: the
    # handlers save r0-r5/lr and r6-r15 in two pushes and restore them
    # with one "pop r0-r15", which only works with push order
    # "highest register lowest" (HYP push_lowest_first = False).
    good, bad = sim.irq_returns
    row.check(good >= 100 and bad == 0,
              'interrupt returns: %d restored, %d clobbered' % (good, bad))
    # Host interface: 0x1fc holds the address of its control block (heap).
    p = sim.mem.r32(0x1fc)
    row.check(0x200 <= p < 0x140000 or 0xfe000000 <= p < 0xfe400000,
              'host block pointer at 0x1fc = %08x' % p)


# The tail of each panel's power-on (state 2): commands in order.
PANELS = {
    0: [0x2d, 0x36, 0x26, 0x3a, 0x30, 0x11, 0x2a, 0x2b, 0x29, 0x35, 0x2b, 0x2a, 0x2c],
    1: [0x0001, 0x0002, 0x0003, 0x0300, 0x030d, 0x0400, 0x0007, 0x0100, 0x0007, 0x0030,
        0x0007, 0x0210, 0x0211, 0x0212, 0x0213, 0x0200, 0x0201, 0x0202],
    2: [0x0001, 0x0002, 0x0003, 0x0039, 0x0007, 0x0013, 0x0010, 0x0071, 0x0044, 0x0045,
        0x0046, 0x0007, 0x0020, 0x0021, 0x0022],
    3: [0x0008, 0x0010, 0x0090, 0x0300, 0x032d, 0x0400, 0x0007, 0x0100, 0x0007, 0x0030,
        0x0007, 0x0210, 0x0213, 0x0200, 0x0201, 0x0202],
}
TIMING = {0: (0x410d0206, 0x41010705), 1: (0x41002a19, 0x40001407),
          2: (0x41012325, 0x41010605), 3: (0x41002a19, 0x40001407)}


def _subseq(want, got):
    k = 0
    for c in got:
        if k < len(want) and c == want[k]:
            k += 1
    return k == len(want)


def _snapshot(sim):
    t, ic = sim.mem.device(M.SysTimer), sim.mem.device(M.IntCtl)
    return (bytes(sim.mem.b), list(sim.r), dict(sim.mem.regs), list(sim.mem.rules),
            (t.clo, list(t.cmp), list(t.armed), t.cs), (ic.level, list(ic.pri)))


def _restore(sim, snap):
    b, r, regs, rules, (clo, cmp_, armed, cs), (level, pri) = snap
    sim.mem.b[:] = b
    sim.r[:] = r
    sim.mem.regs = dict(regs)
    sim.mem.rules = list(rules)
    t, ic = sim.mem.device(M.SysTimer), sim.mem.device(M.IntCtl)
    t.clo, t.cmp, t.armed, t.cs = clo, list(cmp_), list(armed), cs
    ic.level, ic.pri = level, list(pri)


def t_lcd(ctx):
    row = Row(ctx, 'LCD power-on per panel ID (vmcs 0xf672)', 'vmcs',
              'hardware.md "LCD": reset pulse, bus timing, commands')
    sim, _ = _sim(ctx)
    snap = _snapshot(sim)
    for pid in sorted(PANELS):
        _restore(sim, snap)
        sim.mem.rule(S.panel_rule(pid))
        n0 = len(sim.mem.log)
        before = sim.steps
        r = row.guard(lambda: sim.call(LCD_POWER, 2, max_steps=50_000_000),
                      'panel %d power-on' % pid)
        row.instr += sim.steps - before
        if r is None:
            continue
        lines = M.describe_lcd(sim.mem.log[n0:])
        cmds = [int(x.split()[1], 16) for x in lines if x.startswith('cmd ')]
        cmds = [c for c in cmds if c != 0]
        gpio = [x for x in lines if x.startswith('gpio')]
        row.check(gpio[:4] == ['gpio clear 48', 'gpio set 48', 'gpio clear 48', 'gpio set 48'],
                  'panel %d reset pulse %s' % (pid, gpio[:4]))
        tw = [x for x in lines if x.startswith('write 1000401')]
        row.check(tw[:2] == ['write 10004010 <- %08x' % TIMING[pid][0],
                             'write 10004014 <- %08x' % TIMING[pid][1]],
                  'panel %d bus timing %s' % (pid, tw[:2]))
        row.check(_subseq(PANELS[pid], cmds) and cmds[-1] == PANELS[pid][-1],
                  'panel %d commands %s' % (pid, ' '.join('%x' % c for c in cmds[-20:])))


TESTS = [t_boot, t_lcd]
