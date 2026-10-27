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
"""Ground-truth statistics from a VideoCore II .vll (ELF, e_machine 95).

The .vll libraries keep .dynsym (function names, addresses, sizes) and
.rela.text, which makes them a labelled corpus for recovering the ISA:
function boundaries, prologue/epilogue halfwords, and the instruction
bytes at every relocation site, grouped by relocation type.

Usage: vllinfo.py FILE.vll [--relocs N]
"""
import collections
import struct
import sys


def load(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF' or struct.unpack_from('<H', d, 0x12)[0] != 95:
        sys.exit(f'{path}: not a VideoCore ELF')
    shoff, = struct.unpack_from('<I', d, 0x20)
    shnum, shstr = struct.unpack_from('<HH', d, 0x30)
    secs = [struct.unpack_from('<10I', d, shoff + i * 40) for i in range(shnum)]
    name = lambda o: d[secs[shstr][4] + o:].split(b'\0')[0].decode()
    return d, {name(s[0]): s for s in secs}


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    nrel = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[2] == '--relocs' else 6
    d, sec = load(sys.argv[1])
    base, toff = sec['.text'][3], sec['.text'][4]
    dsym, dstr = sec['.dynsym'], sec['.dynstr'][4]
    hw = lambda a: struct.unpack_from('<H', d, toff + a - base)[0]

    syms, funcs = [], []
    for i in range(dsym[5] // 16):
        n, v, sz, info, _, _ = struct.unpack_from('<IIIBBH', d, dsym[4] + i * 16)
        nm = d[dstr + n:].split(b'\0')[0].decode()
        syms.append((v, nm))
        if info & 0xf == 2 and sz > 0:
            funcs.append((v, sz, nm))

    print(f'{len(funcs)} sized functions; odd sizes {sum(s % 2 for _, s, _ in funcs)}, '
          f'sizes not multiple of 4: {sum(s % 4 != 0 for _, s, _ in funcs)}')
    for label, pick in (('first', lambda v, s: hw(v)),
                        ('last', lambda v, s: hw(v + s - 2))):
        c = collections.Counter(pick(v, s) for v, s, _ in funcs)
        print(f'{label:5} halfword:', ', '.join(f'{k:04x}x{n}' for k, n in c.most_common(10)))

    rel = sec['.rela.text']
    by_type = collections.defaultdict(list)
    for i in range(rel[5] // 12):
        off, info, add = struct.unpack_from('<IIi', d, rel[4] + i * 12)
        by_type[info & 0xff].append((off, info >> 8, add))
    for t, lst in sorted(by_type.items()):
        print(f'reloc type {t}: {len(lst)} sites')
        for off, si, add in lst[:nrel]:
            ctx = ' '.join(f'{hw(off + k):04x}' for k in (-2, 0, 2, 4))
            v, nm = syms[si]
            print(f'  {off:08x} {nm or "<section>"}@{v:x}+{add:x}  [-2..+4]: {ctx}')


if __name__ == '__main__':
    main()
