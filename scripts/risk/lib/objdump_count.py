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

"""Per-function static ARM instruction counts from an objdump disassembly.

    objdump_count.py <file.o> [function ...]

Runs `arm-none-eabi-objdump -d <file.o>`, splits it into functions (the
"<addr> <name>:" headers objdump prints), and for each one reports the
number of real instruction lines (data literals -- ".word", emitted for
PC-relative constant pools -- are not instructions and are excluded) and
the set of external symbols it calls (every "bl"/"blx" target name).
With no function names given, reports every function found; with names,
reports only those (and warns about ones not found).

This is the automated half of risk tool 1 (scripts/risk/arm_insn_count.sh,
docs/risk-tooling.md): it is a *static* count (instructions compiled into
the function body), not a dynamic one -- loop trip counts and which
branch of an if/else actually runs at a given input are not visible to
it. arm_insn_count.sh uses it as a live sanity/regression signal next to
the hand-derived per-iteration costs documented in docs/risk-tooling.md
(so a reviewer can see whether a function's total size has drifted far
from the snapshot those were traced from), not as the dynamic estimate
itself.

Importable: parse_objdump(path) -> {name: FuncInfo}.
"""
import re
import subprocess
import sys
from dataclasses import dataclass, field

RE_FUNC = re.compile(r'^([0-9a-f]+) <([^>]+)>:$')
RE_INSN = re.compile(r'^\s*([0-9a-f]+):\s+[0-9a-f]+\s+(\S+)\s*(.*)$')
RE_CALL_TARGET = re.compile(r'<([^>+]+)(?:\+0x[0-9a-f]+)?>\s*$')


@dataclass
class FuncInfo:
    name: str
    start: int
    end: int = 0
    n_insns: int = 0
    n_words: int = 0
    calls: set = field(default_factory=set)
    backward_branches: int = 0


def run_objdump(obj_path, objdump='arm-none-eabi-objdump'):
    out = subprocess.run([objdump, '-d', obj_path], capture_output=True,
                         text=True, check=True)
    return out.stdout.splitlines()


def parse_objdump(obj_path, objdump='arm-none-eabi-objdump'):
    lines = run_objdump(obj_path, objdump)
    funcs = {}
    cur = None
    for line in lines:
        m = RE_FUNC.match(line)
        if m:
            cur = FuncInfo(name=m.group(2), start=int(m.group(1), 16))
            funcs[cur.name] = cur
            continue
        if cur is None:
            continue
        m = RE_INSN.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        mnem = m.group(2)
        rest = m.group(3)
        cur.end = addr
        if mnem == '.word':
            cur.n_words += 1
            continue
        cur.n_insns += 1
        if mnem in ('bl', 'blx'):            # exact: condition codes like
            cm = RE_CALL_TARGET.search(rest)  # "bls"/"blt"/"ble" also start
            if cm:                            # with "bl" but are not calls
                cur.calls.add(cm.group(1))
        if mnem[0] == 'b' or mnem == 'bx':
            tm = re.match(r'^([0-9a-f]+)\b', rest)
            if tm and int(tm.group(1), 16) <= addr and \
                    int(tm.group(1), 16) >= cur.start:
                cur.backward_branches += 1
    return funcs


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    obj_path = sys.argv[1]
    want = sys.argv[2:]
    funcs = parse_objdump(obj_path)
    names = want or sorted(funcs, key=lambda n: funcs[n].start)
    fail = 0
    for name in names:
        f = funcs.get(name)
        if not f:
            print('note: function not found in %s: %s' % (obj_path, name))
            fail = 1
            continue
        calls = ', '.join(sorted(f.calls)) if f.calls else '(none)'
        loop = ' loop' if f.backward_branches else ''
        print('%-20s %4d insns  %2d word(s)  calls: %-28s%s'
              % (f.name, f.n_insns, f.n_words, calls, loop))
    sys.exit(fail)


if __name__ == '__main__':
    main()
