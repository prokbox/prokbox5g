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

"""Two-way agreement between reach.py and the linker's --gc-sections
(Phase 9, T; complements B's scripts/risk/gc_check.sh).

gc_check.sh FAILs when the linker drops something reach.py calls
reachable (the boot-safety direction). This checks the other direction,
which is where reach.py itself could be wrong: every input section the
linker KEEPS (has a relocation path to from the entry) must hold at
least one node reach.py calls reachable. A kept section that reach.py
finds wholly dead means reach.py missed an edge, so its dead list (and
the M8 caller list) can't be trusted: FAIL. Dead pieces inside a kept
section are fine (reach.py splits asm sections at symbols; the linker
can't) and are counted.

  scripts/risk/reach_gc_agree.py OUT     OUT = gc_check.sh's --out dir
                                         (nogc.elf, nogc.map,
                                         gc-sections.txt)

Prints "agree PASS kept=<n> dropped=<n> dead_in_kept=<n>/<bytes>" or
"agree FAIL ..." with one line per wholly-dead kept section; exit 0/1.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reach  # noqa: E402


def main(argv):
    if len(argv) != 1:
        sys.stderr.write(__doc__)
        return 2
    out = argv[0]
    try:
        g = reach.Graph(os.path.join(out, "nogc.elf"),
                        os.path.join(out, "nogc.map"), [])
    except reach.BadInput as e:
        print("agree FAIL bad input: %s" % e)
        return 1
    gcre = re.compile(r"removing unused section '([^']+)' in file '([^']+)'")
    dropped = set()
    for line in open(os.path.join(out, "gc-sections.txt")):
        m = gcre.search(line)
        if m:
            dropped.add((m.group(1), m.group(2)))
    live = g.reach()
    by_isec = {}
    for n in g.nodes:
        if n.isec.obj in ("<linker>", "?"):
            continue
        by_isec.setdefault((n.isec.name, n.isec.obj), []).append(n)
    fails, dead_in_kept, dead_bytes, kept = [], 0, 0, 0
    for key, nodes in sorted(by_isec.items()):
        if key in dropped:
            continue
        if key[0].startswith(".ARM.exidx"):
            # unwind entries are kept with the text they describe
            # (SHF_LINK_ORDER), not through a reference
            continue
        kept += 1
        alive = [n for n in nodes if n.id in live]
        if not alive:
            size = sum(n.end - n.start for n in nodes)
            fails.append("kept-but-dead %s %s %d (%s)" % (
                key[0], g.rel(key[1]), size,
                " ".join(n.name for n in nodes[:4])))
        else:
            for n in nodes:
                if n.id not in live:
                    dead_in_kept += 1
                    dead_bytes += n.end - n.start
    stray = [k for k in dropped if k not in by_isec]
    for f in fails:
        print("FAIL " + f)
    res = "agree %s kept=%d dropped=%d dead_in_kept=%d/%d unmatched_dropped=%d" % (
        "FAIL" if fails else "PASS", kept, len(dropped), dead_in_kept,
        dead_bytes, len(stray))
    print(res)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
