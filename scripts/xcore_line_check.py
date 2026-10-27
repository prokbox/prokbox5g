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

"""Cache lines shared across the UI boundary (P19 S3/S4; warn mode).

usage: scripts/xcore_line_check.py [--strict] [--list] MAPFILE

With the shell on the COP, a 16-byte cache line that holds both a shell
(apps/ipodui) static and any other static is written back whole by
whichever core evicts it last: a write by the other core to its half is
lost (no coherency, write-back caches; tests/febe/cachemodel.c shows the
mechanism).  This reads the linker map's input sections in the cached
.data and .bss and counts the lines where a shell object's data meets
another object's.  Uncached sections (.ncbss, .ncdata, IRAM) are exempt.
--strict exits 1 on any shared line (the S4 gate, once the shell's data
is placed in its own aligned region).
"""
import re, sys

LINE = 16
SEC = re.compile(r'^ (\.(?:data|bss)(?:\.[^\s]*)?|COMMON)\s+(0x[0-9a-f]+)\s+'
                 r'(0x[0-9a-f]+)\s+(\S+\.o\)?)\s*$')
SEC_SPLIT = re.compile(r'^ (\.(?:data|bss)(?:\.[^\s]*)?|COMMON)\s*$')
ADDR = re.compile(r'^\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S+\.o\)?)\s*$')
CACHED_MAX = 0x10000000     # the uncached alias starts here (PP5022)


# the front end's data: the shell's objects and the drawing code it runs
# (the font cache, the framebuffer and the LCD's viewport state)
FE_MEMBERS = ('(font.o)', '(lcd-16bit.o)')


def shell(obj):
    return '/apps/ipodui/' in obj or obj.endswith(FE_MEMBERS)


def main(argv):
    strict = '--strict' in argv
    listing = '--list' in argv
    args = [a for a in argv if not a.startswith('--')]
    if len(args) != 1:
        print(__doc__); return 2
    ranges = []
    pending = None
    for l in open(args[0], errors='replace'):
        m = SEC.match(l)
        if m:
            name, a, n, o = m.groups(); pending = None
        else:
            m2 = SEC_SPLIT.match(l)
            if m2:
                pending = m2.group(1); continue
            m3 = ADDR.match(l) if pending else None
            if not m3:
                pending = None; continue
            name = pending; a, n, o = m3.groups(); pending = None
        a = int(a, 16); n = int(n, 16)
        if n == 0 or a == 0 or a >= CACHED_MAX:
            continue
        if name.startswith('.data.') and 'ncdata' in name:
            continue
        ranges.append((a, a + n, o))
    lines = {}
    for a, e, o in ranges:
        for ln in range(a // LINE, (e - 1) // LINE + 1):
            lines.setdefault(ln, set()).add(o)
    shared = []
    for ln, objs in lines.items():
        if any(shell(o) for o in objs) and any(not shell(o) for o in objs):
            shared.append((ln * LINE, sorted(objs)))
    shell_bytes = sum(e - a for a, e, o in ranges if shell(o))
    print('xcore lines: %d cache lines shared between shell and other data '
          '(%d B of cached shell data) (%s)'
          % (len(shared), shell_bytes, 'strict' if strict else 'warn'))
    if listing:
        for addr, objs in sorted(shared):
            print('  0x%08x %s' % (addr, ' '.join(o.split('/')[-1]
                                                   for o in objs)))
    return 1 if (strict and shared) else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
