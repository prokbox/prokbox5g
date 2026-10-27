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

"""Turn the risk tool 1 drivers' measurements into a device-time estimate.

    device_time.py <driver_output.log>

Reads the CSV lines printed by scripts/risk/drivers/{sortkey,scroll,
library_sort}_drv (one real run's stdout, redirected to a file -- see
scripts/risk/arm_insn_count.sh) and combines them with the ARM7TDMI
instruction-cost constants below to print instructions-at-scale and a
device-time range, compared against the project's existing x400
(docs/process.md) / x200-400 (various docs/hardware.md rows) estimates.

ARM instruction constants (ARM7TDMI, -mcpu=arm7tdmi -Os, no thumb):
two sources, both in docs/risk-tooling.md:

  1. qemu-system-arm + a TCG plugin (build/risk/qemu/libinsn.c, a ~40
     line instruction counter built against the qemu-plugin.h Homebrew's
     qemu ships) running the REAL, unmodified ui_sortkey()/
     ui_scroll_step() under -M versatilepb -cpu arm926 -semihosting,
     counting every instruction actually executed for a representative
     call set, to a clean semihosting SYS_EXIT. This is a genuine
     dynamic count (SORTKEY_INSNS_PER_CALL, SCROLL_INSNS_PER_GLIDE
     below), not an estimate -- the ARM926 core is only the emulator's
     CPU model; it executes the same ARMv4T instruction stream an
     ARM7TDMI would, so the *count* transfers (only the cycle-time
     conversion below is ARM7TDMI-specific). Caveats: (a) no arm-linux
     user-mode qemu is available via Homebrew on macOS (only
     qemu-system-arm), so this runs a tiny bare-metal harness instead of
     the originally-planned qemu-arm + libinsn user-mode path; (b) no
     plugin .so ships prebuilt, so build/risk/qemu/libinsn.c is a
     from-scratch replacement, not upstream QEMU's contrib/plugins/
     insn.c; (c) ui_sortkey's memcpy/memmove calls are resolved against
     a plain C byte-loop shim (build/risk/qemu/libc_shim.c), not
     Rockbox's real hand-optimized firmware/asm/arm/{memcpy,memmove}.S
     (those assemble only inside the full firmware build's macro
     environment -- "ldmpc" and the pull/push shift macros are defined
     elsewhere in that build, not standalone) -- a small, documented,
     conservative (not an undercount) overstatement for the 1-4 byte
     copies ui_sortkey actually does.
  2. hand-traced from `arm-none-eabi-objdump -d` of the same real files,
     address by address (docs/risk-tooling.md) -- used only for
     apps/ipodui/library.c's cmp_rec/key_prefix, where a qemu harness
     would also need stub externs for tagcache_ui_seeks()/label_of()
     (library.c's dependencies on the real tagcache, out of this tool's
     scope) and was not attempted. scripts/risk/lib/objdump_count.py's
     live per-function totals, printed by arm_insn_count.sh, are the
     regression signal for these: if cmp_rec/key_prefix's total static
     instruction count has drifted far from the 104/33 recorded in
     docs/risk-tooling.md, the hand trace needs redoing.

Why instructions, not just the sim's existing x400: the sim host is an
Apple M-class core (>20,000 "MIPS" on this branchy, memory-bound code,
per apps/ipodui/library.c's header) against an ARM7TDMI at 30/80 MHz --
a ~250-650x clock-speed gap alone, before any CPI difference. x400 folds
clock ratio and CPI into one uncalibrated guess; this tool separates them
so each can be checked against a cited basis.
"""
import os
import re
import sys

# -- ARM7TDMI instruction costs (see module docstring) -------------------
#
# sortkey/scroll: qemu-measured (build/risk/qemu/, 2026-10-06), per call,
# for a representative call mix -- not per-character: ui_sortkey's cost
# depends on input length, but title/album/artist names all average
# ~11.7-11.8 chars (scripts/risk/drivers/sortkey_drv.c), so one measured
# per-call constant covers all three roles at this library shape.
SORTKEY_INSNS_PER_CALL = 783.5   # qemu: 1,567,013 insns / 2,000 calls
                                 # (build/risk/qemu/sortkey.elf, 39
                                 # representative names, avg ~13 chars)
SCROLL_INSNS_PER_GLIDE = 854.0   # qemu: 427,052 insns / 500 glides
                                 # (build/risk/qemu/scroll.elf); includes
                                 # this harness's own driving-loop
                                 # overhead (~8-10 insns/tick for the
                                 # call+branch the harness adds), so it
                                 # is a conservative (slightly high)
                                 # stand-in for ui_scroll_step() alone

# Hand-traced (docs/risk-tooling.md), not qemu-measured: library.c's
# cmp_rec/key_prefix call tagcache_ui_seeks()/label_of(), out of this
# tool's scope to stub for a qemu run (see module docstring).
SORTKEY_PER_CHAR_FAST = 40      # sortkey.c:200-268 loop body, common ASCII
                                 # letter path (hand-traced, ui_sortkey.o)
MEMCPY_SMALL = 11               # firmware/asm/arm/memcpy.S label 0:-8:
                                 # (n<=3 bytes: the per-character fold copy)
SORTKEY_PER_CHAR = SORTKEY_PER_CHAR_FAST + MEMCPY_SMALL        # 51
SORTKEY_FIXED = 30              # entry/exit, leading "the "/space strip
MEMMOVE_HASHGROUP = 20          # firmware/asm/arm/memmove.S, ~15-byte
                                 # prefix-insert shift (HASH_GROUP keys)

SCROLL_STEP_FIXED = 18          # ui_scroll_step entry+exit+finalize
SCROLL_STEP_PER_TICK = 18       # loop body + per-tick condition recheck

CMP_REC_FAST = 13               # cmp_rec prefix-mismatch fast path
                                 # (library.c:466-482); tie-break path
                                 # (cmp_tie/cmp_labels, full ui_sortkey x2
                                 # + strcmp + strcasecmp) is NOT modelled:
                                 # rare at this scale, flagged separately
KEY_PREFIX_WRAPPER = 33 + 8     # key_prefix's own body (measured, 33) +
                                 # memset(16 B) (assumed, ~8)

# ARM7TDMI CPI range the brief asks for, cited basis: a 3-stage pipeline
# with single-cycle data-processing ops but 2-3 cycle loads/stores and
# ~3-cycle branches (no prediction) gives a blended ~1.2 for
# branch-light, mostly-register code and up to ~2.0 once SDRAM wait
# states and this code's branch density are counted. Cross-check: the
# project's own existing anchor (apps/ipodui/library.c, scripts/risk
# docstring) states ~70 MIPS at 80 MHz boosted on cached code, i.e.
# CPI = 80e6/70e6 ~= 1.14 -- inside, at the low end of, this range.
CPI_LO, CPI_HI = 1.2, 2.0
CLOCK_NORMAL, CLOCK_BOOST = 30e6, 80e6

RE = {
    'sortkey': re.compile(r'^sortkey,(\w+),(\d+),(\d+),([\d.]+),([\d.]+),'
                          r'([\d.]+),([\d.]+)$'),
    'scroll': re.compile(r'^scroll,(\d+),(\d+),(\d+),([\d.]+)$'),
    'libsort': re.compile(r'^libsort,(\w+),(\d+),(\d+),([\d.]+)$'),
}


def parse(path):
    rows = {'sortkey': [], 'scroll': [], 'libsort': []}
    with open(path) as f:
        for line in f:
            line = line.strip()
            for kind, rx in RE.items():
                m = rx.match(line)
                if m:
                    rows[kind].append(m.groups())
                    break
    return rows


def device_time_ms(total_insns):
    lo = total_insns * CPI_LO / CLOCK_BOOST * 1000
    hi = total_insns * CPI_HI / CLOCK_NORMAL * 1000
    return lo, hi


def fmt_ms(ms):
    if ms < 1:
        return '%.2f ms' % ms
    if ms < 1000:
        return '%.0f ms' % ms
    return '%.1f s' % (ms / 1000)


def main():
    global SORTKEY_INSNS_PER_CALL, SCROLL_INSNS_PER_GLIDE
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    # a fresh qemu/Unicorn run (scripts/risk/arm_insn_count.sh) overrides
    # the documented 2026-10-06 snapshot above with today's measurement
    if os.environ.get('RISK_SORTKEY_INSNS_PER_CALL'):
        SORTKEY_INSNS_PER_CALL = float(os.environ['RISK_SORTKEY_INSNS_PER_CALL'])
    if os.environ.get('RISK_SCROLL_INSNS_PER_GLIDE'):
        SCROLL_INSNS_PER_GLIDE = float(os.environ['RISK_SCROLL_INSNS_PER_GLIDE'])
    rows = parse(sys.argv[1])
    print('%-28s %10s %12s %10s %14s %10s' %
          ('workload', 'n/calls', 'instructions', 'device ms',
           'sim host ms', 'factor'))

    total_all = 0
    for role, n, total_in, avg_in, avg_out, hashpct, us in rows['sortkey']:
        n = int(n); avg_in = float(avg_in); hashpct = float(hashpct)
        us = float(us)
        per_call = SORTKEY_INSNS_PER_CALL      # qemu-measured (see above)
        total = per_call * n
        lo, hi = device_time_ms(total)
        sim_ms = us / 1000
        factor = (lo / sim_ms) if sim_ms > 0 else float('nan')
        print('%-28s %10d %12d %6s-%-6s %13s %9.0fx-%.0fx' %
              ('sortkey precompute (%s)' % role, n, int(total),
               fmt_ms(lo), fmt_ms(hi), fmt_ms(sim_ms),
               factor, (hi / sim_ms) if sim_ms > 0 else 0))
        total_all += total
        # cross-check: the hand-traced, host-measured-length model
        hand = (SORTKEY_FIXED + avg_in * SORTKEY_PER_CHAR
                + (hashpct / 100) * MEMMOVE_HASHGROUP) * n
        print('%-28s %10s %12s   (cross-check, hand-traced model: %d '
              'insns, %+.0f%% vs qemu)'
              % ('', '', '', int(hand), 100 * (hand - total) / total))

    for glides, calls, ticks, us in rows['scroll']:
        glides = int(glides); calls = int(calls); us = float(us)
        total = SCROLL_INSNS_PER_GLIDE * glides
        lo, hi = device_time_ms(total)
        sim_ms = us / 1000
        print('%-28s %10d %12d %6s-%-6s %13s %9.0fx-%.0fx' %
              ('ui_scroll_step (glides)', int(glides), int(total),
               fmt_ms(lo), fmt_ms(hi), fmt_ms(sim_ms),
               (lo / sim_ms) if sim_ms > 0 else 0,
               (hi / sim_ms) if sim_ms > 0 else 0))
        per_tick_lo = lo * 1e6 / calls if calls else 0
        per_tick_hi = hi * 1e6 / calls if calls else 0
        print('%-28s %10s %12s   (per call/tick: %.0f-%.0f ns device, '
              'CPI %.1f-%.1f, 30-80 MHz; 33 ms/frame budget)'
              % ('', '', '', per_tick_lo, per_tick_hi, CPI_LO, CPI_HI))

    for cat, n, cmp_calls, us in rows['libsort']:
        n = int(n); cmp_calls = int(cmp_calls); us = float(us)
        if cmp_calls == 0:
            print('%-28s %10d %12s %6s %19s'
                  % ('lib_open %s (no qsort)' % cat, n, '-', '-',
                     fmt_ms(us / 1000) + ' sim (walk, not sorted here)'))
            continue
        # key_prefix's embedded ui_sortkey(keylen=16) call: approximated
        # with the qemu/Unicorn-measured per-call cost (keylen=32 in
        # that harness) -- conservative (slightly high) for names over
        # ~15 chars, which truncate earlier at keylen=16 than they did
        # in the measurement.
        key_prefix_total = n * (KEY_PREFIX_WRAPPER + SORTKEY_INSNS_PER_CALL)
        cmp_total = cmp_calls * CMP_REC_FAST
        total = key_prefix_total + cmp_total
        lo, hi = device_time_ms(total)
        sim_ms = us / 1000
        print('%-28s %10d %12d %6s-%-6s %13s %9.0fx-%.0fx' %
              ('lib_open %s (sort)' % cat, n, int(total),
               fmt_ms(lo), fmt_ms(hi), fmt_ms(sim_ms),
               (lo / sim_ms) if sim_ms > 0 else 0,
               (hi / sim_ms) if sim_ms > 0 else 0))
        total_all += total

    print()
    print('Existing docs figure: x400 (docs/process.md "CPU", '
          'uncalibrated); x200-400 for I/O-bound paths (docs/hardware.md).')
    print('"factor" above is this tool\'s bottom-up device-ms / sim-ms for '
          'the same operation -- the multiplier x400 should be, per '
          'workload, not a replacement single number.')


if __name__ == '__main__':
    main()
