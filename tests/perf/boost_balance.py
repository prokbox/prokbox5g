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

"""Check the dedicated-build boost in a simulator log (p081).

    tests/perf/boost_balance.py <sim.log>

Reads the devmode "db dedicated on|off why=<path> depth=<n>" lines
(apps/tagcache.c, tc_dedicated()) and fails unless: no "off" was
unmatched, the depth is back at 0 after the run, and the builds a user
waits on each held it: the first build ("first"), the scan after USB
("usb"), a candidate found by an update ("cand"), and, when the log shows
a commit that waited for the stop, that delayed commit ("delayed"). When a
scan was cut ("db scan ms=... cut") the depth must be 0 right after it.
Exit 1 on a failed check, 2 when the log has no scan at all.
"""
import re
import sys


def main():
    lines = open(sys.argv[1], errors='replace').read().splitlines()
    if not any('scantrace ms=' in l for l in lines):
        print('boost_balance: no scantrace line: the run inspected nothing')
        return 2
    depth, held, events, errors = 0, set(), 0, []
    cut_seen = False
    for l in lines:
        m = re.search(r'dedicated (on|off) why=(\S+) depth=(\d+)', l)
        if m:
            events += 1
            depth = int(m.group(3))
            if m.group(1) == 'on':
                held.add(m.group(2))
        elif 'dedicated off unmatched' in l:
            errors.append('unmatched off: ' + l.strip())
        elif re.search(r'db scan ms=\d+ n=\d+ cut', l):
            cut_seen = True
            if depth != 0:
                errors.append('depth %d after a cut scan' % depth)
    want = {'first', 'usb', 'cand'}
    if any('commit delayed until audio stops' in l for l in lines):
        want.add('delayed')
    missing = want - held
    if missing:
        errors.append('never held for: ' + ','.join(sorted(missing)))
    if depth != 0:
        errors.append('final depth %d' % depth)
    print('boost_balance: %d events, held for %s, cut scan %s, final depth %d'
          % (events, ','.join(sorted(held)) or '-',
             'seen' if cut_seen else 'not seen', depth))
    for e in errors:
        print('boost_balance: FAIL ' + e)
    if not errors:
        print('boost_balance: PASS')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
