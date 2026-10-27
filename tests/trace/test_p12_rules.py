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

"""Self-test of scripts/check_trace.py's Phase 12 rules (stdlib only).

    python3 tests/trace/test_p12_rules.py

Synthetic logs and scripts under build/trace-selftest: each p12 rule passes
on a good log, FAILS when it is armed and its lines are absent, and FAILS on
each violation, naming the line and the value.
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
    open(lp, 'w').write('\n'.join(l if l.startswith('sim_script:')
                                  else 'ipodui: ' + l
                                  for l in log_lines) + '\n')
    r = subprocess.run([sys.executable, CT, lp, sp], capture_output=True,
                       text=True)
    return r.stdout


def expect(label, want_fail, name, script_lines, log_lines, needle=None):
    global fails
    out = run(name, script_lines, log_lines)
    p12 = [l for l in out.splitlines() if 'p12' in l]
    bad = [l for l in p12 if l.startswith('FAIL')]
    good = (len(bad) > 0) == want_fail
    if good and needle and want_fail:
        good = any(needle in l for l in bad)
    print('%s %s' % ('ok  ' if good else 'FAIL', label))
    if not good:
        fails += 1
        print('     p12 lines: %r' % p12)


T = lambda k, *more: ['# trace: ' + k] + list(more) + ['wait 1']
mc = lambda n, r=0: 'modclass %s class=A reads=%d writes=0 allocs=0 ' \
                    'boost_pct=0 fps=20' % (n, r)

# ---- modclass
expect('modclass good', False, 'm1', T('modclass'), [mc('clocks', 1), mc('clocks')])
expect('modclass absent (declared)', True, 'm2', T('modclass'),
       ['update full x'], 'no "modclass <name>')
expect('modclass absent by name', True, 'ipodui-p12-clocks-one', ['wait 1'],
       ['update full x'], 'no "modclass <name>')
expect('modclass absent, name opt-out', False, 'ipodui-p12-clocks-two',
       ['# modclass: none no Extra opens', 'wait 1'], ['update full x'])
expect('modclass absent, not armed', False, 'm3', ['wait 1'], ['update full x'])
expect('modclass viol fails any script', True, 'm4', ['wait 1'],
       ['modclass viol stopwatch reads in steady state'], 'modclass viol stopwatch')
expect('modclass viol fails even with good lines', True, 'm5', T('modclass'),
       [mc('clocks'), 'modclass viol clocks cpu_boost'], 'cpu_boost')
expect('modclass steady ok (reads on the first line only)', False, 'm6',
       T('modclass', '# modclass: steady'), [mc('clocks', 2), mc('clocks'), mc('notes', 1), mc('notes')])
expect('modclass steady violated', True, 'm7',
       T('modclass', '# modclass: steady'), [mc('clocks', 2), mc('clocks', 1)],
       'steady state must have reads=0')
expect('while-playing name arms it', True, 'ipodui-p12-stopwatch-while-playing',
       ['wait 1'], ['update full x'], 'no "modclass <name>')

# ---- alarm rings / arms / piezo
ring = 'alarm ring i=0'
expect('ring count ok', False, 'a1', T('alarm', '# alarm-rings: 2'),
       [ring, 'alarm dismiss', ring, 'alarm snooze'])
expect('ring twice for one scripted ring', True, 'a2', T('alarm', '# alarm-rings: 1'),
       [ring, ring, 'alarm dismiss'], 'appears 2 time(s)')
expect('ring declared 0, none', False, 'a3', T('alarm', '# alarm-rings: 0'),
       ['update full x'])
expect('ring declared 0, one rang', True, 'a4', T('alarm', '# alarm-rings: 0'),
       [ring], 'appears 1 time(s)')
expect('alarm-* name must declare', True, 'ipodui-p12-alarm-create', ['wait 1'],
       ['update full x'], 'must declare')
expect('ring in an undeclared script', True, 'a5', ['wait 1'], [ring],
       'no "# alarm-rings')
arm = lambda r, o: 'alarm arm rtc=%d ok=%d' % (r, o)
expect('arm ok', False, 'r1', T('alarmarm'), [arm(846054000, 1), arm(0, 1)])
expect('arm readback mismatch', True, 'r2', T('alarmarm'), [arm(846054000, 0)],
       'ok=0')
expect('arm absent (declared)', True, 'r3', T('alarmarm'), ['update full x'],
       '0 "alarm arm')
expect('arm count', True, 'r4', T('alarmarm', '# alarm-arms: 2'),
       [arm(1, 1)], 'need at least 2')
expect('arm by alarm-* name absent', True, 'ipodui-p12-alarm-keys',
       ['# alarm-rings: 0', 'wait 1'], ['update full x'], '0 "alarm arm')
expect('arm by alarm-* name, opted out', False, 'ipodui-p12-alarm-corrupt',
       ['# alarm-rings: 0', '# alarm-arm: none reads no alarms', 'wait 1'],
       ['update full x'])
piezo = 'piezo play hz=2027 ms=100'
expect('piezo while ringing', False, 'p1', T('piezo', '# alarm-rings: 1'),
       [ring, piezo, piezo, 'alarm dismiss'])
expect('piezo after dismiss', True, 'p2', T('piezo', '# alarm-rings: 1'),
       [ring, piezo, 'alarm dismiss', piezo], 'while no alarm is ringing')
expect('piezo with no alarm', True, 'p3', ['wait 1'], [piezo],
       'while no alarm is ringing')
expect('piezo after missed', True, 'p4', ['# alarm-rings: 1', 'wait 1'],
       [ring, 'alarm missed i=0 at=5', piezo], 'while no alarm')
expect('piezo ring ends at boot', True, 'p5', ['# alarm-rings: 1', 'wait 1'],
       [ring, 'boot screen gen=1', piezo], 'while no alarm')
expect('piezo declared, absent', True, 'p6', T('piezo'), ['update full x'],
       'no "piezo play')
expect('piezo free', False, 'p7', ['# piezo: free by hand', 'wait 1'], [piezo])
expect('alarm-ring name needs piezo', True, 'ipodui-p12-alarm-ring-awake',
       ['# alarm-rings: 1', 'wait 1'], [ring, arm(1, 1), 'alarm dismiss'],
       'no "piezo play')

# ---- xidx
xb = lambda k, n, ms: 'xidx build kind=%s n=%d ms=%d' % (k, n, ms)
usb = 'sim_script: usb_disconnect'
expect('xidx after usb', False, 'x1', T('xidx', '# xidx-builds: 2'),
       [usb, xb('notes', 3, 40), xb('contacts', 0, 1)])
expect('xidx while browsing', True, 'x2', T('xidx'), [xb('notes', 3, 40)],
       'built while browsing')
expect('xidx absent (declared)', True, 'x3', T('xidx'), ['update full x'],
       'no "xidx build')
expect('xidx count', True, 'x4', T('xidx', '# xidx-builds: 1'),
       [usb, xb('notes', 3, 40), xb('notes', 3, 40)], 'appears 2 time(s)')
expect('xidx declared none, none built', False, 'x5',
       T('xidx', '# xidx-builds: 0'), ['update full x'])
expect('xidx declared none, one built', True, 'x6',
       T('xidx', '# xidx-builds: 0'), [usb, xb('notes', 1, 1)], 'appears 1')
expect('xidx after boot', False, 'x7', T('xidx'),
       ['boot screen gen=1', xb('notes', 1, 1)])
expect('xidx too slow', True, 'x8', T('xidx'), [usb, xb('notes', 1, 70000)],
       'over 60000')

# ---- jobopen
expect('jobopen ok', False, 'j1', T('jobopen'), ['mod job_open stk=1312'])
expect('jobopen absent', True, 'j2', T('jobopen'), ['update full x'],
       'no "mod job_open')
expect('jobopen stk 0', True, 'j3', T('jobopen'), ['mod job_open stk=0'],
       'stk must be > 0')

print('%d failure(s)' % fails)
sys.exit(1 if fails else 0)
