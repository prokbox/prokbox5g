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

"""Gate rules, both run by scripts/shots.sh on every script (UI and perf).

Panic rule: any panic or fatal marker in the log fails unless the script
declares it with "# expect: panic <regex>" (see panic_rule below).

Hang rule: a UI script's sim.log may hold a hang record ("hang recovered
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


# Fatal markers in a sim log (the panic rule, run by scripts/shots.sh for
# every script, perf ones included; check_trace.py does not repeat it). panicf() in the sim prints its
# message to stderr with no fixed prefix (firmware/panic.c DEBUGF; the
# conventional text starts "Error!"), and the next boot logs the saved record
# as "ipodui: panic kind=..." lines (apps/ipodui/uilog.c). Hang records
# ("panic hang record", "hang recovered") are the hang rule above
# and are not matched here.
RE_PANIC_HEAD = re.compile(r'^ipodui: panic kind=\d+')
RE_PANIC_CONT = re.compile(r'^ipodui: panic ')
RE_PANIC_MARK = re.compile(
    r'\bpanicf\b|\*PANIC\*|\bError!|^sim_script: panic_fake\b'
    r'|\bStkov\b|[Ss]tack overflow|[Aa]ssertion .*fail|[Aa]ssert(?:ion)? fail'
    r'|Segmentation fault|Abort trap|Bus error|Illegal instruction'
    r'|\bUIE\b|[Dd]ata abort|[Pp]refetch abort|[Uu]ndefined instruction')


def panic_rule(lines, script_lines, ok, bad):
    """Panic rule: any panic or fatal marker in the sim log FAILS the
    script, whatever else passed (tick_add_task "out of tasks", Stkov, an
    assert, a signal, a logged panic record). A script that makes one on
    purpose declares "# expect: panic <regex>" (one per expected panic;
    a regex is matched against the marker line, or against a logged
    record's whole "ipodui: panic ..." block). Every marker must match a
    directive, and every directive must match a marker: one that never
    fires FAILS, so the declaration cannot pass vacuously. Hang records
    are the hang rule's ("# expect: hang <slot>"), above."""
    want = []
    for l in script_lines:
        m = re.match(r'\s*#\s*expect:\s*panic\s+(\S.*?)\s*$', l)
        if m:
            try:
                want.append(re.compile(m.group(1)))
            except re.error as e:
                bad('panic rule: bad "# expect: panic" regex %r: %s'
                    % (m.group(1), e))
                return
    marks = []          # (line number, text matched against the directives)
    i = 0
    while i < len(lines):
        l = lines[i]
        if RE_PANIC_HEAD.search(l):
            j = i + 1
            while j < len(lines) and RE_PANIC_CONT.search(lines[j]):
                j += 1
            marks.append((i + 1, '\n'.join(lines[i:j])))
            i = j
            continue
        if RE_PANIC_MARK.search(l):
            marks.append((i + 1, l))
        i += 1
    used = [False] * len(want)
    unexpected = []
    for n, text in marks:
        hit = False
        for k, rx in enumerate(want):
            if rx.search(text):
                used[k] = hit = True
        if not hit:
            unexpected.append((n, text.splitlines()[0].strip()))
    for n, t in unexpected[:5]:
        bad('panic rule: log line %d is a panic or fatal marker no '
            '"# expect: panic" declares: %s' % (n, t[:160]))
    if len(unexpected) > 5:
        bad('panic rule: %d more unexpected markers' % (len(unexpected) - 5))
    for k, rx in enumerate(want):
        if not used[k]:
            bad('panic rule: "# expect: panic %s" matched no panic marker in '
                'the log (the expected panic never happened)' % rx.pattern)
    if not unexpected and all(used):
        ok('panic rule (%d marker(s), %d declared)' % (len(marks), len(want)))



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
    pfail = []
    panic_rule(lines, open(script).read().splitlines(),
               lambda m: print("ok   " + m),
               lambda m: (print("FAIL: " + m), pfail.append(m)))
    return 1 if bad or pfail else 0


sys.exit(main())
