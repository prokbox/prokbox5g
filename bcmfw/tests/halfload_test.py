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
"""The 32-bit halfword loads with an offset, model and compiler
(quirks.md #16 and #33, isa-scalar.md).

On silicon 0x28xx with offset bit 0 set zero-extends and 0x29xx with
offset bit 0 set sign-extends (the 2026-10-25 Port write check: COMMIT's
F16w of a slot of 0xFFFF came back 0x0000fff8 where the PP's is
0x0007fff8, seven 0x29 loads of -1).  This checks:

- vc2sim: 0x28/o1 loads 0x8001 and 0xFFFF zero-extended, 0x29/o1
  sign-extended, at offsets 0 and 2 mod 4;
- vc2cc: an unsigned 16-bit load at an offset compiles to 0x28/o1 and
  zero-extends; a signed one computes its address and uses the 16-bit
  'ldsh rd, (rs)' (never an offset form) and sign-extends; a halfword
  load from the stack uses no gp/sp-relative or 48-bit form (unproven on
  silicon: bench row HWLD);
- j_f16w (COMMIT's sum, jobs.h) over 8 pixels of 0xFFFF: 0x0007fff8,
  0x0023ffdc, the PP's value, not the 0x0000fff8 the BCM computed with
  the old encoding.

Fails on the old model (0x29/o1 zero-extending) and on the old compiler
(unsigned offset loads as 0x29/o1, sp-relative 0x2d5x).

Usage: bcmfw/tests/halfload_test.py [-v]
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
ROOT = os.path.dirname(FW)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bcmre'))
import vc2cc as C      # noqa: E402
import vc2dis as D     # noqa: E402
import vc2forms as F   # noqa: E402
import vc2sim as S     # noqa: E402

SRC = r'''
#include <stdint.h>
#include "jobs.h"

uint32_t get_u(const uint16_t *p) { return p[1]; }
uint32_t get_u3(const uint16_t *p) { return p[3]; }
int32_t get_s3(const int16_t *p) { return p[3]; }
int32_t get_s2(const int16_t *p) { return p[2]; }
uint32_t stack_u(uint32_t a, uint32_t b)
{
    volatile uint16_t t[4];

    t[0] = (uint16_t)a;
    t[1] = (uint16_t)b;
    t[2] = (uint16_t)(a >> 16);
    t[3] = (uint16_t)(b >> 16);
    return t[1] + t[2] + t[3];
}
uint32_t f16w8(const uint16_t *px, uint32_t *b)
{
    uint32_t a = 0;

    *b = 0;
    j_f16w(&a, b, px, 8);
    return a;
}
'''


class Check:
    def __init__(self, verbose):
        self.n = self.bad = 0
        self.verbose = verbose

    def __call__(self, ok, what):
        self.n += 1
        if not ok:
            self.bad += 1
        if not ok or self.verbose:
            print('%s %s' % ('ok  ' if ok else 'FAIL', what))


def model_rows(chk):
    """vc2sim on hand-placed instructions: ldX r0, (r1+off); rts"""
    sim = S.Sim()
    data = sim.malloc(16)
    # halfwords at +0..+6: 0x8001, 0xFFFF, 0xFFFF, 0x8001
    sim.mem.w32(data, 0xFFFF8001)
    sim.mem.w32(data + 4, 0x8001FFFF)
    rts = D.assemble('rts')
    for top, signed in ((0x28, False), (0x29, True)):
        for off in range(0, 8, 2):
            code = sim.malloc(16)                   # fresh: decodes are cached
            ws = [top << 8 | 0x10, off | 1] + rts      # rd r0, rs r1
            for k, w in enumerate(ws):
                sim.mem.w16(code + 2 * k, w)
            got = sim.call(code, 0, data)
            raw = sim.mem.r16(data + off)
            want = (raw | 0xFFFF0000) if signed and raw & 0x8000 else raw
            chk(got == want, 'vc2sim 0x%02x/o1 at +%d (%04x): %08x, want %08x (%s)' % (
                top, off, raw, got, want, 'sign-extended' if signed else 'zero-extended'))


def halfloads(prog, fn):
    """(form, text) of every halfword load in function fn"""
    lo = prog.symbols[fn]
    hi = min([a for a in prog.symbols.values() if a > lo] + [lo + 4096])
    out = []
    for i in F.code_insns(prog):
        if not lo <= i.addr < hi or i.text.split()[0] not in ('ldh', 'ldsh'):
            continue
        w0 = i.ws[0]
        if len(i.ws) == 1:
            form = '16'
        elif len(i.ws) == 2 and w0 >> 8 in (0x28, 0x29):
            form = '0x%02x/o1' % (w0 >> 8)
        elif len(i.ws) == 2 and w0 >> 8 == 0x2d:
            form = 'gp16'
        else:
            form = '48'
        out.append((form, i.text))
    return out


def compiler_rows(chk):
    with tempfile.NamedTemporaryFile('w', suffix='.c', delete=False) as f:
        f.write(SRC)
        path = f.name
    try:
        sim = S.Sim()
        prog = C.build([path], '-O2', extra=['-I' + FW, '-std=gnu99'],
                       libs=C.DEFAULT_LIBS, no_float=True, conservative=True,
                       extern=sim.import_addr)
    finally:
        os.unlink(path)
    C.load_into_sim(prog, sim)
    p = sim.malloc(16)
    for k, v in enumerate((0x1234, 0x8001, 0xFFFF, 0x8001)):
        sim.mem.w16(p + 2 * k, v)

    hl = halfloads(prog, 'get_u3')
    chk(any(f == '0x28/o1' for f, _ in hl) and all(f in ('16', '0x28/o1') for f, _ in hl),
        'vc2cc: unsigned p[3] as 0x28/o1: %s' % hl)
    chk(sim.call('get_u3', p) == 0x8001, 'unsigned p[3] = 0x8001 zero-extended: %08x' %
        sim.call('get_u3', p))
    chk(sim.call('get_u', p) == 0x8001, 'unsigned p[1] = 0x8001: %08x' % sim.call('get_u', p))
    for fn, want in (('get_s3', 0xFFFF8001), ('get_s2', 0xFFFFFFFF)):
        hl = halfloads(prog, fn)
        chk(hl and all(f == '16' and t.startswith('ldsh') for f, t in hl),
            'vc2cc: %s signed, address first, 16-bit ldsh (rs): %s' % (fn, hl))
        got = sim.call(fn, p)
        chk(got == want, '%s sign-extended: %08x, want %08x' % (fn, got, want))
    hl = halfloads(prog, 'stack_u')
    chk(all(f in ('16', '0x28/o1') for f, _ in hl),
        'vc2cc: stack halfwords by proven forms only (no gp16 / 48-bit): %s' % hl)
    got = sim.call('stack_u', 0xFFFF0001, 0x8001FFFE)
    chk(got == 0xFFFE + 0xFFFF + 0x8001, 'stack halfwords zero-extended: %08x' % got)
    px = sim.malloc(16)
    for k in range(8):
        sim.mem.w16(px + 2 * k, 0xFFFF)
    b = sim.malloc(4)
    a = sim.call('f16w8', px, b)
    chk((a, sim.mem.r32(b)) == (0x0007FFF8, 0x0023FFDC),
        "j_f16w of 8 x 0xFFFF: %08x %08x, want the PP's 0007fff8 0023ffdc (the old "
        "encoding gave 0000fff8 0007ffdc on silicon)" % (a, sim.mem.r32(b)))


def main(argv):
    chk = Check('-v' in argv)
    model_rows(chk)
    if not os.path.exists(C.GCC):
        print('halfload_test: vc4-elf-gcc not found: compiler rows skipped')
    else:
        compiler_rows(chk)
    print('halfload_test: 2 rows, %d cases, %d failed' % (chk.n, chk.bad))
    return 1 if chk.bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
