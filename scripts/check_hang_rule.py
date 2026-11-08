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

"""Gate rule: a UI script's sim.log may hold a hang record ("hang recovered
<slot> after Ns", "panic hang record thread=<name>") only if the script says
so with a "# expect: hang <slot>" line (slots: ui be art sto, the names the
shell logs).  A hang record in a normal run means a thread slept or blocked
without marking itself idle (hang_idle_begin/end, hang_pause), which the
device's watcher would also report.

Usage: check_hang_rule.py SCRIPT SIMLOG
Exit 0 ok; 1 unexpected record(s); 2 the log is missing or empty (the rule
inspected nothing, which must not pass).  Called by scripts/shots.sh, so a
single run and the gate both apply it."""
import re
import sys

# the thread name a panic record carries -> the slot the "recovered" line names
THREAD_SLOT = {'main': 'ui', 'be_svc': 'be', 'artcache': 'art', 'storage': 'sto'}


def main():
    if len(sys.argv) != 3:
        print("usage: check_hang_rule.py SCRIPT SIMLOG", file=sys.stderr)
        return 2
    script, log = sys.argv[1:]
    try:
        want = set(re.findall(r'^#\s*expect:\s*hang\s+(\w+)',
                              open(script).read(), re.M))
        lines = open(log, errors='replace').read().splitlines()
    except OSError as e:
        print("FAIL: hang rule inspected nothing: %s" % e)
        return 2
    if not lines:
        print("FAIL: hang rule inspected nothing: %s is empty" % log)
        return 2
    bad = []
    seen = 0
    for line in lines:
        m = re.search(r'hang recovered (\w+) after (\d+)s', line)
        if m:
            seen += 1
            if m.group(1) not in want:
                bad.append((m.group(1), line.strip()))
        m = re.search(r'panic hang record thread=(\w+)', line)
        if m:
            seen += 1
            slot = THREAD_SLOT.get(m.group(1), m.group(1))
            if slot not in want:
                bad.append((slot, line.strip()))
    for slot, l in bad:
        print("FAIL: hang rule: unexpected hang record on slot '%s' in %s "
              "(add '# expect: hang %s' only if deliberate; else mark the "
              "sleeping thread idle): %s" % (slot, script, slot, l))
    print("hang rule: %s (%d lines, %d hang records, %d unexpected, expected %s)"
          % ("FAIL" if bad else "ok", len(lines), seen, len(bad),
             sorted(want) or "none"))
    return 1 if bad else 0


sys.exit(main())
