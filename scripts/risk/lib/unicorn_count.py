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

"""Dynamic ARM instruction count via Unicorn (risk tool 1, cross-check).

    out/risk-venv/bin/python3 unicorn_count.py <flat.bin> [stop_offset]

Loads a flat binary produced from a bare-metal ARM ELF (built by
scripts/risk/arm_insn_count.sh: arm-none-eabi-gcc -mcpu=arm7tdmi,
-nostartfiles + build/risk/qemu/start.S, linked at 0x0 -- see
build/risk/qemu/link.ld) into Unicorn's ARM emulator at address 0,
counts every instruction executed with a UC_HOOK_CODE callback, and
stops at 'stop_offset' (default: 0x10, the "svc" in start.S -- stopped
*before* executing it, since Unicorn doesn't implement ARM semihosting;
unlike the qemu-system-arm path, which runs the real semihosting SYS_EXIT
to stop cleanly, this script just bounds emulation to the instruction
range we care about).

This is a second, independent dynamic-count engine (Unicorn, a
QEMU-derived CPU emulation library with no TCG plugin involved) used to
cross-check scripts/risk/arm_insn_count.sh's qemu-system-arm + TCG-plugin
counts (build/risk/qemu/libinsn.c) for the same ELF. No ARM7TDMI CPU
model exists in Unicorn (see UC_CPU_ARM_* in unicorn.arm_const); this
uses UC_CPU_ARM_926 (ARMv5TE), the same substitution the qemu path makes
with "-cpu arm926" on qemu-system-arm -- a model instruction count is
determined by the executed ARMv4T-encoded instruction stream, not by
which superset core decodes it, so the count should match regardless of
which of the two emulators (or which ARMv4T-compatible core model) runs
it; a large mismatch between this and the qemu-plugin figure would mean
one of the two harnesses is wrong, not that ARM7TDMI itself behaves
differently.
"""
import sys

try:
    import unicorn as uc
    import unicorn.arm_const as ac
except ImportError:
    sys.exit('unicorn not installed: run\n'
             '  python3 -m venv out/risk-venv && '
             'out/risk-venv/bin/pip install unicorn\n'
             'and invoke this script with out/risk-venv/bin/python3')

RAM_BASE = 0x00000000
RAM_SIZE = 4 * 1024 * 1024    # 4 MiB: plenty for .text/.data/.bss + a
                              # 1 MiB stack (start.S sets sp=0x100000)


def count_insns(bin_path, stop_offset=0x10, cpu=ac.UC_CPU_ARM_926):
    with open(bin_path, 'rb') as f:
        code = f.read()

    mu = uc.Uc(uc.UC_ARCH_ARM, uc.UC_MODE_ARM)
    try:
        mu.ctl_set_cpu_model(cpu)
    except AttributeError:
        pass          # older unicorn: default CPU model only
    mu.mem_map(RAM_BASE, RAM_SIZE)
    mu.mem_write(RAM_BASE, code)

    counter = {'n': 0}

    def hook_code(_mu, _address, _size, _user):
        counter['n'] += 1

    mu.hook_add(uc.UC_HOOK_CODE, hook_code)
    mu.emu_start(RAM_BASE, RAM_BASE + stop_offset)
    return counter['n']


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    stop = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x10
    n = count_insns(path, stop)
    print('UNICORN_INSN_COUNT %d' % n)


if __name__ == '__main__':
    main()
