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

"""Self-test of scripts/check_hang_rule.py's panic rule
(the checker scripts/shots.sh runs on every script, perf ones included) (stdlib only).

    python3 tests/trace/test_panic_rules.py

Synthetic logs and scripts under build/trace-selftest: a panic or fatal
marker FAILS the script; "# expect: panic <regex>" makes the same log pass;
a clean log passes; a directive whose panic never happens FAILS; a marker
the directive does not match FAILS; hang records are not this rule's; check_trace.py does not repeat it.
"""
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
TMP = os.path.join(ROOT, 'build', 'trace-selftest')
CT = os.path.join(ROOT, 'scripts', 'check_hang_rule.py')
fails = 0


def run(name, script_lines, log_lines):
    os.makedirs(TMP, exist_ok=True)
    sp = os.path.join(TMP, name + '.script')
    lp = os.path.join(TMP, name + '.log')
    open(sp, 'w').write('\n'.join(script_lines) + '\n')
    open(lp, 'w').write('\n'.join(log_lines) + '\n')
    r = subprocess.run([sys.executable, CT, sp, lp], capture_output=True,
                       text=True)
    return r.stdout + r.stderr


def expect(label, want_fail, name, script_lines, log_lines, needle=None):
    global fails
    out = run(name, script_lines, log_lines)
    mine = [l for l in out.splitlines() if 'panic rule' in l]
    bad = [l for l in mine if l.startswith('FAIL')]
    good = (len(bad) > 0) == want_fail and bool(mine)
    if good and needle and want_fail:
        good = any(needle in l for l in bad)
    print('%s %s' % ('ok  ' if good else 'FAIL', label))
    if not good:
        fails += 1
        print('     lines: %r' % mine)


S = ['wait 1']
CLEAN = ['ipodui: boot screen gen=b528f32f', 'ipodui: fe shell up mode=inline']
TICK = 'panicf: Error! tick_add_task(): out of tasks'
REC = ['ipodui: panic kind=0 count=1 thread=main',
       'ipodui: panic pc=00012340 lr=00000000 sp=00000000',
       'ipodui: panic msg Error! tick_add_task(): out of tasks']

# (a) a panic fails
expect('panicf line fails', True, 'pa1', S, CLEAN + [TICK],
       'log line 3')
expect('bare Error! message fails', True, 'pa2', S,
       CLEAN + ['Error! tick_add_task(): out of tasks'], 'Error!')
expect('logged record fails', True, 'pa3', S, CLEAN + REC, 'panic kind=0')
expect('Stkov fails', True, 'pa4', S, CLEAN + ['Stkov ui (0)'], 'Stkov')
expect('stack overflow fails', True, 'pa5', S,
       CLEAN + ['thread: stack overflow in artcache'], 'stack overflow')
expect('assert fails', True, 'pa6', S,
       CLEAN + ['x.c:12: Assertion `n > 0\' failed.'], 'Assertion')
expect('signal fails', True, 'pa7', S, CLEAN + ['Segmentation fault: 11'],
       'Segmentation')
expect('*PANIC* banner fails', True, 'pa8', S, CLEAN + ['*PANIC* (x)'],
       'PANIC')
# the panic at the very end, after every other check passed
expect('panic as the last line fails', True, 'pa9', S, CLEAN + [TICK],
       'out of tasks')

# (b) the same logs with the directive pass
expect('directive passes panicf', False, 'pb1',
       ['# expect: panic out of tasks'] + S, CLEAN + [TICK])
expect('directive passes the logged record block', False, 'pb2',
       ['# expect: panic out of tasks'] + S, CLEAN + REC)
expect('two panics, two directives', False, 'pb3',
       ['# expect: panic out of tasks', '# expect: panic Stkov ui'] + S,
       CLEAN + [TICK, 'Stkov ui (0)'])

# (c) a clean log passes
expect('clean log passes', False, 'pc1', S, CLEAN)
expect('hang records are the hang rule\'s, not this one', False, 'pc2', S,
       CLEAN + ['ipodui: hang recovered ui after 8s',
                'ipodui: panic hang record thread=main count=1',
                'ipodui: panic msg hang: ui no progress 6 s ('])

# (d) a directive whose panic never happens fails; a marker the directive
# does not name fails
expect('directive without a panic fails', True, 'pd1',
       ['# expect: panic out of tasks'] + S, CLEAN, 'never happened')
expect('directive naming another panic fails (both ways)', True, 'pd2',
       ['# expect: panic Stkov'] + S, CLEAN + [TICK], 'never happened')
expect('second, undeclared panic fails', True, 'pd3',
       ['# expect: panic out of tasks'] + S, CLEAN + [TICK, 'Stkov ui (0)'],
       'Stkov')
expect('bad regex fails', True, 'pd4', ['# expect: panic ((('] + S, CLEAN,
       'bad "# expect: panic" regex')

print('%d failed' % fails if fails else 'all panic rule cases passed')
sys.exit(1 if fails else 0)
