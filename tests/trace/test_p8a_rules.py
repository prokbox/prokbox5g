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

"""Self-test of scripts/check_trace.py's Phase 8a rules (stdlib only).

    python3 tests/trace/test_p8a_rules.py

Runs check_trace.py on synthetic logs and scripts written under build/ and
checks that each rule passes on a good log, FAILS when its anchor lines are
absent (brief 4.8: a p8a script never skips), and FAILS on each violation.
"""
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
TMP = os.path.join(ROOT, 'build', 'trace-selftest')
CT = os.path.join(ROOT, 'scripts', 'check_trace.py')
GEN = os.path.join(ROOT, 'firmware', 'export', 'bootscreen_data.h')
fails = 0


def run(name, script_lines, log_lines):
    os.makedirs(TMP, exist_ok=True)
    sp = os.path.join(TMP, name + '.script')
    lp = os.path.join(TMP, name + '.log')
    open(sp, 'w').write('\n'.join(script_lines) + '\n')
    open(lp, 'w').write('\n'.join('ipodui: ' + l for l in log_lines) + '\n')
    r = subprocess.run([sys.executable, CT, lp, sp], capture_output=True,
                       text=True)
    return r.returncode, r.stdout


def expect(label, want_fail, name, script_lines, log_lines, needle=None):
    global fails
    rc, out = run(name, script_lines, log_lines)
    p8a = [l for l in out.splitlines() if 'p8a-' in l]
    bad = [l for l in p8a if l.startswith('FAIL')]
    good = (len(bad) > 0) == want_fail
    if good and needle and want_fail:
        good = any(needle in l for l in bad)
    print('%s %s' % ('ok  ' if good else 'FAIL', label))
    if not good:
        fails += 1
        print('     p8a lines: %r' % p8a)


US = ['usb enter glyph=usbc shell=1', 'usb io=0', 'update usb:static y=0 h=240',
      'usb io=1', 'update usb:frame y=53 h=92', 'usb exit frames=3 boost=0']
expect('usb good', False, 'usb1', ['# trace: usb', 'wait 1'], US)
expect('usb anchors absent', True, 'usb2', ['# trace: usb', 'wait 1'],
       ['update full menu'], 'no "usb enter')
expect('usb absent via script name', True, 'ipodui-p8a-usb', ['wait 1'],
       ['update full menu'], 'no "usb enter')
expect('usb boost!=0', True, 'usb3', ['# trace: usb'],
       US[:-1] + ['usb exit frames=3 boost=1'], 'boost=1')
expect('usb foreign update inside', True, 'usb4', ['# trace: usb'],
       US[:3] + ['update band y=0 h=30'] + US[3:], 'non-usb update')
expect('usb frame while io=0', True, 'usb5', ['# trace: usb'],
       ['usb enter glyph=usbc shell=1', 'usb io=0',
        'update usb:frame y=53 h=92', 'usb exit frames=1 boost=0'],
       'io=0')
expect('usb frame wrong band', True, 'usb6', ['# trace: usb'],
       US[:4] + ['update usb:frame y=68 h=80'] + US[5:], 'want (53, 92)')
expect('usb static band restore ok', False, 'usb8', ['# trace: usb'],
       US[:5] + ['usb io=0', 'update usb:static y=53 h=92'] + US[5:])
expect('usb static wrong band', True, 'usb9', ['# trace: usb'],
       US[:5] + ['usb io=0', 'update usb:static y=68 h=80'] + US[5:],
       'usb:static y=68')
expect('usb exit without enter', True, 'usb7', ['# trace: usb'],
       ['usb exit frames=0 boost=0'], 'without')

UL = ['unlit enter', 'update unlit:full', 'update unlit:digits y=62 h=68',
      'update unlit:status y=161 h=37', 'unlit exit key', 'update full menu']
expect('unlit good', False, 'unlit1', ['# trace: unlit'], UL)
expect('unlit anchors absent', True, 'unlit2', ['# trace: unlit'],
       ['update full menu'], 'no "unlit enter')
expect('unlit foreign update inside', True, 'unlit3', ['# trace: unlit'],
       UL[:2] + ['update band y=20 h=30'] + UL[2:], 'non-unlit update')
expect('unlit digits wrong band', True, 'unlit4', ['# trace: unlit'],
       [UL[0], UL[1], 'update unlit:digits y=62 h=60'] + UL[3:], 'unlit:digits')
expect('unlit digits count', True, 'unlit5',
       ['# trace: unlit', '# unlit-digits: 2'], UL, 'unlit-digits')
expect('unlit digits range ok', False, 'unlit6',
       ['# trace: unlit', '# unlit-digits: 1 2'], UL)
expect('unlit update outside face', True, 'unlit7', ['# trace: unlit'],
       UL + ['update unlit:digits y=62 h=68'], 'outside')
expect('unlit exit key then non-full', True, 'unlit8', ['# trace: unlit'],
       UL[:5] + ['update band y=20 h=30'], 'not "update full"')
expect('unlit bad exit value', True, 'unlit9', ['# trace: unlit'],
       UL[:4] + ['unlit exit bogus'], 'bogus')
expect('unlit skip-expired accepted', False, 'unlit11', ['# trace: unlit'],
       ['unlit skip sleep', 'unlit skip-expired'] + UL)
expect('unlit skip bogus', True, 'unlit12', ['# trace: unlit'],
       ['unlit skip bogus'] + UL, 'unlit skip bogus')
expect('unlit never exits', True, 'unlit10', ['# trace: unlit'], UL[:3],
       'without an unlit exit')

DV = ['devmode arm', 'devmode enter', 'devmode exit']
expect('devmode good', False, 'dev1',
       ['# trace: devmode', 'chord SELECT+LEFT 3200'], DV)
expect('devmode anchors absent', True, 'dev2',
       ['# trace: devmode', 'chord SELECT+LEFT 3200'], [], 'no "devmode enter')
expect('devmode short chord', True, 'dev3',
       ['# trace: devmode', 'chord SELECT+LEFT 1500'], DV, 'chord')
expect('devmode enter without arm', True, 'dev4',
       ['# trace: devmode', 'chord SELECT+LEFT 3200'], DV[1:], 'no devmode arm')
expect('devmode boot escape (no arm)', False, 'dev5',
       ['# trace: devmode', '# boot_keys: RIGHT 2500'], DV[1:])
expect('devmode none: no enter', False, 'dev6',
       ['# trace: devmode', '# devmode: none'], ['devmode arm'])
expect('devmode none: enter present', True, 'dev7',
       ['# trace: devmode', '# devmode: none', 'chord A+B 3200'], DV,
       'devmode: none')

expect('splash good', False, 'sp1', ['# trace: splash'],
       ['splash-suppressed probe'])
expect('splash absent', True, 'sp2', ['# trace: splash'], [], 'no "splash')
expect('splash-pass outside devmode', True, 'sp3', [],
       ['splash-pass hello'], 'splash-pass')
expect('splash-pass in devmode script', False, 'sp4',
       ['# devmode: none'], ['splash-pass hello'])

expect('log good', False, 'lg1', ['# trace: log'],
       ['log flush lines=3 seq=4-6'])
expect('log absent', True, 'lg2', ['# trace: log'], [], 'no "log flush')
expect('log lines=0', True, 'lg3', [], ['log flush lines=0 seq=4-4'], 'lines=0')

# boot: needs the generated header; checked against a stand-in when absent
if os.path.exists(GEN):
    gen = None
    for l in open(GEN):
        if l.startswith('#define BOOTSCREEN_GEN'):
            gen = l.split()[2].rstrip('uUlL').lstrip('(').rstrip(')')
    if gen:
        h = gen[2:] if gen.lower().startswith('0x') else gen
        expect('boot good', False, 'bo1', ['# trace: boot'],
               ['boot screen gen=' + h])
        expect('boot wrong gen', True, 'bo2', ['# trace: boot'],
               ['boot screen gen=deadbeef1'], 'BOOTSCREEN_GEN')
expect('boot absent', True, 'bo3', ['# trace: boot'], [], 'no "boot screen')
expect('boot twice', True, 'bo4', ['# trace: boot'],
       ['boot screen gen=1', 'boot screen gen=1'], 'want exactly 1')

print('%d failure(s)' % fails)
sys.exit(1 if fails else 0)
