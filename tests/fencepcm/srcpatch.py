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

"""Copy a real firmware source beside a host test's stubs, with the test's
instrumentation and (optionally) one mutant applied.

    srcpatch.py IN OUT SPEC [MUTANT]
    srcpatch.py --list SPEC

SPEC is a Python file defining:
  SLICES     optional list of (start_regex, end_regex): keep only these
             line ranges of IN (end line excluded), in order; each regex
             must match exactly one line
  INSTRUMENT list of (anchor_line, text): text goes on the line after the
             one line that equals anchor_line (stripped); each anchor must
             appear exactly once
  REPLACE    optional list of (old, new): exact replacements made after
             INSTRUMENT in every build (a host-only necessity, e.g. a
             symbol alias Mach-O can't link); each `old` exactly once
  MUTANTS    dict name -> (why, [(old, new), ...]): exact replacements in
             the copy, each `old` must occur exactly once (a mutant that no
             longer applies is an error, never a silent survivor)

Exit 2 on any anchor, slice or mutant that does not apply exactly once."""
import re
import runpy
import sys


def die(msg):
    sys.stderr.write("srcpatch: %s\n" % msg)
    sys.exit(2)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--list":
        spec = runpy.run_path(sys.argv[2])
        for name in spec.get("MUTANTS", {}):
            print(name)
        return
    if len(sys.argv) not in (4, 5):
        die(__doc__)
    src, dst, specf = sys.argv[1:4]
    mutant = sys.argv[4] if len(sys.argv) == 5 else None
    spec = runpy.run_path(specf)
    with open(src) as f:
        lines = f.read().split("\n")

    slices = spec.get("SLICES")
    if slices:
        out = ["/* srcpatch: slices of %s, verbatim */" % src]
        for start, end in slices:
            s = [i for i, l in enumerate(lines) if re.search(start, l)]
            e = [i for i, l in enumerate(lines) if re.search(end, l)]
            if len(s) != 1 or len(e) != 1 or e[0] <= s[0]:
                die("slice %r..%r: %d/%d matches" % (start, end, len(s), len(e)))
            out.append("#line %d \"%s\"" % (s[0] + 1, src))
            out.extend(lines[s[0]:e[0]])
        lines = out

    for anchor, text in spec.get("INSTRUMENT", []):
        hits = [i for i, l in enumerate(lines) if l.strip() == anchor]
        if len(hits) != 1:
            die("anchor %r: %d matches" % (anchor, len(hits)))
        lines.insert(hits[0] + 1, text)
    body = "\n".join(lines)

    for old, new in spec.get("REPLACE", []):
        if body.count(old) != 1:
            die("replace %r: %d matches" % (old, body.count(old)))
        body = body.replace(old, new)

    if mutant:
        muts = spec.get("MUTANTS", {})
        if mutant not in muts:
            die("no mutant %r" % mutant)
        for old, new in muts[mutant][1]:
            n = body.count(old)
            if n != 1:
                die("mutant %s: %r occurs %d times" % (mutant, old, n))
            body = body.replace(old, new)
    with open(dst, "w") as f:
        f.write(body)


main()
