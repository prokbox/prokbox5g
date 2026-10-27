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
"""Recompile libgcc's helpers to VC IV assembly for vc2cc.

vc2cc links whole programs from VC IV assembly text, so libgcc's helpers
(64-bit divide and shifts, clz/ctz/popcount, soft float for double) are
needed as .s files compiled with vc2cc's register flags, not as the VC IV
objects in libgcc.a.  This replays the compile commands recorded in the
log of a VC IV GCC build tree's 'make all-target-libgcc' with -S and
vc2cc.GCC_FLAGS added (docs/research/bcm2722/compiler.md, "Building").

Usage: vc2libgcc.py BUILD_DIR/libgcc.log BUILD_DIR OUT_DIR
  e.g. vc2libgcc.py ~/Git/re/vc2-gcc-build/libgcc.log ~/Git/re/vc2-gcc-build \\
           ~/Git/re/vc2-gcc-spike/libgcc
"""
import os
import re
import shlex
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vc2cc as C  # noqa: E402

SKIP = re.compile(r'unwind|emutls|enable-execute|_trampoline|_clear_cache|'
                  r'__main|_ctors|_eprintf|crt|gcov|_bcmp')


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    log, build, out = (os.path.expanduser(a) for a in argv)
    os.makedirs(out, exist_ok=True)
    text = open(log).read().replace('\\\n', ' ')
    seen, ok, bad = set(), 0, []
    for line in text.splitlines():
        # commands run in BUILD/vc4-elf/libgcc (multilib subdirs skipped)
        if not line.startswith(build) or ' -c ' not in line or \
                '../../../../' in line:
            continue
        a = shlex.split(line)
        obj = a[a.index('-o') + 1]
        if not obj.endswith('.o') or SKIP.search(obj) or obj in seen:
            continue
        seen.add(obj)
        for f in ('-MT', '-MF'):
            while f in a:
                i = a.index(f)
                del a[i:i + 2]
        a = [x for x in a if x not in ('-g', '-c') and not x.startswith('-M')]
        a[a.index('-o') + 1] = os.path.join(out, obj[:-2] + '.s')
        r = subprocess.run(a + ['-S'] + [f for f in C.GCC_FLAGS if f.startswith(
                           '-ffixed') or f == '-fno-common'],
                           cwd=os.path.join(build, 'vc4-elf/libgcc'),
                           capture_output=True, text=True)
        if r.returncode:
            bad.append(obj)
        else:
            ok += 1
    print('%d helpers compiled to %s%s' % (ok, out, (', failed: ' + ' '.join(bad))
                                             if bad else ''))
    return 1 if bad or not ok else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
