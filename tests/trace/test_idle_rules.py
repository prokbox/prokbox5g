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

"""Self-test of scripts/check_trace.py's #58 idle-build rules (stdlib only).

    python3 tests/trace/test_idle_rules.py

Synthetic logs and scripts under build/trace-selftest: the "artio" rule
(no "art io" line with playing=1 and idle=0) and the "# logbetween:"
directive pass on a good log and FAIL on each violation, on an absent
mark, and (positive form) on an absent line.
"""
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
TMP = os.path.join(ROOT, 'build', 'trace-selftest')
CT = os.path.join(ROOT, 'scripts', 'check_trace.py')
fails = 0


def run(name, script_lines, log_lines):
    os.makedirs(TMP, exist_ok=True)
    sp = os.path.join(TMP, name + '.script')
    lp = os.path.join(TMP, name + '.log')
    open(sp, 'w').write('\n'.join(script_lines) + '\n')
    open(lp, 'w').write('\n'.join(log_lines) + '\n')
    r = subprocess.run([sys.executable, CT, lp, sp], capture_output=True,
                       text=True)
    return r.stdout


def expect(label, want_fail, name, script_lines, log_lines, tag, needle=None):
    global fails
    out = run(name, script_lines, log_lines)
    mine = [l for l in out.splitlines() if tag in l]
    bad = [l for l in mine if l.startswith('FAIL')]
    good = (len(bad) > 0) == want_fail and (want_fail or mine)
    if good and needle and want_fail:
        good = any(needle in l for l in bad)
    print('%s %s' % ('ok  ' if good else 'FAIL', label))
    if not good:
        fails += 1
        print('     lines: %r' % mine)


def io(p, a, i, rw='read'):
    return ('ipodui: art io %s key=0011223344556677 playing=%d active=%d '
            'idle=%d' % (rw, p, a, i))


W = ['wait 1']
# artio
expect('artio idle while playing', False, 'io1', W,
       [io(1, 0, 1), io(1, 1, 1, 'write'), io(0, 0, 0)], 'artio')
expect('artio stopped, user on', False, 'io2', W, [io(0, 0, 0)], 'artio')
expect('artio playing, user on', True, 'io3', W,
       [io(1, 1, 1), io(1, 1, 0, 'write')], 'art io', 'idle=0')
expect('artio old line (no idle=) while playing', True, 'io4', W,
       ['ipodui: art io read key=00 playing=1 active=1'], 'art io')

M = lambda *names: ['sim_script: ab_mark %s' % n for n in names]
LB = '# logbetween: a b no \\bart io\\b'
expect('logbetween no: clean span', False, 'lb1', [LB, 'wait 1'],
       [io(0, 0, 0)] + M('a') + ['ipodui: art build wait lit'] + M('b')
       + [io(0, 0, 0)], 'logbetween')
expect('logbetween no: a hit', True, 'lb2', [LB, 'wait 1'],
       M('a') + [io(1, 1, 1)] + M('b'), 'logbetween', 'art io')
expect('logbetween: missing mark', True, 'lb3', [LB, 'wait 1'],
       M('a') + [io(0, 0, 0)], 'logbetween', 'ab_mark b')
expect('logbetween yes: present', False, 'lb4',
       ['# logbetween: a end art wake ev=1', 'wait 1'],
       M('a') + ['ipodui: art wake ev=1'], 'logbetween')
expect('logbetween yes: absent', True, 'lb5',
       ['# logbetween: start a art wake ev=1', 'wait 1'],
       M('a') + ['ipodui: art wake ev=1'], 'logbetween', 'no /art wake')
expect('logbetween: mark names are whole words', True, 'lb6',
       ['# logbetween: a ab art wake', 'wait 1'],
       M('ab') + ['ipodui: art wake ev=1'], 'logbetween', 'ab_mark a')

print('%d failed' % fails if fails else 'all idle rule cases passed')
sys.exit(1 if fails else 0)
