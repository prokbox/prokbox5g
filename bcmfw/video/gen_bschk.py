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

"""Generate bcmfw/video/bschk_kernels.S: the start-up checks of the vector
forms a loop filter on the vector unit would need (batch 4's proposal,
docs/research/bcm2722/decoder.md, "The loop filter in vectors").  No
decoder code uses these forms yet: each kernel runs the one unproven form
once on data the C (video/bschk.c) prepares, and reads the result out
through forms silicon has run, so a disagreement names that form.  The
results go to the job block's J_VPU2 words (jobs.h) for the bench's jvpu
step to log; nothing depends on them.

Every kernel: r0 the input (32-byte aligned unless said), r1 the output
(32-byte aligned, 32 bytes: 16 halfwords), r2, r3 scratch.

  jv_bschk_l1  BSL1  vldp16 + vldq16 single, no stride, no offset: an H16
               row from r0 at a 2- or 8-byte line phase; stored as is
  jv_bschk_f1  BSF1  vsub discard, H16 - H16, setf: lane = N ? 1 : 0
               (A row at r0, B row at r0 + 32)
  jv_bschk_m1  BSM1  vmov H8 <- #7 ifn (N from v - r2 < 0, v the H16 row
               at r0): lane = v < r2 ? 7 : the background byte (r0 + 32)
  jv_bschk_r1  BSR1  vsub discard, H8 - H8, setf: lane = a != b ? 1 : 0
               (bytes at r0 and r0 + 32)
  jv_bschk_x1  BSX1  vmax H8, H8, H8: lane = max(a, b)
  jv_bschk_s1  BSS1  vst8 d00 single, no stride, no offset: 16 bytes from
               r0 to r1 (the C checks the 16 after them are untouched)
  jv_bschk_z1  BSZ1  vmov H8 <- r3 ifz (Z from v - r2 == 0): lane = v == r2
               ? 9 : the background byte (r0 + 32)

Usage: bcmfw/video/gen_bschk.py [--check]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'h264'))
from vpu_asm import DISCARD, H8, H16, Reg, alu, mem, _decode      # noqa: E402
import vc2vpu as VP                                                  # noqa: E402

OUT_S = os.path.join(HERE, 'bschk_kernels.S')


def out16(row):
    """widen H8 row (row, 0) to the H16 row (3, 0) and store it at r1:
    vadd DH16 AH8 #0 and vst16 (both silicon)"""
    return [alu('vadd', H16(3, 0), H8(row, 0), 0), mem('vst16', H16(3, 0), 1)]


def ld16(text):
    """vldp16/vldq16 (vpu_asm.mem knows only the 8-bit pair): assembled,
    decoded and checked here"""
    ins = _decode(text)
    op9, dd, o, rp, b, s = VP.mem48_fields(ins.ws)
    assert dd == 0 and o == 0 and rp == 1 and b == 0 and s is None, text
    return text


def l1():
    return [ld16('vldp16 d00, (r0)'), ld16('vldq16 d00, (r0)'),
            mem('vst16', H16(0, 0), 1)]


def f1():
    return [mem('vld16', H16(0, 0), 0), 'add r0, #32', mem('vld16', H16(1, 0), 0),
            'mov r2, #0', 'mov r3, #1',
            alu('vmov', H16(2, 0), H8(0, 0), Reg(2)),
            alu('vsub', DISCARD, H16(0, 0), H16(1, 0), setf=True),     # BSF1
            alu('vmov', H16(2, 0), H8(0, 0), Reg(3), pred='ifn'),
            mem('vst16', H16(2, 0), 1)]


def m1():
    return [mem('vld16', H16(0, 0), 0), 'add r0, #32', mem('vld8', H8(1, 0), 0),
            alu('vsub', DISCARD, H16(0, 0), Reg(2), setf=True),
            alu('vmov', H8(1, 0), H8(1, 0), 7, pred='ifn')] + out16(1)   # BSM1


def r1():
    return [mem('vld8', H8(0, 0), 0), 'add r0, #32', mem('vld8', H8(1, 0), 0),
            'mov r2, #0', 'mov r3, #1',
            alu('vmov', H8(2, 0), H8(0, 0), Reg(2)),
            alu('vsub', DISCARD, H8(0, 0), H8(1, 0), setf=True),       # BSR1
            alu('vmov', H8(2, 0), H8(0, 0), Reg(3), pred='ifnz')] + out16(2)


def x1():
    return [mem('vld8', H8(0, 0), 0), 'add r0, #32', mem('vld8', H8(1, 0), 0),
            alu('vmax', H8(2, 0), H8(0, 0), H8(1, 0))] + out16(2)      # BSX1


def s1():
    return [mem('vld8', H8(0, 0), 0), mem('vst8', H8(0, 0), 1)]         # BSS1


def z1():
    return [mem('vld16', H16(0, 0), 0), 'add r0, #32', mem('vld8', H8(1, 0), 0),
            alu('vsub', DISCARD, H16(0, 0), Reg(2), setf=True),
            alu('vmov', H8(1, 0), H8(1, 0), Reg(3), pred='ifz')] + out16(1)   # BSZ1


KERNELS = [('jv_bschk_l1', l1), ('jv_bschk_f1', f1), ('jv_bschk_m1', m1),
           ('jv_bschk_r1', r1), ('jv_bschk_x1', x1), ('jv_bschk_s1', s1),
           ('jv_bschk_z1', z1)]


def gen_s():
    out = ['', '/* GENERATED by bcmfw/video/gen_bschk.py: do not edit.  The start-up',
           ' * checks of the vector forms a loop filter on the vector unit would',
           ' * need (video/bschk.c; decoder.md, "The loop filter in vectors"). */',
           '', '    .syntax vc2', '    .text', '']
    for name, fn in KERNELS:
        out += ['    .global %s' % name, '%s:' % name]
        out += ['    ' + t for t in fn()]
        out += ['    rts', '']
    return '\n'.join(out)


def main(argv):
    text = prokbox_header.convert(gen_s(), '.S')[0]
    if '--check' in argv:
        if not os.path.exists(OUT_S) or open(OUT_S).read() != text:
            print('gen_bschk: stale: bschk_kernels.S')
            return 1
        print('gen_bschk: up to date')
        return 0
    with open(OUT_S, 'w') as f:
        f.write(text)
    print('gen_bschk: %s' % ', '.join('%s %d' % (n, len(fn()) + 1) for n, fn in KERNELS))
    return 0


# the ProkBox banner for the generated file (scripts/prokbox_header.py)
sys.path.insert(0, os.path.join(HERE, '..', '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
