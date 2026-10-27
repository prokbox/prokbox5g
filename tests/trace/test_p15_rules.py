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

"""Self-test of scripts/check_trace.py's Phase 15 rules (stdlib only).

    python3 tests/trace/test_p15_rules.py

Synthetic logs and scripts under build/trace-selftest: each p15 rule passes on
a good log, FAILS when a script declares it and its lines are absent (brief
2.4), and FAILS on each violation, naming the value.
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
    open(lp, 'w').write('\n'.join('ipodui: ' + l for l in log_lines) + '\n')
    r = subprocess.run([sys.executable, CT, lp, sp], capture_output=True,
                       text=True)
    return r.stdout


def expect(label, want_fail, name, script_lines, log_lines, needle=None):
    global fails
    out = run(name, script_lines, log_lines)
    p15 = [l for l in out.splitlines() if 'p15' in l or 'eq-engines' in l
           or 'vol-' in l or 'fade:' in l or 'sc-seq' in l]
    bad = [l for l in p15 if l.startswith('FAIL')]
    good = (len(bad) > 0) == want_fail
    if good and needle and want_fail:
        good = any(needle in l for l in bad)
    print('%s %s' % ('ok  ' if good else 'FAIL', label))
    if not good:
        fails += 1
        print('     p15 lines: %r' % p15)


T = lambda k, *more: ['# trace: ' + k] + list(more) + ['wait 1']
# eq: preset 2 (Bass Booster) is hw 6 dB maxerr 9; preset 5 (Dance) is sw 22
eqs = lambda n, en, pc: 'eq preset=%d enabled=%d precut=%d' % (n, en, pc)
eqe = lambda e, n, m, p: 'eq engine=%s preset=%d maxerr=%d precut=%d' % (e, n, m, p)
expect('eq good', False, 'e1', T('p15eq'),
       [eqs(2, 0, 0), eqe('hw', 2, 9, 6), eqs(5, 0, 0), eqe('hw', 5, 22, 9),
        eqs(0, 0, 0), eqe('sw', 0, 0, 0)])
expect('eq absent', True, 'e2', T('p15eq'), ['update full x'], 'no "eq engine=')
expect('eq absent by name', True, 'ipodui-p15-eq-x', ['wait 1'],
       ['update full x'], 'no "eq engine=')
expect('eq no script decl, no lines', False, 'e3', ['wait 1'], ['update full x'])
expect('eq sw for a preset the table puts on hw (all, owner)', True, 'e4',
       T('p15eq'), [eqs(5, 1, 66), eqe('sw', 5, 22, 7)], 'hw iff')
expect('eq sw but maxerr within 15', True, 'e5', T('p15eq'),
       [eqs(2, 1, 60), eqe('sw', 2, 9, 6)], 'hw iff')
expect('eq wrong maxerr for preset', True, 'e6', T('p15eq'),
       [eqs(2, 0, 0), eqe('hw', 2, 7, 6)], 'table says 9')
expect('eq hw wrong precut', True, 'e7', T('p15eq'),
       [eqs(2, 0, 0), eqe('hw', 2, 9, 3)], 'precut 3')
expect('eq Off must be sw zeros', True, 'e8', T('p15eq'),
       [eqs(0, 0, 0), eqe('hw', 0, 0, 0)], 'Off must be sw')
expect('eq two engines (hw + sw eq on)', True, 'e9', T('p15eq'),
       [eqs(2, 1, 60), eqe('hw', 2, 9, 6)], 'two engines')
expect('eq no engine (Off logged with the sw eq on)', True, 'e10', T('p15eq'),
       [eqs(0, 1, 0), eqe('sw', 0, 0, 0)], 'engine')
expect('eq preset out of range', True, 'e11', T('p15eq'),
       [eqe('sw', 23, 0, 0)], 'outside 0..22')
expect('eq pin mismatch', True, 'e12',
       T('p15eq', '# eq-engines: 2=sw'),
       [eqs(2, 0, 0), eqe('hw', 2, 9, 6)], 'want sw')
# dac
dac = lambda s, sl, w, us=0: 'dac standby=%d sleeps=%d wakes=%d wake_us=%d' % (s, sl, w, us)
expect('dac good', False, 'd1', T('p15dac'),
       [dac(1, 1, 0), dac(0, 1, 1, 8000), dac(1, 2, 1)])
expect('dac absent', True, 'd2', T('p15dac'), ['update full x'], 'no "dac standby=')
expect('dac counts go down', True, 'd3', T('p15dac'),
       [dac(1, 2, 1), dac(0, 1, 2)], 'counts went down')
expect('dac wakes > sleeps+1', True, 'd4', T('p15dac'),
       [dac(0, 0, 2)], 'wakes 2 > sleeps 0 + 1')
expect('dac repeat line', True, 'd5', T('p15dac'),
       [dac(1, 1, 0), dac(1, 1, 0)], 'without a count changing')
expect('dac standby=1 without sleep', True, 'd6', T('p15dac'),
       [dac(1, 0, 1)], 'no sleep counted')
expect('dac counts restart after a boot', False, 'd9', T('p15dac'),
       [dac(1, 3, 3), 'boot screen gen=1', dac(1, 1, 0)])
expect('dac standby 2', True, 'd7', T('p15dac'), [dac(2, 1, 0)], 'not 0|1')
expect('dac final bound', True, 'd8', T('p15dac', '# dac: sleeps>=3'),
       [dac(1, 1, 0)], 'final sleeps=1 < 3')
# vol: step 24 = -17 dB, bar 24*1024//47 = 522; step 0 = -90, 47 = +6
vol = lambda s, db, b: 'vol step=%d db=%d bar=%d' % (s, db, b)
expect('vol good', False, 'v1', T('p15vol'),
       [vol(0, -90, 0), vol(1, -60, 21), vol(24, -17, 522), vol(47, 6, 1024)])
expect('vol absent', True, 'v2', T('p15vol'), ['update full x'], 'no "vol step=')
expect('vol bar off', True, 'v3', T('p15vol'), [vol(24, -17, 523)], 'bar 523')
expect('vol db off', True, 'v4', T('p15vol'), [vol(24, -16, 522)], 'volcurve.c says -17')
expect('vol step out of range', True, 'v5', T('p15vol'), [vol(48, 6, 1024)], 'outside 0..47')
expect('vol cap respected', False, 'v6', T('p15vol', '# vol-max: 24'),
       [vol(23, -18, 501), vol(24, -17, 522), vol(24, -17, 522)])
expect('vol cap exceeded', True, 'v7', T('p15vol', '# vol-max: 24'),
       [vol(25, -16, 544)], 'cap is 24')
expect('vol cap never reached', True, 'v8', T('p15vol', '# vol-max: 24'),
       [vol(20, -21, 435)], 'cap never reached')
expect('vol walk jump', True, 'v9', T('p15vol', '# vol-walk: 1'),
       [vol(1, -60, 21), vol(3, -52, 65)], 'more than 1 apart')
# fade
fade = lambda d, n: 'fade dir=%s steps=%d' % (d, n)
expect('fade good', False, 'f1', T('p15fade', '# fade: reverse'),
       [fade('out', 25), fade('in', 19), fade('out', 25)])
expect('fade absent', True, 'f2', T('p15fade'), ['update full x'], 'no "ipodui: fade')
expect('fade steps over 25', True, 'f3', T('p15fade'), [fade('out', 26)], 'outside 0..25')
expect('fade bad dir', True, 'f4', T('p15fade'), [fade('up', 25)], 'not in|out')
expect('fade reverse missing', True, 'f5', T('p15fade', '# fade: reverse'),
       [fade('out', 25), fade('in', 25)], 'a restart')

# hardware Sound Check
sc = lambda n, k, e='hw': 'sc engine=%s gain_db10=%d track=%d' % (e, n, k)
expect('sc good', False, 's1', T('p15sc', '# sc-seq: 0:0 1:-65 2:32'),
       [sc(0, 0), sc(-65, 1), sc(32, 2)])
expect('sc absent', True, 's2', T('p15sc'), ['update full x'], 'no "ipodui: sc')
expect('sc absent by name', True, 'ipodui-p15-soundcheck-x', ['wait 1'],
       ['update full x'], 'no "ipodui: sc')
expect('sc sw engine', True, 's3', T('p15sc'), [sc(-65, 1, 'sw')], 'not hw')
expect('sc gain out of range', True, 's4', T('p15sc'), [sc(300, 1)],
       'outside -480..290')
expect('sc repeat', True, 's5', T('p15sc'), [sc(-65, 1), sc(-65, 1)],
       'repeats the previous')
expect('sc repeat across a reboot', False, 's7', T('p15sc'),
       [sc(0, 0), 'boot screen gen=1', sc(0, 0)])
expect('sc wrong sequence', True, 's6', T('p15sc', '# sc-seq: 0:0 1:-65'),
       [sc(0, 0), sc(-60, 1)], '# sc-seq')

print('%d failure(s)' % fails)
sys.exit(1 if fails else 0)
