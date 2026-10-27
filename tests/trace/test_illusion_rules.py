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

"""Self-test of scripts/check_trace.py's illusion rule (P12 close, the
"Rockbox" checklist rows; stdlib only).

    python3 tests/trace/test_illusion_rules.py

A drawn text with "Rockbox" or ".rockbox" fails; a str() of LANG id 0 or
out of range in a shell file fails; "# illusion: devmenu" turns drawn text
into a note but not a str() warning; a clean watched log passes; a log
without the watch is a note.
"""
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
TMP = os.path.join(ROOT, 'build', 'trace-selftest')
CT = os.path.join(ROOT, 'scripts', 'check_trace.py')
fails = 0
WATCH = 'sim text: watch on'


def run(name, log_lines, header=''):
    os.makedirs(TMP, exist_ok=True)
    sp = os.path.join(TMP, name + '.script')
    lp = os.path.join(TMP, name + '.log')
    open(sp, 'w').write(header + 'wait 1\n')
    open(lp, 'w').write('\n'.join(log_lines) + '\n')
    return subprocess.run([sys.executable, CT, lp, sp], capture_output=True,
                          text=True).stdout


def expect(label, name, log, want_fail, needle, header=''):
    global fails
    out = run(name, log, header)
    rule = [l for l in out.splitlines() if 'illusion' in l]
    bad = [l for l in rule if l.startswith('FAIL')]
    good = (len(bad) > 0) == want_fail and any(needle in l for l in rule)
    print('%s %s' % ('ok  ' if good else 'FAIL', label))
    if not good:
        fails += 1
        print('     illusion lines: %r' % rule)


expect('clean watched log passes', 'ill_clean', [WATCH], False,
       'no drawn text')
expect('no watch is a note', 'ill_nowatch', [], False, 'no text watch')
expect('a Rockbox row fails', 'ill_row',
       [WATCH, 'sim text: forbidden x=8 y=150 text="Rockbox"'], True,
       '"Rockbox" at x=8 y=150')
expect('a .rockbox path fails', 'ill_path',
       [WATCH, 'sim text: forbidden x=8 y=60 text="/.rockbox/config.cfg"'],
       True, '.rockbox/config.cfg')
expect('str() of id 0 fails', 'ill_lang0',
       [WATCH, 'lang: str id=0 zero at apps/ipodui/screens/checklist.c:163'],
       True, 'checklist.c:163')
expect('str() out of range fails', 'ill_oob',
       [WATCH, 'lang: str id=9999 out-of-range at apps/ipodui/menu.c:12'],
       True, 'out-of-range')
expect('devmenu: drawn text is a note', 'ill_dev',
       [WATCH, 'sim text: forbidden x=0 y=0 text="Rockbox Info"'], False,
       'allowed by', header='# illusion: devmenu stock debug menu\n')
expect('devmenu: str() id 0 still fails', 'ill_dev0',
       [WATCH, 'sim text: forbidden x=0 y=0 text="Rockbox Info"',
        'lang: str id=0 zero at apps/ipodui/x.c:1'], True, 'LANG id 0',
       header='# illusion: devmenu stock debug menu\n')
expect('the Legal credit (allowed sentence) passes', 'ill_legal',
       [WATCH, 'sim text: forbidden x=10 y=28 text="Based on Rockbox '
        '(rockbox.org), licensed under"'], False, 'allowed sentence')
expect('a Rockbox word outside the allowed sentence fails', 'ill_notlegal',
       [WATCH, 'sim text: forbidden x=10 y=28 text="Based on Rockbox 4.0"'],
       True, 'Rockbox 4.0')
sys.exit(1 if fails else 0)
