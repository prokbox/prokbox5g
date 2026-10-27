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

"""Self-test of scripts/check_trace.py's Phase 11 rules (stdlib only).

    python3 tests/trace/test_p11_rules.py

Synthetic logs and scripts under build/trace-selftest: each p11 rule passes on
a good log, FAILS when its anchor lines are absent (brief 4.10: a selected
kind never skips), and FAILS on each violation, naming the value.
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
    p11 = [l for l in out.splitlines() if 'p11-' in l]
    bad = [l for l in p11 if l.startswith('FAIL')]
    good = (len(bad) > 0) == want_fail
    if good and needle and want_fail:
        good = any(needle in l for l in bad)
    print('%s %s' % ('ok  ' if good else 'FAIL', label))
    if not good:
        fails += 1
        print('     p11 lines: %r' % p11)


def sr(q, refine, n=30000, y=7):
    return ('search q=%d kind_n=%d,0,0,0,0 n=%d us=900 yields=%d refine=%d '
            'cap=0' % (q, n, n, y, refine))


T = lambda k: ['# trace: ' + k, 'wait 1']
# arena (every script)
expect('arena good', False, 'ar1', ['wait 1'], ['arena push s=search n=900 free=7000'])
expect('arena full', True, 'ar2', ['wait 1'], ['arena full need=9000 free=100'], 'arena full')
expect('arena full-ok', False, 'ar3', ['# arena: full-ok pinned small', 'wait 1'],
       ['arena full need=9000 free=100'])
expect('screen core_alloc', True, 'ar4', ['wait 1'], ['mem core_alloc s=playlists'], 'core_alloc')
# search
expect('search good', False, 's1', T('search'), [sr(1, 0), sr(2, 1), sr(3, 1)])
expect('search absent', True, 's2', T('search'), ['update full x'], 'no "search q=')
expect('search by name absent', True, 'ipodui-p11-search', ['wait 1'], ['update full x'], 'no "search q=')
expect('search refine missing', True, 's3', T('search'), [sr(1, 0), sr(2, 0)], 'refine=0')
expect('search yields low', True, 's4', T('search'), [sr(1, 0, 90000, 3)], 'yields=3')
# otg
RES = 'otg resolve n=5 unresolved=0 bytes=40000 reads=2 us=100'
expect('otg good', False, 'o1', T('otg'), ['otg add n=2 reads=3 writes=1', RES])
expect('otg absent', True, 'o2', T('otg'), ['update full x'], 'no "otg')
expect('otg add reads', True, 'o3', T('otg'), ['otg add n=1 reads=5 writes=1'], 'reads=5')
expect('otg resolve reads', True, 'o4', T('otg'),
       ['otg resolve n=5 unresolved=0 bytes=40000 reads=4 us=1'], 'reads=4')
expect('otg two resolves', True, 'o5', T('otg'), [RES, RES], '2 otg resolve')
expect('otg two resolves w/ reboot', False, 'o6', ['# trace: otg', 'reboot'], [RES, RES])
expect('otg playlist_create', True, 'o7', T('otg'),
       ['otg play n=3 pos=1 reads=1', 'playlist_create /x/otg.m3u8'], 'playlist_create')
expect('otg write in glide', True, 'o8', T('otg'),
       ['anim start boost=1', 'otg save name=a n=1', 'anim end boost=0'], 'inside an animation')
# hold
expect('hold good', False, 'h1', T('hold'), ['hold open ctx=song items=0x4d', 'hold open ctx=2 items=0x41'])
expect('hold absent', True, 'h2', T('hold'), ['update full x'], 'no "hold open')
expect('hold bad mask', True, 'h3', T('hold'), ['hold open ctx=group items=0x1d'], 'items=0x1d')
# vlimit
expect('vlimit good', False, 'v1', T('vlimit'),
       ['vlimit set=-20 code=1 applied=-20', 'vlimit unlock ok=1', 'vlimit set=-10 code=0 applied=-10'])
expect('vlimit absent', True, 'v2', T('vlimit'), ['update full x'], 'no "vlimit set=')
expect('vlimit applied over', True, 'v3', T('vlimit'), ['vlimit set=-20 code=0 applied=-5'], 'applied=-5')
expect('vlimit locked w/o unlock', True, 'v4', T('vlimit'),
       ['vlimit set=-20 code=1 applied=-20', 'vlimit set=-10 code=1 applied=-10'], 'unlock')
# about
expect('about good', False, 'a1', T('about'), ['about page=1', 'about page=3'])
expect('about page 4 (Save Diagnostics)', False, 'a2', T('about'), ['about page=4'])
expect('about page 5', True, 'a2b', T('about'), ['about page=5'], 'page=5')
expect('about chord no devmode', True, 'a3', ['# trace: about', 'chord select 3200'],
       ['about page=2'], 'devmode enter')
# sys
SL = 'sleep: charging, paused=1 saved'
expect('sys good', False, 'y1', ['# trace: sys', '# sys: dark'],
       [SL, 'sys charging state=charging', 'unlit enter', 'sys charging pop=key'])
expect('sys absent', True, 'y2', T('sys'), ['update full x'], 'no "sys charging')
expect('sys without sleep line', True, 'y3', T('sys'), ['sys charging state=charged'], 'sleep: charging')
expect('sys dark missing', True, 'y4', ['# trace: sys', '# sys: dark'],
       [SL, 'sys charging state=charging'], 'backlight-off')
expect('sys nested', True, 'y5', ['# trace: sys', '# sys: dark'],
       [SL, 'sys charging state=charging', SL, 'unlit enter'], 'nested')
# qw2
expect('qw2 good', False, 'q1', ['# trace: qw2', '# qw2: bundle'],
       ['np art src=ph-note reads=0 us=10', 'np art src=bundle reads=1 us=300', 'np art src=bundle reads=1 us=300'])
expect('qw2 absent', True, 'ipodui-qw2-x', ['wait 1'], ['update full x'], 'no "np art')
expect('qw2 reads 2', True, 'q2', T('qw2'), ['np art src=bundle reads=2 us=1'], 'reads=2')
expect('qw2 legacy after bundle', True, 'q3', ['# trace: qw2', '# qw2: bundle'],
       ['np art src=bundle reads=1 us=1', 'np art src=legacy reads=1 us=1'], 'legacy')
expect('qw2 render unlegacy', True, 'q4', T('qw2'),
       ['np art src=bundle reads=1 us=1', 'np_art_render x'], 'np_art_render')
# panelsync
W = 'panel want key=settings arg=eq'
expect('panelsync good', False, 'p1', T('panelsync'), [W, 'panel show key=settings arg=eq how=fade'])
expect('panelsync mismatch', True, 'p2', ['wait 1'], [W, 'panel show key=settings arg=bl how=fade'], 'arg=bl')
expect('panelsync show before want', True, 'p3', ['wait 1'], ['panel show key=a arg=b how=x'], 'before any panel want')
expect('panelsync declared none', True, 'p4', T('panelsync'), [W], 'no "panel show"')
sys.exit(1 if fails else 0)
