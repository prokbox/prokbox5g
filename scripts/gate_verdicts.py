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
"""Per-script verdicts out of a validate.sh log, and a diff of two logs
(docs/gate-speed-brief.md section 8: the old and the new gate must agree).

  gate_verdicts.py show LOG                  name<TAB>status<TAB>detail
  gate_verdicts.py compare OLD NEW [--md]    where the two logs disagree

status: ok, flaky (failed once, retry passed: counts as ok when comparing),
        FAIL (a real failure). Everything that is not a ui script is compared
        as the set of "FAIL:" lines (digits normalised) and the final verdict.
"""
import re
import sys

RE_OK = re.compile(r'^ok   (ipodui-\S+|library-db|smoke) \(\d+ shots')
RE_FLAKY = re.compile(r'^flaky-retry ok: (\S+)')
RE_SUMMARY = re.compile(r'^  FAIL (\S+): (.*)')
RE_REAL = re.compile(r'^FAIL: (\S+) failed again running alone')
RE_UIFAIL = re.compile(r'^FAIL: ui script (\S+)')
RE_TAG = re.compile(r'\[(ipodui-[^\]\s]+|library-db|smoke)\]\s*$')
RE_GOLD = re.compile(r'^FAIL: golden (\S+?)/')


def parse(path):
    ui = {}
    other = set()
    final = 'unknown'
    ran = None
    for line in open(path, errors='replace'):
        line = line.rstrip('\n')
        m = RE_OK.match(line)
        if m:
            ui.setdefault(m.group(1), ['ok', ''])
            continue
        m = RE_FLAKY.match(line)
        if m:
            ui[m.group(1)] = ['flaky', line[:120]]
            continue
        m = RE_SUMMARY.match(line)
        if m:
            ui[m.group(1)] = ['FAIL', m.group(2)[:140]]
            continue
        m = RE_REAL.match(line)
        if m:
            ui[m.group(1)] = ['FAIL', 'failed again running alone']
            continue
        m = re.match(r'^ui scripts ran (\d+) of (\d+)', line)
        if m:
            ran = (int(m.group(1)), int(m.group(2)))
        if line.startswith('GATE PASSED'):
            final = 'PASSED'
        elif line.startswith('GATE FAILED'):
            final = 'FAILED'
        if line.startswith('FAIL:'):
            tag = (RE_TAG.search(line) or RE_UIFAIL.match(line) or RE_GOLD.match(line))
            if tag:
                continue            # per-script, already in the summary
            if ' failed again running alone' in line:
                continue
            other.add(re.sub(r'\d+(\.\d+)?', 'N', line)[:160])
    return ui, other, final, ran


def cmd_show(path):
    ui, other, final, ran = parse(path)
    for n in sorted(ui):
        print('%s\t%s\t%s' % (n, ui[n][0], ui[n][1]))
    for o in sorted(other):
        print('(other)\tFAIL\t%s' % o)
    print('(final)\t%s\tran=%s' % (final, ran))


def cmd_compare(a, b, md):
    ua, oa, fa, ra = parse(a)
    ub, ob, fb, rb = parse(b)
    norm = lambda s: 'ok' if s == 'flaky' else s
    names = sorted(set(ua) | set(ub))
    diff = [(n, ua.get(n, ['missing'])[0], ub.get(n, ['missing'])[0]) for n in names
            if norm(ua.get(n, ['missing'])[0]) != norm(ub.get(n, ['missing'])[0])]
    real_a = sorted(n for n in ua if ua[n][0] == 'FAIL')
    real_b = sorted(n for n in ub if ub[n][0] == 'FAIL')
    print('%s: %d scripts, %d real FAIL, %d flaky, final %s, ran %s' % (a, len(ua), len(real_a), sum(1 for v in ua.values() if v[0] == 'flaky'), fa, ra))
    print('%s: %d scripts, %d real FAIL, %d flaky, final %s, ran %s' % (b, len(ub), len(real_b), sum(1 for v in ub.values() if v[0] == 'flaky'), fb, rb))
    print('per-script verdict differences (flaky counted as ok): %d' % len(diff))
    for n, x, y in diff:
        print('  %-45s old=%-7s new=%s' % (n, x, y))
    print('non-script FAIL lines only in old: %s' % (sorted(oa - ob) or 'none'))
    print('non-script FAIL lines only in new: %s' % (sorted(ob - oa) or 'none'))
    same = not diff and oa == ob and fa == fb
    print('EQUIVALENT' if same else 'DIFFERENT')
    return 0 if same else 1


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'show':
        cmd_show(sys.argv[2])
    elif len(sys.argv) >= 4 and sys.argv[1] == 'compare':
        sys.exit(cmd_compare(sys.argv[2], sys.argv[3], '--md' in sys.argv))
    else:
        print(__doc__)
        sys.exit(2)
